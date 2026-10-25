/*
 * The USB console on the board (was pconnection.c, by Johannes Taelman and Piers Titus van der
 * Torren): the bulk console's commands (the grammar and the command table: console.hpp), USB MIDI
 * in (midi_in_parser.hpp) and the USB start-up. The main thread runs it (PExReceive, every 2 ms).
 *
 * Every reply goes through one OutStream (out_stream.hpp): a wait for room in the bulk queue
 * times out, so a host that stops reading can't hold the main thread forever (B14). The reply
 * lines are a protocol (Striso Live's protocol.dart, tools/striso.py): byte for byte as before.
 *
 * This file is part of Striso Control: free software under the GNU General Public License,
 * version 3 or later (see COPYING).
 */
#include <cstdint>
#include <cstring>

#include "ch.h"
#include "hal.h"

#include "chprintf.h"

extern "C" {
#include "bulk_usb.h"
#include "button_read.h"
#include "config.h"
#include "config_store.h"
#include "key_scanner.h"
#include "midi.h"
#include "midi_usb.h"
#include "pconnection.h"
#include "synth.h"
#include "usb_audio.h"
#include "usbcfg.h"
#include "version.h"
}

#include "console.hpp"
#include "midi_in_parser.hpp"
#include "out_stream.hpp"

static_assert(sizeof(systime_t) == sizeof(uint32_t), "console: system time is 32 bits (Console::feed)");
static_assert(CH_DBG_FILL_THREADS == TRUE && CH_DBG_ENABLE_STACK_CHECK == TRUE,
    "console: StcI reads the threads' stack fill and limits");

namespace {

using Request = console::Request;
using Reply = OutStream<BulkUSBDriver>;

// a prefix or payload is dropped after 100 ms without a byte (R6); a reply waits at most 500 ms
// for the host to make room (B14), as the raw recording's dump does (key_scanner.cpp)
constexpr sysinterval_t kStale = TIME_MS2I(100);
constexpr sysinterval_t kWriteTimeout = TIME_MS2I(500);

constexpr uint32_t kBootloaderRtcSignature = 0x71a21877;

// the boot signature for the UF2 bootloader, then a reset
void reset_to_uf2_bootloader() {
	PWR->CR1 = PWR->CR1 | PWR_CR1_DBP; // the backup domain writable
	RTC->BKP0R = kBootloaderRtcSignature;
	NVIC_SystemReset();
}

// string length up to a 0 or a byte >= 0xf8 (erased flash), at most len (as pconnection.c
// counted it: a string that ends in 0xff loses its last character, B13)
int strlenmax(const unsigned char* s, int len) {
	const unsigned char* s0 = s;
	while (*s++ && *s < 0xf8 && s <= s0 + len) {
	}
	return static_cast<int>(s - s0) - 1;
}

uint32_t float_bits(float f) {
	uint32_t u;
	std::memcpy(&u, &f, sizeof u);
	return u;
}

uint32_t addr32(const void* p) { return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p)); }

// The handlers of console::kCommands. Each writes its reply to `o`.
struct BoardCommands {
	using Out = Reply;

	// V: the binary stream off; the stream data not sent yet is dropped, a packet the host
	// hasn't fetched stays (a reset under it halted the board, R1/D1)
	static void stop_stream() {
		config.send_usb_bulk = 0;
		chSysLock();
		bduDiscardOutputI(&BDU1);
		chSysUnlock();
	}

	static void dfu(Reply& o, const Request& /*r*/) {
		chprintf(o.stream(), "Resetting to DFU mode...\r\n");
		chThdSleepMilliseconds(2);
	}

	static void bootloader(Reply& o, const Request& /*r*/) {
		chprintf(o.stream(), "Resetting to UF2 bootloader mode...\r\n");
		chThdSleepMilliseconds(2);
		reset_to_uf2_bootloader();
	}

	static void stream_on(Reply& /*o*/, const Request& /*r*/) { config.send_usb_bulk = 1; }

	static void stream_off(Reply& o, const Request& /*r*/) {
		config.send_usb_bulk = 0;
		config.send_midi_monitor = false;
		chprintf(o.stream(), "Stcs\r\n");
	}

	static void monitor_on(Reply& /*o*/, const Request& /*r*/) {
		config.send_usb_bulk = 1;
		config.send_midi_monitor = true;
	}

	static void monitor_off(Reply& /*o*/, const Request& /*r*/) { config.send_midi_monitor = false; }

	// the UID and the device flash are at fixed addresses (integer to pointer casts)
	// NOLINTBEGIN(performance-no-int-to-ptr)
	static void version(Reply& o, const Request& /*r*/) {
		BaseSequentialStream* out = o.stream();
		const uint32_t* uid = reinterpret_cast<const uint32_t*>(UID_BASE);
		chprintf(out, FWVERSION " %08X%08X%08X ", uid[2], uid[1], uid[0]);
		int n = strlenmax(devspec_id->id, sizeof(devspec_id->id) + sizeof(devspec_id->model));
		if (n) o.write(devspec_id->id, n);
		chprintf(out, "\r\n");
		n = strlenmax(calib_dis_force->date, sizeof(calib_dis_force->date));
		if (n) {
			chprintf(out, "Calibration date: ");
			o.write(calib_dis_force->date, n);
			chprintf(out, "\r\n");
		}
		if (config.debug) chprintf(out, "Debug: enabled\r\n");
	}
	// NOLINTEND(performance-no-int-to-ptr)

	static void threads(Reply& o, const Request& /*r*/) {
		static const char* const states[] = {CH_STATE_NAMES};
		BaseSequentialStream* out = o.stream();
		chprintf(out, "stklimit    stack     addr   unused refs prio     state         name\r\n");
		thread_t* tp = chRegFirstThread();
		do {
			const char* stk = reinterpret_cast<const char*>(tp->wabase);
			int nfree = 0;
			while (*stk == 0x55) {
				nfree++;
				stk++;
			}
			chprintf(out, "%08lx %08lx %08lx %8lu %4lu %4lu %9s %12s\r\n", addr32(tp->wabase), addr32(tp->ctx.sp),
			    addr32(tp), nfree, static_cast<uint32_t>(tp->refs) - 1, static_cast<uint32_t>(tp->hdr.pqueue.prio),
			    states[tp->state], tp->name == nullptr ? "" : tp->name);
			tp = chRegNextThread(tp);
		} while (tp != nullptr);
	}

	// the active tuning and every key's pitch (read only, src/synth/tuning.h)
	static void tuning(Reply& o, const Request& /*r*/) { synth_tuning_print(o.stream()); }

	// press a settings key (the whitelist: synth_settings_key in synth_control.cpp)
	static void settings_key(Reply& o, const Request& r) {
		const int ok = synth_settings_key(r.payload[0]);
		chprintf(o.stream(), "K %s %d\r\n", ok ? "ok" : "err", r.payload[0]);
	}

	// the settings-key layout (read only): "X keys <n>", a line per key, "X end"
	static void settings_layout(Reply& o, const Request& /*r*/) { synth_settings_keys_print(o.stream()); }

	// the board's state in one line (read only), format in synth_control.cpp
	static void state(Reply& o, const Request& /*r*/) { synth_state_print(o.stream()); }

	// the state's change counter: goes up when anything StcY or the lists show changed
	static void state_counter(Reply& o, const Request& /*r*/) {
		chprintf(o.stream(), "y %lu\r\n", static_cast<unsigned long>(synth_state_counter()));
	}

	// the synth engine's profile per section since the last read
	static void profile(Reply& o, const Request& /*r*/) { synth_profile_print(o.stream()); }

	// the current voice's live parameters (target 0), the master effects' (target 1)
	static void voice_params(Reply& o, const Request& /*r*/) { synth_params_print(o.stream(), 0); }
	static void fx_params(Reply& o, const Request& /*r*/) { synth_params_print(o.stream(), 1); }

	// the global touch parameters (target 2), in synth_params_print's line format: "index name
	// group min max value kind", floats as hex bits
	static void touch_params(Reply& o, const Request& /*r*/) {
		BaseSequentialStream* out = o.stream();
		chprintf(out, "params 2 touch %d\r\n", synth_touch_count());
		for (int i = 0; i < synth_touch_count(); i++) {
			float lo, hi;
			const char* name = synth_touch_info(i, &lo, &hi);
			chprintf(out, "%d %s touch %08lx %08lx %08lx 0\r\n", i, name, float_bits(lo), float_bits(hi),
			    float_bits(synth_touch_get(i)));
		}
	}

	// the per-key sensitivity factors (target 3)
	static void key_params(Reply& o, const Request& /*r*/) {
		BaseSequentialStream* out = o.stream();
		chprintf(out, "params 3 keys %d\r\n", buttonKeyCount());
		for (int n = 0; n < buttonKeyCount(); n++) {
			char name[4];
			put_button_name(n, name);
			name[3] = 0;
			chprintf(out, "%d %s keys %08lx %08lx %08lx 0\r\n", n, name, float_bits(0.1f), float_bits(10.0f),
			    float_bits(buttonGetKeyFactor(n)));
		}
	}

	// set a parameter: target, index, float32 little endian; a reply only if it was refused
	static void set_param(Reply& o, const Request& r) {
		const std::span<const uint8_t> p = r.payload;
		const uint32_t u = static_cast<uint32_t>(p[2]) | (static_cast<uint32_t>(p[3]) << 8) |
		    (static_cast<uint32_t>(p[4]) << 16) | (static_cast<uint32_t>(p[5]) << 24);
		float v;
		std::memcpy(&v, &u, sizeof v);
		int ok = 0;
		if (p[0] == 2) {
			ok = synth_touch_set(p[1], v);
		} else if (p[0] == 3) {
			ok = buttonSetKeyFactor(p[1], v);
		} else {
			ok = synth_param_set(p[0], p[1], v);
		}
		if (!ok) chprintf(o.stream(), "Q err %d %d\r\n", p[0], p[1]);
	}

	// the voices, '*' marks the current one
	static void voices(Reply& o, const Request& /*r*/) {
		for (int n = 0; n < synth_num_voices(); n++) {
			const char* preset;
			const char* name = synth_voice_name(n, &preset);
			chprintf(o.stream(), "%c %d %s%s%s\r\n", n == synth_current_voice() ? '*' : ' ', n, name,
			    preset ? ":" : "", preset ? preset : "");
		}
	}

	// select voice code - 0x80
	static void select_voice(Reply& o, const Request& r) {
		const int n = r.code - 0x80;
		if (synth_select_voice(n)) {
			const char* preset;
			const char* name = synth_voice_name(n, &preset);
			chprintf(o.stream(), "voice %d %s%s%s\r\n", n, name, preset ? ":" : "", preset ? preset : "");
		} else {
			chprintf(o.stream(), "no voice %d\r\n", n);
		}
	}

	// the synth DSP's load since the last read, in CPU cycles per block
	static void load(Reply& o, const Request& /*r*/) {
		chSysLock();
		const uint64_t sum = synth_load.sum;
		const uint32_t peak = synth_load.peak;
		const uint32_t blocks = synth_load.blocks;
		synth_load.sum = 0;
		synth_load.peak = 0;
		synth_load.blocks = 0;
		chSysUnlock();
		chprintf(o.stream(), "synth blocks %lu avg %lu peak %lu budget %lu cycles\r\n", blocks,
		    blocks ? static_cast<uint32_t>(sum / blocks) : 0, peak, SYNTH_BLOCK_CYCLES);
	}

	// USB audio status and statistics since the last U
	static void audio_status(Reply& o, const Request& /*r*/) { usb_audio_print_status(o.stream()); }

	// the USB audio ring target: 3 ASCII digits, frames (49..256; 000: just report it), e.g.
	// Stct072
	static void audio_target(Reply& o, const Request& r) {
		int n = 0;
		int ok = 1;
		for (int i = 0; i < 3; i++) {
			if (r.payload[i] < '0' || r.payload[i] > '9') ok = 0;
			n = n * 10 + (r.payload[i] - '0');
		}
		if (ok && n > 0) usb_audio_set_target(n);
		chprintf(o.stream(), "usb audio target %d frames (%d us)%s\r\n", usb_audio_target(),
		    usb_audio_target() * 1000 / 48, ok ? "" : ": not 3 digits, unchanged");
	}

	// USB audio latency. '1': a marker in the next packet (usb_audio.h; the reply comes once it
	// was sent, PExReceive), else an echo ("usb audio echo: held <us> us") for the console's
	// round trip
	static void audio_latency(Reply& o, const Request& r) {
		if (r.payload[0] == '1') {
			if (!usb_audio_marker_request()) chprintf(o.stream(), "usb audio marker: busy\r\n");
		} else {
			usb_audio_echo(o.stream());
		}
	}

	// USB audio source: the test tone on or off (the jack keeps the synth)
	static void audio_tone(Reply& o, const Request& /*r*/) {
		usb_audio_set_tone(!usb_audio_tone());
		chprintf(o.stream(), "usb audio source: %s\r\n",
		    usb_audio_tone() ? "test tone (L 1 kHz, R 500 Hz, -6 dBFS)" : "synth");
	}

	// every key's calibration back to the default (prints the old values first)
	static void calibration_reset(Reply& o, const Request& /*r*/) { buttonSetCalibration(o.stream()); }

	// the key scanner's counters since boot (read only)
	static void scanner_stats(Reply& o, const Request& /*r*/) { buttonPrintStats(o.stream()); }

	// every button's zero_offset, c_force and factor (read only)
	static void scanner_keys(Reply& o, const Request& /*r*/) { buttonPrintKeys(o.stream()); }

	// a raw key scan recording: 2 ASCII digits, the seconds to record (StcR05), or 00: send the
	// last recording (tools/record_raw.py; key_scanner.h)
	static void scanner_record(Reply& o, const Request& r) {
		const std::span<const uint8_t> p = r.payload;
		if (p[0] >= '0' && p[0] <= '9' && p[1] >= '0' && p[1] <= '9') {
			ks_capture(o.stream(), (p[0] - '0') * 10 + (p[1] - '0'));
		} else {
			chprintf(o.stream(), "R err not 2 digits\r\n");
		}
	}
};

Reply reply{&BDU1, kWriteTimeout};
console::Console<BoardCommands> board_console{reply, kStale};

} // namespace

extern "C" {

void InitPConnection(void) {
	// the descriptor strings
	init_usb_descriptor();

	mduObjectInit(&MDU1);
	mduStart(&MDU1, &midiusbcfg);
	bduObjectInit(&BDU1);
	bduStart(&BDU1, &bulkusbcfg);

	// the USB driver, then the bus pull-up on D+; the delay: no need to unplug the cable after a
	// reset
	usbDisconnectBus(midiusbcfg.usbp);
	chThdSleepMilliseconds(500);
	usbStart(midiusbcfg.usbp, &usbcfg);
	usbConnectBus(midiusbcfg.usbp);
}

void PExReceiveByte(unsigned char c) { (void)board_console.feed(c, chVTGetSystemTimeX()); }

void PExReceive(void) {
	// the console's bytes from the host
	uint8_t c;
	while (chnReadTimeout(&BDU1, &c, 1, TIME_IMMEDIATE)) {
		PExReceiveByte(c);
	}
	// the reply to a latency marker, once it went out
	reply.begin();
	usb_audio_marker_poll(reply.stream());

	// USB MIDI events from the host, a whole transfer at a time (midi_in_parser.hpp). The copy is
	// static: on main's stack (1 KB) it would add to every path below main (only main calls this).
	static uint8_t ev[MIDI_USB_BUFFERS_SIZE];
	for (;;) {
		size_t n;
		chSysLock();
		if (ibqGetFullBufferTimeoutS(&MDU1.ibqueue, TIME_IMMEDIATE) != MSG_OK) {
			chSysUnlock();
			break;
		}
		n = static_cast<size_t>(MDU1.ibqueue.top - MDU1.ibqueue.ptr);
		std::memcpy(ev, MDU1.ibqueue.ptr, n);
		ibqReleaseEmptyBufferS(&MDU1.ibqueue);
		chSysUnlock();
		midi_in::MidiInParser::transfer(std::span<const uint8_t>(ev, n), [](const midi_in::Event& e) {
			MidiInMsgHandler(MIDI_DEVICE_USB_DEVICE, e.port, e.status, e.data1, e.data2);
		});
	}
}

} // extern "C"
