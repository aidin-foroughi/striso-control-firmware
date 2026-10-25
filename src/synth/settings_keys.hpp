// The settings keys: what every key does with the Settings button held, one row per key in
// synth_control.cpp's table (ascending id). A row is the key's layout entry, printed by StcX so the PC
// shows the board's own layout, and its action, which press() dispatches to the firmware's handler
// (config_but). A voice key is one row: its engine and indicator colour (adding an engine's key is
// one more row). StcK presses only the rows flagged kConsole.
#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace settings_keys {

enum Kind : uint8_t { kSwitch,
	kDial,
	kActionKind }; // a press selects / hold and tilt / does something
enum Group : uint8_t { kMidi,
	kLayout,
	kSens,
	kVoice,
	kTuning,
	kPreset,
	kMidi2,
	kSystem };
enum Flags : uint8_t { kConsole = 1,
	kDanger = 2 }; // StcK may press it / it restarts the board

// what a key does (synth_control.cpp's SettingsKeyActions has a method for each)
enum class Action : uint8_t {
	mpe,         // MPE MIDI mode; knob: the MPE voices
	normal,      // Normal MIDI mode; knob: the channel
	mono,        // Mono MIDI mode; knob: the channel
	legato,      // mono legato on/off (the internal synth: one voice, held notes slur)
	flip,        // flip the layout (with glissando held: debug mode)
	threshold,   // knob: the note-on threshold
	pressure,    // knob: pressure sensitivity
	velocity,    // knob: velocity sensitivity
	bend,        // knob: pitch bend sensitivity
	tilt,        // knob: tilt sensitivity
	voice,       // the row's engine; knob: its presets
	tuning,      // tuning arg; knob: fine tuning
	preset,      // settings preset arg
	volume,      // knob: volume (CC7)
	sustain,     // knob: sustain / decay
	bend_range,  // knob: the MPE pitch bend range
	rate,        // knob: the MIDI message interval
	panic,       // all notes off
	send_config, // send the MPE configuration
	reset,       // restart the board
};

struct Key {
	uint8_t id, kind, group, flags;
	int8_t arg;         // tuning keys: the tuning, preset keys: the preset, else -1
	const char* engine; // voice keys: the engine (as synth_voice_name), else nullptr
	uint32_t led;       // voice keys: the indicator colour, r g b as 0xrrggbb
	const char* label;  // at most 6 characters, no spaces
	const char* title;
	const char* knob; // what tilting the held key sets ("" when nothing)
	Action action;
};

constexpr const Key* find(std::span<const Key> table, int id) {
	for (const Key& k : table)
		if (k.id == id) return &k;
	return nullptr;
}

// the table's rules (synth_control.cpp static_asserts them): ids ascending, labels of at most 6
// characters without spaces, a voice row with its engine, a tuning or preset row with its number
constexpr bool well_formed(std::span<const Key> table) {
	int last = -1;
	for (const Key& k : table) {
		if (k.id <= last) return false;
		last = k.id;
		const std::string_view label(k.label);
		if (label.size() > 6 || label.find(' ') != std::string_view::npos) return false;
		if ((k.action == Action::voice) != (k.engine != nullptr)) return false;
		if (k.action == Action::voice && k.group != kVoice) return false;
		if (k.action == Action::tuning && (k.group != kTuning || k.arg < 0 || k.arg > 8)) return false;
		if (k.action == Action::preset && (k.group != kPreset || k.arg < 1 || k.arg > 8)) return false;
	}
	return true;
}

// key k pressed (type 0), nudged (1) or circled (2) by adjust: its action; returns the part of
// adjust not used (config_but's contract)
template <typename Actions>
float press(const Key& k, Actions& a, int type, float adjust) {
	switch (k.action) {
	case Action::mpe: return a.mpe(k, type, adjust);
	case Action::normal: return a.normal(k, type, adjust);
	case Action::mono: return a.mono(k, type, adjust);
	case Action::legato: return a.legato(k, type, adjust);
	case Action::flip: return a.flip(k, type, adjust);
	case Action::threshold: return a.threshold(k, type, adjust);
	case Action::pressure: return a.pressure(k, type, adjust);
	case Action::velocity: return a.velocity(k, type, adjust);
	case Action::bend: return a.bend(k, type, adjust);
	case Action::tilt: return a.tilt(k, type, adjust);
	case Action::voice: return a.voice(k, type, adjust);
	case Action::tuning: return a.tuning(k, type, adjust);
	case Action::preset: return a.preset(k, type, adjust);
	case Action::volume: return a.volume(k, type, adjust);
	case Action::sustain: return a.sustain(k, type, adjust);
	case Action::bend_range: return a.bend_range(k, type, adjust);
	case Action::rate: return a.rate(k, type, adjust);
	case Action::panic: return a.panic(k, type, adjust);
	case Action::send_config: return a.send_config(k, type, adjust);
	case Action::reset: return a.reset(k, type, adjust);
	}
	return 0;
}

} // namespace settings_keys
