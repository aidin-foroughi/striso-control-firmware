// UMP Stream messages (UMP 1.1, message type 0xF): the endpoint's answers to a MIDI 2.0 host's
// discovery (Windows MIDI Services, macOS) on USB MIDI 2.0 (interface 1 alt 1). HAL free, tested
// on the PC (tests/test_ump_stream.cpp); synth_control.cpp sends the replies (the control thread).
//
// The endpoint: UMP 1.1, one static function block (block 0: group 1, bidirectional, the Group
// Terminal Block's), MIDI 2.0 and MIDI 1.0 protocol capable, no JR timestamps. Its protocol is the
// MIDI mode's (doc/engines.md, "MIDI 2.0 out"): MIDI 2.0 in midi2, MIDI 1.0 otherwise (alt 1
// carries MIDI 1.0 in UMP then). A Stream Configuration Request doesn't change it: the
// notification answers with the protocol in use; a mode change sends one unasked.
#pragma once

#include <cstdint>

namespace ump_stream {

enum Status : uint16_t {
	endpoint_discovery = 0x000,
	endpoint_info = 0x001,
	device_identity = 0x002,
	endpoint_name = 0x003,
	product_instance_id = 0x004,
	stream_config_request = 0x005,
	stream_config = 0x006,
	function_block_discovery = 0x010,
	function_block_info = 0x011,
	function_block_name = 0x012,
};
inline constexpr uint8_t PROTOCOL_MIDI1 = 0x01, PROTOCOL_MIDI2 = 0x02;

// what the endpoint says about itself
struct Identity {
	const char* name;       // Endpoint Name, Function Block Name
	const char* product_id; // Product Instance Id (the serial)
	uint8_t sysex_id[3];    // Device Identity: manufacturer (1-byte ids: id, 0, 0)
	uint16_t family, model; //
	uint8_t revision[4];    //
	uint8_t protocol;       // PROTOCOL_MIDI1 or PROTOCOL_MIDI2: the one in use
};

// one Stream message: form (0 complete, 1 start, 2 continue, 3 end), status, w0's low 16 bits
constexpr uint32_t head(uint8_t form, uint16_t status, uint16_t data) {
	return 0xf0000000u | static_cast<uint32_t>(form & 3) << 26 | static_cast<uint32_t>(status & 0x3ff) << 16 | data;
}

// text in Stream messages: `first` bytes in w0's low 16 bits (2, or 1 after a block number),
// then 12 in w1..w3; more than fits goes on in continue / end messages
template <class Sink> void text(Sink& s, uint16_t status, int first, uint8_t lead, const char* str) {
	int len = 0;
	while (str[len] != '\0' && len < 98) len++;
	const int per = first + 12;
	int at = 0;
	do {
		const int n = len - at < per ? len - at : per;
		const uint8_t form = at == 0 ? (n == len ? 0 : 1) : (at + n >= len ? 3 : 2);
		uint8_t b[14] = {};
		for (int i = 0; i < n; i++) b[i] = static_cast<uint8_t>(str[at + i]);
		uint32_t w[4] = {};
		const uint16_t d = first == 2 ? static_cast<uint16_t>(b[0] << 8 | b[1]) : static_cast<uint16_t>(lead << 8 | b[0]);
		w[0] = head(form, status, d);
		for (int k = 0; k < 12; k++) w[1 + k / 4] |= static_cast<uint32_t>(b[first + k]) << (24 - 8 * (k % 4));
		s.ump4(w);
		at += n;
	} while (at < len);
}

template <class Sink> void configuration(Sink& s, const Identity& id) {
	const uint32_t w[4] = {head(0, stream_config, static_cast<uint16_t>(id.protocol << 8)), 0, 0, 0};
	s.ump4(w);
}

// a Stream message from the host (w: its 4 words; the requests use the first two): false if it
// isn't one this endpoint answers
template <class Sink> bool reply(const uint32_t* w, const Identity& id, Sink& s) {
	if ((w[0] >> 28) != 0xf) return false;
	const uint16_t status = static_cast<uint16_t>((w[0] >> 16) & 0x3ff);
	if (status == endpoint_discovery) {
		const uint8_t filter = static_cast<uint8_t>(w[1]);
		if (filter & 0x01) {
			const uint32_t r[4] = {head(0, endpoint_info, 0x0101), 0x80000000u | 1u << 24 | 0x300u, 0, 0};
			s.ump4(r);
		}
		if (filter & 0x02) {
			const uint32_t r[4] = {head(0, device_identity, 0),
			    static_cast<uint32_t>(id.sysex_id[0]) << 16 | static_cast<uint32_t>(id.sysex_id[1]) << 8 | id.sysex_id[2],
			    static_cast<uint32_t>(id.family & 0x7f) << 24 | static_cast<uint32_t>(id.family >> 7 & 0x7f) << 16 |
			        static_cast<uint32_t>(id.model & 0x7f) << 8 | (id.model >> 7 & 0x7f),
			    static_cast<uint32_t>(id.revision[0]) << 24 | static_cast<uint32_t>(id.revision[1]) << 16 |
			        static_cast<uint32_t>(id.revision[2]) << 8 | id.revision[3]};
			s.ump4(r);
		}
		if (filter & 0x04) text(s, endpoint_name, 2, 0, id.name);
		if (filter & 0x08) text(s, product_instance_id, 2, 0, id.product_id);
		if (filter & 0x10) configuration(s, id);
		return true;
	}
	if (status == stream_config_request) {
		configuration(s, id);
		return true;
	}
	if (status == function_block_discovery) {
		const uint8_t block = static_cast<uint8_t>(w[0] >> 8), filter = static_cast<uint8_t>(w[0]);
		if (block != 0 && block != 0xff) return true; // (only block 0)
		if (filter & 0x01) {
			// active, block 0, UI hint 3 (sender and receiver), not a MIDI 1.0 port, bidirectional;
			// group 1, 1 group, no MIDI-CI, no SysEx8 streams
			const uint32_t r[4] = {head(0, function_block_info, 0x8000 | 3 << 4 | 3), 0x00010000u, 0, 0};
			s.ump4(r);
		}
		if (filter & 0x02) text(s, function_block_name, 1, 0, id.name);
		return true;
	}
	return true; // (the notifications of another endpoint, clips: nothing to answer)
}

} // namespace ump_stream
