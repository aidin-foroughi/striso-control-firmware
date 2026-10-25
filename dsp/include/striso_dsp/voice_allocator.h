// The voice allocator: which note plays which voice of the internal synth. The board's rule
// (synth_control.cpp, its keys and its MIDI in notes) and the SDK's hosts (sdk/: MIDI in on the
// PC) both run it. Pure: no HAL, no heap, no MIDI. A note is a number 0 .. Notes-1 the host
// gives it (the board: a key, or a MIDI in note's slot after the keys); a voice is 0 .. Voices-1.
//
// A note's level is the allocator's estimate of how loud it still is: set when it starts (its
// pressure plus its strike), raised to its floor whenever the host has a new floor for it,
// decaying every tick() towards the host's floor (the note's pres + vpres while held, -1 after:
// a released note fades below any held one).
//
// A new note gets (start): its own voice if it still has it (a repeated note), else a voice no
// note has had yet, else the quietest released one, else the quietest held one if that is
// quieter than the new note; else none (-1). Voices below `first` are not given (the board's
// Mono mode keeps voice 0 for its keys: the host takes it with take()).
//
// The host (any type with these members; the calls are templates, inlined):
//   bool held(int note)      the note's key is down (its note is on)
//   float floor(int note)    its level now: pres + vpres while held, -1 after the release
#ifndef STRISO_DSP_VOICE_ALLOCATOR_H_
#define STRISO_DSP_VOICE_ALLOCATOR_H_

#include <stdint.h>

namespace striso::voices {

// the level estimate's decay per tick: x 0.998 - 0.0005 (in double, as the board always did)
inline constexpr double LEVEL_DECAY = 0.998, LEVEL_STEP = 0.0005;

template <int Notes, int Voices> class VoiceAllocator {
	static_assert(Notes <= 127 && Voices <= 127, "note and voice numbers are int8_t");

public:
	static constexpr int NOTES = Notes, VOICES = Voices;

	// the voices in use, 1 .. Voices (all of them unless the host says fewer)
	void set_voices(int n) { voices_ = n < 1 ? 1 : (n > Voices ? Voices : n); }
	[[nodiscard]] int voices() const { return voices_; }

	// the note's voice while it has one, else -1
	[[nodiscard]] int voice(int note) const {
		const int v = voice_[note];
		return v >= 0 && v < voices_ && owner_[v] == note ? v : -1;
	}
	// the voice's note (the last one that had it: it may be released), -1: none yet
	[[nodiscard]] int owner(int v) const { return owner_[v]; }

	// the level estimate
	[[nodiscard]] float level(int note) const { return level_[note]; }
	void set_level(int note, float level) { level_[note] = level; } // a new note: no old estimate
	void raise_level(int note, float floor) {
		if (level_[note] < floor) level_[note] = floor;
	}

	// a voice for a new note (taken), or -1
	template <class Host> int start(const Host& h, int note, int first = 0) {
		int v = -1;
		if (voice_[note] >= first && owner_[voice_[note]] == note) {
			v = voice_[note];
		} else {
			int released = -1, held = -1;
			float min_released = 0, min_held = level_[note];
			for (int n = first; n < voices_; n++) {
				if (owner_[n] == -1) {
					v = n;
					break;
				}
				const int o = owner_[n];
				if (!h.held(o)) {
					if (released < 0 || level_[o] < min_released) {
						min_released = level_[o];
						released = n;
					}
				} else if (level_[o] < min_held) {
					min_held = level_[o];
					held = n;
				}
			}
			if (v < 0) v = released >= 0 ? released : held;
		}
		if (v >= 0) take(note, v);
		return v;
	}

	// the note takes voice v (one note drives a voice; a note has one voice)
	void take(int note, int v) {
		const int old = owner_[v];
		if (old >= 0 && old != note) voice_[old] = -1;
		const int mine = voice_[note];
		if (mine >= 0 && mine != v && owner_[mine] == note) owner_[mine] = -1;
		owner_[v] = (int8_t)note;
		voice_[note] = (int8_t)v;
	}

	// the note `from` goes on as `to` (the board's portamento and free transpose hand-overs)
	void hand_over(int from, int to) {
		const int v = voice_[from];
		if (v >= 0 && owner_[v] == from) take(to, v);
	}

	// the levels of the voices' notes decay one step towards their floors
	template <class Host> void tick(const Host& h) {
		for (int n = 0; n < voices_; n++) {
			const int o = owner_[n];
			if (o >= 0) {
				level_[o] = level_[o] * LEVEL_DECAY - LEVEL_STEP;
				if (level_[o] < h.floor(o)) level_[o] = h.floor(o);
			}
		}
	}

private:
	int8_t voice_[Notes] = {};
	int8_t owner_[Voices] = {};
	float level_[Notes] = {};
	int voices_ = Voices;

public:
	// (all -1: no note has a voice, no voice a note)
	constexpr VoiceAllocator() {
		for (int8_t& x : voice_) x = -1;
		for (int8_t& x : owner_) x = -1;
	}
};

} // namespace striso::voices

#endif // STRISO_DSP_VOICE_ALLOCATOR_H_
