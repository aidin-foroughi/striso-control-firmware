// MIDI in -> the engines' voice controls on the PC: the board's MIDI note mapping
// (striso_dsp/midi_in.h, the same code the firmware runs) with this host's voices, and the board's
// handling of the other messages a sound needs: the bend range (RPN 0), the MPE zone (MCM), the
// instrument's pitch bend outside the zone, the pedal (CC64).
//
// Time: MIDI is taken in steps of striso::midi::TICK_SECONDS (2 ms), as the board's main loop
// does: message() queues, step() runs one step (the strike fade if one is fading, then the
// messages queued since the last step). The key signals of a voice are in voices(); gate,
// strike and release follow from them (dsp::track_key, once per block), as on the board.
//
// Not the board's: the voices. A note takes a voice never used yet (the lowest), else the one
// released longest ago, else none (every voice holds a note: it isn't played). On the board MIDI
// notes share the voices with the keys and may steal a quieter held one (get_synth_voice in
// synth_control.cpp); with no more notes than voices at once they get the same voices.
#ifndef STRISO_SDK_MIDI_VOICES_H_
#define STRISO_SDK_MIDI_VOICES_H_

#include <cstdint>
#include <vector>

#include "striso_dsp/engine.h"
#include "striso_dsp/midi_in.h"

namespace striso::sdk {

class MidiVoices {
public:
	static constexpr int QUEUE = 512; // messages per step

	explicit MidiVoices(int voices);

	// one MIDI message (status byte first), for the next step; false: the queue is full
	bool message(const uint8_t* m, int len);
	bool message(uint8_t status, uint8_t d1, uint8_t d2) {
		const uint8_t m[3] = {status, d1, d2};
		return message(m, 3);
	}
	// one 2 ms step: the strike fade, then the queued messages
	void step();

	[[nodiscard]] const dsp::VoiceControls* voices() const { return vc_.data(); }
	[[nodiscard]] int num_voices() const { return static_cast<int>(vc_.size()); }
	[[nodiscard]] const dsp::GlobalControls& globals() const { return g_; }
	[[nodiscard]] int notes_held() const;
	[[nodiscard]] int members() const { return in_.members(); }
	[[nodiscard]] float bend_range() const { return bend_range_; }

	// striso::midi::NoteInput's host (midi_in.h)
	void note_started(int s, const midi::NoteSignals& n);
	void note_changed(int s, const midi::NoteSignals& n);
	void note_silenced(int s);
	void note_dropped(int s);
	[[nodiscard]] bool echoed(int) const { return false; } // (nothing goes out)

private:
	void apply(uint8_t status, uint8_t d1, uint8_t d2);

	midi::NoteInput in_;
	midi::RpnSelect rpn_;
	std::vector<dsp::VoiceControls> vc_;
	dsp::GlobalControls g_;
	float bend_range_ = 48.0f;
	struct Voice {
		int slot = -1;         // the note playing it (-1: none)
		bool held = false;     // its note is held
		uint32_t released = 0; // when it was released (0: never used)
	};
	std::vector<Voice> voice_;
	int slot_voice_[midi::NOTES];
	uint32_t clock_ = 0;
	uint8_t queue_[QUEUE][3];
	int queued_ = 0;
};

} // namespace striso::sdk

#endif // STRISO_SDK_MIDI_VOICES_H_
