/*
 * The motion sensor's math: from one LSM6DSL reading (gyro x y z, accel x y z, as read) to the
 * synth's motion controls and the motion message, as motionsensor.c's reader loop had it, bit for
 * bit (tests/motion/test_motion_filter.cpp, against the old code, on the PC and on the board's CPU
 * under QEMU). Pure C++20: the thread (motionsensor.c) reads the sensor, motion_filter.cpp sends.
 */
#pragma once

#include <array>
#include <cmath>
#include <cstdint>

namespace motion {

// the synth's motion controls (synth_interface): the acceleration's size in g and its direction
// (a unit vector when it is not ~0), the rotation in units of the gyro's full scale
struct Controls {
	float acc_abs, acc_x, acc_y, acc_z;
	float rot_x, rot_y, rot_z;
};

enum class Send : uint8_t { none,
	accel,    // ID_ACCEL: the motion message (MIDI motion, the stream)
	stream }; // ID_ACCEL_STREAM: for the stream only (the internal synth only, the stream on)

struct Step {
	Send send = Send::none;
	std::array<int, 7> values{}; // ax ay az |a| gx gy gz, 8 g (16 g for |a|) and 2^13 full scale
	bool controls = false;       // the synth's controls are new (sending motion at all)
	Controls c{};
};

// AccFs: the accelerometer's full scale in g (2, 4, 8 or 16)
template <int AccFs>
class MotionFilter {
	static_assert(AccFs == 2 || AccFs == 4 || AccFs == 8 || AccFs == 16, "ACC_FS: 2, 4, 8 or 16 g");

public:
	int stream_count = 0; // readings since the last stream-only message

	// raw: the sensor's burst (gyro x y z, accel x y z); interval: config.send_motion_interval
	// (0 off, 127 the internal synth only); bulk: the USB stream is on
	Step step(const std::array<int16_t, 6>& raw, int interval, bool bulk) {
		// the board's axes
		const int16_t ax = static_cast<int16_t>(-raw[3]);
		const int16_t gx = static_cast<int16_t>(-raw[0]);
		const int16_t ay = static_cast<int16_t>(-raw[4]);
		const int16_t gy = static_cast<int16_t>(-raw[1]);
		const int16_t az = static_cast<int16_t>(-raw[5]);
		const int16_t gz = static_cast<int16_t>(-raw[2]);

		float acc_x = (static_cast<float>(ax)) * (AccFs / 32768.0f);
		float acc_y = (static_cast<float>(ay)) * (AccFs / 32768.0f);
		float acc_z = (static_cast<float>(az)) * (AccFs / 32768.0f);
		const float acc_abs = std::sqrt((acc_x * acc_x) + (acc_y * acc_y) + (acc_z * acc_z));
		if (acc_abs > 0.001) {
			acc_x /= acc_abs;
			acc_y /= acc_abs;
			acc_z /= acc_abs;
		}

		Step s;
		if (interval == 0) return s;
		if (interval != 127) {
			s.send = Send::accel;
		} else if (bulk && ++stream_count >= 4) {
			// every 4th reading (~60 per second): stream-only motion data, no MIDI side effects
			stream_count = 0;
			s.send = Send::stream;
		}
		if (s.send != Send::none) {
			// acceleration in message has maximum range 8g (16g for acc_abs)
			s.values = {(ax * AccFs / 8) >> 2, (ay * AccFs / 8) >> 2, (az * AccFs / 8) >> 2,
			    (static_cast<int16_t>(acc_abs * 32768.0f / 8.0f)) >> 2, gx >> 2, gy >> 2, gz >> 2};
		}
		s.controls = true;
		s.c = {acc_abs, acc_x, acc_y, acc_z,
		    (static_cast<float>(gx)) * (1.0f / 32768.0f), (static_cast<float>(gy)) * (1.0f / 32768.0f),
		    (static_cast<float>(gz)) * (1.0f / 32768.0f)};
		return s;
	}
};

} // namespace motion
