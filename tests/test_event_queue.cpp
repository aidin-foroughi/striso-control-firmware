// The message path's pure parts (src/app/event_queue.hpp, src/app/wire.hpp): the SPSC ring at its
// boundaries and under two real threads, KeyEvent's saturation, and wire::encode against the
// frame code it replaced (main.c ThreadSend's saturation and pack(), kept below as the reference):
// byte for byte over every value the producers send.
#include <array>
#include <cstdint>
#include <random>
#include <thread>
#include <vector>

#include "doctest/doctest.h"
#include "event_queue.hpp"
#include "wire.hpp"

namespace {

// main.c at c5358f7, ThreadSend's frame of msg[0..size-1] (src, id, the values), as it was
std::vector<uint8_t> old_frame(int size, int* msg) {
	constexpr int ID_DIS = 0, ID_BAS = 1;
	uint8_t cmsg[16];
	if (msg[0] == ID_DIS || msg[0] == ID_BAS) {
		for (int c = 2; c < size; c++) {
			msg[c] = msg[c] > 8191 ? 8191 : msg[c] < -8192 ? -8192 :
			                                                 msg[c];
		}
	}
	cmsg[0] = 0x80 | ((uint8_t)msg[0]) << 3 | ((uint8_t)(size - 2));
	cmsg[1] = 0x7f & (uint8_t)msg[1];
	for (int c = 0; c < size - 2; c++) {
		cmsg[2 + c * 2] = 0x7f & (uint8_t)(msg[2 + c] >> 7);
		cmsg[2 + c * 2 + 1] = 0x7f & (uint8_t)(msg[2 + c]);
	}
	return {cmsg, cmsg + 2 + (size - 2) * 2};
}

std::vector<uint8_t> new_frame(int src, int id, const std::vector<int>& v) {
	std::array<uint8_t, wire::kMaxFrame> f{};
	const KeyEvent e = KeyEvent::make(static_cast<Source>(src), id, std::span<const int>(v.data(), v.size()));
	const std::size_t n = wire::encode(e, f);
	return {f.begin(), f.begin() + static_cast<std::ptrdiff_t>(n)};
}

std::vector<uint8_t> old_frame(int src, int id, const std::vector<int>& v) {
	std::array<int, 9> msg{src, id};
	for (std::size_t c = 0; c < v.size(); c++) msg[2 + c] = v[c];
	return old_frame(static_cast<int>(2 + v.size()), msg.data());
}

constexpr std::array<int, 7> kSources = {0, 1, 2, 3, 4, 5, 0x13}; // every source on the wire

} // namespace

TEST_CASE("EventQueue: N events fit, the next is refused, and full never reads as empty") {
	EventQueue<int, 4> q;
	CHECK(q.empty());
	CHECK(q.front() == nullptr);
	for (int round = 0; round < 3; round++) { // the indices wrap past the buffer
		for (int i = 0; i < 4; i++) CHECK(q.push(round * 10 + i));
		CHECK(q.size() == 4);
		CHECK_FALSE(q.empty());
		CHECK_FALSE(q.push(99));
		for (int i = 0; i < 4; i++) {
			REQUIRE(q.front() != nullptr);
			CHECK(*q.front() == round * 10 + i);
			q.pop();
		}
		CHECK(q.empty());
		CHECK(q.front() == nullptr);
	}
	// interleaved: one in, one out, many times round
	for (int i = 0; i < 1000; i++) {
		CHECK(q.push(i));
		CHECK(q.size() == 1);
		CHECK(*q.front() == i);
		q.pop();
	}
}

TEST_CASE("EventQueue: one producer and one consumer thread, nothing lost, duplicated or reordered") {
	static EventQueue<uint32_t, 16> q;
	constexpr uint32_t kCount = 200000;
	std::thread producer([] {
		for (uint32_t i = 0; i < kCount; i++) {
			while (!q.push(i)) std::this_thread::yield();
		}
	});
	uint32_t next = 0;
	bool in_order = true;
	while (next < kCount) {
		const uint32_t* f = q.front();
		if (f == nullptr) {
			std::this_thread::yield();
			continue;
		}
		in_order = in_order && *f == next;
		q.pop();
		next++;
	}
	producer.join();
	CHECK(in_order);
	CHECK(q.empty());
}

TEST_CASE("KeyEvent::make: up to 7 values, each saturated to 16 bits") {
	const KeyEvent e = KeyEvent::make(Source::accel, 8, {1, -2, 40000, -40000, 32767, -32768, 7, 8});
	CHECK(e.src == Source::accel);
	CHECK(e.id == 8);
	CHECK(e.n == 7);
	CHECK(e.v == std::array<int16_t, 7>{1, -2, 32767, -32768, 32767, -32768, 7});
	CHECK(KeyEvent::make(Source::internal, 2, {}).n == 0);
	CHECK(KeyEvent::make(Source::internal, 2, {}).values().empty());
}

TEST_CASE("wire::encode: the old frames, byte for byte") {
	// the keys (dis, bas): every 16-bit value in every position, and values past 16 bits (the
	// queue saturates them to 16 bits, the old code took them whole: both saturate to 14)
	int mismatches = 0;
	for (int src : {0, 1}) {
		for (int x = -32768; x <= 32767; x++) {
			for (int pos = 0; pos < 4; pos++) {
				std::vector<int> v = {100, -100, 5, 7};
				v[pos] = x;
				mismatches += new_frame(src, 42, v) != old_frame(src, 42, v);
			}
		}
		for (int x : {32768, 40000, 1 << 20, -32769, -40000, -(1 << 20)}) {
			mismatches += new_frame(src, 3, {x, x, x, x}) != old_frame(src, 3, {x, x, x, x});
		}
	}
	CHECK(mismatches == 0);

	// the other sources: every value they send (all within 14 bits: the motion sensor's >> 2 of
	// 16-bit readings, the pedals' 13-bit ADC levels, the slider's moves, MIDI bytes), every count
	// of values and every id
	mismatches = 0;
	for (int src : kSources) {
		for (int x = wire::kMin; x <= wire::kMax; x++) {
			mismatches += new_frame(src, 8, {x}) != old_frame(src, 8, {x});
			mismatches += new_frame(src, 8, {0, 0, 0, 0, 0, 0, x}) != old_frame(src, 8, {0, 0, 0, 0, 0, 0, x});
		}
		for (int id = 0; id < 256; id++) {
			for (std::size_t n = 0; n <= 7; n++) {
				std::vector<int> v(n, -id * 31);
				mismatches += new_frame(src, id, v) != old_frame(src, id, v);
			}
		}
	}
	std::mt19937 rng(1);
	std::uniform_int_distribution<int> val(wire::kMin, wire::kMax);
	for (int i = 0; i < 200000; i++) {
		const int src = kSources[static_cast<std::size_t>(i) % kSources.size()];
		std::vector<int> v(static_cast<std::size_t>(i % 8));
		for (int& x : v) x = val(rng);
		mismatches += new_frame(src, i & 0x7f, v) != old_frame(src, i & 0x7f, v);
	}
	CHECK(mismatches == 0);
}

TEST_CASE("wire::encode: past 14 bits every source saturates (the old frame wrapped all but the keys')") {
	std::array<uint8_t, wire::kMaxFrame> f{};
	const std::array<int16_t, 2> v = {8192, -8193};
	CHECK(wire::encode(Source::accel, 0, v, f) == 6);
	CHECK(f[2] == 0x3f); // 8191
	CHECK(f[3] == 0x7f);
	CHECK(f[4] == 0x40); // -8192
	CHECK(f[5] == 0x00);
	CHECK(f[0] == (0x80 | 3 << 3 | 2));
	CHECK(wire::encode(Source::accel_stream, 0, {}, f) == 2);
	CHECK(f[0] == (0x80 | 3 << 3)); // accel_stream goes out as accel
	CHECK_FALSE(wire::on_wire(Source::internal));
	CHECK(wire::on_wire(Source::midi));
}
