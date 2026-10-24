// A simulated key board for the KeyScanner tests: synthetic scans as the ADC interrupt takes them
// (button_read.c). Keys are pressed with a force over time (ramps, taps, holds, chords, pairs in a
// column, corners of a rectangle); every button reads noise, pressed keys add cross talk to
// their column, and only the columns with a key above the detection threshold are measured, plus
// the stale one the interrupt measures when nothing is detected. The measured values of the
// columns not measured in a scan stay what they were, as in the interrupt's frame.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <random>
#include <vector>

#include "key_scanner.hpp"

namespace scansim {

// the force of a key over the scans: 0 = released
using Envelope = std::function<float(int frame)>;

struct Press {
	int button;
	Envelope force;
	float tilt_x = 0.0f; // corner sample spread
	float tilt_y = 0.0f;
};

// envelopes
inline Envelope ramp(int start, int rise, int hold, int fall, float peak) {
	return [=](int f) {
		const int t = f - start;
		if (t < 0) return 0.0f;
		if (t < rise) return peak * static_cast<float>(t + 1) / static_cast<float>(rise);
		if (t < rise + hold) return peak;
		if (t < rise + hold + fall) return peak * static_cast<float>(rise + hold + fall - t) / static_cast<float>(fall + 1);
		return 0.0f;
	};
}
inline Envelope tap(int start, float peak) { return ramp(start, 3, 30, 4, peak); }
inline Envelope hold(int start, int len, float level) { return ramp(start, 20, len, 20, level); }

class Board {
public:
	explicit Board(uint32_t seed) :
	    rng_(seed) {}

	std::vector<Press> presses;
	float noise = 3.0f;      // +- LSB per sample
	float crosstalk = 0.02f; // of the column's other forces

	// the next scan, into f (which keeps the unmeasured columns' old values)
	void scan(int frame, ScanFrame& f) {
		std::array<float, ks::kButtons> force{};
		std::array<float, ks::kButtons> tx{};
		std::array<float, ks::kButtons> ty{};
		for (const Press& p : presses) {
			const float v = p.force(frame);
			if (v > force[p.button]) {
				force[p.button] = v;
				tx[p.button] = p.tilt_x;
				ty[p.button] = p.tilt_y;
			}
		}
		for (int b = 0; b < ks::kButtons; b++) {
			float col = 0.0f;
			for (int n = 0; n < ks::kRows; n++) {
				const int b2 = b % ks::kColumns + n * ks::kColumns;
				if (b2 != b) col += force[b2];
			}
			force[b] += crosstalk * col;
		}
		f.seq = static_cast<uint32_t>(frame);
		f.t_start += 300000 + static_cast<uint32_t>(rng_() % 20000);
		std::array<bool, ks::kColumns> measured{};
		int nm = 0;
		for (int b = 0; b < ks::kButtons; b++) {
			f.on[b] = static_cast<uint16_t>(sample(force[b]));
			if (f.on[b] > ks::kKeyDetect && !measured[b % ks::kColumns]) {
				measured[b % ks::kColumns] = true;
			}
		}
		for (int c = 0; c < ks::kColumns; c++) {
			if (measured[c]) f.cols[nm++] = static_cast<uint8_t>(c);
		}
		if (nm) {
			stale_ = f.cols[0];
		} else {
			measured[stale_] = true; // the interrupt measures measure[0] again
			f.cols[nm++] = static_cast<uint8_t>(stale_);
		}
		f.n = static_cast<uint8_t>(nm);
		for (int b = 0; b < ks::kButtons; b++) {
			if (!measured[b % ks::kColumns]) continue;
			int p = 0;
			for (int i = 0; i < ks::kMultisample; i++) p += sample(force[b]);
			f.m[b].p = static_cast<uint16_t>(p);
			f.m[b].s0 = static_cast<uint16_t>(sample(force[b] * (1.0f - tx[b] + ty[b] * 0.5f)) * ks::kMultisample);
			f.m[b].s1 = static_cast<uint16_t>(sample(force[b] * (1.0f - ty[b])) * ks::kMultisample);
			f.m[b].s2 = static_cast<uint16_t>(sample(force[b] * (1.0f + tx[b] + ty[b] * 0.5f)) * ks::kMultisample);
		}
	}

	[[nodiscard]] bool measured(const ScanFrame& f, int col) const {
		for (int i = 0; i < f.n; i++) {
			if (f.cols[i] == col) return true;
		}
		return false;
	}

private:
	// one ADC sample, 4095 - adc: saturating in the force, below 4075 so the sums stay out of the
	// int32 overflow near full scale (the PC and the board differ there only in float -> int)
	int sample(float force) {
		const float clean = 4075.0f * (1.0f - std::exp(-3.0f * std::max(force, 0.0f)));
		const float n = (static_cast<float>(rng_() % 2001) / 1000.0f - 1.0f) * noise;
		return std::clamp(static_cast<int>(clean + n + noise), 0, 4075);
	}

	std::mt19937 rng_;
	int stale_ = 0;
};

} // namespace scansim
