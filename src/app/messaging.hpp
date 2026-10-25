/*
 * The message path, C++ side (messaging.h is the C side): events to the control thread.
 */
#pragma once

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

} // namespace messaging
