/*
 * The message path: the board's inbox (inbox.hpp) between the producers and the control thread,
 * and its C and C++ entry points (messaging.h, messaging.hpp). Each entry point writes its event
 * straight into the queue: the producers' stacks only hold their arguments (the key scanner's
 * thread has 256 bytes).
 */
#include <cstddef>
#include <span>

#include "ch.h"

#include "inbox.hpp"
#include "messaging.h"
#include "messaging.hpp"
#include "striso.h"

static_assert(static_cast<int>(Source::dis) == ID_DIS && static_cast<int>(Source::bas) == ID_BAS &&
        static_cast<int>(Source::control) == ID_CONTROL && static_cast<int>(Source::accel) == ID_ACCEL &&
        static_cast<int>(Source::sys) == ID_SYS && static_cast<int>(Source::midi) == ID_MIDI &&
        static_cast<int>(Source::internal) == ID_INTERNAL && static_cast<int>(Source::accel_stream) == ID_ACCEL_STREAM,
    "Source is striso.h's ID_*");

namespace {

// Lanes: the key scanner (keys, slider, aux buttons), the motion sensor, the aux jack, main (the
// console, MIDI in, the tick), the control thread itself (the MIDI monitor), one to spare. 32
// events each: the int ring held 34 key messages, for every producer together.
constinit Inbox<messaging::kLanes, messaging::kLaneCapacity> inbox;

// an event of the values v, written in place
template <class... V>
bool post(Source src, int id, V... v) {
	static_assert(sizeof...(V) <= KeyEvent::kMaxValues);
	return inbox.send_with([&](KeyEvent& e) {
		e = KeyEvent{};
		e.src = src;
		e.id = static_cast<uint8_t>(id);
		std::size_t i = 0;
		((e.v[i++] = KeyEvent::saturate(v)), ...);
		(void)i;
		e.n = static_cast<uint8_t>(sizeof...(V));
	});
}

} // namespace

bool messaging::send(Source src, int id, std::span<const int> values) {
	if (values.size() > KeyEvent::kMaxValues) return false;
	return inbox.send_with([&](KeyEvent& e) { e.assign(src, id, values); });
}
bool messaging::send(Source src, int id) { return post(src, id); }
bool messaging::send(Source src, int id, int a) { return post(src, id, a); }
bool messaging::send(Source src, int id, int a, int b) { return post(src, id, a, b); }
bool messaging::send(Source src, int id, int a, int b, int c) { return post(src, id, a, b, c); }

KeyEvent messaging::receive() { return inbox.receive(); }

void messaging::lane_stats(std::span<LaneStats, kLanes> out) {
	chSysLock();
	for (std::size_t i = 0; i < kLanes; i++) {
		const auto s = inbox.stats(i);
		const char* name = s.owner == nullptr ? nullptr : s.owner->name != nullptr ? s.owner->name :
		                                                                             "?";
		out[i] = LaneStats{name, s.drops, s.max_depth};
	}
	chSysUnlock();
}

uint32_t messaging::drops() {
	chSysLock();
	const uint32_t d = inbox.drops();
	chSysUnlock();
	return d;
}

extern "C" bool event_send(int src, int id, int n, const int* v) {
	if (n < 0) return false;
	return messaging::send(static_cast<Source>(src), id, std::span<const int>(v, static_cast<std::size_t>(n)));
}

extern "C" bool event_send1(int src, int id, int a) { return post(static_cast<Source>(src), id, a); }

extern "C" bool event_send2(int src, int id, int a, int b) { return post(static_cast<Source>(src), id, a, b); }

extern "C" uint32_t messaging_drops(void) { return messaging::drops(); }
