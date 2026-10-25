/*
 * The key scan's sequence, without the hardware: what the ADC interrupt does at each of its
 * callbacks (ScanSequencer), and what the scanner thread does with each column it takes
 * (ScanConsumer). button_read.c had both; scan_thread.cpp runs them on the HAL, the host tests
 * against the old code (tests/scan_seq), so the pins, the samples, the hand-overs and the
 * counters come in the same order as before.
 *
 * A scan, one ADC callback per conversion of the four row inputs:
 *   detection    17 callbacks, a column's three output channels drained per callback: the keys
 *                above the detection threshold mark their column to be measured
 *   measurement  7 callbacks per marked column: the pressure 4 times (all three channels
 *                drained), then each corner (one channel drained); the columns before the next
 *                marked one are then complete (the thread is woken)
 *   delay        with fewer than 4 columns measured, callbacks until 4 columns' time has passed,
 *                so the scan period (and the velocity) stays the same; then the scan is complete
 * The hand-over itself (two frames, the scan numbers) is key_scanner_link.h; the key processing
 * the KeyScanner (key_scanner.hpp).
 *
 * Io of the sequencer:
 *   void open(int ch), drain(int ch)   an output channel (0..50) high / low
 *   void all_opendrain(), all_pushpull()   every output channel's mode (measurement / detection)
 *   ScanFrame* begin(uint32_t seq)      the frame of a new scan (scan_link_begin)
 *   void publish(uint32_t ready)        its complete leading columns, no wakeup
 *   void wake(uint32_t ready)           the same, and wake the thread
 *   uint32_t cycles()                   the cycle counter
 * Io of the consumer:
 *   void boot_column(int col), column(int col), lost()   the KeyScanner (ks_*)
 *   void frame()                        a scan finished (scan_stats.frames)
 *   bool aux_line(int n)                an aux button's input
 *   void aux_send(int id, bool on)      its message
 */
#pragma once

#include <array>
#include <cstdint>

#include "key_scanner.h"

namespace ks {

inline constexpr int kOutChannels = 51;      // 3 per column
inline constexpr int kDetectAdc = 4095 - 64; // a row sample below: a key in the column (KEY_DETECT)
inline constexpr int kMinMeasures = 4;       // columns of time a scan takes at least
inline constexpr int kMultisample = 4;       // the pressure's samples per measurement
inline constexpr int kDelayStart = 20;       // cur_conversion of the delay phase's start
inline constexpr int kMeasure = SCAN_COLUMNS;

class ScanSequencer {
public:
	// button_read.c's interrupt state, as it was
	int cur_channel = 0;
	int cur_conversion = 0;        // 0..16 detection, 17 measurement, 20.. delay
	int cur_phase = 0;             // measurement: 0..3 pressure, 100..102 the corners
	int next_note_id = 0;          // the column being measured
	std::array<int, 20> measure{}; // the marked columns
	int put = 0, get = 0;
	ScanFrame* cur = nullptr; // the frame of the scan being taken
	uint32_t t0 = 0;          // the scan's start (cycles)
	uint32_t callbacks = 0;   // in this scan
	uint32_t seq = 0;         // the scan's number

	// ButtonReadStart: the first scan's frame, before the first conversion
	template <class Io>
	void start(Io& io) {
		cur = io.begin(0);
	}

	// one ADC callback with the four row samples
	template <class Io>
	void callback(Io& io, const uint16_t* adc, volatile scan_stats_t& stats) {
		callbacks++;
		if (cur_conversion < SCAN_COLUMNS) {
			detect(io, adc, stats);
		} else if (cur_conversion == kMeasure) {
			measure_step(io, adc);
		} else {
			// delay phase: with fewer than kMinMeasures columns measured, wait so that the scan
			// period and the velocity sensitivity stay the same (more columns slow it down)
			cur_conversion++;
			if (cur_conversion >= kDelayStart + (kMultisample + 3) * kMinMeasures) {
				cur_conversion = 0;
				next_note_id = 0;
				io.wake(SCAN_COLUMNS); // the scan is complete
			}
		}
	}

private:
	template <class Io>
	void scan_start(Io& io, volatile scan_stats_t& stats) {
		const uint32_t now = io.cycles();
		if (stats.scans) {
			const uint32_t dt = now - t0;
			if (dt < stats.cycles_min || stats.cycles_n == 0) stats.cycles_min = dt;
			if (dt > stats.cycles_max) stats.cycles_max = dt;
			stats.cycles_sum = stats.cycles_sum + dt;
			stats.cycles_n = stats.cycles_n + 1;
			const int bin = dt < SCAN_HIST_BASE ? 0 : static_cast<int>((dt - SCAN_HIST_BASE) / SCAN_HIST_STEP);
			const int hb = bin < SCAN_HIST_BINS ? bin : SCAN_HIST_BINS - 1;
			stats.cycles_hist[hb] = stats.cycles_hist[hb] + 1;
			const int cb = static_cast<int>(callbacks) - SCAN_CB_BASE;
			const int cbb = cb < 0 ? 0 : cb < SCAN_CB_BINS ? cb :
			                                                 SCAN_CB_BINS - 1;
			stats.callbacks_hist[cbb] = stats.callbacks_hist[cbb] + 1;
		}
		t0 = now;
		callbacks = 0;
		seq = stats.scans;
		stats.scans = stats.scans + 1;
		cur = io.begin(seq);
		cur->seq = seq;
		cur->t_start = now;
	}

	template <class Io>
	void drain3(Io& io) {
		io.drain(cur_channel + 0);
		io.drain(cur_channel + 1);
		io.drain(cur_channel + 2);
	}

	// key detection: column cur_conversion
	template <class Io>
	void detect(Io& io, const uint16_t* adc, volatile scan_stats_t& stats) {
		if (cur_conversion == 0) scan_start(io, stats);
		const int next_conversion = cur_conversion + 1;
		io.open(cur_channel + 0); // the old channels
		io.open(cur_channel + 1);
		io.open(cur_channel + 2);
		if (next_conversion == SCAN_COLUMNS) {
			// on to the measurement
			io.all_opendrain();
			get = 0;
			next_note_id = measure[0];
			cur_channel = next_note_id * 3;
			// the measured columns (one, stale, when no key was detected)
			const int nm = put;
			cur->n = static_cast<uint8_t>(nm ? nm : 1);
			for (int n = 0; n < cur->n; n++) cur->cols[n] = static_cast<uint8_t>(measure[n]);
			// the columns before the first measured one are complete (no wakeup: as before)
			io.publish(static_cast<uint32_t>(next_note_id));
		} else {
			cur_channel = next_conversion * 3;
		}
		drain3(io); // the new channels

		cur->on[cur_conversion] = static_cast<uint16_t>(4095 - adc[0]);
		cur->on[cur_conversion + 17] = static_cast<uint16_t>(4095 - adc[1]);
		cur->on[cur_conversion + 34] = static_cast<uint16_t>(4095 - adc[2]);
		cur->on[cur_conversion + 51] = static_cast<uint16_t>(4095 - adc[3]);
		if (adc[0] < kDetectAdc || adc[1] < kDetectAdc || adc[2] < kDetectAdc || adc[3] < kDetectAdc) {
			measure[put++] = cur_conversion;
		}
		cur_conversion = next_conversion;
	}

	// key measurement: column next_note_id (the four rows written out: the interrupt's time sets
	// the scan period, and the firmware's -O1 doesn't unroll loops)
	template <class Io>
	void measure_step(Io& io, const uint16_t* adc) {
		scan_meas_t* m = cur->m;
		const int c = next_note_id;
		switch (cur_phase) {
		case 0: // the whole key, 4x multisampled
			m[c].p = static_cast<uint16_t>(4095 - adc[0]);
			m[c + 17].p = static_cast<uint16_t>(4095 - adc[1]);
			m[c + 34].p = static_cast<uint16_t>(4095 - adc[2]);
			m[c + 51].p = static_cast<uint16_t>(4095 - adc[3]);
			cur_phase++;
			break;
		case 1:
		case 2:
			m[c].p = static_cast<uint16_t>(m[c].p + (4095 - adc[0]));
			m[c + 17].p = static_cast<uint16_t>(m[c + 17].p + (4095 - adc[1]));
			m[c + 34].p = static_cast<uint16_t>(m[c + 34].p + (4095 - adc[2]));
			m[c + 51].p = static_cast<uint16_t>(m[c + 51].p + (4095 - adc[3]));
			cur_phase++;
			break;
		case 3:
			io.open(cur_channel + 1);
			io.open(cur_channel + 2);
			m[c].p = static_cast<uint16_t>(m[c].p + 4095 - adc[0]);
			m[c + 17].p = static_cast<uint16_t>(m[c + 17].p + 4095 - adc[1]);
			m[c + 34].p = static_cast<uint16_t>(m[c + 34].p + 4095 - adc[2]);
			m[c + 51].p = static_cast<uint16_t>(m[c + 51].p + 4095 - adc[3]);
			cur_phase = 100;
			break;
		case 100: // s0
			io.open(cur_channel + 0);
			io.drain(cur_channel + 1);
			m[c].s0 = static_cast<uint16_t>((4095 - adc[0]) * kMultisample);
			m[c + 17].s0 = static_cast<uint16_t>((4095 - adc[1]) * kMultisample);
			m[c + 34].s0 = static_cast<uint16_t>((4095 - adc[2]) * kMultisample);
			m[c + 51].s0 = static_cast<uint16_t>((4095 - adc[3]) * kMultisample);
			cur_phase = 101;
			break;
		case 101: // s1
			io.open(cur_channel + 1);
			io.drain(cur_channel + 2);
			m[c].s1 = static_cast<uint16_t>((4095 - adc[0]) * kMultisample);
			m[c + 17].s1 = static_cast<uint16_t>((4095 - adc[1]) * kMultisample);
			m[c + 34].s1 = static_cast<uint16_t>((4095 - adc[2]) * kMultisample);
			m[c + 51].s1 = static_cast<uint16_t>((4095 - adc[3]) * kMultisample);
			cur_phase = 102;
			break;
		case 102: // s2, then the next column, or the end of the scan
			io.open(cur_channel + 2);
			m[c].s2 = static_cast<uint16_t>((4095 - adc[0]) * kMultisample);
			m[c + 17].s2 = static_cast<uint16_t>((4095 - adc[1]) * kMultisample);
			m[c + 34].s2 = static_cast<uint16_t>((4095 - adc[2]) * kMultisample);
			m[c + 51].s2 = static_cast<uint16_t>((4095 - adc[3]) * kMultisample);
			get++;
			if (get < put) {
				next_note_id = measure[get];
				cur_channel = next_note_id * 3;
			} else {
				if (put < kMinMeasures) {
					cur_conversion = kDelayStart + 4 * put; // on to the delay phase
					next_note_id = 16;                      // process all but last notes to keep from hanging at note_id 0
				} else {
					cur_conversion = 0; // on to the next scan's detection
					next_note_id = 0;
				}
				put = 0;
				cur_channel = 0;
				io.all_pushpull();
			}
			drain3(io); // the new channels
			cur_phase = 0;
			// the columns before next_note_id are complete (0: all of them)
			io.wake(next_note_id ? static_cast<uint32_t>(next_note_id) : static_cast<uint32_t>(SCAN_COLUMNS));
			break;
		default: break;
		}
	}
};

// the scanner thread's work per column it takes
template <int BootFrames, int AuxDebounce>
class ScanConsumer {
public:
	int boot_frames = 0;                 // scans of the zero level pass done
	std::array<uint32_t, 4> aux_state{}; // per aux button: its state and debounce count

	// the aux buttons: their input when on (port, up, down, alt), their messages' ids
	static constexpr std::array<bool, 4> kAuxOn = {false, true, false, false};

	// note_id: the column taken, < 0 if the interrupt overwrote it first (key_scanner_link.h)
	template <class Io>
	void column(Io& io, int note_id) {
		if (note_id < 0) { // on with the scan the interrupt is writing
			io.lost();
			return;
		}
		if (boot_frames < BootFrames) {
			// the zero level pass: keys touched at boot get a zero_offset of their pressure
			io.boot_column(note_id);
			if (note_id == 16) {
				boot_frames++;
				io.frame();
			}
			return;
		}
		// the keys of this column, then the cross talk correction they give the other columns
		io.column(note_id);
		if (note_id == 16) { // once per scan, after the last column
			io.frame();
			if constexpr (AuxDebounce > 0) {
				for (int n = 0; n < 4; n++) {
					if (aux_state[n] & 0xff) {
						aux_state[n]--;
					} else if (io.aux_line(n) == !aux_state[n]) {
						bool on;
						if (aux_state[n]) {
							on = !kAuxOn[n];
							aux_state[n] = AuxDebounce;
						} else {
							on = kAuxOn[n];
							aux_state[n] = AuxDebounce | 0x100;
						}
						io.aux_send(n, on);
					}
				}
			}
		}
	}
};

} // namespace ks
