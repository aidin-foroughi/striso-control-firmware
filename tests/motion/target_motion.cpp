// The motion comparison (motion_compare.hpp) on the board's CPU under QEMU: the old math compiled
// as the firmware compiles motionsensor.c (C, its flags), the filter as it compiles
// motion_filter.cpp (C++, its flags). ctest target.motion: "differences 0".
#include <cstdio>

#include "motion_compare.hpp"

int main() {
	static motion_compare::Compare c;
	c.all();
	std::printf("motion: %ld readings, %ld differences (%ld motion messages, %ld stream-only, %ld with the controls)\n",
	    c.r.readings, c.r.differences, c.r.accel, c.r.stream, c.r.controls);
	if (c.r.differences) std::printf("first: %s\n", c.r.first);
	return c.r.differences ? 1 : 0;
}
