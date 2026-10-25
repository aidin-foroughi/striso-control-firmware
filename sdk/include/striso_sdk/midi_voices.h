// MIDI in -> the engines' voice controls on the PC, by the board's own code: the notes
// (striso_dsp/midi_in.h, NoteInput), the instrument's messages (InstrumentInput: the bend range,
// the MCM, the instrument's bend, the pedal) and the voices (striso_dsp/voice_allocator.h), the
// same units the firmware runs. What the board does beyond them (its MIDI out, its other CCs) a
// host on the PC doesn't need.
//
// Time: MIDI is taken in steps of striso::midi::TICK_SECONDS (2 ms), as the board's main loop
// does: message() queues, step() runs one step (the voices' level decay, the strike fade if one
// is fading, then the messages queued since the last step). The key signals of a voice are in
// voices(); gate, strike and release follow from them (dsp::track_key, once per block), as on
// the board.
#ifndef STRISO_SDK_MIDI_VOICES_H_
#define STRISO_SDK_MIDI_VOICES_H_

#include <cstdint>
#include <vector>

#include "striso_dsp/engine.h"
#include "striso_dsp/midi_in.h"
#include "striso_dsp/voice_allocator.h"

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
	// one 2 ms step: the voices' level decay, the strike fade, then the queued messages
	void step();

	[[nodiscard]] const dsp::VoiceControls* voices() const { return vc_.data(); }
	[[nodiscard]] int num_voices() const { return static_cast<int>(vc_.size()); }
	[[nodiscard]] const dsp::GlobalControls& globals() const { return g_; }
	[[nodiscard]] int notes_held() const;
	[[nodiscard]] int members() const { return in_.members(); }

	// striso::midi::NoteInput's host (midi_in.h)
	void note_started(int s, const midi::NoteSignals& n);
	void note_changed(int s, const midi::NoteSignals& n);
	void note_silenced(int s);
	void note_dropped(int s);
	[[nodiscard]] bool echoed(int) const { return false; } // (nothing goes out)
	[[nodiscard]] float bend_range() const { return bend_range_; }
	// InstrumentInput's host
	// (as a board in its default MPE mode: channel 1 is the zone master)
	[[nodiscard]] bool zone_master(int channel) const { return channel == 0; }
	void set_bend_range(float semitones) { bend_range_ = semitones; }
	void zone_configured(int, int) {}
	void set_pedal(float p) { g_.pedal = p; }
	[[nodiscard]] bool bends_out_on(int) const { return false; }
	void bend_moved(float) {}
	// VoiceAllocator's host: a note slot's key is down, its level floor
	[[nodiscard]] bool held(int s) const { return held_[s]; }
	[[nodiscard]] float floor(int s) const { return floor_[s]; }

private:
	void apply(uint8_t status, uint8_t d1, uint8_t d2);

	midi::NoteInput in_;
	midi::InstrumentInput instrument_;
	voices::VoiceAllocator<midi::NOTES, 16> alloc_;
	std::vector<dsp::VoiceControls> vc_;
	dsp::GlobalControls g_;
	float bend_range_ = 48.0f;
	bool held_[midi::NOTES] = {};
	float floor_[midi::NOTES] = {};
	uint8_t queue_[QUEUE][3];
	int queued_ = 0;
};

} // namespace striso::sdk

#endif // STRISO_SDK_MIDI_VOICES_H_
