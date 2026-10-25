// What the leds show, from the instrument's state: the indicator colour and the up/down leds when
// nothing else is shown (update_leds), the tuning colour of a fifth set by MIDI, and what a
// settings key shows while it is held (its dial on the up/down leds, the indicator in the colour
// of its value). Pure: synth_control.cpp gives the state and makes the led calls (led.cpp,
// led_presenter.hpp below that). The arithmetic is the firmware's of old, conversions included:
// control_replay.settings.leds (every led call of the presets, tunings, settings keys and
// controls) was recorded before it left synth_control.cpp.
//
// Scalars in and out (no structs): update_leds ends the control thread's deepest stack path.
#pragma once

#include <cstdint>

namespace led_view {

// float to int, limited to lo..hi first (an out of range or NaN float to int is UB)
constexpr int to_int_clamped(double x, int lo, int hi) {
	if (!(x >= lo)) return lo; // (also NaN)
	if (x > hi) return hi;
	return static_cast<int>(x);
}
constexpr int min_int(int a, int b) { return a < b ? a : b; }

// --- the indicator and the up/down leds when nothing else is shown ---------------------------------

// Settings is held and the keys show their own values: update_leds leaves the leds alone
constexpr bool settings_shown(int altmode) { return (altmode & 2) != 0; }

// the indicator: the tuning's colour, brighter with Settings or glissando held (64: as it is)
constexpr int brightness(int altmode, int portamento) { return 64 + 16 * (altmode + (portamento & 1)); }
// a colour channel (0..255, of 0xrrggbb at shift) at that brightness
constexpr int indicator(int color, int shift, int brightness) {
	const uint8_t c = color >> shift & 0xff;
	return c * brightness / 64;
}

// the up/down leds: the transposition in semitones from the middle (the layout's direction), by
// up to two octaves each way
constexpr uint32_t octave_leds(int flipdir, float start_note_offset) {
	const int note_offset = flipdir * (static_cast<int>(start_note_offset + 0.5f) - 62);
	if (note_offset <= -24) return 0xff00;
	if (note_offset < -12) return 0xaf00;
	if (note_offset <= -12) return 0x0f00;
	if (note_offset < 0) return 0x0a00;
	if (note_offset >= 24) return 0x00ff;
	if (note_offset > 12) return 0x00fa;
	if (note_offset >= 12) return 0x00f0;
	if (note_offset > 0) return 0x00a0;
	return 0x0000;
}

// the colour of a tuning system by its fifth (notegen1, semitones: 7 is 12-TET), when MIDI sets it
constexpr int generator_color(float notegen1) {
	if (notegen1 < 6.94) return 0x550055;
	if (notegen1 < 6.957) return 0x0000aa;
	if (notegen1 < 6.984) return 0x005555;
	if (notegen1 < 7.010) return 0x00aa00;
	if (notegen1 < 7.03) return 0x555500;
	return 0xaa0000;
}

// --- a settings key held: its dial (0..15 on the up/down leds) and the indicator ---------------------

// the note-on threshold (config.zero_offset): dial and indicator
constexpr int threshold_dial(int zero_offset) {
	const int d = zero_offset / ((1 << 24) / 256);
	return d > 15 ? 15 : (d < 0 ? 0 : d);
}
constexpr int threshold_red(int dial) { return dial * 10; }
constexpr int threshold_green(int dial) { return dial * 5; }

// pressure sensitivity (0..3): the dial has 16 leds and wraps, so it is clamped (SC-27)
constexpr int pressure_dial(float s) { return to_int_clamped(s * 8 + 0.9f, 0, 15); }
constexpr int pressure_level(float s) { return to_int_clamped(s * 64.0f, 0, 255); } // red and green

// velocity sensitivity (0..2): magenta
constexpr int velocity_dial(float s) { return min_int(static_cast<int>(s * 8 + 0.9f), 15); }
constexpr int velocity_level(float s) { return static_cast<int>(s * 64.0f); } // red and blue

// pitch bend sensitivity (0..4): red
constexpr int bend_dial(float s) { return min_int(static_cast<int>(s * 4 + 0.9f), 15); }
constexpr int bend_level(float s) { return static_cast<int>(s * 64.0f); }

// tilt sensitivity (0..3); a negative tilt (a preset, Striso Live) is the lowest (SC-27)
constexpr int tilt_dial(float s) { return to_int_clamped(s * 8 + 0.5f, 0, 15); }
constexpr int tilt_red(float s) { return to_int_clamped(s * 32.0f, 0, 255); }
constexpr int tilt_blue(float s) { return to_int_clamped(s * 64.0f, 0, 255); }

// the fine tuning offset (note_offset, semitones): 1/16 semitone per led step
constexpr int tuning_dial(float note_offset) { return static_cast<int>(note_offset * 16 + 0.5f); }

// the volume (0..127): grey, 8 per led
constexpr int volume_level(float volume) { return static_cast<int>(volume * 2); }
constexpr int volume_dial(float volume) { return static_cast<int>(volume) / 8; }

// the sustain / decay (the synth's pedal, 0..1): magenta
constexpr int sustain_dial(float pedal) { return static_cast<int>(pedal * 15 + 0.5f); }
constexpr int sustain_level(float pedal) { return static_cast<int>(pedal * 64.0f); }

// the MPE pitch bend range, step 0..3 (12, 24, 48, 96 semitones): red
constexpr int bend_range_red(int step) { return 10 + 50 * step; }
constexpr int bend_range_dial(int step) { return 2 + 2 * step; }

// the MIDI message interval (1..15 ms): yellow, brighter and further for a shorter interval
constexpr int rate_level(int interval) { return 164 - 8 * interval; }
constexpr int rate_dial(int interval) { return 16 - interval; }

// a voice key: its engine's colour (0xrrggbb) and the preset of the engine (index of count)
constexpr int voice_dial(int index, int count) { return count > 1 ? index + 1 : 0; }

// --- fixed colours --------------------------------------------------------------------------------
inline constexpr uint32_t kPanic = 0x342000;      // all notes off
inline constexpr uint32_t kSendConfig = 0x300064; // the MPE configuration sent
inline constexpr uint32_t kReset = 0xff0000;      // the board restarts on release
inline constexpr uint32_t kNoEngine = 0x400000;   // a voice key whose engine isn't in this firmware
inline constexpr uint32_t kLegatoOn = 0x004020;   // mono legato on (settings key 9)
inline constexpr uint32_t kLegatoOff = 0x100808;  // ... off

} // namespace led_view
