// One MIDI file through the firmware's path (USB MIDI in on the virtual board: PExReceive ->
// MidiInMsgHandler -> the control thread -> synth_control's MIDI note input -> the synth thread's
// controls) and through the SDK's shell (sdk::Shell, its MidiVoices): both run the one mapping,
// striso_dsp/midi_in.h (the notes, the instrument's messages) and voice_allocator.h (the voices),
// so the engine controls are the same, bit for bit, every time the notes have settled after an
// event (the strikes faded). The expressive performance: MPE notes with their bend, pressure and
// timbre, the master channel's notes with the mod wheel and poly pressure, the instrument's bend,
// a new bend range, the pedal, the releases. The crowded one: more notes than voices, so released
// and held voices are taken by the allocator's rule.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <span>
#include <vector>

#include "board.h"
#include "doctest/doctest.h"
#include "striso_sdk/shell.h"
#include "striso_sdk/smf.h"

const striso::sdk::EngineEntry& striso_engine_strisy();

namespace {

namespace sdk = striso::sdk;

constexpr int SR = 48000, NV = 10, SETTLE_MS = 40;

const sdk::MidiEvent expressive[] = {
    // MPE notes: a strike, the channel's pressure, timbre and bend
    {0.10, {0x91, 60, 100}, 3},
    {0.15, {0xd1, 90, 0}, 2},
    {0.20, {0xb1, 74, 80}, 3},
    {0.25, {0xe1, 0x00, 0x48}, 3},
    {0.30, {0x92, 64, 70}, 3},
    {0.30, {0x93, 67, 110}, 3},
    {0.35, {0xd3, 30, 0}, 2},
    {0.40, {0xe2, 0x7f, 0x3f}, 3},
    // the master channel: a keyboard, no aftertouch at first, then the mod wheel, poly pressure
    {0.50, {0x90, 48, 64}, 3},
    {0.50, {0x90, 55, 64}, 3},
    {0.55, {0xb0, 1, 100}, 3},
    {0.60, {0xa0, 55, 120}, 3},
    // the instrument's bend, the notes' bend range on a member channel, the pedal
    {0.70, {0xe0, 0x00, 0x60}, 3},
    {0.75, {0xb1, 101, 0}, 3},
    {0.75, {0xb1, 100, 0}, 3},
    {0.75, {0xb1, 6, 12}, 3},
    {0.80, {0xe1, 0x00, 0x30}, 3},
    {0.85, {0xb0, 64, 127}, 3},
    // releases with velocities, re-strikes of the same keys, more notes
    {0.90, {0x81, 60, 50}, 3},
    {0.95, {0x80, 48, 0}, 3},
    {1.00, {0x94, 72, 127}, 3},
    {1.00, {0x95, 74, 20}, 3},
    {1.05, {0x90, 50, 90}, 3},
    {1.10, {0x96, 76, 100}, 3},
    {1.15, {0xd6, 127, 0}, 2},
    {1.20, {0xb6, 74, 0}, 3},
    {1.30, {0x82, 64, 0}, 3},
    {1.30, {0x83, 67, 127}, 3},
    {1.35, {0x84, 72, 1}, 3},
    {1.35, {0x85, 74, 64}, 3},
    {1.40, {0x90, 55, 0}, 3}, // (a note on with velocity 0: off)
    {1.40, {0x80, 50, 30}, 3},
    {1.45, {0x86, 76, 90}, 3},
    {1.50, {0xb0, 64, 0}, 3},
    {1.50, {0xe0, 0x00, 0x40}, 3},
};

// 16 notes over 10 voices: a chord held while others come and go, then more held notes than voices
const sdk::MidiEvent crowded[] = {
    {0.10, {0x91, 48, 120}, 3}, {0.10, {0x92, 52, 110}, 3}, {0.10, {0x93, 55, 100}, 3}, {0.10, {0x94, 59, 90}, 3},
    {0.20, {0x95, 62, 80}, 3}, {0.20, {0x96, 65, 70}, 3}, {0.20, {0x97, 67, 60}, 3},
    {0.30, {0x98, 69, 50}, 3}, {0.30, {0x99, 71, 40}, 3}, {0.30, {0x9a, 72, 30}, 3},
    {0.40, {0x85, 62, 20}, 3}, {0.45, {0x86, 65, 64}, 3},  // two released
    {0.50, {0x9b, 74, 100}, 3},                            // takes the quieter released voice
    {0.55, {0x9c, 76, 127}, 3},                            // the other
    {0.60, {0x9d, 77, 127}, 3},                            // all held: steals the quietest held one
    {0.65, {0x9e, 79, 5}, 3},                              // quieter than every held note: no voice
    {0.70, {0xd2, 127, 0}, 2},                             // a held note gets louder
    {0.75, {0x9f, 81, 90}, 3},
    {0.80, {0x81, 48, 0}, 3}, {0.80, {0x84, 59, 0}, 3},
    {0.85, {0x95, 62, 100}, 3},                            // a released key struck again
    {1.00, {0x82, 52, 0}, 3}, {1.00, {0x83, 55, 0}, 3}, {1.00, {0x87, 67, 0}, 3}, {1.00, {0x88, 69, 0}, 3},
    {1.00, {0x89, 71, 0}, 3}, {1.00, {0x8a, 72, 0}, 3}, {1.00, {0x8b, 74, 0}, 3}, {1.00, {0x8c, 76, 0}, 3},
    {1.00, {0x8d, 77, 0}, 3}, {1.00, {0x8e, 79, 0}, 3}, {1.00, {0x8f, 81, 0}, 3}, {1.00, {0x85, 62, 0}, 3},
};

// USB-MIDI event packets (cable 0): code index = the status's high nibble
std::vector<uint8_t> packets(const std::vector<sdk::MidiEvent>& msgs) {
	std::vector<uint8_t> p;
	for (const sdk::MidiEvent& m : msgs) {
		p.push_back(static_cast<uint8_t>(m.bytes[0] >> 4));
		for (int i = 0; i < 3; i++) p.push_back(i < m.len ? m.bytes[i] : 0);
	}
	return p;
}

void compare(std::span<const sdk::MidiEvent> performance, const char* path) {
	// the file, written and read back: what both get
	const std::vector<uint8_t> bytes = sdk::make_midi(performance);
	FILE* f = std::fopen(path, "wb");
	REQUIRE(f != nullptr);
	std::fwrite(bytes.data(), 1, bytes.size(), f);
	std::fclose(f);
	const sdk::MidiFile file = sdk::read_midi_file(path);
	std::remove(path);
	REQUIRE(file.error.empty());
	REQUIRE(file.events.size() == performance.size());
	std::map<double, std::vector<sdk::MidiEvent>> groups; // the events of one moment
	for (const sdk::MidiEvent& e : file.events) groups[e.t].push_back(e);

	vt::Board bd;
	REQUIRE(sv_select_voice(bd.b, 0) == 0); // strisy (the engine doesn't matter: the controls)
	bd.run_ms(SETTLE_MS);
	sdk::Shell shell(striso_engine_strisy(), SR, NV, false);
	std::vector<float> l(SR * SETTLE_MS / 1000), r(l.size());
	shell.process(l.data(), r.data(), static_cast<int>(l.size()));

	int moment = 0, compared = 0, sounding = 0;
	for (const auto& [t, msgs] : groups) {
		CAPTURE(t);
		const std::vector<uint8_t> p = packets(msgs);
		REQUIRE(sv_midi_write(bd.b, p.data(), static_cast<int32_t>(p.size())) == static_cast<int32_t>(p.size()));
		bd.run_ms(SETTLE_MS);
		for (const sdk::MidiEvent& m : msgs) shell.midi(m.bytes, m.len);
		shell.process(l.data(), r.data(), static_cast<int>(l.size()));

		sv_state st{};
		st.size = sizeof st;
		REQUIRE(sv_get_state(bd.b, &st) >= 0);
		for (int v = 0; v < NV; v++) {
			const striso::dsp::VoiceControls& c = shell.controls()[v];
			const float mine[5] = {c.note, c.pres, c.vpres, c.but_x, c.but_y};
			CAPTURE(moment);
			CAPTURE(v);
			CAPTURE(st.controls[v][0]);
			CAPTURE(mine[0]);
			CAPTURE(st.controls[v][1]);
			CAPTURE(mine[1]);
			CHECK(std::memcmp(st.controls[v], mine, sizeof mine) == 0);
			sounding += mine[1] > 0.0f;
			compared++;
		}
		CHECK(std::memcmp(&st.globals[7], &shell.globals().pedal, sizeof(float)) == 0);
		moment++;
	}
	CHECK(compared == NV * static_cast<int>(groups.size()));
	CHECK(sounding > 20); // (the notes did sound)
	MESSAGE(moment << " moments, " << compared << " voice controls compared, " << sounding << " of them sounding");
}

} // namespace

TEST_CASE("midi in: one MIDI file through the firmware and through the SDK's shell gives the same engine controls") { compare(expressive, "striso_virtual_midi_expressive.mid"); }

TEST_CASE("midi in: more notes than voices: the firmware and the shell take the same voices") { compare(crowded, "striso_virtual_midi_crowded.mid"); }
