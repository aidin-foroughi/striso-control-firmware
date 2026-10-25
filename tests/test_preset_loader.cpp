// PresetLoader (src/synth/preset_loader.hpp) on a board that records what it is told: what a
// settings preset and a tuning set, in which order, and what they leave alone. The firmware's own
// board (synth_control.cpp InstrumentBoard) is checked bit for bit by control_replay.settings.* and
// virtual/tests/transcripts/preset_loads.txt.
#include "preset_loader.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "config_page.hpp"
#include "doctest/doctest.h"

namespace {

struct RecordingBoard {
	std::vector<std::string> log;
	midi_mode_t mode = MIDI_MODE_MPE;
	int flip_dir = 1, channel = 1, voices = 6, cur_tuning = 0, color = 0x00aa00;
	float tuning_off = 0, preset_off = 0;
	std::vector<std::pair<int, float>> keys;

	void say(const std::string& s) { log.push_back(s); }
	void led(uint32_t rgb) { say("led " + std::to_string(rgb)); }
	void message_interval(int ms) { say("interval " + std::to_string(ms)); }
	void motion_interval(int i) { say("motion " + std::to_string(i)); }
	[[nodiscard]] midi_mode_t midi_mode() const { return mode; }
	void set_midi_mode(midi_mode_t m) {
		mode = m;
		say("mode " + std::to_string(m));
	}
	void set_midi2() {
		mode = MIDI_MODE_MPE;
		say("mode midi2");
	}
	void midinote_mode(midinote_mode_t m) { say("midinote " + std::to_string(m)); }
	void jack2(jack2_mode_t m) { say("jack " + std::to_string(m)); }
	void legato(bool on) { say(on ? "legato on" : "legato off"); }
	[[nodiscard]] int flipdir() const { return flip_dir; }
	void flip() {
		flip_dir = -flip_dir;
		say("flip");
	}
	void load_tuning(int n) { say("tuning " + std::to_string(n)); }
	void preset_offset(float s) {
		preset_off = s;
		say("offset");
	}
	void axis_cc(MidiAxis a, bool normal, int cc) { say(std::string(normal ? "normal " : "mpe ") + std::to_string(static_cast<int>(a)) + " " + std::to_string(cc)); }
	void contvelo(int cc) { say("contvelo " + std::to_string(cc)); }
	[[nodiscard]] int channel_offset() const { return channel; }
	void channel_offset(int c) {
		channel = c;
		say("channel " + std::to_string(c));
	}
	void bend_range(int s) { say("bendrange " + std::to_string(s)); }
	[[nodiscard]] int voicecount() const { return voices; }
	void voicecount(int n) {
		voices = n;
		say("voices " + std::to_string(n));
	}
	void channels_changed() { say("channels"); }
	void zero_offset(int v) { say("zero " + std::to_string(v)); }
	void bend_sensitivity(float f) { say("bend " + std::to_string(f)); }
	void pres_sensitivity(float f) { say("pres " + std::to_string(f)); }
	void velo_sensitivity(float f) { say("velo " + std::to_string(f)); }
	void tilt_sensitivity(float f) { say("tilt " + std::to_string(f)); }
	void velo_offset(int v) { say("velooff " + std::to_string(v)); }
	void volume(float v) { say("volume " + std::to_string(v)); }
	void decay(float v) { say("decay " + std::to_string(v)); }
	void pedal_decay(float v) { say("pedaldecay " + std::to_string(v)); }
	void midi_config() { say("midi_config"); }
	[[nodiscard]] int voice_channel(int i) const { return channel + i; }
	void program_change(int ch, int p) { say("program " + std::to_string(ch) + " " + std::to_string(p)); }
	[[nodiscard]] int tuning() const { return cur_tuning; }
	[[nodiscard]] int tuning_color() const { return color; }
	void equal_temperament() { say("12tet"); }
	void generators(float fifth, float octave) { say("generators " + std::to_string(fifth) + " " + std::to_string(octave)); }
	void tuning_offset(float s) { tuning_off = s; }
	void key_offset(int k, float s) { keys.emplace_back(k, s); }
	void set_tuning(int n, int c) {
		cur_tuning = n;
		color = c;
		say("set_tuning " + std::to_string(n) + " " + std::to_string(c));
	}
};

std::vector<std::string> load(const std::vector<std::pair<std::string, std::string>>& recs, int preset, RecordingBoard& b) {
	const std::vector<ConfigParam> page = config_page(recs);
	const settings::ConfigTable cfg(page, settings::defaults());
	PresetLoader<RecordingBoard>(cfg, b).load_preset(preset);
	return b.log;
}

bool has(const std::vector<std::string>& log, const std::string& s) {
	for (const auto& l : log)
		if (l == s) return true;
	return false;
}

} // namespace

TEST_CASE("preset loader: a preset of name and colour only leaves the instrument alone") {
	RecordingBoard b;
	const auto log = load({}, 5, b); // the defaults: presets 2-8 are a name and a colour
	CHECK(log == std::vector<std::string>{"led " + std::to_string(0x003838), "channels", "led " + std::to_string(0x003838)});
}

TEST_CASE("preset loader: preset 1's defaults, in the firmware's order") {
	RecordingBoard b;
	const auto log = load({}, 1, b);
	REQUIRE(log.size() > 10);
	CHECK(log.front() == "led " + std::to_string(0x380000));
	CHECK(log[1] == "interval 1");
	CHECK(has(log, "mode 0"));
	CHECK(has(log, "tuning 0"));
	CHECK(has(log, "voices 6"));
	CHECK(has(log, "midi_config"));
	CHECK(log.back() == "led " + std::to_string(0x380000));
	// the MIDI mode before the CCs (they go to MPE's, the mode set first), the tuning before its offset
	const auto at = [&](const std::string& s) { return std::find(log.begin(), log.end(), s) - log.begin(); };
	CHECK(at("mode 0") < at("mpe 0 121"));
	CHECK(at("tuning 0") < at("offset"));
	CHECK(at("midi_config") == static_cast<long>(log.size()) - 2);
}

TEST_CASE("preset loader: Normal mode's CCs and program change, out of range and malformed values are left alone") {
	RecordingBoard b;
	const auto log = load({{"sP2Mmode", "normal"}, {"iP2Mpres", "10"}, {"iP2MChan", "17"}, {"iP2Mpgm", "5"},
	                          {"fP2presS", "1.2.3"}, {"iP2voice", "4"}, {"fP2volum", "255"}, {"fP2decay", "200"}},
	    2, b);
	CHECK(has(log, "mode 1"));
	CHECK(has(log, "normal 0 10"));
	CHECK_FALSE(has(log, "channel 16")); // MChan 17: out of range
	CHECK_FALSE(has(log, "voices 4"));   // voices only in MPE mode
	CHECK(has(log, "program 1 5"));      // Normal mode: one channel
	CHECK(has(log, "decay 127.000000")); // the decay at most 127
	for (const auto& l : log) {
		CHECK(l.rfind("pres ", 0) != 0);  // presS "1.2.3": malformed
		CHECK(l.rfind("volume", 0) != 0); // volum 255: out of range
	}
}

TEST_CASE("preset loader: MPE's program change goes to channel 1 and every voice channel") {
	RecordingBoard b;
	b.channel = 1;
	const auto log = load({{"sP4Mmode", "mpe"}, {"iP4voice", "3"}, {"iP4Mpgm", "0"}}, 4, b);
	std::vector<std::string> pc;
	for (const auto& l : log)
		if (l.rfind("program", 0) == 0) pc.push_back(l);
	CHECK(pc == std::vector<std::string>{"program 0 0", "program 1 0", "program 2 0", "program 3 0"});
}

TEST_CASE("preset loader: MIDI mode midi2 is MPE on MIDI 1.0 (its voices, its program changes) and MIDI 2.0 out") {
	RecordingBoard b;
	b.channel = 1;
	const auto log = load({{"sP4Mmode", "midi2"}, {"iP4voice", "3"}, {"iP4Mpgm", "0"}}, 4, b);
	CHECK(has(log, "mode midi2"));
	CHECK(b.midi_mode() == MIDI_MODE_MPE);
	std::vector<std::string> pc;
	for (const auto& l : log)
		if (l.rfind("program", 0) == 0) pc.push_back(l);
	CHECK(pc == std::vector<std::string>{"program 0 0", "program 1 0", "program 2 0", "program 3 0"});
}

TEST_CASE("preset loader: flip only when the layout isn't already as the preset wants") {
	RecordingBoard b;
	CHECK(has(load({{"sP3flip", "yes"}}, 3, b), "flip"));
	b.log.clear();
	CHECK_FALSE(has(load({{"sP3flip", "yes"}}, 3, b), "flip"));
	b.log.clear();
	CHECK(has(load({{"sP3flip", "no"}}, 3, b), "flip"));
}

TEST_CASE("preset loader: tunings, the user's own and the built-in 12-TET") {
	const std::vector<ConfigParam> page = config_page({{"fT1fifth", "705.8824"}, {"fT1C_3", "-30"}, {"hT1color", "#123456"}});
	const settings::ConfigTable cfg(page, settings::defaults());
	RecordingBoard b;
	PresetLoader<RecordingBoard>(cfg, b).load_tuning(1);
	CHECK(b.log.front() == "generators 7.058825 12.000000"); // 705.8824 cents / 100, as floats
	REQUIRE(b.keys.size() == 61);
	char name[3];
	for (const auto& [k, v] : b.keys) {
		put_button_name(k, name);
		CHECK(v == (std::string(name, 3) == "C_3" ? -0.3f : 0.0f));
	}
	CHECK(b.log.back() == "set_tuning 1 " + std::to_string(0x123456));
	// the same tuning again: only its colour
	b.log.clear();
	PresetLoader<RecordingBoard>(cfg, b).load_tuning(1);
	CHECK(b.log == std::vector<std::string>{"led " + std::to_string(0x123456)});
	// tuning 0 is built in
	b.log.clear();
	PresetLoader<RecordingBoard>(cfg, b).load_tuning(0);
	CHECK(b.log.front() == "12tet");
	CHECK(b.cur_tuning == 0);
}
