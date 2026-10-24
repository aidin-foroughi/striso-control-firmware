// MIDI note input end to end on the virtual board: USB MIDI packets in through sv_midi_write (the
// board's PExReceive -> MidiInMsgHandler -> the key thread -> synth_control's MIDI note input ->
// the internal synth), rendered by the board's synth thread: they sound, fall silent after the
// note-off, and send nothing back out as MIDI.
#include <cmath>
#include <cstdint>
#include <vector>

#include "board.h"
#include "doctest/doctest.h"

using vt::Board;

namespace {

constexpr int SR = 48000;

double rms(const std::vector<float>& x) {
	double s = 0;
	for (float v : x) s += static_cast<double>(v) * v;
	return x.empty() ? 0 : std::sqrt(s / static_cast<double>(x.size()));
}

std::vector<float> render(Board& bd, double seconds) {
	std::vector<float> out(static_cast<size_t>(seconds * SR) * 2);
	REQUIRE(sv_render(bd.b, out.data(), static_cast<int32_t>(out.size() / 2)) == static_cast<int32_t>(out.size() / 2));
	return out;
}

// USB-MIDI event packets (cable 0): code index = the status's high nibble
std::vector<uint8_t> packets(std::initializer_list<std::vector<uint8_t>> msgs) {
	std::vector<uint8_t> p;
	for (const auto& m : msgs) {
		p.push_back(static_cast<uint8_t>(m[0] >> 4));
		for (size_t i = 0; i < 3; i++) p.push_back(i < m.size() ? m[i] : 0);
	}
	return p;
}

// the note messages the board sent (sv_midi_read) since the last call
int notes_out(Board& bd) {
	uint8_t midi[4096];
	int n, notes = 0;
	while ((n = sv_midi_read(bd.b, midi, sizeof midi)) > 0)
		for (int i = 0; i + 3 < n; i += 4) notes += (midi[i + 1] & 0xe0) == 0x80;
	return notes;
}

} // namespace

TEST_CASE("midi in: MPE and one-channel notes through sv_midi_write sound, then fall silent; nothing goes out") {
	Board bd;
	REQUIRE(sv_select_voice(bd.b, 0) == 0); // strisy
	render(bd, 0.2);
	notes_out(bd);
	const std::vector<float> before = render(bd, 0.2);

	// MPE: channel 2, its pressure, bend and timbre; one channel: channel 1, no aftertouch
	const std::vector<uint8_t> on = packets({{0x91, 60, 100}, {0xD1, 90}, {0xE1, 0x00, 0x48}, {0xB1, 74, 80}, {0x90, 67, 90}});
	REQUIRE(sv_midi_write(bd.b, on.data(), static_cast<int32_t>(on.size())) == static_cast<int32_t>(on.size()));
	const std::vector<float> held = render(bd, 1.0);
	const std::vector<uint8_t> off = packets({{0x81, 60, 40}, {0x80, 67, 0}});
	REQUIRE(sv_midi_write(bd.b, off.data(), static_cast<int32_t>(off.size())) == static_cast<int32_t>(off.size()));
	render(bd, 10.0);
	const std::vector<float> after = render(bd, 0.5);

	CHECK(rms(before) < 1e-4);
	CHECK(rms(held) > 1e-3);
	CHECK(rms(after) < 1e-4);
	CHECK(notes_out(bd) == 0); // the MIDI notes are not sent back out
	MESSAGE("strisy, two MIDI notes: rms while held " << rms(held));
}

TEST_CASE("midi in: the board's own MIDI out echoed back doesn't play twice") {
	Board bd;
	REQUIRE(sv_select_voice(bd.b, 0) == 0);
	render(bd, 0.2);
	notes_out(bd);
	sv_inject_key(bd.b, 0, 30, 4000, 3000, 0, 0); // a key
	std::vector<float> key_only;
	for (int i = 0; i < 40; i++) {
		sv_inject_key(bd.b, 0, 30, 4000, 100, 0, 0);
		const std::vector<float> x = render(bd, 0.01);
		key_only.insert(key_only.end(), x.begin(), x.end());
	}
	// the same again with its MIDI out written back in (a DAW's MIDI thru)
	Board echo;
	REQUIRE(sv_select_voice(echo.b, 0) == 0);
	render(echo, 0.2);
	notes_out(echo);
	sv_inject_key(echo.b, 0, 30, 4000, 3000, 0, 0);
	std::vector<float> with_echo;
	uint8_t midi[4096];
	for (int i = 0; i < 40; i++) {
		sv_inject_key(echo.b, 0, 30, 4000, 100, 0, 0);
		int n;
		while ((n = sv_midi_read(echo.b, midi, sizeof midi)) > 0) sv_midi_write(echo.b, midi, n);
		const std::vector<float> x = render(echo, 0.01);
		with_echo.insert(with_echo.end(), x.begin(), x.end());
	}
	CHECK(rms(key_only) > 1e-3);
	CHECK(std::fabs(rms(with_echo) - rms(key_only)) < 0.05 * rms(key_only)); // not two voices
	MESSAGE("key alone: rms " << rms(key_only) << ", with its MIDI echoed back: " << rms(with_echo));
}
