// MIDI 2.0 in on the virtual board (sv_ump_write -> UmpInMsgHandler -> the key thread ->
// midi_in.h's message2 -> the Striso profile's decoder -> the synth thread's controls), against
// the same performance as MPE bytes (sv_midi_write). Where MPE can say a value, the standard
// MIDI 2.0 message carrying that value gives the same engine controls, bit for bit; where it
// can't (16-bit velocities, 32-bit pressures, fine bends, a note's own pitch), the controls follow
// the finer value.
#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <vector>

#include "board.h"
#include "doctest/doctest.h"

using vt::Board;

namespace {

// USB-MIDI event packets (cable 0): code index = the status's high nibble
std::vector<uint8_t> packets(std::initializer_list<std::vector<uint8_t>> msgs) {
	std::vector<uint8_t> p;
	for (const auto& m : msgs) {
		p.push_back(static_cast<uint8_t>(m[0] >> 4));
		for (size_t i = 0; i < 3; i++) p.push_back(i < m.size() ? m[i] : 0);
	}
	return p;
}

// MIDI 2.0 channel voice messages (UMP type 4, group 0)
struct Ump {
	std::vector<uint32_t> w;
	Ump& msg(uint8_t status, uint8_t ch, uint8_t b3, uint8_t b4, uint32_t data) {
		w.push_back(0x40000000u | static_cast<uint32_t>(status | ch) << 16 | static_cast<uint32_t>(b3) << 8 | b4);
		w.push_back(data);
		return *this;
	}
	Ump& note_on(uint8_t ch, uint8_t key, uint16_t velo) { return msg(0x90, ch, key, 0, static_cast<uint32_t>(velo) << 16); }
	Ump& note_off(uint8_t ch, uint8_t key, uint16_t velo) { return msg(0x80, ch, key, 0, static_cast<uint32_t>(velo) << 16); }
	Ump& poly_pressure(uint8_t ch, uint8_t key, uint32_t v) { return msg(0xa0, ch, key, 0, v); }
	Ump& pressure(uint8_t ch, uint32_t v) { return msg(0xd0, ch, 0, 0, v); }
	Ump& cc(uint8_t ch, uint8_t c, uint32_t v) { return msg(0xb0, ch, c, 0, v); }
	Ump& bend(uint8_t ch, uint32_t v) { return msg(0xe0, ch, 0, 0, v); }
	Ump& registered(uint8_t ch, uint8_t bank, uint8_t index, uint32_t v) { return msg(0x20, ch, bank, index, v); }
	Ump& note_pitch(uint8_t ch, uint8_t key, double semitones) { // Registered Per-Note Controller 3
		return msg(0x00, ch, key, 3, static_cast<uint32_t>(std::llround(semitones * 33554432.0)));
	}
};

// the standard MIDI 2.0 value of MIDI 1.0's v / 127 (unsigned, full scale 2^32 - 1)
uint32_t u32(int v) { return static_cast<uint32_t>(std::llround(v / 127.0 * 4294967295.0)); }
// a bipolar value x (-1 .. 1, the centre 0x80000000), each side to its own end
uint32_t bipolar(double x) {
	return static_cast<uint32_t>(0x80000000LL + std::llround(x * (x <= 0 ? 2147483648.0 : 2147483647.0)));
}
// MIDI 1.0's 14-bit bend v as the same bipolar value
uint32_t bend14(int v) { return bipolar((v - 0x2000) / 8192.0); }

sv_state state(Board& bd) {
	sv_state s{};
	s.size = sizeof s;
	REQUIRE(sv_get_state(bd.b, &s) == 0);
	return s;
}

void midi(Board& bd, const std::vector<uint8_t>& p) {
	REQUIRE(sv_midi_write(bd.b, p.data(), static_cast<int32_t>(p.size())) == static_cast<int32_t>(p.size()));
}
// the same USB-MIDI packets as UMP MIDI 1.0 channel voice messages (type 2)
std::vector<uint32_t> midi1_ump(const std::vector<uint8_t>& p) {
	std::vector<uint32_t> w;
	for (size_t i = 0; i + 3 < p.size(); i += 4)
		w.push_back(0x20000000u | static_cast<uint32_t>(p[i + 1]) << 16 | static_cast<uint32_t>(p[i + 2]) << 8 | p[i + 3]);
	return w;
}
void ump(Board& bd, const Ump& u) {
	REQUIRE(sv_ump_write(bd.b, u.w.data(), static_cast<int32_t>(u.w.size())) == static_cast<int32_t>(u.w.size()));
}

// the voice playing the note nearest to `note`
int voice_near(const sv_state& s, float note) {
	int best = 0;
	for (int v = 1; v < s.voices && v < static_cast<int>(std::size(s.controls)); v++)
		if (std::fabs(s.controls[v][0] - note) < std::fabs(s.controls[best][0] - note)) best = v;
	return best;
}

void same_controls(Board& a, Board& b, const char* when) {
	const sv_state sa = state(a), sb = state(b);
	INFO(when);
	for (size_t v = 0; v < std::size(sa.controls); v++)
		for (int c = 0; c < 5; c++) {
			INFO("voice " << v << " control " << c);
			CHECK(std::memcmp(&sa.controls[v][c], &sb.controls[v][c], sizeof(float)) == 0);
		}
	CHECK(std::memcmp(sa.globals, sb.globals, sizeof sa.globals) == 0);
}

} // namespace

TEST_CASE("ump in: the API version is 2 and sv_ump_write checks its arguments") {
	Board bd;
	CHECK(sv_api_version() == 2);
	const uint32_t w[2] = {0x40903c00u, 0xffff0000u};
	CHECK(sv_ump_write(bd.b, nullptr, 2) == SV_EINVAL);
	CHECK(sv_ump_write(bd.b, w, -1) == SV_EINVAL);
	CHECK(sv_ump_write(nullptr, w, 2) == SV_EINVAL);
	CHECK(sv_ump_write(bd.b, w, 0) == 0);
}

TEST_CASE("ump in: what MPE can say, standard MIDI 2.0 says the same, bit for bit") {
	Board a, b, c; // MPE bytes, MIDI 2.0, the MPE bytes in UMP (type 2)
	for (Board* bd : {&a, &b, &c}) {
		REQUIRE(sv_select_voice(bd->b, 0) == 0); // strisy
		bd->run_ms(200);
	}
	// the bend range (RPN 0 on a member channel), two MPE notes with pressure, bend and timbre,
	// the master channel's note with poly pressure and the mod wheel, the instrument's bend, the
	// pedal. (Velocity 127: MIDI 1.0's 7 bits say 1.0 as 16 bits do; pressures avoid MIDI 1.0's
	// eight values whose float product v * (1/127) is a step off v / 127, the 32-bit value's.)
	const std::vector<uint8_t> mpe = packets({{0xb1, 101, 0}, {0xb1, 100, 0}, {0xb1, 6, 24}, {0x91, 60, 127}, {0xd1, 90},
	    {0xe1, 0x52, 0x49}, {0xb1, 74, 80}, {0x92, 64, 127}, {0xd2, 37}, {0xe2, 0x48, 0x28}, {0xb2, 74, 20}, {0x90, 67, 127},
	    {0xa0, 67, 50}, {0xb0, 1, 100}, {0xe0, 0x74, 0x43}, {0xb0, 64, 127}});
	midi(a, mpe);
	ump(c, Ump{midi1_ump(mpe)});
	ump(b, Ump()
	           .registered(1, 0, 0, 24u << 25)
	           .note_on(1, 60, 0xffff)
	           .pressure(1, u32(90))
	           .bend(1, bend14(0x49 << 7 | 0x52))
	           .cc(1, 74, bipolar((80 - 64) / 64.0))
	           .note_on(2, 64, 0xffff)
	           .pressure(2, u32(37))
	           .bend(2, bend14(0x28 << 7 | 0x48))
	           .cc(2, 74, bipolar((20 - 64) / 64.0))
	           .note_on(0, 67, 0xffff)
	           .poly_pressure(0, 67, u32(50))
	           .cc(0, 1, u32(100))
	           .bend(0, bend14(0x43 << 7 | 0x74))
	           .cc(0, 64, 0xffffffffu));
	a.run_ms(200);
	b.run_ms(200);
	c.run_ms(200);
	same_controls(a, b, "held");
	same_controls(a, c, "held, MIDI 1.0 in UMP");
	const sv_state held = state(b);
	CHECK(held.controls[voice_near(held, 60)][1] > 0.5f); // they play

	const std::vector<uint8_t> offs = packets({{0x81, 60, 0}, {0x82, 64, 0}, {0x80, 67, 0}});
	midi(a, offs);
	ump(b, Ump().note_off(1, 60, 0).note_off(2, 64, 0).note_off(0, 67, 0));
	ump(c, Ump{midi1_ump(offs)});
	a.run_ms(100);
	b.run_ms(100);
	c.run_ms(100);
	same_controls(a, b, "released");
	same_controls(a, c, "released, MIDI 1.0 in UMP");
}

TEST_CASE("ump in: what MPE can't say, the controls follow (16-bit velocity, 32-bit pressure, fine bend, a note's pitch)") {
	Board bd;
	REQUIRE(sv_select_voice(bd.b, 0) == 0);
	bd.run_ms(200);

	// a 16-bit velocity: the note's pres until pressure comes; no 7-bit velocity is it
	ump(bd, Ump().note_on(1, 60, 0x8001));
	bd.run_ms(100);
	sv_state s = state(bd);
	int v = voice_near(s, 60.0f);
	const float velo = static_cast<float>(0x8001 / 65535.0);
	CHECK(s.controls[v][1] == velo);
	CHECK(std::fabs(velo - 64 / 127.0f) > 1e-3f);

	// a 32-bit pressure
	ump(bd, Ump().pressure(1, 0x12345678u));
	bd.run_ms(50);
	s = state(bd);
	CHECK(s.controls[v][1] == static_cast<float>(0x12345678u / 4294967295.0));
	CHECK(std::fabs(s.controls[v][1] * 127.0f - std::round(s.controls[v][1] * 127.0f)) > 1e-3f);

	// a bend finer than MIDI 1.0's 14 bits (48 semitones: 0.0059 a step)
	ump(bd, Ump().bend(1, 0x80000000u + 12345u));
	bd.run_ms(50);
	s = state(bd);
	const double fine = 12345.0 / 2147483647.0 * 48.0;
	CHECK(std::fabs(s.controls[v][0] - (60.0 + fine)) < 1e-5);
	CHECK(fine < 48.0 / 8192 / 4); // MPE's nearest bends: 60 or 60.0059

	// a note's own pitch (Registered Per-Note Controller 3, Pitch 7.25), not its key's
	ump(bd, Ump().note_on(2, 61, 0xffff).note_pitch(2, 61, 61.3));
	bd.run_ms(50);
	s = state(bd);
	const int w = voice_near(s, 61.3f);
	CHECK(s.controls[w][0] == static_cast<float>(std::llround(61.3 * 33554432.0) / 33554432.0));

	ump(bd, Ump().note_off(1, 60, 0).note_off(2, 61, 0));
	bd.run_ms(100);
}
