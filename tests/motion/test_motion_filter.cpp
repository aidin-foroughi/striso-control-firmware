// The MotionFilter (src/drivers/motion_filter.hpp) against the old motion math (tests/ref/motion_ref.c)
// on the PC: the message and the synth's controls, bit for bit, for every reading of
// motion_compare.hpp. tools/target_render runs the same comparison on the board's CPU (QEMU),
// compiled with the firmware's flags (target.motion).
#include "doctest/doctest.h"
#include "motion_compare.hpp"

TEST_CASE("the old motion math, bit for bit") {
	motion_compare::Compare c;
	c.all();
	MESSAGE(c.r.readings << " readings: " << c.r.accel << " motion messages, " << c.r.stream << " stream-only, " << c.r.controls << " with the synth's controls");
	CHECK_MESSAGE(c.r.differences == 0, c.r.first);
	CHECK(c.r.accel > 0);
	CHECK(c.r.stream > 0);
	CHECK(c.r.controls > c.r.accel);
}

TEST_CASE("the stream-only message is every 4th reading, while the stream is on") {
	motion::MotionFilter<2> f;
	const std::array<int16_t, 6> flat = {0, 0, 0, 0, 0, 16384};
	int sent = 0;
	for (int i = 0; i < 40; i++) sent += f.step(flat, 127, true).send == motion::Send::stream;
	CHECK(sent == 10);
	CHECK(f.step(flat, 127, false).send == motion::Send::none);
	CHECK(f.step(flat, 0, true).controls == false);
	const motion::Step s = f.step(flat, 5, false);
	CHECK(s.send == motion::Send::accel);
	CHECK(s.values == std::array<int, 7>{0, 0, -1024, 1024, 0, 0, 0}); // flat: -1 g on z
	CHECK(s.c.acc_abs == 1.0f);
	CHECK(s.c.acc_z == -1.0f);
}
