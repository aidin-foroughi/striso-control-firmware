/**
 * Copyright (C) 2019 Piers Titus van der Torren
 *
 * This file is part of Striso Control.
 *
 * Striso Control is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * Striso Control is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
 * A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * Striso Control. If not, see <http://www.gnu.org/licenses/>.
 */
/*
 * KeyScanner: the key processing of button_read.c (update_button and the cross talk correction of
 * ThreadReadButtons, by Piers), moved out of the thread so the same code runs on the board and on
 * the PC. It turns ScanFrames (the ADC interrupt's samples of one scan) into key messages:
 * {ID_DIS, button, pres, velo, x, y}, 14-bit scaled as the board sends them.
 *
 * It reproduces the C code bit for bit, its quirks included (they are fixed one at a time, later,
 * against recorded scans): the same integer and float operations in the same order. Where the C
 * code overflowed (int32 products near full pressure) or converted out-of-range floats to int,
 * the board's behaviour is spelled out (wrapping arithmetic, the saturating vcvt), so the PC gets
 * the same numbers and the sanitizers stay quiet. Only the configuration the firmware builds is
 * here: DETECT_STUCK_NOTES on; CALIBRATION_MODE, DETECT_STUCK_NOTES_DECREASE, BREAKPOINT_CALIBRATION
 * and USE_BAS off (config.h).
 *
 * Header only, no ChibiOS: the firmware glue is key_scanner.cpp, the PC tests are
 * tests/test_key_scanner.cpp and tests/scan_replay.cpp.
 */
#pragma once

#include <array>
#include <cstdint>

#include "key_scanner.h"

namespace ks {

inline constexpr int kColumns = SCAN_COLUMNS;
inline constexpr int kRows = SCAN_ROWS;
inline constexpr int kButtons = SCAN_BUTTONS;

inline constexpr int32_t kInternalOne = 1 << 24;
inline constexpr int kAdcBits = 12;
inline constexpr int32_t kAdcFact = kInternalOne >> kAdcBits; // adc sample -> kInternalOne
inline constexpr int32_t kVeloFact = 32;                      // extra factor for velocity
inline constexpr int32_t kMsgFact = 1 << 11;                  // 14 bit message -> kInternalOne
inline constexpr int32_t kMsgFactVelo = kMsgFact / kVeloFact;
inline constexpr int32_t kFilt = 8;
inline constexpr int32_t kFiltV = 8;
inline constexpr int32_t kZeroLevelFactNum = 300; // zero level safety factor 300 / 256
inline constexpr int32_t kZeroLevelFactDen = 256;
inline constexpr int32_t kZeroLevelTime = 500;
inline constexpr int32_t kZeroLevelMaxPres = kInternalOne / 32;
inline constexpr int32_t kZeroLevelMaxVelo = 500;
inline constexpr int32_t kKeyDetect = 64;                              // key detect threshold
inline constexpr int32_t kKeyDetect2 = 256 - kKeyDetect;               // another key in the column
inline constexpr int32_t kKeyDetect3 = 320 - kKeyDetect2 - kKeyDetect; // 3 or 4 corners pressed
inline constexpr int kMultisample = 4;                                 // pressure samples per measurement
inline constexpr int32_t kAdcMax = 4095;
inline constexpr int32_t kSampleMax = kMultisample * kAdcMax; // largest p and s values
inline constexpr int32_t kIntegratedPresThreshold = kInternalOne / 8;
inline constexpr float kCenterTend = 0.02f;
inline constexpr int kBootFrames = 100;                    // scans of the zero level pass at boot
inline constexpr int kSrcId = 0;                           // ID_DIS (striso.h)
inline constexpr int32_t kCalibForce = (1 << 18) / 64 + 1; // CALIB_FORCE (config.h)
// positions without a key (their channels read noise): c_force 0
inline constexpr std::array<int, 7> kNoKey = {52, 54, 57, 59, 61, 64, 66};

// --- the board's integer and float semantics ---------------------------------------------------
// int32 arithmetic that wraps, as the C code's products do on the board near full pressure
[[nodiscard]] constexpr int32_t wadd(int32_t a, int32_t b) { return static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b)); }
[[nodiscard]] constexpr int32_t wsub(int32_t a, int32_t b) { return static_cast<int32_t>(static_cast<uint32_t>(a) - static_cast<uint32_t>(b)); }
[[nodiscard]] constexpr int32_t wmul(int32_t a, int32_t b) { return static_cast<int32_t>(static_cast<uint32_t>(a) * static_cast<uint32_t>(b)); }
// float -> int32 as the board's vcvt.s32.f32: toward zero, saturating, NaN -> 0
[[nodiscard]] constexpr int32_t f2i(float f) {
	if (!(f == f)) return 0; // NOLINT(misc-redundant-expression): NaN
	if (f >= 2147483648.0f) return INT32_MAX;
	if (f <= -2147483648.0f) return INT32_MIN;
	return static_cast<int32_t>(f);
}
template <typename T>
[[nodiscard]] constexpr T cmax(T x, T y) { return x > y ? x : y; } // button_read.c's max()
template <typename T>
[[nodiscard]] constexpr T cmin(T x, T y) { return x < y ? x : y; } // button_read.c's min()

enum class Status : uint8_t { off = 0,
	starting = 1,
	on = 2 };

// one key position (button = column + 17 * row)
struct KeyState {
	// inputs of the column being processed (load_column)
	int32_t on = 0; // detection sample, 0 when not above kKeyDetect
	int32_t p = 0;  // pressure, sum of kMultisample samples
	int32_t s0 = 0; // the three corner samples (x kMultisample)
	int32_t s1 = 0;
	int32_t s2 = 0;
	// state
	int32_t pres = 0; // filtered force, kInternalOne = full
	int32_t velo = 0; // its velocity
	int32_t c_force = kCalibForce;
	int32_t c_breakpoint = INT32_MAX; // only with BREAKPOINT_CALIBRATION
	int32_t c_force2 = 0;             // only with BREAKPOINT_CALIBRATION
	int32_t zero_offset = 0;          // learned zero level
	int32_t zero_time = 0;
	int32_t zero_max = 0;
	int32_t key_detect3 = 0; // extra threshold from the cross talk correction, | 1: corner signal
	float fact = 1.0f;       // cross talk correction factor for the next update
	Status status = Status::off;
	int32_t timer = 0; // integration timer, then scans until the next message
};

struct Config {
	int32_t zero_offset = 0;      // config.zero_offset
	int32_t message_interval = 1; // config.message_interval (SENDFACT)
};

// The key processing state. process(frame, sink) per scan; Sink is any type with
//   int send(const int (&msg)[6])  0: queued (msgSend), else refused
//   void wait()                    a tick's sleep before a note-off is retried
class KeyScanner {
public:
	std::array<KeyState, kButtons> keys{};
	std::array<int32_t, kColumns> col_pressed{}; // keys not off per column
	int32_t buttons_pressed = 0;                 // keys on
	Config cfg{};

	// ButtonReadStart's initial state: every position kCalibForce, the ones without a key 0
	void init() {
		*this = KeyScanner{};
		for (int b : kNoKey) keys[b].c_force = 0;
	}

	// the inputs of column col from frame f, as update_button reads them. A column in which no
	// key is above kKeyDetect is all zeros, and so is a key at or below it, except its pressure p
	// (the four corner correction may read it): update_button doesn't read the rest of them, so
	// the recordings leave them out (key_scanner_capture.hpp), and the board does the same.
	void load_column(const ScanFrame& f, int col) {
		bool any = false;
		for (int n = 0; n < kRows; n++) any = any || f.on[col + n * kColumns] > kKeyDetect;
		for (int n = 0; n < kRows; n++) {
			const int b = col + n * kColumns;
			KeyState& k = keys[b];
			const bool up = f.on[b] > kKeyDetect;
			k.on = up ? f.on[b] : 0;
			k.p = any ? f.m[b].p : 0;
			k.s0 = up ? f.m[b].s0 : 0;
			k.s1 = up ? f.m[b].s1 : 0;
			k.s2 = up ? f.m[b].s2 : 0;
		}
	}

	// The thread's work for column col (ThreadReadButtons): every key of the column, then the
	// cross talk correction factors it gives the other columns.
	template <class Sink>
	void process_column(const ScanFrame& f, int col, Sink& sink) {
		load_column(f, col);
		for (int n = 0; n < kRows; n++) update_button(col + n * kColumns, sink);
		cross_talk(col);
	}

	template <class Sink>
	void process(const ScanFrame& f, Sink& sink) {
		for (int col = 0; col < kColumns; col++) process_column(f, col, sink);
	}

	// The zero level pass of the first kBootFrames scans (DETECT_STUCK_NOTES): keys touched at
	// boot get a zero_offset of their pressure.
	void boot_column(const ScanFrame& f, int col) {
		load_column(f, col);
		for (int n = 0; n < kRows; n++) {
			KeyState& but = keys[col + n * kColumns];
			if (but.on > kKeyDetect) {
				int32_t s_new = calibrate(linearize(but.p), but);
				update_and_filter(but.pres, but.velo, s_new);
				s_new = but.pres * kZeroLevelFactNum / kZeroLevelFactDen;
				if (s_new > but.zero_offset) but.zero_offset = s_new;
			}
		}
	}

	// Second order Kalman like filter with fast signal end conditions
	static void update_and_filter(int32_t& s, int32_t& v, int32_t s_new) {
		const int32_t old_s = s;
		s = wadd(wmul(kFilt - 1, wadd(old_s, v)), s_new) / kFilt;
		if (s >= kInternalOne) {
			s = kInternalOne - 1;
			v = 0;
		} else {
			v = wadd(wmul(kFiltV - 1, v), wsub(s, old_s)) / kFiltV;
			if (v >= (kInternalOne / kVeloFact)) {
				v = (kInternalOne / kVeloFact) - 1;
			} else if (v <= -(kInternalOne / kVeloFact)) {
				v = -(kInternalOne / kVeloFact) + 1;
			}
			if (s < 0) s = 0;
		}
	}

	// adc sum -> force; s <= kSampleMax
	[[nodiscard]] static int32_t linearize(int32_t s) { return (kAdcFact >> 6) * s / ((kMultisample * kAdcMax) - s + 1); }

	[[nodiscard]] static int32_t calibrate(int32_t s, const KeyState& but) {
		return f2i(static_cast<float>(wmul(but.c_force, s)) * but.fact);
	}

private:
	template <class Sink>
	void send_off(int b, int32_t velo, Sink& sink) {
		const int msg[6] = {kSrcId, b, 0, cmin<int32_t>(velo, 0) / kMsgFactVelo, 0, 0};
		while (sink.send(msg)) { // note off messages are more important so keep trying
			sink.wait();
		}
	}

	template <class Sink>
	void update_button(int b, Sink& sink) {
		KeyState& but = keys[b];
		const int col = b % kColumns;
		const int32_t key_detect2 = kKeyDetect2 * ((col_pressed[col] - (but.status != Status::off)) >= 1);

		if (but.on > kKeyDetect + key_detect2 + but.key_detect3) {
			int32_t s_new = calibrate(linearize(but.p), but);
			// four corner correction algorithm
			if (but.key_detect3) {
				for (int b2 = col; b2 < kButtons; b2 += kColumns) {
					const KeyState& but2 = keys[b2];
					if (b2 != b && but2.key_detect3 && but2.status != Status::off) {
						const int32_t s_new2 = calibrate(linearize(but2.p), but2);
						if (s_new2 > s_new) {
							s_new = wsub(s_new, wsub(s_new2, s_new) / 2);
						} else {
							s_new = wsub(s_new, wsub(s_new2, s_new));
						}
						break;
					}
				}
			}
			update_and_filter(but.pres, but.velo, s_new);

			// adjust zero pressure level dynamically (DETECT_STUCK_NOTES)
			if (but.pres < kZeroLevelMaxPres && but.velo < kZeroLevelMaxVelo && but.velo > -kZeroLevelMaxVelo) {
				if (but.pres > but.zero_max) but.zero_max = but.pres;
				but.zero_time++;
				if (but.zero_time > kZeroLevelTime) {
					but.zero_time = 0;
					but.zero_offset = cmax<int32_t>(but.zero_offset, but.zero_max * kZeroLevelFactNum / kZeroLevelFactDen);
				}
			} else {
				but.zero_time = 0;
				but.zero_max = 0;
			}

			// if button is off start integration timer
			if (but.status == Status::off) {
				but.status = Status::starting;
				but.timer = kIntegratedPresThreshold;
				col_pressed[col]++;
			}
			// if button is in start integration reduce timer
			if (but.status == Status::starting && but.pres > (cfg.zero_offset + but.zero_offset + kMsgFact)) {
				but.timer -= (but.pres - but.zero_offset - kMsgFact);
			}
			// note off if .pres is too low even though .on is high enough
			else if (but.status == Status::on && but.pres < (cfg.zero_offset / 2 + but.zero_offset + kMsgFact)) {
				but.status = Status::starting;
				buttons_pressed--;
				but.timer = kIntegratedPresThreshold;
				send_off(b, but.velo, sink);
			}
			// if integration is successful and interval is ready send note message
			if (--but.timer <= 0) {
				if (but.status != Status::on) {
					but.status = Status::on;
					buttons_pressed++;
				}

				// calculate values from signals
				int32_t but_x = 0;
				int32_t but_y = 0;
				const int32_t s0 = calibrate(linearize(but.s0), but);
				const int32_t s1 = calibrate(linearize(but.s1), but);
				const int32_t s2 = calibrate(linearize(but.s2), but);
				int32_t m = s0;
				if (s1 > m) m = s1;
				if (s2 > m) m = s2;
				if (m > 0) {
					const float mf = (static_cast<float>(m)) / kInternalOne;
					const float fact = 1.0f / (mf + kCenterTend / mf - kCenterTend);
					but_x = f2i(static_cast<float>(wsub(s2, s0)) * fact);
					but_y = f2i(static_cast<float>(wsub(wadd(s0, s2) / 2, s1)) * fact);
				}
				const int msg[6] = {kSrcId, b, but.pres / kMsgFact, but.velo / kMsgFactVelo, but_x / kMsgFact, but_y / kMsgFact};
				(void)sink.send(msg);
				but.timer = buttons_pressed * cfg.message_interval;
			}
		} else if (but.status != Status::off) {
			if (but.status == Status::on) {
				send_off(b, but.velo, sink);
				buttons_pressed--;
			}
			but.status = Status::off;
			but.p = 0;
			col_pressed[col]--;
			// reset filter
			but.pres = 0;
			but.velo = 0;
			but.zero_time = 0;
		} else {
			but.p = 0;
		}
		but.fact = 1.0f;
		but.key_detect3 = 0;
	}

	/* calculate cross talk correction factors
	   correction factor for 1k adc pull up resistors:
	   1: 600/570 = 1.05
	   2: 600/320 = 1.9
	   3: 600/230 = 2.6
	   4: 600/180 = 3.3
	*/
	void cross_talk(int note_id) {
		float oct_fact[kRows] = {1.0f}; // (only [0] is 1: the others start at 0)
		for (int n = 0; n < kRows; n++) {
			float fact = 1.0f + keys[note_id + n * kColumns].p * (0.05f / 0.9f / (kMultisample * 4095.0f));
			oct_fact[n] = cmax(oct_fact[n], fact);
			for (int k = n + 1; k < kRows; k++) {
				fact = 1.0f + (cmin<int32_t>(keys[note_id + n * kColumns].p, keys[note_id + k * kColumns].p) - (kKeyDetect + kKeyDetect2)) * (0.9f / 0.95f / (kMultisample * 4095.0f));
				oct_fact[n] = cmax(oct_fact[n], fact);
				oct_fact[k] = cmax(oct_fact[k], fact);
			}
		}
		bool set_oct[kRows];
		for (int b = 0; b < kColumns; b++) {
			if (b == note_id) continue;
			for (bool& s : set_oct) s = true;
			for (int n = 0; n < kRows; n++) {
				for (int k = n + 1; k < kRows; k++) {
					KeyState& bn = keys[b + n * kColumns];
					KeyState& bk = keys[b + k * kColumns];
					if (keys[note_id + n * kColumns].status != Status::off && keys[note_id + k * kColumns].status != Status::off &&
					    (bk.status != Status::off || bn.status != Status::off)) {
						if (bn.status != Status::off && bk.status != Status::off) {
							// four corners pressed, do not set correction and reduce sensitivity
							set_oct[n] = false;
							set_oct[k] = false;
							bn.key_detect3 = kKeyDetect3 | 1;
							bk.key_detect3 = kKeyDetect3 | 1;
						} else {
							// three corners pressed, only reduce sensitivity for not pressed corner.
							// | 1 as signal to the four corner correction algorithm
							bn.key_detect3 = (kKeyDetect3 * (bk.status != Status::off)) | 1;
							bk.key_detect3 = (kKeyDetect3 * (bn.status != Status::off)) | 1;
						}
					}
				}
				// set correction if larger than current correction
				KeyState& bn = keys[b + n * kColumns];
				if (set_oct[n] && oct_fact[n] > bn.fact) bn.fact = oct_fact[n];
			}
		}
	}
};

} // namespace ks
