// The settings keys' table and dispatch (src/synth/settings_keys.hpp): a row is a key's layout and
// its action. The firmware's table (synth_control.cpp settings_key_table) is checked by its
// static_assert, by control_replay.settings.keys (every key pressed, nudged, circled: bit for bit
// as before the table) and the virtual transcripts (StcX, StcK).
#include "settings_keys.hpp"

#include <string>
#include <vector>

#include "doctest/doctest.h"

using settings_keys::Action;
using settings_keys::Key;

namespace {

#define KNOB "step through its presets (the up/down leds show which)"
constexpr Key kTable[] = {
    {0, settings_keys::kSwitch, settings_keys::kMidi, settings_keys::kConsole, -1, nullptr, 0, "MPE", "MPE MIDI mode", "", Action::mpe},
    {15, settings_keys::kSwitch, settings_keys::kVoice, settings_keys::kConsole, -1, "trumpet", 0x003030, "tpt", "Voice: trumpet", KNOB, Action::voice},
    // an engine's key: one row
    {16, settings_keys::kSwitch, settings_keys::kVoice, settings_keys::kConsole, -1, "accordion", 0x301000, "acc", "Voice: accordion", KNOB, Action::voice},
    {36, settings_keys::kSwitch, settings_keys::kTuning, settings_keys::kConsole, 1, nullptr, 0, "", "", "", Action::tuning},
    {37, settings_keys::kSwitch, settings_keys::kPreset, settings_keys::kConsole, 2, nullptr, 0, "P2", "Load settings preset 2", "", Action::preset},
};
static_assert(settings_keys::well_formed(kTable));

// what press() calls, with which row
struct Recorder {
	std::vector<std::string> calls;
	float rec(const char* what, const Key& k, int type, float adjust) {
		calls.push_back(std::string(what) + " " + std::to_string(k.id) + " " + std::to_string(type));
		return adjust / 2;
	}
	float mpe(const Key& k, int t, float a) { return rec("mpe", k, t, a); }
	float normal(const Key& k, int t, float a) { return rec("normal", k, t, a); }
	float mono(const Key& k, int t, float a) { return rec("mono", k, t, a); }
	float flip(const Key& k, int t, float a) { return rec("flip", k, t, a); }
	float threshold(const Key& k, int t, float a) { return rec("threshold", k, t, a); }
	float pressure(const Key& k, int t, float a) { return rec("pressure", k, t, a); }
	float velocity(const Key& k, int t, float a) { return rec("velocity", k, t, a); }
	float bend(const Key& k, int t, float a) { return rec("bend", k, t, a); }
	float tilt(const Key& k, int t, float a) { return rec("tilt", k, t, a); }
	float voice(const Key& k, int t, float a) { return rec("voice", k, t, a); }
	float tuning(const Key& k, int t, float a) { return rec("tuning", k, t, a); }
	float preset(const Key& k, int t, float a) { return rec("preset", k, t, a); }
	float volume(const Key& k, int t, float a) { return rec("volume", k, t, a); }
	float sustain(const Key& k, int t, float a) { return rec("sustain", k, t, a); }
	float bend_range(const Key& k, int t, float a) { return rec("bend_range", k, t, a); }
	float rate(const Key& k, int t, float a) { return rec("rate", k, t, a); }
	float panic(const Key& k, int t, float a) { return rec("panic", k, t, a); }
	float send_config(const Key& k, int t, float a) { return rec("send_config", k, t, a); }
	float reset(const Key& k, int t, float a) { return rec("reset", k, t, a); }
};

constexpr Key row(int id, Action a, const char* engine = nullptr, int group = settings_keys::kVoice, int arg = -1, const char* label = "x") {
	return {static_cast<uint8_t>(id), settings_keys::kSwitch, static_cast<uint8_t>(group), 0, static_cast<int8_t>(arg), engine, 0, label, "", "", a};
}

} // namespace

TEST_CASE("settings keys: a key's row by id, its action with its row") {
	CHECK(settings_keys::find(kTable, 16)->engine == std::string("accordion"));
	CHECK(settings_keys::find(kTable, 17) == nullptr);
	Recorder r;
	CHECK(settings_keys::press(*settings_keys::find(kTable, 16), r, 0, 0) == 0);
	CHECK(settings_keys::press(*settings_keys::find(kTable, 36), r, 1, 3.0f) == 1.5f);
	CHECK(settings_keys::press(*settings_keys::find(kTable, 37), r, 0, 0) == 0);
	CHECK(r.calls == std::vector<std::string>{"voice 16 0", "tuning 36 1", "preset 37 0"});
}

TEST_CASE("settings keys: the table's rules") {
	const Key ascending[] = {row(13, Action::voice, "brass"), row(15, Action::voice, "trumpet")};
	CHECK(settings_keys::well_formed(ascending));
	const Key twice[] = {row(15, Action::voice, "brass"), row(15, Action::voice, "trumpet")};
	CHECK_FALSE(settings_keys::well_formed(twice));
	const Key no_engine[] = {row(13, Action::voice)};
	CHECK_FALSE(settings_keys::well_formed(no_engine));
	const Key long_label[] = {row(13, Action::voice, "brass", settings_keys::kVoice, -1, "trumpet")};
	CHECK_FALSE(settings_keys::well_formed(long_label));
	const Key tuning_9[] = {row(33, Action::tuning, nullptr, settings_keys::kTuning, 9)};
	CHECK_FALSE(settings_keys::well_formed(tuning_9));
	const Key preset_0[] = {row(35, Action::preset, nullptr, settings_keys::kPreset, 0)};
	CHECK_FALSE(settings_keys::well_formed(preset_0));
}
