// The MotionFilter (src/drivers/motion_filter.hpp) against the old motion math (tests/ref/motion_ref.c)
// over the same readings and settings: the message (its source and values) and the synth's seven
// controls, bit for bit, per reading. Shared by the host test (test_motion_filter.cpp) and the
// board-CPU test on QEMU (target_motion.cpp, the firmware's own C and C++ flags), so both compare
// the same readings:
//   edges    every combination of 8 values per component (int16 limits, 0, +-1, +-1 g): 262144
//   still    the acceleration around 0 (the 0.001 g threshold of the direction): 41^3
//   random   readings near 1 g in every direction with noise, and uniform ones
// with the motion interval (0, a MIDI interval, 127: the internal synth only) and the stream
// changing as they go (the stream-only message every 4th reading).
#pragma once

#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>

#include "motion_filter.hpp"
#include "motion_ref.h"

namespace motion_compare {

struct Sent {
	int n = 0; // messages sent this reading
	int src = 0, id = 0, count = 0;
	std::array<int, 7> v{};
};
inline Sent sent;

struct Report {
	long readings = 0, differences = 0;
	long accel = 0, stream = 0, controls = 0; // what the readings did (coverage)
	char first[200] = "";
};

class Compare {
public:
	Report r;

	Compare() { ref_motion_reset(); }

	void reading(const std::array<int16_t, 6>& raw, int interval, bool bulk) {
		// the old code
		ref_motion_config.send_motion_interval = interval;
		ref_motion_config.send_usb_bulk = bulk;
		std::array<float, 7> old_c{};
		old_c.fill(std::bit_cast<float>(kUnset));
		ref_motion_synth = {&old_c[0], &old_c[1], &old_c[2], &old_c[3], &old_c[4], &old_c[5], &old_c[6]};
		sent = Sent{};
		ref_motion_reading(raw.data());
		// the new
		const motion::Step s = filter.step(raw, interval, bulk);
		std::array<float, 7> new_c{};
		new_c.fill(std::bit_cast<float>(kUnset));
		if (s.controls) new_c = {s.c.acc_abs, s.c.acc_x, s.c.acc_y, s.c.acc_z, s.c.rot_x, s.c.rot_y, s.c.rot_z};

		r.readings++;
		r.accel += s.send == motion::Send::accel;
		r.stream += s.send == motion::Send::stream;
		r.controls += s.controls;
		const int src = s.send == motion::Send::accel ? 3 : s.send == motion::Send::stream ? 0x13 :
		                                                                                     -1;
		bool same = (sent.n == 0) == (s.send == motion::Send::none) && sent.n <= 1;
		if (same && sent.n == 1) same = sent.src == src && sent.id == 8 && sent.count == 7 && sent.v == s.values;
		for (int i = 0; i < 7; i++) same = same && std::bit_cast<uint32_t>(old_c[i]) == std::bit_cast<uint32_t>(new_c[i]);
		if (!same && r.differences++ == 0) {
			std::snprintf(r.first, sizeof r.first, "reading %ld (%d %d %d %d %d %d, interval %d, stream %d): old %d msg, new %d; acc_abs old %08lx new %08lx",
			    r.readings, raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], interval, bulk, sent.n, static_cast<int>(s.send),
			    static_cast<unsigned long>(std::bit_cast<uint32_t>(old_c[0])), static_cast<unsigned long>(std::bit_cast<uint32_t>(new_c[0])));
		}
	}

	// every reading the comparison makes
	void all() {
		static constexpr std::array<int16_t, 8> kEdge = {-32768, -32767, -16384, -1, 0, 1, 16384, 32767};
		static constexpr std::array<int, 6> kIntervals = {127, 1, 127, 0, 3, 127};
		uint32_t n = 0;
		for (int16_t g0 : kEdge) {
			for (int16_t g1 : kEdge) {
				for (int16_t g2 : kEdge) {
					for (int16_t a0 : kEdge) {
						for (int16_t a1 : kEdge) {
							for (int16_t a2 : kEdge) {
								n++;
								reading({g0, g1, g2, a0, a1, a2}, kIntervals[(n / 97) % 6], (n / 13) % 3 != 0);
							}
						}
					}
				}
			}
		}
		for (int x = -20; x <= 20; x++) {
			for (int y = -20; y <= 20; y++) {
				for (int z = -20; z <= 20; z++) {
					n++;
					reading({static_cast<int16_t>(x * 7), static_cast<int16_t>(y), static_cast<int16_t>(z * 3), static_cast<int16_t>(x),
					            static_cast<int16_t>(y), static_cast<int16_t>(z)},
					    kIntervals[(n / 31) % 6], (n / 7) % 2 != 0);
				}
			}
		}
		for (int i = 0; i < 200000; i++) {
			n++;
			std::array<int16_t, 6> raw{};
			if (i % 4 == 0) {
				for (int16_t& c : raw) c = static_cast<int16_t>(next());
			} else {
				// about 1 g (16384 at 2 g full scale) in a random direction, a turning gyro (integers
				// only: the same readings on the PC and on the board's CPU)
				const int g = 13000 + static_cast<int>(next() % 6000);
				const int x = static_cast<int>(next() % static_cast<uint32_t>(2 * g + 1)) - g;
				const int rest = g * g - x * x;
				const int y = static_cast<int>(next() % static_cast<uint32_t>(2 * isqrt(rest) + 1)) - isqrt(rest);
				const int z = isqrt(rest - y * y) * (next() % 2 ? 1 : -1);
				raw[3] = static_cast<int16_t>(x + static_cast<int>(next() % 61) - 30);
				raw[4] = static_cast<int16_t>(y + static_cast<int>(next() % 61) - 30);
				raw[5] = static_cast<int16_t>(z + static_cast<int>(next() % 61) - 30);
				for (int k = 0; k < 3; k++) raw[k] = static_cast<int16_t>(static_cast<int>(next() % 8001) - 4000);
			}
			reading(raw, kIntervals[(n / 50) % 6], (n / 11) % 4 != 0);
		}
	}

private:
	static constexpr uint32_t kUnset = 0x7fc0dead; // a NaN no control is
	motion::MotionFilter<2> filter;
	uint32_t state = 2463534242u;
	static int isqrt(int v) { // the floor of the square root, bit by bit
		uint32_t x = static_cast<uint32_t>(v < 0 ? 0 : v), s = 0;
		for (uint32_t bit = 1u << 30; bit != 0; bit >>= 2) {
			if (x >= s + bit) {
				x -= s + bit;
				s = (s >> 1) + bit;
			} else {
				s >>= 1;
			}
		}
		return static_cast<int>(s);
	}
	uint32_t next() { // xorshift32: the same readings on the PC and on the board's CPU
		state ^= state << 13;
		state ^= state >> 17;
		state ^= state << 5;
		return state;
	}
};

} // namespace motion_compare

// (in the one file of each test that includes this)
extern "C" bool ref_motion_send(int src, int id, int n, const int* v) {
	motion_compare::Sent& s = motion_compare::sent;
	s.n++;
	s.src = src;
	s.id = id;
	s.count = n;
	for (int i = 0; i < 7 && i < n; i++) s.v[i] = v[i];
	return true;
}
