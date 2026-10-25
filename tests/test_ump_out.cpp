// MIDI 2.0 out (striso_dsp/ump_out.h, the board's output edge): the Striso profile's notes as
// standard MIDI 2.0, the clamps, the note numbers, only what changes, the exact values.
#include <cstdint>
#include <utility>
#include <vector>

#include "doctest/doctest.h"
#include "awva/ump.h"
#include "striso_dsp/ump_out.h"

namespace {
namespace ump = awva::ump;
} // namespace

// --- MIDI 2.0 out (ump_out.h) ------------------------------------------------------------------

namespace {

struct Sink {
	std::vector<std::pair<uint32_t, uint32_t>> m;
	void ump(uint32_t w0, uint32_t w1) { m.emplace_back(w0, w1); }
};

// the key's packets, as synth_control's update_synth_voice sends them
template <class Out> void key(Out& o, int id, float note, float pres, float vpres, float x, float y) {
	o.packet(ump::pitch_packet(id, note));
	o.packet(ump::pres_packet(id, pres));
	o.packet(ump::signed_packet(id, ump::ctl_vpres, vpres));
	o.packet(ump::signed_packet(id, ump::ctl_x, x));
	o.packet(ump::signed_packet(id, ump::ctl_y, y));
}

uint32_t std_head(uint8_t status, uint8_t number, uint8_t index) {
	return 0x40000000u | static_cast<uint32_t>(status) << 20 | static_cast<uint32_t>(number) << 8 | index;
}

} // namespace

TEST_CASE("ump out: standard values are clamped to their ranges") {
	CHECK(ump::std_unsigned(1.0f) == 0xffffffffu);
	CHECK(ump::std_unsigned(3.5f) == 0xffffffffu); // (inside: room up to 4.0)
	CHECK(ump::std_unsigned(-0.1f) == 0u);
	CHECK(ump::std_unsigned(0.5f) == 0x80000000u);
	CHECK(ump::std_bipolar(0.0f) == 0x80000000u);
	CHECK(ump::std_bipolar(-1.0f) == 0u);
	CHECK(ump::std_bipolar(-2.0f) == 0u);
	CHECK(ump::std_bipolar(1.0f) == 0xffffffffu);
	CHECK(ump::std_bipolar(-0.5f) == 0x40000000u);
	CHECK(ump::std_velocity(1.0f) == 0xffff);
	CHECK(ump::std_velocity(-0.3f) == 0);
	CHECK(ump::std_pitch(60.0f) == 60u << 25);
	CHECK(ump::std_pitch(-48.0f) == 0u);          // MPE's pitches below 0 ...
	CHECK(ump::std_pitch(175.0f) == 0xffffffffu); // ... and above 128 clamp at the edge only
}

TEST_CASE("ump out: a key's note in standard MIDI 2.0, then only what changes") {
	ump::StandardOut<68> o;
	Sink s;
	key(o, 5, 61.25f, 2.0f, 0.5f, -0.25f, 0.75f);
	o.packet(ump::note_on_packet(5, 0.5f, false));
	o.flush(s);
	const uint32_t pitch = 0x7a800000u; // 61.25 x 2^25
	REQUIRE(s.m.size() == 6);
	CHECK(s.m[0] == std::make_pair(std_head(0x9, 61, 3), 0x8000u << 16 | (pitch >> 16))); // velocity 0.5, Pitch 7.9
	CHECK(s.m[1] == std::make_pair(std_head(0x0, 61, 3), pitch));                         // Pitch 7.25
	CHECK(s.m[2] == std::make_pair(std_head(0xa, 61, 0), 0xffffffffu));                   // pres 2.0: full
	CHECK(s.m[3] == std::make_pair(std_head(0x0, 61, 74), ump::std_bipolar(0.75f)));      // y: brightness
	CHECK(s.m[4] == std::make_pair(std_head(0x1, 61, 3), ump::std_bipolar(-0.25f)));      // x
	CHECK(s.m[5] == std::make_pair(std_head(0x1, 61, 2), ump::std_bipolar(0.5f)));        // vpres
	CHECK(o.number(5) == 61);

	s.m.clear();
	key(o, 5, 61.25f, 2.0f, 0.5f, -0.25f, 0.75f); // the same again: nothing
	o.flush(s);
	CHECK(s.m.empty());
	key(o, 5, 61.25f, 0.5f, 0.5f, -0.25f, 0.75f);
	o.flush(s);
	REQUIRE(s.m.size() == 1);
	CHECK(s.m[0] == std::make_pair(std_head(0xa, 61, 0), 0x80000000u));

	// another key of the same 12-TET note (C# and Db): the nearest free number
	s.m.clear();
	key(o, 9, 61.0f, 0.2f, 0.3f, 0.0f, 0.0f);
	o.packet(ump::note_on_packet(9, 0.3f, false));
	o.flush(s);
	CHECK(o.number(9) == 60);

	// the release: Note Off with the release velocity; the number is free again
	s.m.clear();
	key(o, 5, 61.25f, 0.0f, -0.4f, -0.25f, 0.75f);
	o.packet(ump::note_off_packet(5));
	o.flush(s);
	REQUIRE(s.m.size() == 3);
	CHECK(s.m[0].first == std_head(0xa, 61, 0));
	CHECK(s.m[2] == std::make_pair(std_head(0x8, 61, 0), static_cast<uint32_t>(ump::std_velocity(0.4f)) << 16));
	CHECK(o.number(5) == -1);
}

TEST_CASE("ump out: the exact values (the float32 escape) are converted too") {
	ump::StandardOut<8> o;
	Sink s;
	const float avg = 0.0015899f;
	REQUIRE((ump::signed_packet(2, ump::ctl_y, avg).w0 & 0xff) == ump::exact_y);
	key(o, 2, 0.1f, 0.25f, 0.0f, 0.0f, avg); // 0.1: an exact pitch too
	o.packet(ump::note_on_packet(2, 0.0f, false));
	o.flush(s);
	REQUIRE(s.m.size() == 6);
	CHECK(s.m[1].second == ump::std_pitch(0.1f));
	CHECK(s.m[3].second == ump::std_bipolar(avg));
}
