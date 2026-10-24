// synth_control.cpp on the PC: the firmware's key, MIDI and settings logic, compiled unchanged with
// stubs for ChibiOS, the HAL, the leds and MIDI out (tests/synth_control/stubs), driven through its
// real entry points (synth_message, MidiInMsgHandler, config_but, synth_touch_set, synth_tick).
// The config is the real config_store.c with the firmware defaults (tests/config_defaults.c) and a
// user settings table mapped at the board's flash address.
//
//   test_synth_control <scenario>       one scenario per process (the firmware's state is global);
//                                       exit 1 if one of its checks fails (ctest synth_control.*)
//   test_synth_control list             the scenario names
//   test_synth_control replay <take.csv> [--mode mpe|normal|mono] [--trace FILE] [--expect HASH]
//                                       [--golden-name N]
//       a recorded key stream (tools/harness/rec, tools/record_keys.py) through synth_message, as
//       the board's key thread gets it, with synth_tick every 2 ms. The trace is every MIDI message
//       sent and every change of the internal synth's voice controls, per key message. Checks that
//       no MIDI note and no internal voice is left on at the end; prints "hash <FNV-1a 64 of the
//       trace>". --expect: fail unless it matches (tests/golden/control_replay.txt);
//       STRISO_GOLDEN_OUT=<dir> with --golden-name: write <dir>/N.hash instead (update_goldens.sh).
//
// The internal synth's control block mirrors synth_strisy.cpp: VOICECOUNT real voices, the rest of
// the MAX_VOICECOUNT slots write to a dummy.
//
// Standard headers first: synth_control.cpp defines function-like macros min/max/clamp.
#include <sys/mman.h>

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "synth_control.cpp"

// ------------------------------------------------------------------------------------------ stubs
extern "C" {
systime_t host_now = 1000;
void chThdSleepMilliseconds(int /*ms*/) {}
static bool reset_called = false;
void NVIC_SystemReset(void) { reset_called = true; }
int palReadLine(int /*line*/) { return 1; }
int chprintf(BaseSequentialStream* /*chp*/, const char* fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	const int r = vprintf(fmt, ap);
	va_end(ap);
	return r;
}
static int last_r, last_g, last_b, last_dial = -999, blink_count = 0;
void led_rgb3(int r, int g, int b) {
	last_r = r;
	last_g = g;
	last_b = b;
}
void led_rgb(uint32_t rgb) { led_rgb3(rgb >> 16 & 0xff, rgb >> 8 & 0xff, rgb & 0xff); }
void led_rgb3_blink(int /*r*/, int /*g*/, int /*b*/, sysinterval_t /*time*/) { blink_count++; }
void led_updown(uint32_t /*state*/) {}
void led_updown_dial(int angle) { last_dial = angle; }
void aux_jack_switch_mode_setting(jack2_mode_t /*mode*/) {}
int msgSend(int /*size*/, int* /*msg*/) { return 0; }
float volume = 0;
synth_interface_t synth_interface;
int synth_num_voices(void) { return 2; }
const char* synth_voice_name(int n, const char** preset) {
	*preset = nullptr;
	return n == 0 ? "strisy" : n == 1 ? "va" :
	                                    nullptr;
}
int synth_current_voice(void) { return 0; }
int synth_select_voice(int /*n*/) { return 1; }
}

static std::vector<std::vector<uint8_t>> midi_log;
extern "C" void midi_usb_MidiSend1(uint8_t /*port*/, uint8_t b0) { midi_log.push_back({b0}); }
extern "C" void midi_usb_MidiSend2(uint8_t /*port*/, uint8_t b0, uint8_t b1) { midi_log.push_back({b0, b1}); }
extern "C" void midi_usb_MidiSend3(uint8_t /*port*/, uint8_t b0, uint8_t b1, uint8_t b2) { midi_log.push_back({b0, b1, b2}); }
extern "C" void serial_MidiSend1(uint8_t /*b0*/) {}
extern "C" void serial_MidiSend2(uint8_t /*b0*/, uint8_t /*b1*/) {}
extern "C" void serial_MidiSend3(uint8_t /*b0*/, uint8_t /*b1*/, uint8_t /*b2*/) {}

// config lookups of keys that exist neither in the user table nor in default_config (linker --wrap)
static std::vector<std::string> misses;
extern "C" {
const char* __real_getConfigSetting(const char* name);
float __real_getConfigFloat(const char* name);
int __real_getConfigInt(const char* name);
unsigned int __real_getConfigHex(const char* name);
static void check_key(const char* name) {
	if (strcmp(__real_getConfigSetting(name), "(ERROR!)") == 0) misses.emplace_back(name, 8);
}
const char* __wrap_getConfigSetting(const char* name) {
	check_key(name);
	return __real_getConfigSetting(name);
}
float __wrap_getConfigFloat(const char* name) {
	check_key(name);
	return __real_getConfigFloat(name);
}
int __wrap_getConfigInt(const char* name) {
	check_key(name);
	return __real_getConfigInt(name);
}
unsigned int __wrap_getConfigHex(const char* name) {
	check_key(name);
	return __real_getConfigHex(name);
}
}

// the user settings (config flash page at its board address, 0x08020000), as test_config_store.cpp
static ConfigParam* user_config() {
	static ConfigParam* page = [] {
		void* want = reinterpret_cast<void*>(flash_config);
		void* p = mmap(want, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
		if (p == MAP_FAILED || p != want) {
			std::fprintf(stderr, "can't map the config flash page at %p\n", want);
			std::exit(2);
		}
		auto* c = static_cast<ConfigParam*>(p);
		memcpy(c[0].key, "MCfgEnd ", 8);
		return c;
	}();
	return page;
}
// set a user setting: key and value are padded to 8 characters
static void set_cfg(const char* k, const char* v) {
	ConfigParam* c = user_config();
	int i = 0;
	while (memcmp(c[i].key, "MCfgEnd ", 8) != 0) i++;
	char key[9], val[9];
	snprintf(key, sizeof key, "%-8s", k);
	snprintf(val, sizeof val, "%-8s", v);
	memcpy(c[i].key, key, 8);
	memcpy(c[i].value, val, 8);
	memcpy(c[i + 1].key, "MCfgEnd ", 8);
}

// the internal synth's control block (synth_strisy.cpp)
struct VC {
	float note, pres, vpres, but_x, but_y;
};
static VC vc[VOICECOUNT];
static float dump_f, pedal_f = 3.0f / 16, acc[7];
static void init_synth_interface() {
	synth_interface.acc_abs = &acc[0];
	synth_interface.acc_x = &acc[1];
	synth_interface.acc_y = &acc[2];
	synth_interface.acc_z = &acc[3];
	synth_interface.rot_x = &acc[4];
	synth_interface.rot_y = &acc[5];
	synth_interface.rot_z = &acc[6];
	synth_interface.pedal = &pedal_f;
	for (int v = 0; v < MAX_VOICECOUNT; v++) {
		const bool used = v < VOICECOUNT;
		synth_interface.note[v] = used ? &vc[v].note : &dump_f;
		synth_interface.pres[v] = used ? &vc[v].pres : &dump_f;
		synth_interface.vpres[v] = used ? &vc[v].vpres : &dump_f;
		synth_interface.but_x[v] = used ? &vc[v].but_x : &dump_f;
		synth_interface.but_y[v] = used ? &vc[v].but_y : &dump_f;
	}
	for (auto& c : vc) c = {69.0f, 0.0f, 0.0f, 0.0f, 0.0f};
}

// --------------------------------------------------------------------------------------- checks
static int failures = 0;
#define EXPECT(cond, ...) \
	do { \
		if (!(cond)) { \
			failures++; \
			std::printf("FAIL line %d: %s: ", __LINE__, #cond); \
			std::printf(__VA_ARGS__); \
			std::printf("\n"); \
		} \
	} while (0)

// ------------------------------------------------------------------------------------------ input
static int K(int n) { return button_number_map[n]; } // button id of key number n (0..60)
static void key(int but, float pres, float vpres = 0, float x = 0, float y = 0) {
	int msg[6] = {ID_DIS, but, (int)lrintf(pres * 0x1fff), (int)lrintf(vpres * 0x1fff),
	    (int)lrintf(x * 0x1fff), (int)lrintf(y * 0x1fff)};
	synth_message(6, msg);
}
static void ctl(int id, int v) {
	int msg[3] = {ID_CONTROL, id, v};
	synth_message(3, msg);
}
static void stck(int id) { // a settings key from the console (StcK), as Striso Live does
	int msg[3] = {ID_INTERNAL, IDI_SETTINGS_KEY, id};
	synth_message(3, msg);
}
static void midi_in(uint8_t s, uint8_t d1, uint8_t d2) { MidiInMsgHandler(MIDI_DEVICE_USB_DEVICE, 1, s, d1, d2); }
static void rpn(int ch, int msb, int lsb, int value) {
	midi_in(0xB0 | ch, 101, msb);
	midi_in(0xB0 | ch, 100, lsb);
	midi_in(0xB0 | ch, 6, value);
}
static void ticks(int n) { // n main loop ticks of 2 ms
	for (int i = 0; i < n; i++) {
		host_now += TIME_MS2I(2);
		synth_tick();
	}
}

// ----------------------------------------------------------------------------------------- output
static const char* kind(uint8_t s) {
	switch (s & 0xf0) {
	case 0x80: return "NoteOff";
	case 0x90: return "NoteOn";
	case 0xa0: return "PolyAT";
	case 0xb0: return "CC";
	case 0xc0: return "PgmCh";
	case 0xd0: return "ChanAT";
	case 0xe0: return "PB";
	default: return "??";
	}
}
static void dump_midi(size_t from = 0, size_t max = 40) {
	for (size_t i = from; i < midi_log.size() && i < from + max; i++) {
		auto& m = midi_log[i];
		std::printf("    [%zu] %-7s ch%-2d", i, kind(m[0]), (m[0] & 0xf) + 1);
		for (auto b : m) std::printf(" %02X", b);
		std::printf("\n");
	}
}
// messages since from with (status & mask) == status, and data1 == d1 if given
static int count_midi(size_t from, uint8_t status_mask, uint8_t status, int d1 = -1) {
	int c = 0;
	for (size_t i = from; i < midi_log.size(); i++) {
		auto& m = midi_log[i];
		if ((m[0] & status_mask) == status && (d1 < 0 || (m.size() > 1 && m[1] == d1))) c++;
	}
	return c;
}
static int data_bytes_with_bit7(size_t from = 0) {
	int c = 0;
	for (size_t i = from; i < midi_log.size(); i++)
		for (size_t j = 1; j < midi_log[i].size(); j++) c += (midi_log[i][j] & 0x80) != 0;
	return c;
}
// every note-on has its note-off on the same channel (CC120/123 end a channel's notes)
static bool notes_balanced(bool print = true) {
	std::map<std::pair<int, int>, int> on;
	for (auto& m : midi_log) {
		const int s = m[0] & 0xf0, ch = m[0] & 0x0f;
		if (s == 0x90 && m.size() == 3 && m[2] > 0) on[{ch, m[1]}]++;
		if ((s == 0x80 || (s == 0x90 && m.size() == 3 && m[2] == 0)) && on[{ch, m[1]}] > 0) on[{ch, m[1]}]--;
		if (s == 0xb0 && m.size() == 3 && (m[1] == 120 || m[1] == 123))
			for (auto& [k, n] : on)
				if (k.first == ch) n = 0;
	}
	bool ok = true;
	for (auto& [k, n] : on)
		if (n > 0) {
			ok = false;
			if (print) std::printf("    hanging note: ch%d note %d (%d)\n", k.first + 1, k.second, n);
		}
	return ok;
}
// MIDI notes on now (note-ons without their note-off)
static int notes_on() {
	std::map<std::pair<int, int>, int> on;
	for (auto& m : midi_log) {
		const int st = m[0] & 0xf0, ch = m[0] & 0x0f;
		if (st == 0x90 && m.size() == 3 && m[2] > 0) on[{ch, m[1]}]++;
		if ((st == 0x80 || (st == 0x90 && m.size() == 3 && m[2] == 0)) && on[{ch, m[1]}] > 0) on[{ch, m[1]}]--;
	}
	int n = 0;
	for (auto& [k, c] : on) n += c;
	return n;
}
// internal synth voices with pres > 0
static int count_sounding() {
	int c = 0;
	for (auto& v : vc) c += v.pres > 0.0f;
	return c;
}
// the internal voice that plays note (pres > 0), or -1
static int voice_playing(float note, float tol = 1e-4f) {
	for (int v = 0; v < VOICECOUNT; v++)
		if (vc[v].pres > 0.0f && std::fabs(vc[v].note - note) < tol) return v;
	return -1;
}
static float max_pres() {
	float m = 0;
	for (auto& v : vc) m = v.pres > m ? v.pres : m;
	return m;
}
// one MIDI pitch bend value (14 bit) from message m
static int pb_value(const std::vector<uint8_t>& m) { return m[1] | m[2] << 7; }
static int last_pb(size_t from) {
	int pb = -1;
	for (size_t i = from; i < midi_log.size(); i++)
		if ((midi_log[i][0] & 0xf0) == 0xe0) pb = pb_value(midi_log[i]);
	return pb;
}

// ------------------------------------------------------------------------------------- scenarios
// B1: a preset's tuning (iP<n>tunin) must not overwrite the MIDI message interval
static void t_b1_message_interval() {
	load_preset(1); // the default preset: Mint 1, tuning 0 (iGpreset 1, striso.py save-touch)
	EXPECT(config.message_interval == 1, "preset 1: message_interval %d", config.message_interval);
	set_cfg("iP3Mint", "4");
	set_cfg("iP3tunin", "5");
	load_preset(3);
	EXPECT(config.message_interval == 4, "preset 3 (Mint 4, tuning 5): message_interval %d", config.message_interval);
}

// B2/B4: CC126 (mono mode off: MPE-like from this channel) with 16 channels, then 15 notes
static void t_b2_cc126_count() {
	midi_in(0xB0, 126, 16);
	EXPECT(dis.voicecount >= 1 && dis.voicecount <= MAX_VOICECOUNT, "voicecount %d", dis.voicecount);
	EXPECT(dis.midi_channel_offset + dis.voicecount <= 16, "channels %d..%d", dis.midi_channel_offset + 1, dis.midi_channel_offset + dis.voicecount);
	for (int n = 0; n < 16; n++) key(K(10 + 3 * n % 50), 0.3f + 0.02f * n, 0.2f);
	EXPECT(count_midi(0, 0xf0, 0x90) >= 1, "no note-on");
}

// B3: free transpose with one key: transpose_button2 == -1 must not be read as buttons[-1]
static void t_b3_transpose_one_key() {
	ctl(IDC_OCT_UP, 1);
	ctl(IDC_OCT_DOWN, 1); // both octave keys: free transpose
	const int a = K(30);
	key(a, 0.5f, 0.2f);
	key(a, 0.6f, 0.0f, 0.1f, 0.2f);
	const int v = voice_playing(dis.buttons[a].note + dis.bend_sensitivity * 0.001f, 0.01f);
	EXPECT(v >= 0, "the key doesn't sound");
	if (v >= 0) {
		EXPECT(std::fabs(vc[v].pres - 0.6f) < 1e-3f, "pres %g (expected 0.6)", vc[v].pres);
		EXPECT(std::fabs(vc[v].vpres) < 1e-3f, "vpres %g (expected 0)", vc[v].vpres);
	}
}

// B4: a preset with MIDI channel 16 and 6 MPE channels: channels must not run into the status
static void t_b4_channel_overflow() {
	set_cfg("iP2MChan", "16");
	set_cfg("iP2voice", "6");
	set_cfg("sP2Mmode", "mpe");
	load_preset(2);
	const size_t m0 = midi_log.size();
	for (int n = 0; n < 8; n++) key(K(10 + 4 * n), 0.4f + 0.05f * n, 0.2f);
	for (int n = 0; n < 8; n++) key(K(10 + 4 * n), 0.0f, -0.1f);
	int bad = 0;
	for (size_t i = m0; i < midi_log.size(); i++) {
		const int s = midi_log[i][0] & 0xf0;
		bad += s != 0x80 && s != 0x90 && s != 0xb0 && s != 0xd0 && s != 0xe0;
	}
	EXPECT(bad == 0, "%d messages with a status MPE notes don't use (channel overflow)", bad);
	EXPECT(count_midi(m0, 0xf0, 0x90) == count_midi(m0, 0xf0, 0x80), "note-ons %d note-offs %d", count_midi(m0, 0xf0, 0x90), count_midi(m0, 0xf0, 0x80));
	EXPECT(notes_balanced(), "hanging notes");
}

// B5 and SC-17: a pitch bend range of 0 (RPN 0, or a bare CC6 at boot) must be refused
static void t_b5_bend_range_zero() {
	rpn(1, 0, 0, 0); // bend range 0 on member channel 2
	EXPECT(dis.midi_bend_range >= 1, "bend range %g", dis.midi_bend_range);
	key(K(30), 0.5f, 0.2f);
	key(K(30), 0.5f, 0.0f, 0.3f);
	EXPECT(data_bytes_with_bit7() == 0, "bad data bytes");
}
static void t_sc17_rpn_boot() {
	midi_in(0xB0, 6, 0); // a bare Data Entry (no RPN selected since boot)
	EXPECT(dis.midi_bend_range == 48, "bend range after a bare CC6=0: %g", dis.midi_bend_range);
	key(K(30), 0.5f, 0.2f);
	// an MPE host: the member channels' range (24), then the master channel's (2)
	rpn(1, 0, 0, 24);
	rpn(0, 0, 0, 2);
	EXPECT(dis.midi_bend_range == 24, "members 24 then master 2: bend range %g (expected 24)", dis.midi_bend_range);
	// the RPN state is per channel: a data entry on channel 3 without its own RPN select
	midi_in(0xB2, 6, 12);
	EXPECT(dis.midi_bend_range == 24, "bare CC6 on another channel: %g", dis.midi_bend_range);
}

// SC-01: with 15 MPE channels every note must sound on the internal synth (10 voices)
static void t_sc01_dummy_voices() {
	config_but(0, 1, 5.0f); // settings + key 0, nudge up: MPE channel count
	EXPECT(dis.voicecount == 15, "voicecount after the knob: %d", dis.voicecount);
	int silent = 0, total = 0;
	for (int round = 0; round < 2; round++)
		for (int n = 10; n < 40; n += 2) { // a melody, one note at a time
			const int b = K(n);
			key(b, 0.5f, 0.2f);
			key(b, 0.5f);
			silent += voice_playing(dis.buttons[b].note) < 0;
			total++;
			key(b, 0.0f, -0.1f);
			ticks(50);
		}
	EXPECT(silent == 0, "%d of %d single notes silent on the internal synth", silent, total);
	// 12 keys held: 12 MIDI notes on 12 channels, the internal synth's 10 voices all sound
	for (int n = 0; n < 12; n++) key(K(5 + 4 * n), 0.2f + 0.05f * n, 0.1f);
	for (int n = 0; n < 12; n++) key(K(5 + 4 * n), 0.2f + 0.05f * n);
	EXPECT(notes_on() == 12, "MIDI notes on: %d (expected 12)", notes_on());
	EXPECT(count_sounding() == VOICECOUNT, "internal voices sounding %d", count_sounding());
	int loudest = 0; // the 10 firmest keys are the ones that sound
	for (int n = 2; n < 12; n++) loudest += voice_playing(dis.buttons[K(5 + 4 * n)].note) >= 0;
	EXPECT(loudest == 10, "%d of the 10 firmest keys sound", loudest);
}

// SC-02: Normal -> MPE with a key held
static void t_sc02_poly_to_mpe() {
	stck(2); // Normal (POLY) MIDI mode, as Striso Live's settings overlay does
	const int b = K(40);
	key(b, 0.5f, 0.2f);
	stck(0); // switch to MPE while the key is held
	const size_t m0 = midi_log.size();
	pedal_f = 0.25f;
	key(b, 0.6f, 0.0f, 0.2f, 0.1f);
	EXPECT(pedal_f == 0.25f, "pedal (= note[-1]) now %g", pedal_f);
	int master = 0; // channel 1 is the MPE master channel: no note data there
	for (size_t i = m0; i < midi_log.size(); i++) master += (midi_log[i][0] & 0x0f) == 0 && (midi_log[i][0] & 0xf0) != 0xb0;
	EXPECT(master == 0, "%d note messages on the master channel", master);
	EXPECT(count_midi(m0, 0xf0, 0x90) == 1, "the held key continues as an MPE note: note-ons %d", count_midi(m0, 0xf0, 0x90));
	EXPECT(count_sounding() == 1, "internal voices sounding %d", count_sounding());
	key(b, 0.0f, -0.1f);
	EXPECT(notes_balanced(), "hanging notes");
	EXPECT(count_sounding() == 0, "internal voices sounding after the release: %d", count_sounding());
	if (failures) dump_midi(m0);
}

// SC-02: MPE -> Normal with a key held
static void t_sc02_mpe_to_poly() {
	const int b = K(30);
	key(b, 0.5f, 0.2f);
	key(b, 0.6f);
	stck(2); // to Normal mode while held
	key(b, 0.6f);
	key(b, 0.0f, -0.1f); // release
	EXPECT(notes_balanced(), "hanging notes");
	EXPECT(count_sounding() == 0, "internal synth max pres after release: %g", max_pres());
	if (failures) dump_midi();
}

// SC-02, SC-10, SC-31: 3 keys held through MPE -> Normal -> Mono -> MPE
static void t_sc02_switch_held() {
	const int b[3] = {K(20), K(30), K(40)};
	auto play = [&](int n) {
		for (int r = 0; r < n; r++)
			for (int i = 0; i < 3; i++) key(b[i], 0.3f + 0.2f * i, r == 0 ? 0.2f : 0.0f, 0.1f * i, -0.2f * i);
	};
	play(3);
	EXPECT(count_sounding() == 3, "MPE: %d internal voices sound", count_sounding());
	stck(2);
	play(3);
	EXPECT(count_sounding() == 3, "Normal: %d internal voices sound", count_sounding());
	stck(4);
	play(3);
	EXPECT(count_sounding() == 1, "Mono: %d internal voices sound (glissando: one)", count_sounding());
	stck(0);
	play(3);
	EXPECT(count_sounding() == 3, "MPE again: %d internal voices sound", count_sounding());
	for (int i = 0; i < 3; i++) key(b[i], 0.0f, -0.1f);
	EXPECT(count_sounding() == 0, "after the release: %d internal voices sound", count_sounding());
	EXPECT(notes_balanced(), "hanging notes");
	for (int i = 0; i < 3; i++) EXPECT(dis.buttons[b[i]].state == STATE_OFF, "key %d state %d", i, dis.buttons[b[i]].state);
	for (int v = 0; v < dis.voicecount; v++)
		for (int w = v + 1; w < dis.voicecount; w++) EXPECT(dis.voices[v] < 0 || dis.voices[v] != dis.voices[w], "button %d in voices[%d] and [%d]", dis.voices[v], v, w);
}

// SC-03: Mono mode, a softer note after a firm one must sound at once
static void t_sc03_mono_delay() {
	stck(4); // Mono (glissando) MIDI mode: voicecount 1
	const int a = K(30), b = K(32);
	key(a, 0.9f, 0.5f);
	key(a, 0.9f);
	key(a, 0.0f, -0.2f); // a firm note, released
	ticks(25);           // 50 ms later
	int ms = 0;
	for (; ms < 3000; ms += 2) { // the next note, played softer, held steadily (message every 2 ms)
		key(b, 0.25f, ms == 0 ? 0.15f : 0.0f);
		if (dis.buttons[b].state == STATE_ON) break;
		ticks(1);
	}
	EXPECT(ms <= 2, "mono: the next note sounds after %d ms", ms);
}

// SC-04: a preset without Toff after one with Toff
static void t_sc04_toff_stale() {
	set_cfg("iP2tunin", "0");
	set_cfg("fP2Toff", "50.0");
	config_but(37, 0, 0); // preset 2: 12-TET + 50 cents
	EXPECT(std::fabs(dis.note_offset - 0.5f) < 1e-6f, "preset 2 (Toff 50 cents): note_offset %g", dis.note_offset);
	config_but(35, 0, 0); // preset 1: 12-TET, no Toff
	EXPECT(dis.note_offset == 0.0f, "preset 1 (no Toff): note_offset %g (expected 0)", dis.note_offset);
}

// SC-05: pressure hysteresis (MPE channel pressure)
static void t_sc05_pres_hyst() {
	const int b = K(30);
	key(b, 0.5f, 0.1f);
	const size_t m0 = midi_log.size();
	const float p0 = 63.25f / 127.0f, y0 = (70.25f - 64.0f) / 64.0f; // a rounding edge of each
	for (int i = 0; i < 50; i++) {
		const float j = (i & 1 ? 6 : -6) / 8191.0f; // +-0.09 of a MIDI step
		key(b, p0 + j, 0.0f, 0.0f, y0 + j * 127.0f / 64.0f);
	}
	EXPECT(count_midi(m0, 0xf0, 0xd0) <= 2, "channel pressure msgs %d of 50", count_midi(m0, 0xf0, 0xd0));
	EXPECT(count_midi(m0, 0xf0, 0xb0, 74) <= 2, "CC74 msgs %d of 50", count_midi(m0, 0xf0, 0xb0, 74));
}
// SC-05: the same in Normal mode (poly pressure, then the one-channel pressure)
static void t_sc05_pres_hyst_normal() {
	stck(2);
	const int b = K(30);
	key(b, 0.5f, 0.1f);
	size_t m0 = midi_log.size();
	const float p0 = 63.25f / 127.0f;
	for (int i = 0; i < 50; i++) key(b, p0 + (i & 1 ? 6 : -6) / 8191.0f);
	EXPECT(count_midi(m0, 0xf0, 0xa0) <= 2, "poly pressure msgs %d of 50", count_midi(m0, 0xf0, 0xa0));
	config.midi_pres = CFG_CHANNEL_PRESSURE;
	m0 = midi_log.size();
	for (int i = 0; i < 50; i++) key(b, p0 + (i & 1 ? 6 : -6) / 8191.0f);
	EXPECT(count_midi(m0, 0xf0, 0xd0) <= 2, "channel pressure msgs %d of 50", count_midi(m0, 0xf0, 0xd0));
}

// SC-06: more keys held than channels must not make two notes steal each other back and forth
static void t_sc06_pingpong() {
	config_but(0, 1, -8.0f); // 2 MPE channels
	EXPECT(dis.voicecount == 2, "voicecount %d", dis.voicecount);
	const int b[3] = {K(20), K(30), K(40)};
	for (int i = 0; i < 3; i++) key(b[i], 0.4f, 0.1f);
	const size_t m0 = midi_log.size();
	srand(1);
	for (int r = 0; r < 100; r++) {
		for (int i = 0; i < 3; i++) {
			const float j = ((rand() % 200) - 100) / 8191.0f; // +-0.012 pressure noise
			key(b[i], 0.4f + j, j);
		}
		ticks(1);
	}
	EXPECT(count_midi(m0, 0xf0, 0x90) <= 3, "100 rounds: note-ons %d, note-offs %d", count_midi(m0, 0xf0, 0x90), count_midi(m0, 0xf0, 0x80));
}

// SC-07: a note that takes over a channel sends its own CC74 (not the stolen note's)
static void t_sc07_stale_cc74() {
	config_but(0, 1, -9.0f); // 1 MPE channel
	const int a = K(20), b = K(40);
	key(b, 0.5f, 0.2f);
	key(b, 0.5f);
	key(b, 0.0f, -0.1f); // B played and released (its last CC74 was 64)
	key(a, 0.3f, 0.1f);
	key(a, 0.3f, 0.0f, 0.0f, 0.8f); // A held, tilted: CC74 115
	const size_t m0 = midi_log.size();
	key(b, 0.9f, 0.3f);
	key(b, 0.9f); // B louder: steals A's channel
	int cc74 = -1;
	for (size_t i = m0; i < midi_log.size(); i++)
		if ((midi_log[i][0] & 0xf0) == 0xb0 && midi_log[i][1] == 74) cc74 = midi_log[i][2];
	EXPECT(count_midi(m0, 0xf0, 0x90) == 1, "B's note-on: %d", count_midi(m0, 0xf0, 0x90));
	EXPECT(cc74 == 64, "CC74 for B: %d (the channel holds A's 115 unless B sends 64)", cc74);
	if (failures) dump_midi(m0);
}

// SC-08: Normal mode bends by the same setting as MPE (only with SYNTHCTL_NORMAL_BEND_LIKE_MPE;
// the default keeps the old bend of Normal mode, twice MPE's)
static void bend_units(double& mpe, double& normal) {
	const int b = K(30);
	const float x = 0.5f;
	dis.bend_sensitivity = 0.5f;
	key(b, 0.5f, 0.2f);
	size_t m0 = midi_log.size();
	key(b, 0.5f, 0.0f, x);
	mpe = (last_pb(m0) - 8192) / 8192.0 * dis.midi_bend_range;
	key(b, 0.0f);
	stck(2);
	key(b, 0.5f, 0.2f);
	m0 = midi_log.size();
	key(b, 0.5f, 0.0f, x);
	normal = (last_pb(m0) - 8192) / 8192.0 * 2; // Normal mode's range: 2 (midi_config)
}
static void t_sc08_bend_default() {
	double mpe, normal;
	bend_units(mpe, normal);
	EXPECT(std::fabs(normal - 2 * 0.5 * 0.125) < 0.001, "Normal mode: %.4f semitones (the old 0.125)", normal);
}
#if defined(SYNTHCTL_NORMAL_BEND_LIKE_MPE) && SYNTHCTL_NORMAL_BEND_LIKE_MPE
static void t_sc08_bend_like_mpe() {
	double mpe, normal;
	bend_units(mpe, normal);
	EXPECT(std::fabs(normal - mpe) < 2.0 / 8192 + 48.0 / 8192, "MPE %.4f, Normal %.4f semitones", mpe, normal);
	EXPECT(std::fabs(normal - 0.0625) < 0.001, "Normal mode: %.4f semitones (expected 0.0625)", normal);
}
#endif

// SC-09: in settings mode a light first touch (pres <= 0.05) must not change the setting
static void t_sc09_alt_light() {
	ctl(IDC_ALT, 1);
	const float v0 = volume_linear;
	key(51, 0.03f, 0.0f, 0.0f, 0.8f); // settings key 51 (volume), light first touch, finger off centre
	EXPECT(volume_linear == v0, "volume before %g, after a light touch: %g", v0, volume_linear);
	key(51, 0.5f, 0.0f, 0.0f, 0.8f); // a real press: shows the volume, then nudges it up
	EXPECT(volume_linear > v0, "a real press with a nudge up: volume %g", volume_linear);
}

// SC-10 (user decision): in Normal mode the internal synth plays every key with its own expression
static void t_sc10_normal_internal() {
	stck(2);
	const int b[3] = {K(20), K(30), K(40)};
	const float p[3] = {0.3f, 0.5f, 0.7f}, x[3] = {0.1f, -0.2f, 0.3f}, y[3] = {0.4f, 0.0f, -0.4f};
	for (int i = 0; i < 3; i++) key(b[i], p[i], 0.2f);
	for (int r = 0; r < 3; r++)
		for (int i = 0; i < 3; i++) key(b[i], p[i], 0.0f, x[i], y[i]);
	EXPECT(count_sounding() == 3, "internal voices sounding %d (expected 3)", count_sounding());
	int seen = 0;
	for (int i = 0; i < 3; i++) {
		const float xs = lrintf(x[i] * 0x1fff) / 8191.0f, ys = lrintf(y[i] * 0x1fff) / 8191.0f;
		const float note = dis.buttons[b[i]].note + dis.bend_sensitivity * xs * xs * xs;
		const int v = voice_playing(note, 1e-3f);
		EXPECT(v >= 0, "key %d (note %g) has no internal voice", i, note);
		if (v < 0) continue;
		seen |= 1 << v;
		EXPECT(std::fabs(vc[v].pres - lrintf(p[i] * 0x1fff) / 8191.0f) < 1e-5f, "key %d pres %g", i, vc[v].pres);
		EXPECT(std::fabs(vc[v].but_x - xs) < 1e-5f, "key %d x %g", i, vc[v].but_x);
		EXPECT(std::fabs(vc[v].but_y - ys) < 1e-5f, "key %d y %g", i, vc[v].but_y);
		EXPECT(vc[v].vpres == 0.0f, "key %d vpres %g", i, vc[v].vpres);
	}
	EXPECT(__builtin_popcount(seen) == 3, "the keys share voices");
	for (int i = 0; i < 3; i++) key(b[i], 0.0f, -0.1f);
	EXPECT(count_sounding() == 0, "after the release: %d voices sound", count_sounding());
	EXPECT(notes_balanced(), "hanging notes");
}

// SC-10: the internal synth playing in Normal mode leaves Normal mode's MIDI output as it was.
// The expected messages are those of the firmware before the change (3d6f498).
static const uint8_t normal_midi_expected[][3] = {
#include "normal_midi_expected.inc"
};
static void t_sc10_normal_midi_unchanged() {
	stck(2);
	const size_t m0 = midi_log.size();
	const int b[3] = {K(22), K(33), K(41)};
	for (int r = 0; r < 12; r++)
		for (int i = 0; i < 3; i++) {
			if (r == 9 && i == 1) {
				key(b[i], 0.0f, -0.2f); // the middle key released early
				continue;
			}
			if (r > 9 && i == 1) continue;
			// pressures 0.1 above a MIDI step: the same value whichever way the hysteresis rounds
			const float p = (25.1f + 13 * i + 4 * r) / 127.0f, x = 0.05f * (r - 5) * (i - 1), y = 0.06f * r - 0.3f;
			key(b[i], r == 11 ? 0.0f : p, r == 0 ? 0.3f : r == 11 ? -0.15f :
			                                                        0.0f,
			    x, y);
		}
	const size_t n = midi_log.size() - m0;
	if (std::getenv("SC_PRINT_EXPECTED")) {
		for (size_t i = m0; i < midi_log.size(); i++) {
			auto& m = midi_log[i];
			std::printf("{0x%02x, 0x%02x, 0x%02x},\n", m[0], m.size() > 1 ? m[1] : 0, m.size() > 2 ? m[2] : 0);
		}
		return;
	}
	const size_t expected = sizeof normal_midi_expected / sizeof normal_midi_expected[0];
	EXPECT(n == expected, "%zu messages (expected %zu)", n, expected);
	for (size_t i = 0; i < n && i < expected; i++) {
		auto& m = midi_log[m0 + i];
		const uint8_t got[3] = {m[0], m.size() > 1 ? m[1] : uint8_t{0}, m.size() > 2 ? m[2] : uint8_t{0}};
		if (memcmp(got, normal_midi_expected[i], 3) != 0) {
			EXPECT(false, "message %zu: %02X %02X %02X, expected %02X %02X %02X", i, got[0], got[1], got[2],
			    normal_midi_expected[i][0], normal_midi_expected[i][1], normal_midi_expected[i][2]);
			break;
		}
	}
}

// SC-11: MIDI note numbers stay in 0..127 (a data byte with bit 7 set is a status byte)
static void t_sc11_midinote_knob() {
	config.midinote_mode = MIDINOTE_MODE_TUNING;
	for (int i = 0; i < 400; i++) config_but(34, 2, 4.0f); // circle the 12-TET key's fine tune knob
	const size_t m0 = midi_log.size();
	key(K(60), 0.5f, 0.2f);
	key(K(60), 0.5f, 0.0f, 0.4f);
	EXPECT(data_bytes_with_bit7(m0) == 0, "data bytes with bit 7 set");
	if (failures) dump_midi(m0);
}
static void t_sc11_midinote_preset() {
	set_cfg("sP2Mnote", "tuning");
	set_cfg("fP2Toff", "2400.0");
	config_but(37, 0, 0);
	ctl(IDC_OCT_UP, 1);
	ctl(IDC_OCT_UP, 0);
	ctl(IDC_OCT_UP, 1);
	ctl(IDC_OCT_UP, 0); // two octaves up
	size_t m0 = midi_log.size();
	key(K(60), 0.5f, 0.2f);
	EXPECT(data_bytes_with_bit7(m0) == 0, "top key, +2400 cents, +2 octaves: data bytes with bit 7 set");
	key(K(60), 0.0f);
	set_cfg("fP3Toff", "-4800.0");
	set_cfg("sP3Mnote", "tuning");
	config_but(39, 0, 0);
	for (int i = 0; i < 4; i++) {
		ctl(IDC_OCT_DOWN, 1);
		ctl(IDC_OCT_DOWN, 0);
	}
	m0 = midi_log.size();
	key(K(0), 0.5f, 0.2f);
	EXPECT(data_bytes_with_bit7(m0) == 0, "bottom key, -4800 cents, octaves down: data bytes with bit 7 set");
	if (failures) dump_midi(m0);
}

// SC-12: flip moves each key's tuning offset with its note
static float key_pitch(int b) {
	auto& x = dis.buttons[b];
	return dis.start_note_offset + x.tuning_note_offset + dis.notegen0 * x.coord0 + dis.notegen1 * x.coord1 + dis.note_offset;
}
static void t_sc12_flip_offsets() {
	config_but(33, 0, 0); // tuning 8 (7-limit JI): per-key offsets
	float before[61];
	for (int n = 0; n < 61; n++) before[n] = key_pitch(K(n));
	dis.flip();
	int bad = 0;
	for (int n = 0; n < 61; n++) bad += std::fabs(key_pitch(K(n)) - before[60 - n]) > 1e-4f;
	EXPECT(bad == 0, "flip: %d of 61 keys play their mirrored note with the wrong offset", bad);
	// a tuning loaded while flipped
	config_but(34, 0, 0);
	config_but(33, 0, 0);
	bad = 0;
	for (int n = 0; n < 61; n++) bad += std::fabs(key_pitch(K(n)) - before[60 - n]) > 1e-4f;
	EXPECT(bad == 0, "tuning loaded on a flipped layout: %d of 61 keys wrong", bad);
}

// SC-13: a knob nudge never moves a value against its direction; preset volume and decay in range
static void t_sc13_ranges() {
	synth_touch_set(4, 0.3f); // threshold via Striso Live
	config_but(17, 1, 1.0f);  // one nudge UP on settings key 17
	EXPECT(synth_touch_get(4) >= 0.3f - 1e-6f, "threshold 0.3, nudge up: %g", synth_touch_get(4));
	synth_touch_set(3, 8.0f); // bend 8 (bend_sensitivity 16)
	config_but(6, 1, 1.0f);
	EXPECT(dis.bend_sensitivity >= 16.0f, "bend_sensitivity 16, nudge up: %g", dis.bend_sensitivity);
	config_but(6, 1, -1.0f);
	EXPECT(dis.bend_sensitivity < 16.0f, "nudge down: %g", dis.bend_sensitivity);
	synth_touch_set(2, -2.0f);
	config_but(8, 1, -1.0f);
	EXPECT(dis.y_sensitivity <= -2.0f, "tilt -2, nudge down: %g", dis.y_sensitivity);
	set_cfg("fP2volum", "254.0");
	set_cfg("fP2decay", "200.0");
	set_cfg("fP2decaP", "250.0");
	config_but(37, 0, 0);
	EXPECT(volume_linear <= 127.0f && volume <= 1.0f, "preset volum 254: volume_linear %g, gain %g", volume_linear, volume);
	EXPECT(pedal_f <= 1.0f, "preset decay 200: pedal %g", pedal_f);
	ctl(IDC_PEDAL_1, 1);
	EXPECT(pedal_f <= 1.0f, "preset decaP 250, pedal down: pedal %g", pedal_f);
}

// SC-14: an MPE Configuration Message with 0 channels must not silence the instrument
static void t_sc14_mcm_zero() {
	rpn(0, 0, 6, 0);
	EXPECT(dis.voicecount >= 1, "after MCM(0): voicecount %d", dis.voicecount);
	const size_t m0 = midi_log.size();
	key(K(30), 0.5f, 0.2f);
	key(K(30), 0.6f);
	EXPECT(count_midi(m0, 0xf0, 0x90) == 1, "note-ons %d", count_midi(m0, 0xf0, 0x90));
	EXPECT(count_sounding() == 1, "internal voices sounding %d", count_sounding());
}

// SC-15: incoming pitch bend must not erase the tuning's offset
static void t_sc15_pb_in() {
	set_cfg("fT3off", "25.0");
	config_but(23, 0, 0); // tuning 3 with a 25 cent offset
	EXPECT(std::fabs(dis.note_offset - 0.25f) < 1e-6f, "tuning 3: note_offset %g", dis.note_offset);
	midi_in(0xE5, 0x00, 0x40); // a centred pitch bend on channel 6
	EXPECT(std::fabs(dis.note_offset - 0.25f) < 1e-6f, "after a centred pitch bend: note_offset %g", dis.note_offset);
	midi_in(0xE0, 0x00, 0x60); // master channel: +1 semitone (range 2)
	EXPECT(std::fabs(dis.note_offset - 1.25f) < 1e-6f, "after +1 semitone on the master channel: %g", dis.note_offset);
	midi_in(0xE0, 0x00, 0x40);
	EXPECT(std::fabs(dis.note_offset - 0.25f) < 1e-6f, "centred again: %g", dis.note_offset);
}

// SC-16: an echo of the Striso's own MIDI output must not reconfigure it
static void t_sc16_echo() {
	const int b = K(30);
	config.send_motion_interval = 1;
	key(b, 0.5f, 0.2f);
	key(b, 0.5f, 0.0f, 0.3f, 0.4f);
	key(b, 0.6f, 0.0f, -0.3f, 0.9f);
	const int a0[9] = {ID_ACCEL, 0, 300, 2000, -900, 1100, 0, 0, 0};
	int motion[9];
	memcpy(motion, a0, sizeof motion);
	synth_message(9, motion);
	ctl(IDC_PEDAL_1, 1);
	config_but(51, 1, 1.0f); // volume knob: CC7
	midi_config();
	const int mpe_y = config.mpe_y, mint = config.message_interval, smi = config.send_motion_interval;
	const float sno = dis.start_note_offset, no = dis.note_offset, br = dis.midi_bend_range, pedal0 = pedal_f;
	const int vcount = dis.voicecount, off = dis.midi_channel_offset;
	const size_t n = midi_log.size();
	for (size_t i = 0; i < n; i++) {
		auto m = midi_log[i];
		if (m.size() == 3) midi_in(m[0], m[1], m[2]);
		if (m.size() == 2) midi_in(m[0], m[1], 0);
	}
	EXPECT(config.mpe_y == mpe_y, "mpe_y CC %d (was %d)", config.mpe_y, mpe_y);
	EXPECT(config.message_interval == mint, "message_interval %d (was %d)", config.message_interval, mint);
	EXPECT(config.send_motion_interval == smi, "send_motion_interval %d (was %d)", config.send_motion_interval, smi);
	EXPECT(dis.start_note_offset == sno, "start_note_offset %g (was %g)", dis.start_note_offset, sno);
	EXPECT(dis.note_offset == no, "note_offset %g (was %g)", dis.note_offset, no);
	EXPECT(dis.midi_bend_range == br, "bend range %g (was %g)", dis.midi_bend_range, br);
	EXPECT(dis.voicecount == vcount && dis.midi_channel_offset == off, "channels %d+%d (were %d+%d)", dis.midi_channel_offset, dis.voicecount, off, vcount);
	EXPECT(pedal_f == pedal0, "pedal %g (was %g)", pedal_f, pedal0);
	EXPECT(count_midi(n, 0xf0, 0x80) == 0, "the echo ended the held note");
	key(b, 0.6f);
	EXPECT(count_sounding() == 1, "the held key sounds: %d", count_sounding());
}

// SC-18: sustain (CC64) and volume (CC7) on the instrument's channel in Normal mode
static void t_sc18_pedal_channel() {
	size_t m0 = midi_log.size();
	ctl(IDC_PEDAL_1, 1);
	EXPECT(count_midi(m0, 0xff, 0xb0, 64) == 1, "MPE: CC64 on channel 1 (the master channel)");
	ctl(IDC_PEDAL_1, 0);
	stck(2);
	config_but(2, 1, 4.0f);
	EXPECT(dis.midi_channel_offset == 4, "Normal mode on channel %d", dis.midi_channel_offset + 1);
	m0 = midi_log.size();
	ctl(IDC_PEDAL_1, 1);
	config_but(51, 1, 1.0f);
	EXPECT(count_midi(m0, 0xff, 0xb4, 64) == 1, "CC64 on channel 5");
	EXPECT(count_midi(m0, 0xff, 0xb4, 7) == 1, "CC7 on channel 5");
	EXPECT(count_midi(m0, 0xff, 0xb0) == 0, "messages on channel 1");
	if (failures) dump_midi(m0);
}

// SC-19, B4: Normal mode sends one program change, on its channel
static void t_sc19_poly_pgm() {
	set_cfg("sP2Mmode", "normal");
	set_cfg("iP2MChan", "10");
	set_cfg("iP2Mpgm", "5");
	const size_t m0 = midi_log.size();
	config_but(37, 0, 0);
	EXPECT(count_midi(m0, 0xf0, 0xc0) == 1, "program changes %d", count_midi(m0, 0xf0, 0xc0));
	EXPECT(count_midi(m0, 0xff, 0xc9) == 1, "program change on channel 10");
	EXPECT(count_midi(m0, 0xf0, 0xd0) == 0, "channel pressure messages %d", count_midi(m0, 0xf0, 0xd0));
	if (failures) dump_midi(m0);
}

// SC-20: settings key 56 (MPE bend range) with ranges that aren't 12, 24, 48 or 96
static void t_sc20_bend_range_key() {
	rpn(1, 0, 0, 2); // a host sets the members' bend range to 2
	EXPECT(dis.midi_bend_range == 2, "bend range %g", dis.midi_bend_range);
	config_but(56, 0, 0);
	EXPECT(last_dial >= 0 && last_dial <= 15, "dial %d", last_dial);
	dis.midi_bend_range = 36; // fP<n>MPEpb 36 is accepted (1..127)
	config_but(56, 1, 1.0f);
	EXPECT(dis.midi_bend_range == 48, "one nudge up from 36 -> %g", dis.midi_bend_range);
	dis.midi_bend_range = 36;
	config_but(56, 1, -1.0f);
	EXPECT(dis.midi_bend_range == 24, "one nudge down from 36 -> %g", dis.midi_bend_range);
	dis.midi_bend_range = 48;
	config_but(56, 1, 1.0f);
	EXPECT(dis.midi_bend_range == 96, "one nudge up from 48 -> %g", dis.midi_bend_range);
	config_but(56, 1, 1.0f);
	EXPECT(dis.midi_bend_range == 96, "one nudge up from 96 -> %g", dis.midi_bend_range);
}

// SC-21: motion CC rounding and hysteresis (only with SYNTHCTL_MOTION_CC_FIX)
#if defined(SYNTHCTL_MOTION_CC_FIX) && SYNTHCTL_MOTION_CC_FIX
static void t_sc21_motion_hyst() {
	config.send_motion_interval = 1;
	auto acc = [](int ax) {
		int msg[9] = {ID_ACCEL, 0, ax, 0, 0, 0, 0, 0, 0};
		synth_message(9, msg);
	};
	acc(0);
	acc(8);
	EXPECT(midi_log.back()[2] == 64, "acc_x 8 (0.25 of a step) -> CC16 %d (64 expected)", midi_log.back()[2]);
	acc(60);
	acc(55);
	const size_t m0 = midi_log.size();
	for (int i = 0; i < 40; i++) acc(i & 1 ? 57 : 55); // +-1 LSB of 32 per step
	EXPECT(count_midi(m0, 0xff, 0xb0, 16) <= 2, "acc_x jitter 55/57: CC16 msgs %d of 40", count_midi(m0, 0xff, 0xb0, 16));
}
#endif

// SC-22: free transpose beyond the transposition range: the second key continues the first's pitch
static void t_sc22_free_transpose_range() {
	ctl(IDC_OCT_UP, 1);
	ctl(IDC_OCT_DOWN, 1); // both octave keys: free transpose
	const int a = K(60), b = K(0);
	key(a, 0.5f, 0.2f);
	key(a, 0.5f);
	const float pa = dis.buttons[a].note;
	key(b, 0.5f, 0.2f);
	key(a, 0.5f);
	key(a, 0.0f, -0.1f); // release A: B should continue A's pitch
	key(b, 0.5f);
	EXPECT(std::fabs(dis.buttons[b].note - pa) < 1e-3f, "A played %.2f; after the hand-over B plays %.2f", pa, dis.buttons[b].note);
	EXPECT(voice_playing(pa, 1e-3f) >= 0, "no internal voice plays A's pitch");
	key(b, 0.0f, -0.1f);
	EXPECT(notes_balanced(), "hanging notes");
	EXPECT(count_sounding() == 0, "voices sounding after the release: %d", count_sounding());
}
// SC-22: an octave key released during the hand-over leaves the second key silent
static void t_sc22_octave_release() {
	ctl(IDC_OCT_UP, 1);
	ctl(IDC_OCT_DOWN, 1);
	const int a = K(30), b = K(35);
	key(a, 0.5f, 0.2f);
	key(a, 0.5f);
	key(b, 0.5f, 0.2f); // B: the second key of the free transpose
	ctl(IDC_OCT_UP, 0); // free transpose ends before A is released
	key(a, 0.0f, -0.1f);
	key(b, 0.5f);
	key(b, 0.5f);
	EXPECT(voice_playing(dis.buttons[b].note, 1e-3f) >= 0, "B doesn't sound (state %d)", dis.buttons[b].state);
	key(b, 0.0f, -0.1f);
	EXPECT(notes_balanced(), "hanging notes");
}

// SC-23: the panic key ends every note, MIDI and internal, and resets the key state
static void t_sc23_panic() {
	const int b = K(30), c = K(40);
	key(b, 0.5f, 0.2f);
	key(c, 0.6f, 0.2f);
	key(b, 0.5f);
	key(c, 0.6f);
	const size_t m0 = midi_log.size();
	stck(63);
	EXPECT(count_sounding() == 0, "internal voices sounding after panic: %d", count_sounding());
	for (int ch = 0; ch <= dis.voicecount; ch++) {
		EXPECT(count_midi(m0, 0xff, 0xb0 | ch, 123) == 1, "CC123 on channel %d", ch + 1);
		EXPECT(count_midi(m0, 0xff, 0xb0 | ch, 120) == 1, "CC120 on channel %d", ch + 1);
	}
	EXPECT(dis.buttons[b].state == STATE_OFF && dis.buttons[c].state == STATE_OFF, "key states %d %d", dis.buttons[b].state, dis.buttons[c].state);
	for (int v = 0; v < MAX_VOICECOUNT; v++) EXPECT(dis.voices[v] == -1, "voices[%d] = %d", v, dis.voices[v]);
	// the stuck internal voice of a mode switch (the old SC-02) is gone after panic too
	key(b, 0.0f);
	key(c, 0.0f);
	EXPECT(count_sounding() == 0, "after the release: %d", count_sounding());
	EXPECT(notes_balanced(), "hanging notes");
}

// SC-24: knob auto-repeat with the finger exactly at y = 0 (a division by zero, then inf -> int)
static void t_sc24_knob_y0() {
	ctl(IDC_ALT, 1);
	key(51, 0.5f, 0.0f, 0.1f, 0.6f); // settings key 51: press, finger up-right: nudge
	host_now += 2000;
	key(51, 0.5f, 0.0f, 0.55f, 0.0f); // roll to the right edge, y exactly 0
	key(51, 0.5f, 0.0f, 0.55f, 0.0f);
	EXPECT(volume_linear <= 127, "volume %g", volume_linear);
}

// SC-25: the pedal's first CC (MidiParam starts with INT32_MAX: INT32_MAX * 4)
static void t_sc25_midiparam_first() {
	ctl(IDC_PEDAL_1, 1);
	EXPECT(count_midi(0, 0xff, 0xb0, 64) == 1, "CC64 sent");
	EXPECT(midi_log.size() == 1 && midi_log[0][2] == 96, "CC64 value %d (expected 96)", midi_log.empty() ? -1 : midi_log[0][2]);
}

// SC-27: the pressure and tilt dials stay within the 16 leds, the colours non-negative
static void t_sc27_dials() {
	dis.pres_sensitivity = 3.0f;
	config_but(19, 0, 0);
	EXPECT(last_dial >= 0 && last_dial <= 15, "pressure 3 -> dial %d", last_dial);
	dis.y_sensitivity = 3.0f;
	config_but(8, 0, 0);
	EXPECT(last_dial >= 0 && last_dial <= 15, "tilt 3 -> dial %d", last_dial);
	dis.y_sensitivity = -2.0f;
	config_but(8, 0, 0);
	EXPECT(last_dial >= 0 && last_dial <= 15, "tilt -2 -> dial %d", last_dial);
	EXPECT(last_r >= 0 && last_g >= 0 && last_b >= 0, "tilt -2 -> led %d %d %d", last_r, last_g, last_b);
}

// SC-28: CC1 (fifth) MSB and CC33 LSB: an MSB resets the LSB, an LSB applies at once
static void t_sc28_cc1_lsb() {
	midi_in(0xB0, 1, 64);
	const float f0 = dis.notegen1;
	EXPECT(std::fabs(f0 - 7.0f) < 1e-6f, "CC1=64: fifth %f", f0);
	midi_in(0xB0, 33, 100);
	EXPECT(dis.notegen1 > f0, "CC33=100: fifth %f (unchanged)", dis.notegen1);
	midi_in(0xB0, 1, 64);
	EXPECT(std::fabs(dis.notegen1 - 7.0f) < 1e-6f, "CC1=64 again (resets the LSB): fifth %f", dis.notegen1);
}

// SC-29: CC65 (portamento) is on at >= 64 only
static void t_sc29_cc65() {
	midi_in(0xB0, 65, 1);
	EXPECT(dis.portamento == 0, "CC65=1: portamento %d", dis.portamento);
	midi_in(0xB0, 65, 64);
	EXPECT(dis.portamento == 1, "CC65=64: portamento %d", dis.portamento);
	midi_in(0xB0, 65, 63);
	EXPECT(dis.portamento == 0, "CC65=63: portamento %d", dis.portamento);
}

// SC-30: the MCM is only for the lower zone (channel 1); CC126 never runs past channel 16
static void t_sc30_mode_messages() {
	rpn(5, 0, 6, 3); // "MCM" on channel 6
	EXPECT(dis.midi_channel_offset == 1 && dis.voicecount == VOICECOUNT, "MCM on channel 6: offset %d voicecount %d", dis.midi_channel_offset, dis.voicecount);
	midi_in(0xB9, 126, 0); // CC126 0 on channel 10: MPE-like from channel 10, channels unspecified
	EXPECT(dis.midi_channel_offset + dis.voicecount <= 16, "CC126 0 on channel 10: channels %d..%d", dis.midi_channel_offset + 1, dis.midi_channel_offset + dis.voicecount);
	for (int n = 0; n < 12; n++) key(K(4 * n + 3), 0.3f + 0.03f * n, 0.2f);
	int bad = 0;
	for (auto& m : midi_log) bad += (m[0] & 0xf0) == 0xa0 || (m[0] & 0xf0) == 0xc0 || (m[0] & 0xf0) == 0xf0;
	EXPECT(bad == 0, "%d messages with a wrong status", bad);
}

// SC-31: after MPE -> Mono -> MPE no button owns two channels
static void t_sc31_steal_dup() {
	const int x = K(30);
	for (int n = 0; n < 4; n++) {
		key(K(10 + n), 0.5f, 0.2f);
		key(K(10 + n), 0.0f);
		ticks(5);
	}
	key(x, 0.5f, 0.2f);
	key(x, 0.0f);
	ticks(500);
	stck(4);
	ticks(500);
	key(x, 0.5f, 0.2f);
	key(x, 0.0f);
	stck(0);
	int c = 0;
	for (int v = 0; v < dis.voicecount; v++) c += dis.voices[v] == x;
	EXPECT(c <= 1, "back in MPE: the button is in voices[] %d times", c);
}

// SC-35: a tuning with an absurd octave must not overflow the pitch bend's float -> int
static void t_sc35_tuning_overflow() {
	set_cfg("fT3oct", "9999999"); // (99999999 reads as 1e8: CONFIG_UNDEFINED)
	config_but(23, 0, 0);
	dis.midi_bend_range = 1;
	const size_t m0 = midi_log.size();
	key(0, 0.5f, 0.2f); // button 0: coord0 6
	key(0, 0.5f, 0.0f, 0.2f);
	EXPECT(data_bytes_with_bit7(m0) == 0, "data bytes with bit 7 set");
}

// SC-37: MIDI clock (and other system messages) don't blink the led
static void t_sc37_blink() {
	for (int i = 0; i < 24; i++) MidiInMsgHandler(MIDI_DEVICE_USB_DEVICE, 1, 0xF8, 0, 0);
	EXPECT(blink_count == 0, "24 clocks: %d blinks", blink_count);
	midi_in(0xB0, 7, 100);
	EXPECT(blink_count == 1, "a CC: %d blinks", blink_count);
}

// SC-38: without the internal synth (synth_interface NULL) the pedal and keys don't dereference it
static void t_sc38_no_synth() {
	dis.synth_interface = nullptr;
	ctl(IDC_PEDAL_1, 1);
	int msg[3] = {ID_CONTROL, IDC_PEDAL_EXP, 1000};
	synth_message(3, msg);
	midi_in(0xB0, 64, 100);
	config_but(53, 1, 1.0f);
	load_preset(1);
	key(K(30), 0.5f, 0.2f);
	key(K(30), 0.0f);
	stck(63);
	EXPECT(count_midi(0, 0xf0, 0x90) == 1, "the key's MIDI note-on");
}

// B15: knob auto-repeat across the 32-bit system time wrap (after 24.9 days)
static void t_b15_time_wrap() {
	host_now = 0xffffffffu - TIME_MS2I(100);
	ctl(IDC_ALT, 1);
	const float v0 = volume_linear;
	key(51, 0.5f, 0.0f, 0.0f, 0.8f); // press settings key 51 with a nudge up: one step
	const float v1 = volume_linear;
	EXPECT(v1 > v0, "the nudge: volume %g -> %g", v0, v1);
	for (int i = 0; i < 50; i++) { // 200 ms held: the auto-repeat starts after 500 ms
		host_now += TIME_MS2I(4);
		key(51, 0.5f, 0.0f, 0.0f, 0.8f);
	}
	EXPECT(volume_linear == v1, "200 ms after the nudge (time wrapped): volume %g (expected %g)", volume_linear, v1);
}

// every config key the presets and tunings look up exists in default_config
static void t_preset_keys() {
	for (int p = 1; p <= 8; p++) load_preset(p);
	for (int t = 1; t <= 8; t++) {
		dis.cur_tuning = -1;
		dis.load_tuning(t);
	}
	EXPECT(misses.empty(), "%zu config keys looked up that aren't in default_config", misses.size());
	for (auto& m : misses) std::printf("    '%s'\n", m.c_str());
}

// ----------------------------------------------------------------------------------------- replay
static uint64_t fnv(uint64_t h, const char* s) {
	for (; *s; s++) h = (h ^ (uint8_t)*s) * 0x100000001b3ULL;
	return h;
}
static int replay(int argc, char** argv) {
	const char* csv = argv[2];
	const char *mode = "mpe", *trace_path = nullptr, *expect = nullptr, *golden_name = nullptr;
	for (int i = 3; i + 1 < argc; i += 2) {
		const std::string a = argv[i];
		if (a == "--mode") mode = argv[i + 1];
		else if (a == "--trace") trace_path = argv[i + 1];
		else if (a == "--expect") expect = argv[i + 1];
		else if (a == "--golden-name") golden_name = argv[i + 1];
		else {
			std::fprintf(stderr, "unknown argument %s\n", argv[i]);
			return 2;
		}
	}
	FILE* f = std::fopen(csv, "r");
	if (!f) {
		std::perror(csv);
		return 2;
	}
	FILE* tr = trace_path ? std::fopen(trace_path, "w") : nullptr;
	uint64_t h = 0xcbf29ce484222325ULL;
	auto out = [&](const char* line) {
		h = fnv(h, line);
		if (tr) std::fputs(line, tr);
	};
	// the board as the user has it: settings preset 1 at boot (iGpreset 1, striso.py save-touch)
	set_cfg("iGpreset", "1");
	synth_control_init();
	midi_config();
	if (std::string(mode) == "normal") stck(2);
	else if (std::string(mode) == "mono") stck(4);
	else if (std::string(mode) != "mpe") {
		std::fprintf(stderr, "unknown mode %s\n", mode);
		return 2;
	}
	midi_log.clear();
	VC last[VOICECOUNT];
	memcpy(last, vc, sizeof last);
	const systime_t t0 = host_now;
	systime_t next_tick = t0 + TIME_MS2I(2);
	size_t sent = 0;
	long rows = 0;
	std::map<int, int> down; // last pres per key
	char line[256], buf[256];
	auto flush = [&](double t, int id) {
		for (; sent < midi_log.size(); sent++) {
			int n = snprintf(buf, sizeof buf, "M %.4f", t);
			for (auto b : midi_log[sent]) n += snprintf(buf + n, sizeof buf - n, " %02X", b);
			snprintf(buf + n, sizeof buf - n, "\n");
			out(buf);
		}
		for (int v = 0; v < VOICECOUNT; v++)
			if (memcmp(&vc[v], &last[v], sizeof(VC)) != 0) {
				snprintf(buf, sizeof buf, "S %.4f k%d v%d %.6g %.6g %.6g %.6g %.6g\n", t, id, v, vc[v].note, vc[v].pres, vc[v].vpres, vc[v].but_x, vc[v].but_y);
				out(buf);
				last[v] = vc[v];
			}
	};
	double t = 0;
	while (std::fgets(line, sizeof line, f)) {
		int src, id, p, v, x, y;
		if (std::sscanf(line, "%lf,%d,%d,%d,%d,%d,%d", &t, &src, &id, &p, &v, &x, &y) != 7) continue;
		const systime_t now = t0 + (systime_t)lrint(t * CH_CFG_ST_FREQUENCY);
		while ((int32_t)(now - next_tick) >= 0) { // the main loop's synth_tick every 2 ms
			host_now = next_tick;
			synth_tick();
			next_tick += TIME_MS2I(2);
		}
		host_now = now;
		int msg[6] = {src, id, p, v, x, y};
		synth_message(6, msg);
		flush(t, id);
		down[id] = p;
		rows++;
	}
	std::fclose(f);
	// the takes end with a key still down (the recording stopped): release it as the scanner does
	for (auto& [id, p] : down)
		if (p > 0) {
			t += 0.002;
			host_now += TIME_MS2I(2);
			int msg[6] = {ID_DIS, id, 0, 0, 0, 0};
			synth_message(6, msg);
			flush(t, id);
		}
	for (int i = 0; i < 1000; i++) { // 2 s of idle main loop
		host_now += TIME_MS2I(2);
		synth_tick();
	}
	flush(t + 2, -1);
	if (tr) std::fclose(tr);
	const bool balanced = notes_balanced();
	const int sounding = count_sounding();
	std::printf("%s (%s): %ld key messages, %zu MIDI messages (%d note-ons), hash %016llx\n", csv, mode, rows,
	    midi_log.size(), count_midi(0, 0xf0, 0x90), static_cast<unsigned long long>(h));
	int rc = 0;
	if (!balanced) {
		std::printf("FAIL: MIDI notes left on\n");
		rc = 1;
	}
	if (sounding) {
		std::printf("FAIL: %d internal voices left sounding\n", sounding);
		rc = 1;
	}
	const char* golden_out = std::getenv("STRISO_GOLDEN_OUT");
	if (golden_name && golden_out && *golden_out) {
		const std::string path = std::string(golden_out) + "/" + golden_name + ".hash";
		FILE* g = std::fopen(path.c_str(), "w");
		if (!g) {
			std::perror(path.c_str());
			return 1;
		}
		std::fprintf(g, "%s %016llx\n", golden_name, static_cast<unsigned long long>(h));
		std::fclose(g);
		expect = nullptr;
	}
	if (expect) {
		char got[17];
		snprintf(got, sizeof got, "%016llx", static_cast<unsigned long long>(h));
		if (std::string(expect) != got) {
			std::printf("FAIL: hash %s, golden %s (tests/golden/control_replay.txt)\n", got, expect);
			rc = 1;
		}
	}
	return rc;
}

// ------------------------------------------------------------------------------------------- main
struct Scenario {
	const char* name;
	void (*fn)();
};
static const Scenario scenarios[] = {
    {"b1_message_interval", t_b1_message_interval},
    {"b2_cc126_count", t_b2_cc126_count},
    {"b3_transpose_one_key", t_b3_transpose_one_key},
    {"b4_channel_overflow", t_b4_channel_overflow},
    {"b5_bend_range_zero", t_b5_bend_range_zero},
    {"b15_time_wrap", t_b15_time_wrap},
    {"sc01_dummy_voices", t_sc01_dummy_voices},
    {"sc02_poly_to_mpe", t_sc02_poly_to_mpe},
    {"sc02_mpe_to_poly", t_sc02_mpe_to_poly},
    {"sc02_switch_held", t_sc02_switch_held},
    {"sc03_mono_delay", t_sc03_mono_delay},
    {"sc04_toff_stale", t_sc04_toff_stale},
    {"sc05_pres_hyst", t_sc05_pres_hyst},
    {"sc05_pres_hyst_normal", t_sc05_pres_hyst_normal},
    {"sc06_pingpong", t_sc06_pingpong},
    {"sc07_stale_cc74", t_sc07_stale_cc74},
    {"sc08_bend_default", t_sc08_bend_default},
#if defined(SYNTHCTL_NORMAL_BEND_LIKE_MPE) && SYNTHCTL_NORMAL_BEND_LIKE_MPE
    {"sc08_bend_like_mpe", t_sc08_bend_like_mpe},
#endif
    {"sc09_alt_light", t_sc09_alt_light},
    {"sc10_normal_internal", t_sc10_normal_internal},
    {"sc10_normal_midi_unchanged", t_sc10_normal_midi_unchanged},
    {"sc11_midinote_knob", t_sc11_midinote_knob},
    {"sc11_midinote_preset", t_sc11_midinote_preset},
    {"sc12_flip_offsets", t_sc12_flip_offsets},
    {"sc13_ranges", t_sc13_ranges},
    {"sc14_mcm_zero", t_sc14_mcm_zero},
    {"sc15_pb_in", t_sc15_pb_in},
    {"sc16_echo", t_sc16_echo},
    {"sc17_rpn_boot", t_sc17_rpn_boot},
    {"sc18_pedal_channel", t_sc18_pedal_channel},
    {"sc19_poly_pgm", t_sc19_poly_pgm},
    {"sc20_bend_range_key", t_sc20_bend_range_key},
#if defined(SYNTHCTL_MOTION_CC_FIX) && SYNTHCTL_MOTION_CC_FIX
    {"sc21_motion_hyst", t_sc21_motion_hyst},
#endif
    {"sc22_free_transpose_range", t_sc22_free_transpose_range},
    {"sc22_octave_release", t_sc22_octave_release},
    {"sc23_panic", t_sc23_panic},
    {"sc24_knob_y0", t_sc24_knob_y0},
    {"sc25_midiparam_first", t_sc25_midiparam_first},
    {"sc27_dials", t_sc27_dials},
    {"sc28_cc1_lsb", t_sc28_cc1_lsb},
    {"sc29_cc65", t_sc29_cc65},
    {"sc30_mode_messages", t_sc30_mode_messages},
    {"sc31_steal_dup", t_sc31_steal_dup},
    {"sc35_tuning_overflow", t_sc35_tuning_overflow},
    {"sc37_blink", t_sc37_blink},
    {"sc38_no_synth", t_sc38_no_synth},
    {"preset_keys", t_preset_keys},
};

int main(int argc, char** argv) {
	setvbuf(stdout, nullptr, _IONBF, 0);
	init_synth_interface();
	user_config();
	const std::string t = argc > 1 ? argv[1] : "";
	if (t == "list") {
		for (auto& s : scenarios) std::printf("%s\n", s.name);
		return 0;
	}
	if (t == "replay" && argc > 2) return replay(argc, argv);
	for (auto& s : scenarios)
		if (t == s.name) {
			s.fn();
			if (reset_called) std::printf("(system reset requested)\n");
			std::printf("%s: %s\n", s.name, failures ? "FAILED" : "ok");
			return failures ? 1 : 0;
		}
	std::fprintf(stderr, "usage: %s <scenario>|list|replay <take.csv> [...]\n", argv[0]);
	return 2;
}
