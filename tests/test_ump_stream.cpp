// UMP Stream messages (src/app/ump_stream.hpp): the endpoint's answers to discovery, word by word
// against UMP 1.1's layouts.
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "doctest/doctest.h"
#include "ump_stream.hpp"

namespace {

struct Sink {
	std::vector<std::array<uint32_t, 4>> m;
	void ump4(const uint32_t* w) { m.push_back({w[0], w[1], w[2], w[3]}); }
};

ump_stream::Identity identity(uint8_t protocol) {
	// family 0x81 (LSB 0x01, MSB 0x01), model 0x102 (LSB 0x02, MSB 0x02): 7-bit bytes, LSB first
	return {"Striso", "0123456789ABCDEF01234567", {0x7d, 0x00, 0x00}, 0x0081, 0x0102, {2, 2, 4, 0}, protocol};
}

// the text of Stream messages: w0's low bytes (first of them), then w1..w3
std::string text(const std::vector<std::array<uint32_t, 4>>& m, int first) {
	std::string s;
	for (const auto& w : m) {
		std::vector<uint8_t> b;
		if (first == 2) b.push_back(static_cast<uint8_t>(w[0] >> 8));
		b.push_back(static_cast<uint8_t>(w[0]));
		for (int k = 0; k < 12; k++) b.push_back(static_cast<uint8_t>(w[1 + k / 4] >> (24 - 8 * (k % 4))));
		for (const uint8_t c : b)
			if (c) s += static_cast<char>(c);
	}
	return s;
}

uint16_t status(const std::array<uint32_t, 4>& w) { return static_cast<uint16_t>((w[0] >> 16) & 0x3ff); }
uint8_t form(const std::array<uint32_t, 4>& w) { return static_cast<uint8_t>((w[0] >> 26) & 3); }

} // namespace

TEST_CASE("ump stream: endpoint discovery, every notification") {
	Sink s;
	const uint32_t req[4] = {0xf0000101u, 0x0000001fu, 0, 0}; // UMP 1.1, filter: all five
	REQUIRE(ump_stream::reply(req, identity(ump_stream::PROTOCOL_MIDI1), s));
	REQUIRE(s.m.size() == 6);                                                 // info, identity, name, product id (2: 24 characters), configuration
	CHECK(s.m[0] == std::array<uint32_t, 4>{0xf0010101u, 0x81000300u, 0, 0}); // static, 1 block, M2 + M1, no JR
	CHECK(s.m[1] == std::array<uint32_t, 4>{0xf0020000u, 0x007d0000u, 0x01010202u, 0x02020400u});
	CHECK(status(s.m[2]) == ump_stream::endpoint_name);
	CHECK(form(s.m[2]) == 0);
	CHECK(text({s.m[2]}, 2) == "Striso");
	CHECK(status(s.m[3]) == ump_stream::product_instance_id);
	CHECK(form(s.m[3]) == 1);
	CHECK(form(s.m[4]) == 3);
	CHECK(text({s.m[3], s.m[4]}, 2) == "0123456789ABCDEF01234567");
	CHECK(s.m[5] == std::array<uint32_t, 4>{0xf0060100u, 0, 0, 0}); // MIDI 1.0 protocol (mpe, normal, mono)
}

TEST_CASE("ump stream: the configuration is the mode's; a request doesn't change it") {
	Sink s;
	const uint32_t req[4] = {0xf0050200u, 0, 0, 0}; // the host asks for MIDI 2.0
	REQUIRE(ump_stream::reply(req, identity(ump_stream::PROTOCOL_MIDI1), s));
	REQUIRE(ump_stream::reply(req, identity(ump_stream::PROTOCOL_MIDI2), s));
	REQUIRE(s.m.size() == 2);
	CHECK(s.m[0][0] == 0xf0060100u);
	CHECK(s.m[1][0] == 0xf0060200u);
}

TEST_CASE("ump stream: function block 0, as the Group Terminal Block") {
	Sink s;
	const uint32_t all[4] = {0xf010ff03u, 0, 0, 0}; // every block, info and name
	REQUIRE(ump_stream::reply(all, identity(ump_stream::PROTOCOL_MIDI2), s));
	REQUIRE(s.m.size() == 2);
	CHECK(s.m[0] == std::array<uint32_t, 4>{0xf0118033u, 0x00010000u, 0, 0}); // active, bidirectional; group 1 of 1
	CHECK(status(s.m[1]) == ump_stream::function_block_name);
	CHECK(((s.m[1][0] >> 8) & 0xff) == 0); // block 0
	CHECK(text({s.m[1]}, 1) == "Striso");
	s.m.clear();
	const uint32_t other[4] = {0xf0100103u, 0, 0, 0}; // block 1: none
	CHECK(ump_stream::reply(other, identity(ump_stream::PROTOCOL_MIDI2), s));
	CHECK(s.m.empty());
	const uint32_t note[4] = {0x40903c00u, 0, 0, 0};
	CHECK_FALSE(ump_stream::reply(note, identity(ump_stream::PROTOCOL_MIDI2), s));
}
