// Stand-in for src/app/messaging.cpp's lane_stats in the console's simulation (the real one runs
// on the virtual board): two lanes in use, one of them full once, a thread name with a space.
#include "messaging.hpp"

void messaging::lane_stats(std::span<LaneStats, kLanes> out) {
	for (LaneStats& l : out) l = LaneStats{nullptr, 0, 0};
	out[0] = LaneStats{"read_buttons", 3, 32};
	out[1] = LaneStats{"send messages", 0, 5};
}
