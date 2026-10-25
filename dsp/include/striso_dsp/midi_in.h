// MIDI note input: the notes that MIDI in plays on the internal synth, with the key signals a key
// would give them. The one copy of the mapping: the board's synth_control.cpp (MIDI in from USB
// and from the virtual board) and the SDK's hosts (sdk/: the shell, the renderer's MIDI files)
// both run it. Pure: no HAL, no heap; the voices and the echo filter are the host's.
//
// - MPE: the lower zone, member channels 2 .. 1 + members() (an MCM on channel 1 sets it with
//   set_members(); 15 at the start). Per member channel: its notes; pitch bend x the host's bend
//   range (RPN 0) on top of the note number; channel pressure (or poly pressure) -> pres; CC74 -> y.
// - Every other channel (the master channel 1, all of them with no zone): its notes; poly pressure
//   (that note) or channel pressure (all its notes) -> pres; CC1 (the mod wheel) -> y (0..1),
//   CC74 -> y (64 = 0). Its pitch bend is the instrument's (+-2 semitones: instrument_bend(),
//   set_bend()), which the notes follow too.
// - Note-on velocity -> vpres, a strike that fades (x0.6 per tick(), every TICK_SECONDS while
//   fading()); until pressure comes, pres is the velocity (a keyboard without aftertouch still
//   sounds), and a held note's pres never drops to 0. Note-off velocity -> the release (vpres < 0).
// - With all NOTES slots held, a new note takes the oldest one.
//
// The host (any type with these members; the calls are templates, inlined):
//   void note_started(int s, const NoteSignals&)  a new note in slot s: give it a voice
//   void note_changed(int s, const NoteSignals&)  slot s's signals changed: to its voice
//   void note_silenced(int s)                     slot s is taken by a newer note: end its sound now
//   void note_dropped(int s)                      all_off(): slot s ends silently
//   bool echoed(int channel)                      the host's own notes went out on this channel
//                                                 lately (an echo of them is not played)
//   float bend_range()                            the notes' pitch bend range, semitones (RPN 0)
// A host that runs MIDI in steps of TICK_SECONDS does, every step: tick() if fading(), then the
// step's messages (the board: synth_tick, then the messages its main loop received).
#ifndef STRISO_DSP_MIDI_IN_H_
#define STRISO_DSP_MIDI_IN_H_

#include <stdint.h>

namespace striso::midi {

inline constexpr int NOTES = 16;              // notes held at once (the board's MIDI_IN_NOTES)
inline constexpr double TICK_SECONDS = 0.002; // the strike fade's step (the board's synth_tick)

// a note's key signals, as a key gives them (controls.h), and whether it is held
struct NoteSignals {
	float note, pres, vpres, but_x, but_y;
	bool on;
};

// the instrument's bend from a pitch bend message outside the MPE zone: +-2 semitones
[[nodiscard]] inline float instrument_bend(uint8_t data1, uint8_t data2) {
	return (float)((int)((data2 << 7) + data1) - 0x2000) * (2.0f / 8192);
}

// The registered parameter selected on each channel by CC 101/100 (an NRPN select, CC 99/98,
// deselects it). 0x7f/0x7f: none, as at the start, so a bare Data Entry (CC 6) selects nothing.
class RpnSelect {
public:
	// a control change: true if it was one of the selects
	bool control(int channel, uint8_t cc, uint8_t value) {
		switch (cc) {
		case 0x64: lsb_[channel] = value; return true;
		case 0x65: msb_[channel] = value; return true;
		case 0x62:
		case 0x63: lsb_[channel] = msb_[channel] = 0x7f; return true;
		default: return false;
		}
	}
	// RPN msb/lsb is the channel's selection
	[[nodiscard]] bool is(int channel, uint8_t msb, uint8_t lsb) const { return lsb_[channel] == lsb && msb_[channel] == msb; }

private:
	// (constant initialized: a function's static needs no guard)
	uint8_t lsb_[16] = {0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f};
	uint8_t msb_[16] = {0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f};
};

class NoteInput {
public:
	// a note message (note on/off, poly and channel pressure, a member channel's pitch bend,
	// CC1, CC74): played, or ignored as an echo; false: not a note message (the host's)
	template <class Host> bool message(Host& h, uint8_t channel, uint8_t status, uint8_t data1, uint8_t data2) {
		if (status == 0x80 || (status == 0x90 && data2 == 0)) {
			// (always: a note-off of a MIDI note that sounds is never an echo)
			note_off(h, channel, data1, status == 0x80 ? data2 : 0);
			return true;
		}
		const bool note_data = status == 0x90 || status == 0xa0 || status == 0xd0 || (status == 0xe0 && member(channel)) ||
		    (status == 0xb0 && (data1 == 1 || data1 == 74));
		if (!note_data) return false;
		if (h.echoed(channel)) return true; // the host's own notes, echoed
		switch (status) {
		case 0x90: note_on(h, channel, data1, data2); break;
		case 0xa0: pressure(h, channel, data1, data2); break;
		case 0xd0: pressure(h, channel, -1, data1); break;
		case 0xe0:
			chan_bend_[channel] = (float)((int)((data2 << 7) + data1) - 0x2000) * (1.0f / 8192) * h.bend_range();
			update(h, channel);
			break;
		default: // CC1 (mod wheel, 0..1) or CC74 (64 = 0)
			chan_y_[channel] = data1 == 1 ? data2 * (1.0f / 127.0f) : clampf((data2 - 64) * (1.0f / 64.0f), -1.0f, 1.0f);
			update(h, channel);
			break;
		}
		return true;
	}

	// the strikes fade one step
	template <class Host> void tick(Host& h) {
		bool fading = false;
		for (int s = 0; s < NOTES; s++) {
			Slot& m = slot_[s];
			if (m.channel >= 0 && m.sig.vpres > 0.0f) {
				m.sig.vpres = m.sig.vpres > 0.01f ? m.sig.vpres * 0.6f : 0.0f;
				fading |= m.sig.vpres > 0.0f;
				write(h, s);
			}
		}
		fading_ = fading;
	}
	// a strike is fading: tick() is due every TICK_SECONDS
	[[nodiscard]] bool fading() const { return fading_; }

	// every note off at once, silently (the panic key)
	template <class Host> void all_off(Host& h) {
		for (int s = 0; s < NOTES; s++) {
			if (slot_[s].channel >= 0) {
				h.note_dropped(s);
				slot_[s].sig.on = false;
				slot_[s].channel = -1;
			}
		}
		for (bool& p : chan_pres_set_) p = false;
	}

	// the instrument's bend (instrument_bend(), semitones), which every note follows
	template <class Host> void set_bend(Host& h, float semitones) {
		bend_ = semitones;
		update(h, -1);
	}
	[[nodiscard]] float bend() const { return bend_; }

	// the MPE zone: member channels 2 .. 1 + n (0: none, every channel is like the master)
	void set_members(int n) { members_ = n; }
	[[nodiscard]] int members() const { return members_; }
	[[nodiscard]] bool member(int channel) const { return channel >= 1 && channel <= members_; }

	// slot s's note: its signals (on: held), its channel (-1: free) and key
	[[nodiscard]] const NoteSignals& signals(int s) const { return slot_[s].sig; }
	[[nodiscard]] int channel(int s) const { return slot_[s].channel; }
	[[nodiscard]] int key(int s) const { return slot_[s].key; }

private:
	struct Slot {
		int8_t channel = -1; // -1: free
		uint8_t key = 0;
		bool pressure = false; // pressure received: pres follows it, not the velocity
		uint32_t age = 0;
		NoteSignals sig = {};
	};

	[[nodiscard]] static float clampf(float x, float lo, float hi) { return x > hi ? hi : (x < lo ? lo : x); }
	// a held note's pres stays above 0 (pres 0 is a release for the engines)
	[[nodiscard]] static float held_pres(float p) { return p > 1.0f / 127.0f ? p : 1.0f / 127.0f; }

	[[nodiscard]] int find(int channel, int key) const {
		for (int s = 0; s < NOTES; s++)
			if (slot_[s].channel == channel && slot_[s].key == key) return s;
		return -1;
	}

	// the slot's pitch and y from its channel, then to the host
	template <class Host> void write(Host& h, int s) {
		Slot& m = slot_[s];
		if (m.channel >= 0) {
			m.sig.note = m.key + bend_ + (member(m.channel) ? chan_bend_[m.channel] : 0.0f);
			m.sig.but_y = chan_y_[m.channel];
		}
		h.note_changed(s, m.sig);
	}

	template <class Host> void note_on(Host& h, int channel, int key, int velo) {
		int s = find(channel, key);
		for (int n = 0; s < 0 && n < NOTES; n++)
			if (slot_[n].channel < 0) s = n;
		if (s < 0) { // all in use: the oldest
			s = 0;
			for (int n = 1; n < NOTES; n++)
				if (slot_[n].age < slot_[s].age) s = n;
			h.note_silenced(s);
		}
		Slot& m = slot_[s];
		m.channel = (int8_t)channel;
		m.key = (uint8_t)key;
		m.age = ++age_;
		m.pressure = chan_pres_set_[channel]; // MPE: the pressure sent before the note-on
		m.sig.on = true;
		m.sig.vpres = velo * (1.0f / 127.0f);
		m.sig.pres = held_pres(m.pressure ? chan_pres_[channel] : m.sig.vpres);
		m.sig.but_x = 0.0f;
		fading_ = true;
		h.note_started(s, m.sig); // (its pitch and y follow in write)
		write(h, s);
	}

	template <class Host> void note_off(Host& h, int channel, int key, int velo) {
		const int s = find(channel, key);
		if (s < 0) return;
		Slot& m = slot_[s];
		m.sig.on = false;
		m.sig.pres = 0.0f;
		m.sig.vpres = -velo * (1.0f / 127.0f);
		write(h, s);
		m.channel = -1;
		bool any = false;
		for (const Slot& x : slot_) any |= x.channel == channel;
		if (!any) chan_pres_set_[channel] = false; // pressure is per note (MPE)
	}

	// pressure 0..127 of the notes of a channel (key -1: channel pressure)
	template <class Host> void pressure(Host& h, int channel, int key, int value) {
		const float p = value * (1.0f / 127.0f);
		if (key < 0) {
			chan_pres_[channel] = p;
			chan_pres_set_[channel] = true;
		}
		for (int s = 0; s < NOTES; s++) {
			if (slot_[s].channel == channel && (key < 0 || slot_[s].key == key)) {
				slot_[s].pressure = true;
				slot_[s].sig.pres = held_pres(p);
				write(h, s);
			}
		}
	}

	// rewrite the notes of a channel (-1: all) after a change of its bend or y
	template <class Host> void update(Host& h, int channel) {
		for (int s = 0; s < NOTES; s++)
			if (slot_[s].channel >= 0 && (channel < 0 || slot_[s].channel == channel)) write(h, s);
	}

	Slot slot_[NOTES];
	uint32_t age_ = 0;
	int members_ = 15;
	float bend_ = 0.0f;        // the instrument's bend (semitones)
	float chan_bend_[16] = {}; // semitones, member channels
	float chan_y_[16] = {};
	float chan_pres_[16] = {};
	bool chan_pres_set_[16] = {};
	volatile bool fading_ = false; // (read by the board's main loop: synth_tick)
};

} // namespace striso::midi

#endif // STRISO_DSP_MIDI_IN_H_
