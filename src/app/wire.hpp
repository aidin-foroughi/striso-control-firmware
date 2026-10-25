/*
 * The binary stream's frame of an event: what the control thread writes to the USB bulk console
 * (and the UART), and what the host parses (Striso Live protocol.dart StreamParser, tools/striso.py,
 * tests/console_sim/host_parser.h):
 *
 *   byte 0       1 ssss nnn   s: the source (its low 4 bits: accel_stream goes out as accel),
 *                             n: the number of values, 0..7
 *   byte 1       0 iiiiiii    the id
 *   2 per value  0 hhhhhhh  0 lllllll   the value, 14 bit two's complement, high 7 bits first
 *
 * Values saturate to -8192..8191, so a hard press (pres >= 1.0) never reads as a release. Pure:
 * no ChibiOS, tested on the host (tests/test_event_queue.cpp).
 */
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

#include "event_queue.hpp"

namespace wire {

inline constexpr std::size_t kMaxFrame = 2 + 2 * KeyEvent::kMaxValues;
inline constexpr int kMin = -8192;
inline constexpr int kMax = 8191;
static_assert(KeyEvent::kMaxValues <= 7, "the value count is 3 bits");

// requests to the control thread stay there
constexpr bool on_wire(Source s) { return s != Source::internal; }

// the frame of src, id and the values (the first 7) in out; its length (2 + 2 per value)
constexpr std::size_t encode(Source src, uint8_t id, std::span<const int16_t> v, std::span<uint8_t, kMaxFrame> out) {
	const std::size_t n = std::min(v.size(), KeyEvent::kMaxValues);
	out[0] = static_cast<uint8_t>(0x80 | ((static_cast<unsigned>(src) << 3) & 0x78) | n);
	out[1] = static_cast<uint8_t>(id & 0x7f);
	for (std::size_t c = 0; c < n; c++) {
		const int x = std::clamp<int>(v[c], kMin, kMax);
		out[2 + 2 * c] = static_cast<uint8_t>((x >> 7) & 0x7f);
		out[3 + 2 * c] = static_cast<uint8_t>(x & 0x7f);
	}
	return 2 + 2 * n;
}

constexpr std::size_t encode(const KeyEvent& e, std::span<uint8_t, kMaxFrame> out) {
	return encode(e.src, e.id, e.values(), out);
}

} // namespace wire
