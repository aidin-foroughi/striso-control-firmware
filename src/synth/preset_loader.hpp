// The settings presets and tunings (config keys P<n>..., T<n>...): what loading one does, read from
// a ConfigTable and applied to a Board, the instrument's side. synth_control.cpp's board does the
// effects (MIDI mode, jack, leds, the synth's pedal and volume, MIDI out); tests/test_preset_loader.cpp's
// records them.
//
// A preset sets only what it has: a blank, missing, malformed or out of range setting leaves the
// instrument as it is. The order is the firmware's of old (the MIDI mode first: the CC settings and
// the voice count depend on it; the tuning before the preset's offset; the MIDI configuration, then
// the program change), and so is the arithmetic: tests/golden/control_replay.txt
// (control_replay.settings.*) and virtual/tests/transcripts/preset_loads.txt were recorded before
// it left synth_control.cpp.
//
// A Board has (all called from the control thread, on its 1 kB stack: keep them small):
//   led(rgb)                                       the indicator colour
//   message_interval(ms), motion_interval(i)       config.message_interval, send_motion_interval
//   midi_mode(), set_midi_mode(m)                  the MIDI mode (set_midi_mode sends its messages)
//   midinote_mode(m), jack2(m)                     config.midinote_mode, the jack's setting
//   flipdir(), flip()                              the layout (1, or -1 when turned 180 degrees)
//   load_tuning(n)                                 PresetLoader::load_tuning on this board
//   preset_offset(semitones)                       note_offset = the tuning's offset + this + MIDI in's bend
//   axis_cc(axis, normal_mode, cc), contvelo(cc)   the CCs of pressure, x, y; continuous velocity
//   channel_offset(), channel_offset(c)            the first channel (0..15)
//   bend_range(semitones), voicecount(), voicecount(n), channels_changed()
//   zero_offset(v), bend/pres/velo/tilt_sensitivity(f), velo_offset(v)
//   volume(v), decay(v), pedal_decay(v)            set_volume; the pedal's values (decay: also the synth's)
//   midi_config(), voice_channel(i), program_change(channel, program)
//   tuning(), tuning_color(), equal_temperament()  the tuning loaded, its colour; tuning 0 (12-TET)
//   generators(fifth, octave)                      notegen1, notegen0 in semitones
//   tuning_offset(semitones)                       tuning_note_offset; note_offset = it + MIDI in's bend
//   key_offset(key, semitones)                     key number key's tuning offset (the board flips it)
//   set_tuning(n, colour)                          cur_tuning, tuning_color
#pragma once

#include <optional>

#include "config.h"
#include "config_table.hpp"

enum class MidiAxis { pressure,
	x,
	y };

template <typename Board>
class PresetLoader {
public:
	PresetLoader(const settings::ConfigTable& cfg, Board& board) :
	    cfg_(cfg), b_(board) {}

	// settings preset n (1-8). (Both inline into their one caller each: the control thread's deepest
	// path is config_but -> load_preset -> load_tuning -> update_leds, on a 1 kB stack.)
	[[gnu::always_inline]] void load_preset(int n) {
		using settings::KeyName;
		auto P = [n](settings::PresetField f) { return KeyName::preset(n, f); };
		bool sendcfg = false;
		const uint32_t led = cfg_.get_hex(P("color")).value_or(0);
		b_.led(led);

		if (auto i = int_in(P("Mint"), 1, 127)) b_.message_interval(*i);
		if (auto i = int_in(P("Mmint"), 0, 127)) b_.motion_interval(*i);

		// (cfg.is: no values kept, the frame is on the 1 kB stacks of main and the control thread)
		if (cfg_.is(P("Mmode"), "mpe")) {
			b_.set_midi_mode(MIDI_MODE_MPE);
			sendcfg = true;
		} else if (cfg_.is(P("Mmode"), "normal")) {
			b_.set_midi_mode(MIDI_MODE_POLY);
			sendcfg = true;
		} else if (cfg_.is(P("Mmode"), "mono")) {
			b_.set_midi_mode(MIDI_MODE_MONO);
			sendcfg = true;
		}

		if (cfg_.is(P("Mnote"), "default")) {
			b_.midinote_mode(MIDINOTE_MODE_DEFAULT);
		} else if (cfg_.is(P("Mnote"), "tuning")) {
			b_.midinote_mode(MIDINOTE_MODE_TUNING);
		} else if (cfg_.is(P("Mnote"), "button")) {
			b_.midinote_mode(MIDINOTE_MODE_BUTTON);
		}

		if (cfg_.is(P("jack2"), "pedal_ex")) {
			b_.jack2(JACK2_MODE_PEDAL_EXPRESSION);
		} else if (cfg_.is(P("jack2"), "pedal_sw")) {
			b_.jack2(JACK2_MODE_PEDAL_SWITCH);
		} else if (cfg_.is(P("jack2"), "midi")) {
			b_.jack2(JACK2_MODE_MIDI);
		} else if (cfg_.is(P("jack2"), "linein")) {
			b_.jack2(JACK2_MODE_LINEIN);
		} else if (cfg_.is(P("jack2"), "auto")) {
			b_.jack2(JACK2_MODE_AUTODETECT);
		}

		if ((cfg_.is(P("flip"), "yes") && b_.flipdir() == 1) || (cfg_.is(P("flip"), "no") && b_.flipdir() == -1)) b_.flip();

		const std::optional<int> tuning = int_in(P("tunin"), 0, 8);
		if (tuning) b_.load_tuning(*tuning);

		const std::optional<float> toff = float_in(P("Toff"), -4800.0f, 4800.0f);
		if (tuning || toff) {
			// the tuning's offset plus the preset's: a preset with a tuning and no Toff also clears an
			// earlier preset's Toff and the fine tune (load_tuning keeps them for the same tuning, SC-04)
			b_.preset_offset(toff ? *toff / 100.0f : 0.0f);
		}

		const bool normal = b_.midi_mode() == MIDI_MODE_POLY; // (the mode as the preset left it)
		if (auto i = int_in(P("Mpres"), 0, 127)) b_.axis_cc(MidiAxis::pressure, normal, *i);
		if (auto i = int_in(P("Mx"), 0, 127)) b_.axis_cc(MidiAxis::x, normal, *i);
		if (auto i = int_in(P("My"), 0, 127)) b_.axis_cc(MidiAxis::y, normal, *i);
		if (auto i = int_in(P("Mvelo"), 0, 127)) b_.contvelo(*i);

		if (auto i = int_in(P("MChan"), 1, 16)) {
			b_.channel_offset(*i - 1);
			sendcfg = true;
		}
		if (auto i = int_in(P("MPEpb"), 1, 127)) {
			b_.bend_range(*i);
			sendcfg = true;
		}
		if (auto i = int_in(P("voice"), 1, MAX_VOICECOUNT); i && b_.midi_mode() == MIDI_MODE_MPE) {
			b_.voicecount(*i);
			sendcfg = true;
		}
		b_.channels_changed();

		if (auto f = float_in(P("thres"), 0.0f, 1.0f)) {
			const float t = *f > 0.5f ? 0.5f : (*f < 0.0f ? 0.0f : *f);
			b_.zero_offset(static_cast<int>(t * (1 << 24)));
		}
		if (auto f = float_in(P("bendS"), -10.0f, 10.0f)) b_.bend_sensitivity(*f * 2);
		if (auto f = float_in(P("presS"), 0.0f, 10.0f)) b_.pres_sensitivity(*f);
		if (auto f = float_in(P("veloS"), 0.0f, 10.0f)) b_.velo_sensitivity(*f);
		if (auto f = float_in(P("tiltS"), -10.0f, 10.0f)) b_.tilt_sensitivity(*f);

		// volume and decays are 0..127 (config.h); up to 254 gave 4x the knob's maximum gain (+12 dB)
		// and a pedal value of 2 (SC-13)
		if (auto f = float_below(P("volum"), 0.0f, 255.0f)) b_.volume(below127(*f));
		if (auto f = float_below(P("decay"), 0.0f, 255.0f)) b_.decay(below127(*f));
		if (auto f = float_below(P("decaP"), 0.0f, 255.0f)) b_.pedal_decay(below127(*f));

		if (auto i = int_in(P("veloO"), 0, 127)) b_.velo_offset(*i);

		if (sendcfg) b_.midi_config();

		if (auto i = int_in(P("Mpgm"), 0, 127)) {
			if (b_.midi_mode() == MIDI_MODE_POLY) {
				// Normal mode has one channel (it went to voicecount channels, SC-19)
				b_.program_change(b_.voice_channel(0), *i);
			} else {
				if (b_.channel_offset() == 1) b_.program_change(0, *i);
				for (int c = 0; c < b_.voicecount(); c++) b_.program_change(b_.voice_channel(c), *i);
			}
		}

		b_.led(led);
	}

	// tuning n (0-8; 0 is 12-TET, built in): its fifth and octave, its offset and every key's
	[[gnu::always_inline]] void load_tuning(int n) {
		if (b_.tuning() == n) {
			b_.led(b_.tuning_color());
			return;
		}
		if (n == 0) {
			b_.equal_temperament();
			b_.led(b_.tuning_color());
			b_.set_tuning(n, b_.tuning_color());
			return;
		}
		using settings::KeyName;
		const float fifth = cfg_.get_float(KeyName::tuning(n, "fifth")).value_or(700.0f) / 100;
		const float octave = cfg_.get_float(KeyName::tuning(n, "oct")).value_or(1200.0f) / 100;
		b_.generators(fifth, octave);
		b_.tuning_offset(cfg_.get_float(KeyName::tuning(n, "off")).value_or(0.0f) / 100);
		for (int k = 0; k < config_schema::kKeys; k++) b_.key_offset(k, cfg_.get_float(KeyName::tuning_key(n, k)).value_or(0.0f) / 100);
		const int color = static_cast<int>(cfg_.get_hex(KeyName::tuning(n, "color")).value_or(0));
		b_.led(static_cast<uint32_t>(color));
		b_.set_tuning(n, color);
	}

private:
	// the integer or decimal if it is in [lo, hi]
	std::optional<int> int_in(settings::KeyName k, int lo, int hi) const {
		const std::optional<int> i = cfg_.get_int(k);
		return i && *i >= lo && *i <= hi ? i : std::nullopt;
	}
	std::optional<float> float_in(settings::KeyName k, float lo, float hi) const {
		const std::optional<float> f = cfg_.get_float(k);
		return f && *f >= lo && *f <= hi ? f : std::nullopt;
	}
	// in [lo, hi) (volume, decays)
	std::optional<float> float_below(settings::KeyName k, float lo, float hi) const {
		const std::optional<float> f = cfg_.get_float(k);
		return f && *f >= lo && *f < hi ? f : std::nullopt;
	}
	static float below127(float f) { return f < 127.0f ? f : 127.0f; }

	const settings::ConfigTable& cfg_;
	Board& b_;
};
