// The aux jack (src/drivers/aux_jack.cpp with the JackDetector, jack_detector.hpp) against the old
// aux_jack.c (tests/ref/aux_jack_ref.c), on the same stand-in HAL (tests/aux_jack/stubs): every
// pin mode, contact toggle, digital read, sleep, conversion, LED blink, pedal message and mode
// change is recorded, with the active mode at each. The new code must make the old code's calls,
// in the same order, from the same ADC and pin readings.
//
//   electrical   an electrical model of the jack (the detect contact, the pulls, a floating tip
//                that holds its charge, a TS plug that shorts the ring, a TRS plug with the ring on
//                aux power): the plug-in cases, D3 (a TS switch pedal is detected), D9 (unplugged
//                while pressed: a pedal-off, the DMA buffer never written), each on both
//   table        every mode's transitions (what a pass does from each mode for each kind of
//                reading), as a table of expected modes, LED blinks and pedal messages, on both
//   classify     the autodetect pass over a grid of readings at and around every threshold, with
//                and without a closed tip before, the contact followed or not: both the same
//   random       2000 seeded random runs from every mode and setting, readings near the thresholds
//   settings     a setting from the control thread is taken at the jack thread's next pass
//   init         aux_jack_init: the pins, the ADC and the deprecated setting, as before
#include <algorithm>
#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "doctest/doctest.h"

#include "aux_jack.cpp" // NOLINT(bugprone-suspicious-include): the test reaches its thread and detector

config_t config = {JACK2_MODE_AUTODETECT, JACK2_MODE_DISABLED};
ADCDriver ADCD3;

extern "C" {
void ref_aux_jack_run(void);
void ref_aux_jack_reset(void);
void ref_aux_jack_init(void);
void ref_aux_jack_switch_mode_setting(jack2_mode_t mode);
}

namespace {

using jack::Sample;

// --- the recorded calls -------------------------------------------------------------------------
struct Op {
	char kind; // C convert, M pin mode, S set line, L clear line, R read line, m/u sleep, B blink,
	           // 1/2 messages, Y serial midi, N line in, A adc start, T thread
	int a = 0, b = 0, c = 0, d = 0;
	int mode = 0; // config.jack2_mode at the call
	bool operator==(const Op&) const = default;
};
std::vector<Op> trace;
void rec(char k, int a = 0, int b = 0, int c = 0, int d = 0) { trace.push_back({k, a, b, c, d, static_cast<int>(config.jack2_mode)}); }

std::string show(const Op& o) {
	char s[96];
	std::snprintf(s, sizeof s, "%c(%d,%d,%d,%d) mode %d", o.kind, o.a, o.b, o.c, o.d, o.mode);
	return s;
}
// the first difference of two traces, "" if none
std::string diff(const std::vector<Op>& a, const std::vector<Op>& b) {
	const std::size_t n = std::min(a.size(), b.size());
	for (std::size_t i = 0; i < n; i++) {
		if (!(a[i] == b[i])) return "op " + std::to_string(i) + ": old " + show(a[i]) + ", new " + show(b[i]);
	}
	if (a.size() != b.size()) return "lengths: old " + std::to_string(a.size()) + ", new " + std::to_string(b.size());
	return "";
}

// --- the world: what the ADC and the tip's digital input read -----------------------------------
struct World {
	int tx_mode = PAL_MODE_INPUT_PULLDOWN, rx_mode = PAL_MODE_INPUT_PULLDOWN;
	bool detect = true, vdd_low = false;
	virtual ~World() = default;
	virtual void convert(adcsample_t* s) = 0;
	virtual bool tx_high() = 0;
};
World* world = nullptr;
std::jmp_buf stop;
int conversions = 0, budget = 0;

// the old code's and the new code's thread, until the world's conversions run out
std::vector<Op> run(bool old, World& w, jack2_mode_t mode, jack2_mode_t setting, int n) {
	world = &w;
	trace.clear();
	conversions = 0;
	budget = n;
	config.jack2_mode = mode;
	config.jack2_mode_setting = setting;
	if (old) {
		ref_aux_jack_reset();
		if (setjmp(stop) == 0) ref_aux_jack_run();
	} else {
		detector = jack::JackDetector<HalIo>{};
		detector.mode = static_cast<jack::Mode>(mode);
		detector.setting = static_cast<jack::Mode>(setting);
		requested_setting = -1;
		if (setjmp(stop) == 0) AuxJack(nullptr);
	}
	world = nullptr;
	return trace;
}

// a world that answers from a script: per conversion (tip, ring), per digital read a level
struct Script : World {
	std::vector<Sample> conv;
	std::vector<int> reads; // -1: follow the detect contact (nothing plugged in)
	std::size_t ci = 0, ri = 0;
	Sample last{};
	void convert(adcsample_t* s) override {
		if (ci < conv.size()) last = conv[ci++];
		s[0] = static_cast<adcsample_t>(last.tip);
		s[1] = static_cast<adcsample_t>(last.ring);
	}
	bool tx_high() override {
		const int r = ri < reads.size() ? reads[ri++] : -1;
		return r < 0 ? detect : r != 0;
	}
};
} // namespace

// --- the stand-in HAL (stubs/hal.h, ch.h): both versions call these ---------------------------------
extern "C" {
void adcConvert(ADCDriver* /*d*/, const ADCConversionGroup* /*g*/, adcsample_t* s, int /*depth*/) {
	if (conversions++ >= budget) std::longjmp(stop, 1);
	world->convert(s);
	rec('C', s[0], s[1]);
}
void adcStart(ADCDriver* /*d*/, const ADCConfig* /*c*/) { rec('A'); }
void palSetLineMode(ioline_t l, int mode) {
	if (world && l == LINE_AUX_UART2_TX) world->tx_mode = mode;
	if (world && l == LINE_AUX_UART2_RX) world->rx_mode = mode;
	rec('M', l, mode);
}
void palSetLine(ioline_t l) {
	if (world && l == LINE_AUX_JACK_DETECT) world->detect = true;
	if (world && l == LINE_AUX_VDD) world->vdd_low = false;
	rec('S', l);
}
void palClearLine(ioline_t l) {
	if (world && l == LINE_AUX_JACK_DETECT) world->detect = false;
	if (world && l == LINE_AUX_VDD) world->vdd_low = true;
	rec('L', l);
}
int palReadLine(ioline_t l) {
	const int v = l == LINE_AUX_UART2_TX && world ? world->tx_high() : 0;
	rec('R', l, v);
	return v;
}
void chRegSetThreadName(const char* /*n*/) {}
void chThdSleepMilliseconds(int ms) { rec('m', ms); }
void chThdSleepMicroseconds(int us) { rec('u', us); }
void* chThdCreateStatic(void* /*wa*/, size_t s, int p, void (* /*f*/)(void*), void* /*a*/) {
	rec('T', static_cast<int>(s), p);
	return nullptr;
}
void codec_linein_enable(void) { rec('N', 1); }
void codec_linein_disable(void) { rec('N', 0); }
void serial_midi_enable(void) { rec('Y', 1); }
void serial_midi_disable(void) { rec('Y', 0); }
const char* jack_setting = "auto    ";
bool config_text_is(const char* key, const char* text) {
	if (std::memcmp(key, "sGjack2 ", 8) != 0) return false;
	size_t n = 8;
	while (n > 0 && jack_setting[n - 1] == ' ') n--;
	return std::strlen(text) == n && std::memcmp(jack_setting, text, n) == 0;
}
bool event_send1(int src, int id, int a) {
	rec('1', src, id, a);
	return true;
}
bool event_send2(int src, int id, int a, int b) {
	rec('2', src, id, a, b);
	return true;
}
void led_rgb3_blink(int r, int g, int b, sysinterval_t t) { rec('B', r, g, b, static_cast<int>(t)); }
}

namespace {

// --- the electrical model (the cases test_aux_jack.c had) ---------------------------------
enum class Plug { none,
	ts_pedal,
	trs_pedal };
struct Electrical : World {
	Plug plug = Plug::none;
	int (*script)(int conv) = nullptr; // the pedal closed at this conversion
	int unplug_at = 0;
	bool closed = false;
	int charge = 0; // a floating tip holds its last level
	adcsample_t last[2] = {0, 0};
	int cpu_writes = 0, detected_at = 0;
	int tip() {
		if (plug == Plug::none) return detect ? 16383 : 0; // the tip touches the detect contact
		if (closed) return 0;
		if (tx_mode == PAL_MODE_INPUT_PULLUP) charge = 16383;
		else if (tx_mode == PAL_MODE_INPUT_PULLDOWN) charge = 30;
		return charge;
	}
	[[nodiscard]] int ring() const {
		if (plug == Plug::ts_pedal) return 0;                      // a mono plug shorts the ring
		if (plug == Plug::trs_pedal) return vdd_low ? 16000 : 100; // aux power on the ring, or pulled down
		return 16383;
	}
	void convert(adcsample_t* s) override {
		if (detected_at == 0 && config.jack2_mode == JACK2_MODE_PEDAL_SWITCH) detected_at = conversions;
		if (conversions > 1 && (s[0] != last[0] || s[1] != last[1])) cpu_writes++; // D9
		if (unplug_at && conversions == unplug_at) plug = Plug::none;
		closed = script(conversions) != 0;
		s[0] = static_cast<adcsample_t>(tip());
		s[1] = static_cast<adcsample_t>(ring());
		last[0] = s[0];
		last[1] = s[1];
	}
	bool tx_high() override { return tip() > 8192; }
};
int open_always(int /*c*/) { return 0; }
int tap_then_hold(int c) { return c < 200 ? (c / 40) % 2 : 1; }
int tap_every_40(int c) { return (c / 40) % 2; } // pressed ~40 conversions (~0.5 s), released ~40

struct Outcome {
	jack2_mode_t mode;
	int detected_at, pedal1_msgs, last_pedal1, cpu_writes;
};
Outcome outcome(const std::vector<Op>& t, const Electrical& w) {
	Outcome o{config.jack2_mode, w.detected_at, 0, -1, w.cpu_writes};
	for (const Op& op : t) {
		if (op.kind == '1' && op.b == IDC_PEDAL_1) {
			o.pedal1_msgs++;
			o.last_pedal1 = op.c;
		}
	}
	return o;
}

// one plug-in from an empty jack with the setting auto, on both versions
Outcome electrical(Plug p, int (*sc)(int), int n, int unplug_at = 0) {
	Outcome res[2];
	std::vector<Op> t[2];
	for (int old = 1; old >= 0; old--) {
		Electrical w;
		w.plug = p;
		w.script = sc;
		w.unplug_at = unplug_at;
		t[old] = run(old != 0, w, JACK2_MODE_DISABLED, JACK2_MODE_AUTODETECT, n);
		res[old] = outcome(t[old], w);
	}
	CHECK_MESSAGE(diff(t[1], t[0]) == "", diff(t[1], t[0]));
	return res[0];
}

} // namespace

TEST_CASE("electrical: plug-ins as the old code detected them (D3, D9), call for call") {
	// ~80 conversions per second; a switch pedal should show (pink) within about half a second
	Outcome o = electrical(Plug::ts_pedal, open_always, 2000);
	CHECK(o.mode == JACK2_MODE_PEDAL_SWITCH);
	CHECK((o.detected_at > 0 && o.detected_at <= 60));
	o = electrical(Plug::ts_pedal, tap_every_40, 2000);
	CHECK(o.mode == JACK2_MODE_PEDAL_SWITCH);
	CHECK((o.detected_at > 0 && o.detected_at <= 60));
	CHECK(o.pedal1_msgs >= 20);
	o = electrical(Plug::trs_pedal, tap_every_40, 2000);
	CHECK(o.mode == JACK2_MODE_PEDAL_SWITCH);
	CHECK((o.detected_at > 0 && o.detected_at <= 200));
	CHECK(o.pedal1_msgs >= 20);
	o = electrical(Plug::none, open_always, 2000);
	CHECK(o.mode == JACK2_MODE_DISABLED);
	CHECK(o.detected_at == 0);
	// D9: unplugged while pressed: a pedal-off message, without writing the ADC's DMA buffer
	o = electrical(Plug::trs_pedal, tap_then_hold, 400, 300);
	CHECK(o.mode == JACK2_MODE_DISABLED);
	CHECK((o.detected_at > 0 && o.detected_at <= 200));
	CHECK(o.last_pedal1 == 0);
	CHECK(o.cpu_writes == 0);
}

// --- the transition table -----------------------------------------------------------------------------
namespace {

constexpr int U = -1; // a digital read that follows the contact: nothing plugged in
struct Row {
	const char* what;
	jack2_mode_t from, setting;
	int passes;               // run this many passes
	std::vector<Sample> conv; // the conversions (the last repeats)
	std::vector<int> reads;   // the digital reads of the tip (then: follow the contact)
	jack2_mode_t to;
	int blinks;      // LED blinks expected (-1: don't check)
	int first_blink; // its colour 0xRRGGBB (-1: don't check)
	int msgs;        // pedal messages expected (-1: don't check)
};
// conversions of one autodetect pass: pulldown, pullup (twice, stable), floating and ring
std::vector<Sample> ad(int pulldown, int pullup, int floating, int ring, int passes = 1) {
	std::vector<Sample> v;
	for (int i = 0; i < passes; i++) {
		v.insert(v.end(), {{pulldown, 16383}, {pullup, 16383}, {pulldown, 16383}, {pullup, 16383}, {floating, ring}});
	}
	return v;
}
std::vector<int> stuck(int n, int level) {
	std::vector<int> v(static_cast<std::size_t>(n), level);
	return v;
}

std::vector<Row> rows() {
	using M = jack2_mode_t;
	const M D = JACK2_MODE_DISABLED, AD = JACK2_MODE_AUTODETECT, MI = JACK2_MODE_MIDI, EX = JACK2_MODE_PEDAL_EXPRESSION,
	        SW = JACK2_MODE_PEDAL_SWITCH, LI = JACK2_MODE_LINEIN, ER = JACK2_MODE_ERROR;
	std::vector<Row> r;
	// disabled: the tip low means a plug: the setting's mode
	for (M s : {AD, MI, EX, SW, LI}) {
		static const int colour[] = {0, 0x005a5a, 0x5a5a00, 0x5a005a, 0x00005a, 0x00ba00};
		r.push_back({"disabled, tip 499: the setting", D, s, 1, {{499, 16383}}, {}, s, 1, colour[s], 0});
	}
	r.push_back({"disabled, tip 500: stays", D, AD, 3, {{500, 16383}}, {}, D, 0, -1, 0});
	// autodetect, one pass per kind of reading
	r.push_back({"autodetect, unstable pull-down: nothing", AD, AD, 1, {{3000, 1}, {9000, 1}, {3101, 1}, {9000, 1}}, {}, AD, 0, -1, 0});
	r.push_back({"autodetect, unstable pull-up: nothing", AD, AD, 1, {{3000, 1}, {9000, 1}, {3000, 1}, {9101, 1}}, {}, AD, 0, -1, 0});
	r.push_back({"autodetect, tip at 3.3 V: error", AD, AD, 1, ad(16383, 16383, 16383, 16383), {}, ER, 1, 0x5a0000, 0});
	r.push_back({"autodetect, ring at 3.3 V: error", AD, AD, 1, ad(3000, 9000, 5000, 16383), {}, ER, 1, 0x5a0000, 0});
	r.push_back({"autodetect, ring shorted 6 passes: waits", AD, AD, 6, ad(30, 16383, 30, 0, 6), {}, AD, 0, -1, 0});
	r.push_back({"autodetect, ring shorted 7 passes: switch pedal", AD, AD, 7, ad(30, 16383, 30, 0, 7), {}, SW, 1, 0x5a005a, 0});
	r.push_back({"autodetect, ring shorted 6 passes, a pass not, then shorted: the count starts again", AD, AD, 8, [] { auto v = ad(30, 16383, 30, 0, 6); auto o = ad(600, 16000, 100, 16000); auto s = ad(30, 16383, 30, 0); v.insert(v.end(), o.begin(), o.end()); v.insert(v.end(), s.begin(), s.end()); return v; }(), {}, AD, 0, -1, 0});
	r.push_back({"autodetect, tip closed then floating: switch pedal", AD, AD, 2, [] { auto v = ad(0, 0, 0, 16000); auto f = ad(30, 16383, 30, 16000); v.insert(v.end(), f.begin(), f.end()); return v; }(), {}, SW, 1, 0x5a005a, 0});
	r.push_back({"autodetect, tip floating, never closed: nothing", AD, AD, 2, ad(30, 16383, 30, 16000, 2), {}, AD, 0, -1, 0});
	r.push_back({"autodetect, tip high, contact followed: disabled", AD, AD, 1, ad(16000, 16383, 16000, 16000), {}, D, 1, 0x5a1400, 0});
	r.push_back({"autodetect, tip high, contact not followed: stays", AD, AD, 1, ad(16000, 16383, 16000, 16000), stuck(2, 1), AD, 0, -1, 0});
	r.push_back({"autodetect, tip holds its level: expression pedal", AD, AD, 1, ad(8000, 8100, 8050, 16000), {}, EX, 1, 0x5a5a00, 0});
	r.push_back({"autodetect, high with the pull-up, not low: midi", AD, AD, 1, ad(5200, 16383, 400, 16000), {}, MI, 1, 0x005a5a, 0});
	r.push_back({"autodetect, nothing to decide", AD, AD, 1, ad(600, 16000, 100, 16000), {}, AD, 0, -1, 0});
	// midi
	r.push_back({"midi, tip 15001: disabled", MI, AD, 1, {{15001, 16000}}, {}, D, 1, 0x5a1400, 0});
	r.push_back({"midi, ring shorted 6 passes: stays", MI, AD, 6, {{5000, 5999}}, {}, MI, 0, -1, 0});
	r.push_back({"midi, ring shorted 7 passes: error", MI, AD, 7, {{5000, 5999}}, {}, ER, 1, 0x5a0000, 0});
	// line in
	r.push_back({"line in, tip 15001: disabled", LI, AD, 1, {{15001, 16000}}, {}, D, 1, 0x5a1400, 0});
	r.push_back({"line in, tip 15000: stays", LI, AD, 3, {{15000, 0}}, {}, LI, 0, -1, 0});
	// switch pedal
	r.push_back({"switch pedal, unplugged: disabled, pedal 2 off", SW, AD, 1, {{16383, 16383}}, {}, D, 1, 0x5a1400, 1});
	r.push_back({"switch pedal, plugged, tip closed: pedal 1 down", SW, AD, 1, {{0, 0}}, stuck(1, 0), SW, 0, -1, 1});
	r.push_back({"switch pedal, plugged, both open: pedal 2 up, once", SW, AD, 3, {{16383, 16383}}, stuck(3, 0), SW, 0, -1, 1});
	r.push_back({"switch pedal, contact follows once, then not: plugged", SW, AD, 1, {{8191, 16383}}, {1, 0, 1, 1, 1}, SW, 0, -1, 2});
	// expression pedal
	r.push_back({"expression pedal, at the contact level, unplugged: disabled", EX, AD, 1, {{31 << 9, 16000}}, {}, D, 1, 0x5a1400, 0});
	r.push_back({"expression pedal, at the contact level, plugged: contact low, message", EX, AD, 1, {{31 << 9, 16000}}, stuck(1, 1), EX, 0, -1, 1});
	r.push_back({"expression pedal, levels: a message per pass", EX, AD, 4, {{4000, 16000}}, {}, EX, 0, -1, 4});
	r.push_back({"expression pedal, ring shorted 7 passes: error", EX, AD, 7, {{4000, 100}}, {}, ER, 1, 0x5a0000, 7});
	// error
	r.push_back({"error, unplugged: disabled", ER, AD, 1, {{16383, 16383}}, {}, D, 1, 0x5a1400, 0});
	r.push_back({"error, plugged: stays, a red blink every 82 passes", ER, AD, 164, {{16383, 16383}}, stuck(400, 0), ER, 2, 0x5a0000, 0});
	return r;
}

struct Effects {
	int blinks = 0, first_blink = -1, msgs = 0;
};
Effects effects(const std::vector<Op>& t) {
	Effects e;
	for (const Op& o : t) {
		if (o.kind == 'B') {
			if (e.blinks++ == 0) e.first_blink = o.a << 16 | o.b << 8 | o.c;
		}
		if (o.kind == '1' || o.kind == '2') e.msgs++;
	}
	return e;
}

// the conversions a row's passes take (autodetect passes take 5, or 4 when unstable)
int conversions_of(const Row& r) {
	Script probe;
	probe.conv = r.conv;
	probe.reads = r.reads;
	int n = 0;
	// count by running the new code pass by pass from the row's start
	world = &probe;
	trace.clear();
	conversions = 0;
	budget = 1 << 30;
	config.jack2_mode = r.from;
	config.jack2_mode_setting = r.setting;
	detector = jack::JackDetector<HalIo>{};
	detector.mode = static_cast<jack::Mode>(r.from);
	detector.setting = static_cast<jack::Mode>(r.setting);
	for (int p = 0; p < r.passes; p++) detector.pass(io);
	n = conversions;
	world = nullptr;
	return n;
}

} // namespace

TEST_CASE("table: every mode's transitions, on both") {
	for (const Row& r : rows()) {
		CAPTURE(r.what);
		const int n = conversions_of(r);
		std::vector<Op> t[2];
		jack2_mode_t end[2];
		for (int old = 1; old >= 0; old--) {
			Script w;
			w.conv = r.conv;
			w.reads = r.reads;
			t[old] = run(old != 0, w, r.from, r.setting, n);
			end[old] = config.jack2_mode;
		}
		CHECK_MESSAGE(diff(t[1], t[0]) == "", diff(t[1], t[0]));
		CHECK(end[0] == r.to);
		CHECK(end[1] == r.to);
		const Effects e = effects(t[0]);
		if (r.blinks >= 0) CHECK(e.blinks == r.blinks);
		if (r.first_blink >= 0) CHECK(e.first_blink == r.first_blink);
		if (r.msgs >= 0) CHECK(e.msgs == r.msgs);
	}
}

TEST_CASE("classify: the autodetect pass at and around every threshold, on both") {
	const int pulls[] = {0, 1, 499, 500, 999, 1000, 1001, 8000, 15000, 15001, 15999, 16000, 16001, 16382, 16383};
	const int floats[] = {0, 100, 2999, 3000, 8000, 13383, 13384, 15000, 16383};
	const int rings[] = {0, 5999, 6000, 16382, 16383};
	long runs = 0, differ = 0;
	std::string first;
	int seen[8] = {};
	for (int pd : pulls) {
		for (int pu : pulls) {
			for (int fl : floats) {
				for (int rg : rings) {
					seen[static_cast<int>(jack::classify(pd, pu, fl, rg))]++;
					for (int closed = 0; closed < 2; closed++) {
						for (int follow = 0; follow < 2; follow++) {
							std::vector<Op> t[2];
							for (int old = 1; old >= 0; old--) {
								Script w;
								if (closed) w.conv = ad(0, 0, 0, 16000); // a pass that closes the tip
								const auto p = ad(pd, pu, fl, rg);
								w.conv.insert(w.conv.end(), p.begin(), p.end());
								if (!follow) w.reads = stuck(20, 1);
								t[old] = run(old != 0, w, JACK2_MODE_AUTODETECT, JACK2_MODE_AUTODETECT, static_cast<int>(w.conv.size()));
							}
							runs++;
							const std::string d = diff(t[1], t[0]);
							if (!d.empty() && differ++ == 0) first = d;
						}
					}
				}
			}
		}
	}
	MESSAGE(runs << " autodetect runs");
	CHECK_MESSAGE(differ == 0, first);
	for (int s = 0; s < 8; s++) CHECK(seen[s] > 0); // the grid reaches every kind of reading
}

namespace {
// A random plug: a tip level per pull (pull-down, pull-up, none) and a ring level, mostly near
// the thresholds, held for a random number of conversions with a little noise (so autodetect's
// readings are often stable and it decides), then another; digital reads mostly follow the contact.
struct Random : World {
	std::mt19937 rng;
	int down = 0, up = 0, none = 0, ring = 0, left = 0, noise = 0;
	explicit Random(unsigned seed) :
	    rng(seed) {}
	int level() {
		static const int near[] = {0, 30, 499, 500, 1000, 1001, 5999, 6000, 8191, 8192, 15000, 15001, 15872, 16000, 16001, 16383};
		if (rng() % 4 == 0) return static_cast<int>(rng() % 16384);
		const int base = near[rng() % (sizeof near / sizeof near[0])];
		return std::clamp(base + static_cast<int>(rng() % 41) - 20, 0, 16383);
	}
	int jitter(int v) { return noise ? std::clamp(v + static_cast<int>(rng() % (2 * noise + 1)) - noise, 0, 16383) : v; }
	void convert(adcsample_t* s) override {
		if (left-- <= 0) {
			down = level();
			up = rng() % 2 ? level() : std::max(down, level());
			none = rng() % 2 ? level() : (down + up) / 2;
			ring = level();
			left = 1 + static_cast<int>(rng() % 40);
			noise = rng() % 3 == 0 ? static_cast<int>(rng() % 150) : 0;
		}
		const int tip = tx_mode == PAL_MODE_INPUT_PULLUP ? up : tx_mode == PAL_MODE_INPUT_PULLDOWN ? down :
		                                                                                             none;
		s[0] = static_cast<adcsample_t>(jitter(tip));
		s[1] = static_cast<adcsample_t>(jitter(ring));
	}
	bool tx_high() override { return rng() % 4 ? detect : (rng() & 1) != 0; }
};
} // namespace

TEST_CASE("random: 2000 seeded runs from every mode and setting, on both") {
	const jack2_mode_t modes[] = {JACK2_MODE_DISABLED, JACK2_MODE_MIDI, JACK2_MODE_PEDAL_EXPRESSION, JACK2_MODE_PEDAL_SWITCH,
	    JACK2_MODE_LINEIN, JACK2_MODE_AUTODETECT, JACK2_MODE_ERROR};
	long differ = 0, ops = 0;
	int reached[7] = {};
	int transitions[7][7] = {}; // from, to
	std::string first;
	for (unsigned seed = 1; seed <= 2000; seed++) {
		const jack2_mode_t from = modes[seed % 7];
		const jack2_mode_t setting = modes[1 + (seed / 7) % 5]; // the settings: midi .. autodetect
		std::vector<Op> t[2];
		for (int old = 1; old >= 0; old--) {
			Random w(seed);
			t[old] = run(old != 0, w, from, setting, 400);
		}
		for (std::size_t i = 0; i < t[0].size(); i++) {
			reached[t[0][i].mode]++;
			if (i > 0 && t[0][i].mode != t[0][i - 1].mode) transitions[t[0][i - 1].mode][t[0][i].mode]++;
		}
		ops += static_cast<long>(t[0].size());
		const std::string d = diff(t[1], t[0]);
		if (!d.empty() && differ++ == 0) first = "seed " + std::to_string(seed) + ": " + d;
	}
	MESSAGE(ops << " calls compared");
	CHECK_MESSAGE(differ == 0, first);
	for (int m = 0; m < 7; m++) CHECK(reached[m] > 0);
	// autodetect decided every way, every mode went back to disabled, the errors came
	for (jack2_mode_t to : {JACK2_MODE_DISABLED, JACK2_MODE_MIDI, JACK2_MODE_PEDAL_EXPRESSION, JACK2_MODE_PEDAL_SWITCH, JACK2_MODE_ERROR}) {
		CAPTURE(to);
		CHECK(transitions[JACK2_MODE_AUTODETECT][to] > 0);
	}
	for (int m = 1; m < 7; m++) CHECK(transitions[m][JACK2_MODE_DISABLED] > 0);
	CHECK(transitions[JACK2_MODE_MIDI][JACK2_MODE_ERROR] > 0);
	CHECK(transitions[JACK2_MODE_PEDAL_EXPRESSION][JACK2_MODE_ERROR] > 0);
}

TEST_CASE("settings: a new setting is taken at the jack thread's next pass") {
	// the old code switched on the calling thread; the new one at the jack thread's next pass,
	// with the same calls
	Script w;
	w.conv = {{16383, 16383}};
	std::vector<Op> t[2];
	// old: the setting, then a pass
	world = &w;
	trace.clear();
	config.jack2_mode = JACK2_MODE_PEDAL_SWITCH;
	config.jack2_mode_setting = JACK2_MODE_AUTODETECT;
	ref_aux_jack_reset();
	ref_aux_jack_switch_mode_setting(JACK2_MODE_MIDI);
	t[1] = trace;
	CHECK(config.jack2_mode_setting == JACK2_MODE_MIDI);
	// new: the request does nothing until the thread's pass
	config.jack2_mode = JACK2_MODE_PEDAL_SWITCH;
	config.jack2_mode_setting = JACK2_MODE_AUTODETECT;
	detector = jack::JackDetector<HalIo>{};
	detector.mode = jack::Mode::pedal_switch;
	trace.clear();
	aux_jack_switch_mode_setting(JACK2_MODE_MIDI);
	CHECK(trace.empty());
	CHECK(config.jack2_mode_setting == JACK2_MODE_AUTODETECT);
	conversions = 0;
	budget = 0; // stop at the pass's conversion: only the request's calls
	if (setjmp(stop) == 0) AuxJack(nullptr);
	t[0] = trace;
	CHECK_MESSAGE(diff(t[1], t[0]) == "", diff(t[1], t[0]));
	CHECK(config.jack2_mode_setting == JACK2_MODE_MIDI);
	CHECK(config.jack2_mode == JACK2_MODE_DISABLED);
	CHECK(requested_setting.load() == -1);
	world = nullptr;
}

TEST_CASE("init: the pins, the ADC and the deprecated setting, as before") {
	for (const char* s : {"auto    ", "pedal_ex", "pedal_sw", "midi    ", "linein  ", "other   "}) {
		CAPTURE(s);
		jack_setting = s;
		std::vector<Op> t[2];
		jack2_mode_t set[2];
		for (int old = 1; old >= 0; old--) {
			trace.clear();
			config.jack2_mode = JACK2_MODE_DISABLED;
			config.jack2_mode_setting = JACK2_MODE_AUTODETECT;
			if (old) {
				ref_aux_jack_init();
			} else {
				detector = jack::JackDetector<HalIo>{};
				aux_jack_init();
			}
			t[old] = trace;
			set[old] = config.jack2_mode_setting;
		}
		CHECK_MESSAGE(diff(t[1], t[0]) == "", diff(t[1], t[0]));
		CHECK(set[0] == set[1]);
	}
	jack_setting = "auto    ";
}
