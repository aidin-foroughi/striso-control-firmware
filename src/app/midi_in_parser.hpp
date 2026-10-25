/*
 * USB MIDI in: the event packets of one OUT transfer (USB MIDI 1.0, section 4), checked before
 * they reach MidiInMsgHandler. HAL free, tested on the PC (tests/test_console.cpp).
 *
 * - A transfer is taken whole: events are 4 bytes and never span transfers, so the tail of a
 *   transfer that isn't a multiple of 4 is dropped instead of shifting every later event (R9).
 * - An event is dispatched by its code index number (CIN), 7-bit data only (R8). Dropped: reserved
 *   CINs (0, 1), SysEx (4-7, not handled), a channel message whose status doesn't match its CIN,
 *   a single byte (CIN 0xF) that isn't real-time, a status without bit 7, a data byte with bit 7
 *   (e.g. CC7 = 255 would set the volume to 4x).
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace midi_in {

struct Event {
	uint8_t port; // the virtual cable + 1
	uint8_t status, data1, data2;
};

struct MidiInParser {
	// one event packet; false if it is dropped
	static constexpr bool decode(std::span<const uint8_t, 4> e, Event& ev) {
		// data bytes per CIN
		constexpr uint8_t kLen[16] = {0, 0, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1};
		const uint8_t cin = e[0] & 0x0F;
		if (cin < 2 || (cin >= 4 && cin <= 7)) return false;
		if ((e[1] & 0x80) == 0) return false;
		if (cin >= 8 && cin <= 0x0E && (e[1] >> 4) != cin) return false;
		if (cin == 0x0F && e[1] < 0xF8) return false;
		for (int i = 2; i <= kLen[cin]; i++) {
			if ((e[i] & 0x80) != 0) return false;
		}
		ev = Event{static_cast<uint8_t>(((e[0] & 0xF0) >> 4) + 1), e[1], kLen[cin] >= 2 ? e[2] : uint8_t{0},
		    kLen[cin] >= 3 ? e[3] : uint8_t{0}};
		return true;
	}

	// every whole event of one transfer, in order: sink(const Event&) for those that pass
	template <class Sink>
	static void transfer(std::span<const uint8_t> t, Sink&& sink) {
		for (size_t i = 0; i + 4 <= t.size(); i += 4) {
			Event ev{};
			if (decode(t.subspan(i).first<4>(), ev)) sink(ev);
		}
	}
};

} // namespace midi_in
