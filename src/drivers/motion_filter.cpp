/*
 * One motion reading through the filter (motion_filter.hpp), on the motion thread: the motion
 * message to the control thread, the synth's motion controls. The board's motionsensor.c and the
 * virtual board's motion_host.c call it with their readings.
 */
#include <array>
#include <cstdint>

extern "C" {
#include "config.h"
#include "messaging.h"
#include "motionsensor.h"
#include "striso.h"
#include "synth.h"
}

#include "motion_filter.hpp"

namespace {

// accelerometer full scale in g (2, 4, 8 or 16): motionsensor.c sets CTRL1_XL from it
motion::MotionFilter<MOTION_ACC_FS> filter;

} // namespace

extern "C" void motion_reading(const int16_t raw[6]) {
	const std::array<int16_t, 6> r = {raw[0], raw[1], raw[2], raw[3], raw[4], raw[5]};
	const motion::Step s = filter.step(r, config.send_motion_interval, config.send_usb_bulk);
	if (s.send != motion::Send::none) {
		(void)event_send(s.send == motion::Send::accel ? ID_ACCEL : ID_ACCEL_STREAM, 8, 7, s.values.data());
	}
#ifdef USE_INTERNAL_SYNTH
	if (s.controls) {
		*(synth_interface.acc_abs) = s.c.acc_abs;
		*(synth_interface.acc_x) = s.c.acc_x;
		*(synth_interface.acc_y) = s.c.acc_y;
		*(synth_interface.acc_z) = s.c.acc_z;
		*(synth_interface.rot_x) = s.c.rot_x;
		*(synth_interface.rot_y) = s.c.rot_y;
		*(synth_interface.rot_z) = s.c.rot_z;
	}
#endif
}
