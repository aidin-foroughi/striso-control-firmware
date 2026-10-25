// The Striso UMP profile (striso_dsp/ump.h) on its own: every value the board and MIDI 1.0 give
// arrives exactly (in the scale, or as an exact value), the decoder's callbacks and their order,
// the touch factors, the strike fade.
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "doctest/doctest.h"
#include "striso_dsp/ump.h"

namespace {

namespace ump = striso::ump;

bool same(float a, float b) { return std::memcmp(&a, &b, sizeof a) == 0; }

// the index byte of a packet (an assignable controller's number)
int index_of(const ump::Packet& p) { return static_cast<int>(p.w0 & 0xff); }

// a host that logs the decoder's calls
struct Host {
	ump::Touch t{1.0f, 1.0f, 1.0f};
	std::vector<std::string> log;
	ump::NoteSignals last{};
	[[nodiscard]] ump::Touch touch(int /*id*/) const { return t; }
	void note_started(int id, const ump::NoteSignals& /*n*/) { log.push_back("start " + std::to_string(id)); }
	void note_changed(int id, const ump::NoteSignals& n) {
		log.push_back("change " + std::to_string(id));
		last = n;
	}
	void note_silenced(int id) { log.push_back("silence " + std::to_string(id)); }
	void note_dropped(int id) { log.push_back("drop " + std::to_string(id)); }
};

} // namespace

TEST_CASE("ump: every board code is exact in the Striso scale") {
	// the key thread's floats: code / 8191 (synth_control.cpp int2float), int16 codes
	int escaped_pres = 0;
	for (int code = -32768; code <= 32767; code++) {
		const float x = static_cast<float>(code) / 0x1fff;
		const ump::Packet s = ump::signed_packet(5, ump::ctl_x, x);
		REQUIRE(index_of(s) == ump::ctl_x); // in the scale, not an exact value
		REQUIRE(same(ump::signed_of(s.w1), x));
		const ump::Packet p = ump::pres_packet(5, x);
		if (code >= 0) {
			REQUIRE(((p.w0 >> 20) & 0xf) == ump::poly_pressure);
			REQUIRE(same(ump::pres_of(p.w1), x));
		} else {
			escaped_pres++; // (a negative pressure, which no key gives: an exact value)
		}
	}
	CHECK(escaped_pres == 32768);
}

TEST_CASE("ump: MIDI 1.0's values and MPE's pitches are exact in the Striso scale") {
	for (int v = 0; v < 128; v++) {
		const float x = static_cast<float>(v) * (1.0f / 127.0f);
		CHECK(ump::pres_of(ump::pres_value(x)) == x);
		CHECK(ump::signed_of(ump::signed_value(x)) == x);
		CHECK(ump::signed_of(ump::signed_value(-x)) == -x);
	}
	// a member channel's notes bent by 48 semitones: -48 .. 175 (beyond MIDI's 0 .. 127)
	for (int key = 0; key < 128; key += 7)
		for (int b = 0; b < 16384; b += 37) {
			const float bend = static_cast<float>(b - 0x2000) * (1.0f / 8192) * 48.0f;
			const float note = static_cast<float>(key) + bend;
			CHECK(ump::pitch_of(ump::pitch_value(note)) == note);
		}
}

TEST_CASE("ump: a value the scale can't hold goes as an exact value (its float bits)") {
	const float avg = 0.0015899f; // a weighted average (portamento): more bits than the scale
	const ump::Packet p = ump::signed_packet(3, ump::ctl_y, avg);
	CHECK(index_of(p) == ump::exact_y);
	CHECK(p.w1 == ump::float_bits(avg));
	ump::Decoder<8> d;
	Host h;
	d.packet(ump::note_on_packet(3, 0.5f, false));
	d.packet(p);
	d.flush(h);
	CHECK(same(h.last.but_y, avg));
	CHECK(index_of(ump::pitch_packet(3, 60.0000001f)) == ump::ctl_pitch); // (the float is 60)
	CHECK(index_of(ump::pitch_packet(3, 0.1f)) == ump::exact_pitch);     // 0.1 needs 2^-27
}

TEST_CASE("ump: a packet's length from its message type") {
	CHECK(ump::words(0x20903c40u) == 1);
	CHECK(ump::words(0x40903c00u) == 2);
	CHECK(ump::words(0x30000000u) == 2);
	CHECK(ump::words(0x50000000u) == 4);
	CHECK(ump::words(0xd0000000u) == 4);
	CHECK(ump::words(0xb0000000u) == 3);
}

TEST_CASE("ump: the decoder calls its host once per changed note, in the order they changed") {
	ump::Decoder<8> d;
	Host h;
	d.packet(ump::pres_packet(2, 0.25f));
	d.packet(ump::note_on_packet(2, 0.5f, false));
	d.packet(ump::pitch_packet(4, 62.0f));
	d.packet(ump::note_on_packet(4, 0.5f, false));
	d.packet(ump::signed_packet(2, ump::ctl_x, 0.5f));
	d.flush(h);
	CHECK(h.log == std::vector<std::string>{"start 2", "change 2", "start 4", "change 4"});
	h.log.clear();
	d.flush(h);
	CHECK(h.log.empty());

	// the id taken by a newer note: its sound ends, the new one starts
	d.packet(ump::management_packet(2, ump::PNM_SILENCE));
	d.packet(ump::note_on_packet(2, 1.0f, false));
	d.packet(ump::management_packet(4, ump::PNM_DROP));
	d.flush(h);
	CHECK(h.log == std::vector<std::string>{"silence 2", "start 2", "change 2", "drop 4"});
	CHECK_FALSE(d.on(4));
	h.log.clear();

	d.packet(ump::note_off_packet(2));
	d.flush(h);
	CHECK(h.log == std::vector<std::string>{"change 2"});
	CHECK_FALSE(h.last.on);

	// not the profile's: other message types, ids past the decoder's
	d.packet(0x20903c40u, 0);
	d.packet(ump::note_on_packet(9, 1.0f, false));
	h.log.clear();
	d.flush(h);
	CHECK(h.log.empty());
}

TEST_CASE("ump: the touch factors, in the board's float expressions") {
	ump::Decoder<8> d;
	Host h;
	h.t = {1.3f, 0.7f, 2.0f};
	const float pres = 4000.0f / 0x1fff, vpres = 3000.0f / 0x1fff, x = -0.25f, y = 6000.0f / 0x1fff;
	d.packet(ump::pres_packet(1, pres));
	d.packet(ump::signed_packet(1, ump::ctl_vpres, vpres));
	d.packet(ump::signed_packet(1, ump::ctl_x, x));
	d.packet(ump::signed_packet(1, ump::ctl_y, y));
	d.packet(ump::note_on_packet(1, vpres, false));
	d.flush(h);
	CHECK(same(h.last.pres, pres * 1.3f));
	CHECK(same(h.last.vpres, vpres * 0.7f));
	CHECK(same(h.last.but_x, x));
	CHECK(h.last.but_y == 1.0f); // y x 2 clamped
	CHECK_FALSE(d.fading());     // (not a strike)
}

TEST_CASE("ump: a strike's vpres fades x0.6 per tick while above 0.01, then 0") {
	ump::Decoder<8> d;
	Host h;
	d.packet(ump::signed_packet(0, ump::ctl_vpres, 1.0f));
	d.packet(ump::note_on_packet(0, 1.0f, true));
	d.flush(h);
	CHECK(d.fading());
	float v = 1.0f;
	int ticks = 0;
	while (d.fading()) {
		d.tick(h);
		v = v > 0.01f ? v * 0.6f : 0.0f;
		REQUIRE(same(h.last.vpres, v));
		REQUIRE(++ticks < 100);
	}
	CHECK(v == 0.0f);
	CHECK(ticks == 11);
}
