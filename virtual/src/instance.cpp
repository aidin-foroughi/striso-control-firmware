// One firmware instance: boots the firmware on the hal stand-in and runs it in virtual time.
//
// Time runs in frames at 48 kHz. Its interrupts, in this order when they fall on the same frame:
//   the system tick (2 kHz: every 24 frames)   sleeping threads wake (chThdSleep, the main loop)
//   the USB frame (1 kHz: every 48 frames)      the console's IN buffers go to the host
//   the codec block (every 32 frames, from codec_init on)   computebufI: the synth renders a block
// After each, the woken threads run until they all wait (sched.cpp). A block rendered at frame t
// covers frames t..t+31 of the output.
#include <algorithm>
#include <cstring>

extern "C" {
#include "ch.h"
#include "config.h"
#include "hal.h"
#include "led.h"
#include "messaging.h"
#include "sv_host.h"
#include "synth.h"
#include "usb_audio.h"
#include "version.h"
void svh_usb_audio_init(void);
void svh_set_engine_voice(int v, const float* c);
void svh_set_engine_globals(const float* g);
extern int last_color[3];
extern ioline_t led_dimmed;
}

#include "fw_api.h"

struct sv_shared* svh_shared;

namespace {

constexpr int RING = 8192; // audio frames kept between a block's render and its copy out
constexpr int CHUNK = 4096;
constexpr uint64_t NEVER = ~0ull;

struct Taps {
	float usb[RING * 2];
	float engine[RING];
	float fx[RING * 2];
};

Taps* taps;
uint64_t next_tick, next_sof, next_block;
int blocks;
bool reset_seen;

void store_block(uint64_t t) {
	const svh_block& b = svh_last_block;
	for (int n = 0; n < 32; n++) {
		const size_t i = static_cast<size_t>((t + n) % RING);
		taps->usb[2 * i] = static_cast<float>(b.usb[2 * n]) * (1.0f / 2147483648.0f);
		taps->usb[2 * i + 1] = static_cast<float>(b.usb[2 * n + 1]) * (1.0f / 2147483648.0f);
		taps->engine[i] = b.engine[n];
		taps->fx[2 * i] = b.fx[n];
		taps->fx[2 * i + 1] = b.fx[32 + n];
	}
}

void copy_out(uint64_t from, int n, float* usb, float* engine, float* fx) {
	for (int k = 0; k < n; k++) {
		const size_t i = static_cast<size_t>((from + k) % RING);
		if (usb) {
			usb[2 * k] = taps->usb[2 * i];
			usb[2 * k + 1] = taps->usb[2 * i + 1];
		}
		if (engine) engine[k] = taps->engine[i];
		if (fx) {
			fx[2 * k] = taps->fx[2 * i];
			fx[2 * k + 1] = taps->fx[2 * i + 1];
		}
		taps->usb[2 * i] = taps->usb[2 * i + 1] = taps->engine[i] = taps->fx[2 * i] = taps->fx[2 * i + 1] = 0.0f;
	}
}

// runs [now, now + n): every interrupt in it; returns the frames run (fewer after a reset)
int run_chunk(int n) {
	const uint64_t start = svh_now(), end = start + static_cast<uint64_t>(n);
	uint64_t t = start;
	while (!svh_halted()) {
		if (next_block == NEVER && svh_codec_running()) next_block = t + 32; // the first DMA half
		t = std::min({next_tick, next_sof, next_block});
		if (t >= end) break;
		svh_set_now(t);
		if (t == next_tick) {
			svh_tick();
			next_tick += SVH_TICK_FRAMES;
		}
		if (t == next_sof) {
			svh_usb_sof();
			next_sof += SVH_SOF_FRAMES;
		}
		svh_run();
		if (t == next_block && !svh_halted()) {
			svh_last_block.valid = 0;
			svh_codec_interrupt();
			svh_run();
			if (svh_last_block.valid) {
				store_block(t);
				blocks++;
			}
			next_block += 32;
		}
	}
	if (svh_halted()) {
		reset_seen = true;
		return static_cast<int>(svh_now() - start);
	}
	svh_set_now(end);
	return n;
}

// --- the API of one instance -------------------------------------------------------------------------

int32_t fw_boot(sv_shared* shared) {
	svh_shared = shared;
	sv_rtc = reinterpret_cast<sv_rtc_t*>(shared->rtc_bkp);
	sv_uid = shared->uid;
	taps = new Taps{};
	std::memset(&svh_io, 0, sizeof svh_io);
	// inputs at power on: the portamento key (pressed: 0) and the aux keys up
	palWriteLine(LINE_BUTTON_PORT, shared->port_key_held ? 0 : 1);
	svh_sched_init();
	svh_usb_init();
	svh_usb_audio_init();
	next_tick = SVH_TICK_FRAMES;
	next_sof = SVH_SOF_FRAMES;
	next_block = NEVER;
	blocks = 0;
	reset_seen = false;
	svh_start_main(svh_firmware_main, 1024);
	svh_dummy_thread("idle", IDLEPRIO, CH_STATE_READY);
	svh_run();
	return 0;
}

void fw_shutdown() {
	svh_sched_shutdown();
	svh_external_free();
	svh_usb_free();
	delete taps;
	taps = nullptr;
}

int32_t fw_advance(int32_t frames, float* usb, float* engine, float* fx) {
	int32_t done = 0;
	while (done < frames && !svh_halted()) {
		const uint64_t from = svh_now();
		const int n = std::min<int32_t>(CHUNK, frames - done);
		const int ran = run_chunk(n);
		copy_out(from, ran, usb ? usb + 2 * done : nullptr, engine ? engine + done : nullptr, fx ? fx + 2 * done : nullptr);
		done += ran;
		if (ran < n) break;
	}
	return done;
}

uint64_t fw_now() { return svh_now(); }
int32_t fw_reset_requested() { return svh_halted() ? 1 : 0; }
int32_t fw_usb_attached() { return svh_usb_attached(); }
int32_t fw_console_write(const uint8_t* b, int32_t n) { return svh_console_write(b, n); }
int32_t fw_console_read(uint8_t* b, int32_t n) { return svh_console_read(b, n); }
int32_t fw_midi_write(const uint8_t* b, int32_t n) { return svh_midi_write(b, n); }
int32_t fw_midi_read(uint8_t* b, int32_t n) { return svh_midi_read(b, n); }

int32_t fw_usb_control(uint8_t type, uint8_t req, uint16_t value, uint16_t index, uint8_t* data, uint16_t len) {
	const int32_t r = svh_usb_control(type, req, value, index, data, len);
	return r < 0 ? SV_ESTATE : r;
}

int32_t fw_ump_write(const uint32_t* words, int32_t n) {
	const int32_t r = svh_ump_write(words, n);
	svh_run();
	return r < 0 ? SV_EBUSY : r;
}

int32_t fw_inject_key(int32_t src, int32_t id, int32_t pres, int32_t velo, int32_t x, int32_t y) {
	const int32_t r = svh_inject_key(src, id, pres, velo, x, y);
	svh_run();
	return r < 0 ? SV_EBUSY : 0;
}

int32_t fw_inject_control(int32_t id, int32_t value) {
	const int32_t r = svh_inject_control(id, value);
	svh_run();
	return r < 0 ? SV_EBUSY : 0;
}

int32_t fw_inject_motion(const int16_t raw[6]) {
	svh_inject_motion(raw);
	return 0;
}

int32_t fw_set_engine_controls(int32_t v, const float* values) {
	if (!values) return SV_EINVAL;
	if (v == -1) {
		svh_motion_off();
		svh_set_engine_globals(values);
		return 0;
	}
	if (v < 0 || v >= VOICECOUNT) return SV_EINVAL;
	svh_set_engine_voice(v, values);
	return 0;
}

int32_t fw_select_voice(int32_t n) { return synth_select_voice(n) ? 0 : SV_EINVAL; }

int32_t updown_state(ioline_t line) {
	if (PWMD3.periodic_notification && PWMD3.channel_notification[0] && led_dimmed == line) return SV_LED_DIM;
	return palReadLine(line) ? SV_LED_ON : SV_LED_OFF;
}

int32_t fw_get_leds(sv_leds* out) {
	for (int i = 0; i < 3; i++) out->rgb[i] = last_color[i];
	out->pwm[0] = static_cast<int32_t>(PWMD4.duty[0]); // led.c: channel 0 red, 2 green, 1 blue
	out->pwm[1] = static_cast<int32_t>(PWMD4.duty[2]);
	out->pwm[2] = static_cast<int32_t>(PWMD4.duty[1]);
	out->updown[0] = updown_state(LINE_LED_UP2);
	out->updown[1] = updown_state(LINE_LED_UP);
	out->updown[2] = updown_state(LINE_LED_DOWN);
	out->updown[3] = updown_state(LINE_LED_DOWN2);
	out->led1 = palReadLine(LINE_LED1);
	return 0;
}

int32_t fw_get_state(sv_state* out) {
	out->usb = svh_usb_attached() ? SV_USB_ATTACHED : SV_USB_DETACHED;
	out->voice = synth_current_voice();
	out->voices = synth_num_voices();
	out->streaming = config.send_usb_bulk;
	out->midi_mode = static_cast<int32_t>(config.midi_mode);
	out->blocks = blocks;
	out->to_block = next_block == NEVER ? -1 : static_cast<int32_t>(next_block - svh_now());
	out->underruns = messaging_drops();
	out->volume = volume;
	std::memcpy(out->controls, svh_last_block.controls, sizeof out->controls);
	std::memcpy(out->globals, svh_last_block.globals, sizeof out->globals);
	return 0;
}

int32_t fw_test(int32_t what, int32_t arg) {
	(void)arg;
	switch (what) {
	case SV_TEST_FX_INIT: svh_synth_fx_init(); return 0;
	case SV_TEST_MOTION_OFF: svh_motion_off(); return 0;
	default: return SV_EINVAL;
	}
}

const char* fw_version() { return FWVERSION; }

const sv_fw_api api = {
    SV_FW_API_VERSION,
    fw_boot,
    fw_shutdown,
    fw_advance,
    fw_now,
    fw_reset_requested,
    fw_usb_attached,
    fw_console_write,
    fw_console_read,
    fw_midi_write,
    fw_midi_read,
    fw_ump_write,
    fw_usb_control,
    fw_inject_key,
    fw_inject_control,
    fw_inject_motion,
    fw_set_engine_controls,
    fw_select_voice,
    fw_get_leds,
    fw_get_state,
    fw_test,
    fw_version,
};

} // namespace

extern "C" const sv_fw_api* svfw_api(void) { return &api; }
