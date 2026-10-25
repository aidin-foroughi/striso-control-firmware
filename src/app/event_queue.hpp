/*
 * The events the producers (the key scanner, the motion sensor, the aux jack, the console, MIDI in)
 * send to the control thread, and the queue that carries them. Plain C++20 with no ChibiOS, heap or
 * exceptions: the host tests use it as it is (tests/test_event_queue.cpp, tests/messaging).
 */
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <span>
#include <type_traits>

// Where an event comes from: the wire's source field (striso.h ID_*, messaging.cpp checks they
// agree), and two that are not sources on the wire: internal (requests to the control thread
// itself, never sent) and accel_stream (motion for the stream only, sent as accel).
enum class Source : uint8_t {
	dis = 0,     // the keys
	bas = 1,     // the bass side's keys (no board has one)
	control = 2, // the slider, the aux buttons, the pedals
	accel = 3,   // the motion sensor
	sys = 4,     // system and status
	midi = 5,    // the MIDI in monitor
	internal = 6,
	accel_stream = 0x13,
};

// One event: a source, an id (a key, a control, a MIDI channel, a request) and up to 7 values.
struct KeyEvent {
	static constexpr std::size_t kMaxValues = 7;

	Source src = Source::dis;
	uint8_t id = 0;
	std::array<int16_t, kMaxValues> v{};
	uint8_t n = 0; // values used

	[[nodiscard]] constexpr std::span<const int16_t> values() const { return std::span<const int16_t>(v).first(n); }

	// a value as an event holds it: saturated to int16 (the producers' values all fit: the keys'
	// are 14 bit but for x and y on a wrapped corner sum, which the wire saturates to 14 bit anyway)
	static constexpr int16_t saturate(int x) {
		return static_cast<int16_t>(std::clamp<int>(x, std::numeric_limits<int16_t>::min(), std::numeric_limits<int16_t>::max()));
	}

	// this event: src, id and the first kMaxValues values (the rest 0)
	constexpr void assign(Source s, int ident, std::span<const int> vals) {
		src = s;
		id = static_cast<uint8_t>(ident);
		n = static_cast<uint8_t>(std::min(vals.size(), kMaxValues));
		for (std::size_t i = 0; i < kMaxValues; i++) v[i] = i < n ? saturate(vals[i]) : int16_t{0};
	}

	static constexpr KeyEvent make(Source src, int id, std::span<const int> values) {
		KeyEvent e;
		e.assign(src, id, values);
		return e;
	}
	static constexpr KeyEvent make(Source src, int id, std::initializer_list<int> values) {
		return make(src, id, std::span<const int>(values.begin(), values.size()));
	}
};

// A single-producer single-consumer ring of N events (N a power of two), lock free: one thread
// push()es, one thread reads front() and pop()s. The indices run free (mod 2^32) and the count is
// their difference, so a full ring (count N) never reads as empty (count 0): the int ring's R2.
template <class T, std::size_t N>
class EventQueue {
	static_assert(N >= 1 && std::has_single_bit(N) && N <= (std::size_t{1} << 31), "N: a power of two");
	static_assert(std::is_trivially_copyable_v<T>, "events are copied as bytes");
	static_assert(std::atomic<uint32_t>::is_always_lock_free);

public:
	static constexpr std::size_t capacity() { return N; }

	// the producer: fill(T&) writes the next event in its slot (no copy on the producer's stack);
	// false, and fill not called, if the ring is full
	template <class Fill>
	bool push_with(Fill&& fill) {
		const uint32_t w = w_.load(std::memory_order_relaxed);
		if (w - r_.load(std::memory_order_acquire) >= N) return false;
		fill(buf_[w & (N - 1)]);
		w_.store(w + 1, std::memory_order_release);
		return true;
	}
	bool push(const T& e) {
		return push_with([&e](T& slot) { slot = e; });
	}

	// the consumer: the oldest event, nullptr if there is none; it stays until pop()
	[[nodiscard]] const T* front() const {
		const uint32_t r = r_.load(std::memory_order_relaxed);
		return r == w_.load(std::memory_order_acquire) ? nullptr : &buf_[r & (N - 1)];
	}
	// the consumer, after a front() that was not nullptr
	void pop() { r_.store(r_.load(std::memory_order_relaxed) + 1, std::memory_order_release); }

	// either side (a snapshot while the other one runs)
	[[nodiscard]] std::size_t size() const {
		const uint32_t r = r_.load(std::memory_order_acquire);
		const uint32_t w = w_.load(std::memory_order_acquire);
		return std::min<std::size_t>(w - r, N);
	}
	[[nodiscard]] bool empty() const { return size() == 0; }

private:
	std::array<T, N> buf_{};
	std::atomic<uint32_t> w_{0}; // written by the producer
	std::atomic<uint32_t> r_{0}; // written by the consumer
};
