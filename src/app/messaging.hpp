/*
 * The message path, C++ side (messaging.h is the C side): events to the control thread.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "event_queue.hpp"

namespace messaging {

// Queue an event for the control thread: src, id and the values (at most KeyEvent::kMaxValues,
// each saturated to 16 bits), written straight into the queue. Never blocks; false if the sending
// thread's lane is full (counted in drops()) or there are too many values. From threads only,
// not interrupts.
bool send(Source src, int id, std::span<const int> values);
// the same with no, one, two or three values, in registers: no array on the sender's stack (main's
// and the key scanner's are tight)
bool send(Source src, int id);
bool send(Source src, int id, int a);
bool send(Source src, int id, int a, int b);
bool send(Source src, int id, int a, int b, int c);

// the control thread: the oldest event, waiting for one
KeyEvent receive();

// the events refused since boot
uint32_t drops();

// The inbox's lanes (inbox.hpp: one per sending thread), for the console's Stcq (R23)
inline constexpr std::size_t kLanes = 6;
inline constexpr std::size_t kLaneCapacity = 32;
struct LaneStats {
	const char* owner;  // the sending thread's name ("?" if it has none); nullptr: not used yet
	uint32_t drops;     // events refused (the lane was full)
	uint32_t max_depth; // the most events it held
};
// every lane's, taken together under the kernel lock
void lane_stats(std::span<LaneStats, kLanes> out);

} // namespace messaging
