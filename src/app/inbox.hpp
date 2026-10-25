/*
 * The control thread's inbox: one EventQueue (a lane) per producing thread, and the consumer's
 * wakeup. messaging.cpp has the board's instance; the host model checks build small ones on a
 * stand-in kernel (tests/messaging).
 *
 * - A lane per thread. A thread gets the next free lane at its first send(), so each lane has one
 *   producer and the consumer reads it without a lock. A flood from one producer (motion, a MIDI
 *   in burst, the monitor) fills only its own lane: the keys' note-offs keep their room. The
 *   threads past the first Lanes - 1 share the last lane; that is safe as well, because every
 *   push happens under the kernel lock (it also stamps the event, below).
 * - One order over all lanes. Each event gets a sequence number when it is queued, and receive()
 *   takes the lowest one: the events come out in the order they were sent, as from one queue.
 * - Never blocks the sender. A full lane refuses the event and counts it (drops()).
 * - No lost wakeup. receive() checks for events and suspends under the kernel lock, and send()
 *   pushes and resumes the consumer under it, so a send can't fall between the check and the
 *   sleep (B7).
 * send() is for threads (not interrupts): every producer is one.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "ch.h"
#include "event_queue.hpp"

template <std::size_t Lanes, std::size_t N>
class Inbox {
	static_assert(Lanes >= 1);

public:
	struct LaneStats {
		const thread_t* owner; // nullptr: not used yet
		uint32_t drops;        // events refused (the lane was full)
		uint32_t max_depth;    // the most events it held
	};

	// any thread: queue an event for the consumer, written in place by fill(KeyEvent&) (no copy
	// on the sender's stack: the key scanner's is small); false if the thread's lane is full
	// (counted, fill not called)
	template <class Fill>
	bool send_with(Fill&& fill) {
		chSysLock();
		Lane& l = lane_of(chThdGetSelfX());
		const bool ok = l.q.push_with([this, &fill](Stamped& s) {
			s.seq = seq_;
			fill(s.e);
		});
		if (ok) {
			seq_++;
			const auto depth = static_cast<uint32_t>(l.q.size());
			if (depth > l.max_depth) l.max_depth = depth;
			chThdResumeS(&waiter_, MSG_OK);
		} else {
			l.drops++;
		}
		chSysUnlock();
		return ok;
	}
	bool send(const KeyEvent& e) {
		return send_with([&e](KeyEvent& slot) { slot = e; });
	}

	// the consumer thread: the oldest event; it sleeps until there is one
	KeyEvent receive() {
		KeyEvent e;
		while (!try_receive(e)) {
			chSysLock();
			if (empty()) (void)chThdSuspendS(&waiter_);
			chSysUnlock();
		}
		return e;
	}

	// the consumer thread: the oldest event in out, false if there is none
	bool try_receive(KeyEvent& out) {
		Lane* oldest = nullptr;
		uint32_t oldest_seq = 0;
		for (Lane& l : lanes_) {
			const Stamped* s = l.q.front();
			if (s != nullptr && (oldest == nullptr || static_cast<int32_t>(s->seq - oldest_seq) < 0)) {
				oldest = &l;
				oldest_seq = s->seq;
			}
		}
		if (oldest == nullptr) return false;
		out = oldest->q.front()->e;
		oldest->q.pop();
		return true;
	}

	[[nodiscard]] bool empty() const {
		for (const Lane& l : lanes_) {
			if (!l.q.empty()) return false;
		}
		return true;
	}

	// the events refused, all lanes
	[[nodiscard]] uint32_t drops() const {
		uint32_t d = 0;
		for (const Lane& l : lanes_) d += l.drops;
		return d;
	}
	[[nodiscard]] LaneStats stats(std::size_t lane) const {
		const Lane& l = lanes_[lane];
		return {l.owner, l.drops, l.max_depth};
	}
	static constexpr std::size_t lanes() { return Lanes; }
	static constexpr std::size_t lane_capacity() { return N; }

private:
	struct Stamped {
		uint32_t seq;
		KeyEvent e;
	};
	struct Lane {
		const thread_t* owner = nullptr;
		EventQueue<Stamped, N> q;
		uint32_t drops = 0;
		uint32_t max_depth = 0;
	};

	// under the kernel lock: tp's lane, taken at its first send
	Lane& lane_of(const thread_t* tp) {
		if (tp != nullptr) {
			for (std::size_t i = 0; i + 1 < Lanes; i++) {
				Lane& l = lanes_[i];
				if (l.owner == nullptr) l.owner = tp;
				if (l.owner == tp) return l;
			}
		}
		Lane& shared = lanes_[Lanes - 1];
		if (shared.owner == nullptr) shared.owner = tp;
		return shared;
	}

	std::array<Lane, Lanes> lanes_{};
	uint32_t seq_ = 0;
	thread_reference_t waiter_ = nullptr;
};
