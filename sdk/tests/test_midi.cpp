// MIDI in on the PC (midi_voices.h: the board's mapping, striso_dsp/midi_in.h, with this host's
// voices) and the MIDI file reader (smf.h).
#include <array>
#include <cmath>

#include "doctest/doctest.h"
#include "striso_sdk/midi_voices.h"
#include "striso_sdk/smf.h"

namespace sdk = striso::sdk;

namespace {

// the voice playing a note (pres > 0 at the note's pitch), -1 if none
int voice_of(const sdk::MidiVoices& m, float note) {
	for (int v = 0; v < m.num_voices(); v++)
		if (m.voices()[v].pres > 0.0f && std::fabs(m.voices()[v].note - note) < 1e-3f) return v;
	return -1;
}

// messages, then one 2 ms step
void send(sdk::MidiVoices& m, std::initializer_list<std::array<uint8_t, 3>> msgs) {
	for (const auto& x : msgs) REQUIRE(m.message(x[0], x[1], x[2]));
	m.step();
}

} // namespace

TEST_CASE("midi in: a note on and off, as the board maps them") {
	sdk::MidiVoices m(10);
	send(m, {{0x90, 60, 100}}); // master channel: not an MPE member
	const int v = voice_of(m, 60.0f);
	REQUIRE(v == 0);
	const striso::dsp::VoiceControls& c = m.voices()[v];
	CHECK(c.vpres == doctest::Approx(100.0f / 127));
	CHECK(c.pres == doctest::Approx(100.0f / 127)); // until pressure comes, the velocity
	send(m, {});                                    // the strike fades
	CHECK(c.vpres == doctest::Approx(0.6f * 100.0f / 127));
	CHECK(c.pres == doctest::Approx(100.0f / 127));
	send(m, {{0xd0, 0, 0}}); // channel pressure 0: held notes stay above 0
	CHECK(c.pres == doctest::Approx(1.0f / 127));
	send(m, {{0xd0, 64, 0}});
	CHECK(c.pres == doctest::Approx(64.0f / 127));
	send(m, {{0x80, 60, 40}});
	CHECK(c.pres == 0.0f);
	CHECK(c.vpres == doctest::Approx(-40.0f / 127));
	CHECK(m.notes_held() == 0);
}

TEST_CASE("midi in: MPE members bend, press and tilt their own notes") {
	sdk::MidiVoices m(10);
	send(m, {{0x91, 60, 90}, {0x92, 64, 90}}); // member channels 2 and 3
	const int a = voice_of(m, 60.0f), b = voice_of(m, 64.0f);
	REQUIRE(a == 0);
	REQUIRE(b == 1);
	send(m, {{0xe1, 0x00, 0x48}}); // channel 2 bend: (0x2400 - 0x2000) / 8192 * 48 = +6 semitones
	CHECK(m.voices()[a].note == doctest::Approx(66.0f));
	CHECK(m.voices()[b].note == doctest::Approx(64.0f));
	send(m, {{0xb2, 74, 127}}); // channel 3 timbre -> y
	CHECK(m.voices()[b].but_y == doctest::Approx(63.0f / 64));
	CHECK(m.voices()[a].but_y == 0.0f);
	send(m, {{0xd2, 32, 0}}); // channel 3 pressure
	CHECK(m.voices()[b].pres == doctest::Approx(32.0f / 127));
	CHECK(m.voices()[a].pres == doctest::Approx(90.0f / 127));
	// the bend range (RPN 0 on a member channel), then the master channel's bend: the instrument's
	send(m, {{0xb1, 101, 0}, {0xb1, 100, 0}, {0xb1, 6, 24}});
	CHECK(m.bend_range() == 24.0f);
	send(m, {{0xe0, 0x00, 0x60}}); // master: (0x3000 - 0x2000) / 8192 * 2 = +1
	CHECK(m.voices()[b].note == doctest::Approx(65.0f));
	// the MCM: no zone, every channel is like the master
	send(m, {{0xb0, 101, 0}, {0xb0, 100, 6}, {0xb0, 6, 0}});
	CHECK(m.members() == 0);
	send(m, {{0xb0, 64, 127}}); // sustain -> pedal
	CHECK(m.globals().pedal == 1.0f);
}

TEST_CASE("midi in: more notes than voices: a released voice first, then none") {
	sdk::MidiVoices m(2);
	send(m, {{0x90, 60, 100}, {0x90, 62, 30}});
	send(m, {{0x80, 60, 0}});
	send(m, {{0x90, 64, 80}}); // takes 60's released voice
	CHECK(voice_of(m, 64.0f) == 0);
	CHECK(voice_of(m, 62.0f) == 1);
	send(m, {{0x90, 65, 100}}); // both held: not played
	CHECK(voice_of(m, 65.0f) < 0);
	CHECK(voice_of(m, 62.0f) == 1);
	CHECK(voice_of(m, 64.0f) == 0);
}

TEST_CASE("midi files: written and read back, tempo map applied") {
	const sdk::MidiEvent ev[] = {
	    {0.0, {0x90, 60, 100}, 3},
	    {0.5, {0xd0, 64, 0}, 2},
	    {1.0, {0x80, 60, 0}, 3},
	};
	const std::vector<uint8_t> bytes = sdk::make_midi(ev);
	const sdk::MidiFile f = sdk::parse_midi(bytes);
	REQUIRE(f.error.empty());
	CHECK(f.format == 0);
	REQUIRE(f.events.size() == 3);
	CHECK(f.events[1].t == doctest::Approx(0.5));
	CHECK(f.events[1].len == 2);
	CHECK(f.events[2].t == doctest::Approx(1.0));
	CHECK(f.events[2].bytes[0] == 0x80);
	// a tempo change to 60 bpm at 0.5 s (480 ticks): the next 480 ticks take 1 s
	std::vector<uint8_t> t = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 1, 0, 1, 0x01, 0xe0, 'M', 'T', 'r', 'k', 0, 0, 0, 0};
	const std::vector<uint8_t> body = {0x00, 0x90, 60, 100, 0x83, 0x60, 0xff, 0x51, 0x03, 0x0f, 0x42, 0x40, 0x83, 0x60, 0x60, 0, 0x00, 0xff, 0x2f, 0x00};
	t[21] = static_cast<uint8_t>(body.size());
	t.insert(t.end(), body.begin(), body.end());
	const sdk::MidiFile g = sdk::parse_midi(t);
	REQUIRE(g.error.empty());
	REQUIRE(g.events.size() == 2);
	CHECK(g.events[1].bytes[0] == 0x90); // running status
	CHECK(g.events[1].bytes[2] == 0);
	CHECK(g.events[1].t == doctest::Approx(1.5));
	CHECK(!sdk::parse_midi(std::vector<uint8_t>{'R', 'I', 'F', 'F'}).error.empty());
}
