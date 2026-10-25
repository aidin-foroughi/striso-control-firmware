// MIDI and MPE in -> the engines' VoiceControls: the mapping the board's MIDI note input uses
// (src/synth/synth_control.cpp, Instrument::midi_in_*), for hosts on the PC (the shell, the
// renderer's MIDI files). The same rules:
// - MPE: the lower zone, member channels 2 .. 1 + members (an MCM, RPN 6 on channel 1, sets it;
//   15 at the start). Per member channel: its notes; pitch bend x the bend range (RPN 0, 48 at the
//   start) on top of the note number; channel pressure (or poly pressure) -> pres; CC74 -> y.
// - Every other channel (the master channel 1, all of them with members 0): its notes; poly
//   pressure (that note) or channel pressure (all its notes) -> pres; CC1 -> y (0..1), CC74 -> y
//   (64 = 0). Its pitch bend is the instrument's: +-2 semitones on every note.
// - Note-on velocity -> vpres, a strike that fades (x0.6 every 2 ms, tick()); until pressure comes
//   pres is the velocity (a keyboard without aftertouch still sounds), and never below 1/127 while
//   held. Note-off velocity -> the release (negative vpres). CC64 -> the pedal (0..1).
// - gate, strike and release of VoiceControls come straight from the note-on and note-off.
// Not the board's: notes take voices by this file's own rule (a free voice, else the quietest
// released one, else the quietest held one); on the board MIDI notes share the voices with the
// keys (get_synth_voice). The board's echo filtering of its own MIDI out isn't needed here.
// (The board's code isn't separable from its key handling yet: this is the second copy of the
// mapping; a change to one must go to the other, docs/engines.md.)
#ifndef STRISO_SDK_MIDI_IN_H_
#define STRISO_SDK_MIDI_IN_H_

#include <cstdint>
#include <vector>

#include "striso_dsp/engine.h"

namespace striso::sdk {

class MidiIn {
public:
	static constexpr int SLOTS = 16;               // notes held at once (the board's MIDI_IN_NOTES)
	static constexpr double TICK_SECONDS = 0.002; // the strike fade's step (the board's synth_tick)

	explicit MidiIn(int voices);

	// one complete MIDI message (status byte first; running status is the caller's)
	void message(const uint8_t* m, int len);
	void message(uint8_t status, uint8_t d1, uint8_t d2) {
		const uint8_t m[3] = {status, d1, d2};
		message(m, 3);
	}
	// the strikes fade: call every TICK_SECONDS
	void tick();
	// every note off, silently (MIDI all sound off)
	void all_off();

	// the controls of every voice and the globals, as they are now
	[[nodiscard]] const dsp::VoiceControls* voices() const { return vc_.data(); }
	[[nodiscard]] int num_voices() const { return static_cast<int>(vc_.size()); }
	[[nodiscard]] const dsp::GlobalControls& globals() const { return g_; }
	[[nodiscard]] int notes_held() const;

	// the MPE zone's member channels (0: none) and the notes' bend range (semitones)
	[[nodiscard]] int members() const { return members_; }
	[[nodiscard]] float bend_range() const { return bend_range_; }

private:
	struct Slot {
		int8_t channel = -1; // -1: free
		uint8_t key = 0;
		bool pressure = false; // pressure received: pres follows it, not the velocity
		uint32_t age = 0;
		int voice = -1;
		float pres = 0, vpres = 0;
	};
	struct Voice {
		int slot = -1;         // the note playing it (-1: none)
		bool held = false;     // its note is held
		uint32_t released = 0; // age of its release
	};

	[[nodiscard]] bool member(int channel) const { return channel >= 1 && channel <= members_; }
	[[nodiscard]] int find(int channel, int key) const;
	[[nodiscard]] int take_voice(int slot);
	void write(int s);
	void note_on(int channel, int key, int velo);
	void note_off(int channel, int key, int velo);
	void pressure(int channel, int key, int value);
	void update(int channel);
	void control_change(int channel, int cc, int value);

	std::vector<dsp::VoiceControls> vc_;
	std::vector<Voice> voice_;
	dsp::GlobalControls g_;
	Slot slot_[SLOTS];
	uint32_t age_ = 0;
	int members_ = 15;
	float bend_range_ = 48.0f;
	float master_bend_ = 0.0f;
	float chan_bend_[16] = {};
	float chan_y_[16] = {};
	float chan_pres_[16] = {};
	bool chan_pres_set_[16] = {};
	uint8_t rpn_lsb_[16], rpn_msb_[16];
};

} // namespace striso::sdk

#endif // STRISO_SDK_MIDI_IN_H_
