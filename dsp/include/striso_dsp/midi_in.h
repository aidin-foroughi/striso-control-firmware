// MIDI in: the notes that MIDI in plays on the internal synth (NoteInput) and the instrument's
// messages (InstrumentInput, below). NoteInput is the up-converter: MIDI 1.0 and MPE, and standard
// MIDI 2.0 (an edge), into the Striso UMP profile (ump.h), whose decoder then plays the engines
// like every other source. The one copy: the board's synth_control.cpp (MIDI in from USB and from
// the virtual board) and the SDK's hosts (sdk/: the shell, the renderer's MIDI files) both run it.
// Pure: no HAL, no heap; the voices (voice_allocator.h) and the echo filter are the host's.
//
// - MPE: the lower zone, member channels 2 .. 1 + members() (an MCM on channel 1 sets it with
//   set_members(); 15 at the start). Per member channel: its notes; pitch bend x the host's bend
//   range (RPN 0) on top of the note number; channel pressure (or poly pressure) -> pres; CC74 -> y.
// - Every other channel (the master channel 1, all of them with no zone): its notes; poly pressure
//   (that note) or channel pressure (all its notes) -> pres; CC1 (the mod wheel) -> y (0..1),
//   CC74 -> y (64 = 0). Its pitch bend is the instrument's (+-2 semitones: instrument_bend(),
//   set_bend()), which the notes follow too.
// - Note-on velocity -> vpres, a strike (the decoder fades it, ump.h); until pressure comes, pres
//   is the velocity (a keyboard without aftertouch still sounds), and a held note's pres never
//   drops to 0. Note-off velocity -> the release (vpres < 0).
// - With all NOTES slots held, a new note takes the oldest one.
// - Standard MIDI 2.0 (message2): the same with its resolution: 16-bit velocities, 32-bit
//   pressures and controllers (full scale = the Striso's full press, 1.0), CC74 and pitch bends
//   bipolar by the spec (the centre exact), Registered Per-Note Controller 3 (Pitch 7.25: the
//   note's absolute pitch), the Registered Controllers 0 (bend range) and 6 (MCM).
// A note in slot s is the profile's note id_base() + s.
//
// The host (any type with these members; the calls are templates, inlined):
//   void ump(const ump::Packet&)    a packet of the Striso profile, for the decoder
//   bool echoed(int channel)        the host's own notes went out on this channel lately (an echo
//                                   of them is not played)
//   float bend_range()              the notes' pitch bend range, semitones (RPN 0)
// The host flushes its decoder after each message.
#ifndef STRISO_DSP_MIDI_IN_H_
#define STRISO_DSP_MIDI_IN_H_

#include <stdint.h>

#include "ump.h"

namespace striso::midi {

inline constexpr int NOTES = 16; // notes held at once (the board's MIDI_IN_NOTES)
using ump::NoteSignals;
using ump::TICK_SECONDS;

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

// a MIDI 2.0 value (32 bits) as a float: unsigned 0 .. 1, or bipolar -1 .. 1 by the spec (the
// centre 0x80000000 is 0 exactly; each side scaled to its own end)
[[nodiscard]] inline float unsigned32(uint32_t v) { return (float)((double)v / 4294967295.0); }
[[nodiscard]] inline float bipolar32(uint32_t v) {
	const double c = 2147483648.0, d = (double)v - c;
	return (float)(d < 0 ? d / c : d / 2147483647.0);
}

class NoteInput {
public:
	// the profile's note id of slot 0 (the board: after its keys)
	void set_id_base(int base) { base_ = base; }
	[[nodiscard]] int id_base() const { return base_; }

	// a MIDI 1.0 note message (note on/off, poly and channel pressure, a member channel's pitch
	// bend, CC1, CC74): played, or ignored as an echo; false: not a note message (the host's)
	template <class Host> bool message(Host& h, uint8_t channel, uint8_t status, uint8_t data1, uint8_t data2) {
		if (status == 0x80 || (status == 0x90 && data2 == 0)) {
			// (always: a note-off of a MIDI note that sounds is never an echo)
			note_off(h, channel, data1, -(status == 0x80 ? data2 : 0) * (1.0f / 127.0f));
			return true;
		}
		const bool note_data = status == 0x90 || status == 0xa0 || status == 0xd0 || (status == 0xe0 && member(channel)) ||
		    (status == 0xb0 && (data1 == 1 || data1 == 74));
		if (!note_data) return false;
		if (h.echoed(channel)) return true; // the host's own notes, echoed
		switch (status) {
		case 0x90: note_on(h, channel, data1, data2 * (1.0f / 127.0f)); break;
		case 0xa0: pressure(h, channel, data1, data2 * (1.0f / 127.0f)); break;
		case 0xd0: pressure(h, channel, -1, data1 * (1.0f / 127.0f)); break;
		case 0xe0: bend(h, channel, (float)((int)((data2 << 7) + data1) - 0x2000) * (1.0f / 8192)); break;
		default: // CC1 (mod wheel, 0..1) or CC74 (64 = 0)
			tilt(h, channel, data1 == 1 ? data2 * (1.0f / 127.0f) : clampf((data2 - 64) * (1.0f / 64.0f), -1.0f, 1.0f));
			break;
		}
		return true;
	}

	// a standard MIDI 2.0 channel voice message (UMP type 4): the same notes in its resolution;
	// false: not one of these (the host's: InstrumentInput::message2)
	template <class Host> bool message2(Host& h, uint32_t w0, uint32_t w1) {
		if ((w0 >> 28) != 4) return false;
		const uint8_t status = (uint8_t)((w0 >> 16) & 0xf0), channel = (uint8_t)((w0 >> 16) & 0x0f);
		const uint8_t d1 = (uint8_t)((w0 >> 8) & 0x7f), d2 = (uint8_t)(w0 & 0xff);
		if (status == 0x80) {
			note_off(h, channel, d1, -(float)((double)(w1 >> 16) / 65535.0));
			return true;
		}
		const bool note_data = status == 0x90 || status == 0xa0 || status == 0xd0 || status == 0x00 || (status == 0xe0 && member(channel)) ||
		    (status == 0xb0 && (d1 == 1 || d1 == 74));
		if (!note_data) return false;
		if (h.echoed(channel)) return true;
		switch (status) {
		case 0x90: note_on(h, channel, d1, (float)((double)(w1 >> 16) / 65535.0)); break;
		case 0xa0: pressure(h, channel, d1, unsigned32(w1)); break;
		case 0xd0: pressure(h, channel, -1, unsigned32(w1)); break;
		case 0xe0: bend(h, channel, bipolar32(w1)); break;
		case 0x00: // Registered Per-Note Controller: 3, Pitch 7.25 (the note's absolute pitch)
			if (d2 == 3) {
				const int s = find(channel, d1);
				if (s >= 0) {
					slot_[s].pitch = (float)((double)w1 / 33554432.0);
					slot_[s].absolute = true;
					h.ump(ump::pitch_packet(base_ + s, pitch(s)));
				}
			}
			break;
		default: // CC1 (0 .. 1) or CC74 (bipolar)
			tilt(h, channel, d1 == 1 ? unsigned32(w1) : bipolar32(w1));
			break;
		}
		return true;
	}

	// every note off at once, silently (the panic key)
	template <class Host> void all_off(Host& h) {
		for (int s = 0; s < NOTES; s++) {
			if (slot_[s].channel >= 0) {
				h.ump(ump::management_packet(base_ + s, ump::PNM_DROP));
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

	// slot s's note: its channel (-1: free) and key
	[[nodiscard]] int channel(int s) const { return slot_[s].channel; }
	[[nodiscard]] int key(int s) const { return slot_[s].key; }

private:
	struct Slot {
		int8_t channel = -1; // -1: free
		uint8_t key = 0;
		bool pressure = false; // pressure received: pres follows it, not the velocity
		bool absolute = false; // its pitch came as Pitch 7.25 (MIDI 2.0)
		uint32_t age = 0;
		float pitch = 0; // (absolute)
	};

	[[nodiscard]] static float clampf(float x, float lo, float hi) { return x > hi ? hi : (x < lo ? lo : x); }
	// a held note's pres stays above 0 (pres 0 is a release for the engines)
	[[nodiscard]] static float held_pres(float p) { return p > 1.0f / 127.0f ? p : 1.0f / 127.0f; }

	[[nodiscard]] int find(int channel, int key) const {
		for (int s = 0; s < NOTES; s++)
			if (slot_[s].channel == channel && slot_[s].key == key) return s;
		return -1;
	}

	// the slot's pitch: its key, the instrument's bend and its channel's (or its own, absolute)
	[[nodiscard]] float pitch(int s) const {
		const Slot& m = slot_[s];
		return m.absolute ? m.pitch + bend_ : m.key + bend_ + (member(m.channel) ? chan_bend_[m.channel] : 0.0f);
	}

	template <class Host> void note_on(Host& h, int channel, int key, float velo) {
		int s = find(channel, key);
		for (int n = 0; s < 0 && n < NOTES; n++)
			if (slot_[n].channel < 0) s = n;
		if (s < 0) { // all in use: the oldest
			s = 0;
			for (int n = 1; n < NOTES; n++)
				if (slot_[n].age < slot_[s].age) s = n;
			h.ump(ump::management_packet(base_ + s, ump::PNM_SILENCE));
		}
		Slot& m = slot_[s];
		m.channel = (int8_t)channel;
		m.key = (uint8_t)key;
		m.age = ++age_;
		m.absolute = false;
		m.pressure = chan_pres_set_[channel]; // MPE: the pressure sent before the note-on
		const int id = base_ + s;
		h.ump(ump::pitch_packet(id, pitch(s)));
		h.ump(ump::pres_packet(id, held_pres(m.pressure ? chan_pres_[channel] : velo)));
		h.ump(ump::signed_packet(id, ump::ctl_vpres, velo));
		h.ump(ump::signed_packet(id, ump::ctl_x, 0.0f));
		h.ump(ump::signed_packet(id, ump::ctl_y, chan_y_[channel]));
		h.ump(ump::note_on_packet(id, velo, true));
	}

	// release: the note-off velocity, negative
	template <class Host> void note_off(Host& h, int channel, int key, float release) {
		const int s = find(channel, key);
		if (s < 0) return;
		Slot& m = slot_[s];
		const int id = base_ + s;
		h.ump(ump::pres_packet(id, 0.0f));
		h.ump(ump::signed_packet(id, ump::ctl_vpres, release));
		h.ump(ump::note_off_packet(id));
		m.channel = -1;
		bool any = false;
		for (const Slot& x : slot_) any |= x.channel == channel;
		if (!any) chan_pres_set_[channel] = false; // pressure is per note (MPE)
	}

	// pressure of the notes of a channel (key -1: channel pressure)
	template <class Host> void pressure(Host& h, int channel, int key, float p) {
		if (key < 0) {
			chan_pres_[channel] = p;
			chan_pres_set_[channel] = true;
		}
		for (int s = 0; s < NOTES; s++) {
			if (slot_[s].channel == channel && (key < 0 || slot_[s].key == key)) {
				slot_[s].pressure = true;
				h.ump(ump::pres_packet(base_ + s, held_pres(p)));
			}
		}
	}

	// a member channel's bend, -1 .. 1 (x the bend range)
	template <class Host> void bend(Host& h, int channel, float b) {
		chan_bend_[channel] = b * h.bend_range();
		update(h, channel);
	}

	// a channel's y
	template <class Host> void tilt(Host& h, int channel, float y) {
		chan_y_[channel] = y;
		for (int s = 0; s < NOTES; s++)
			if (slot_[s].channel == channel) h.ump(ump::signed_packet(base_ + s, ump::ctl_y, y));
	}

	// the pitches of the notes of a channel (-1: all) after a change of its bend
	template <class Host> void update(Host& h, int channel) {
		for (int s = 0; s < NOTES; s++)
			if (slot_[s].channel >= 0 && (channel < 0 || slot_[s].channel == channel)) h.ump(ump::pitch_packet(base_ + s, pitch(s)));
	}

	Slot slot_[NOTES];
	int base_ = 0;
	uint32_t age_ = 0;
	int members_ = 15;
	float bend_ = 0.0f;        // the instrument's bend (semitones)
	float chan_bend_[16] = {}; // semitones, member channels
	float chan_y_[16] = {};
	float chan_pres_[16] = {};
	bool chan_pres_set_[16] = {};
};

// The messages of the instrument that MIDI in plays (after NoteInput took the notes'): the RPN
// select, the notes' bend range (RPN 0, at least 1, not the MPE zone master's), the MCM (RPN 6 on
// channel 1: the zone's member channels, at most 15), the instrument's bend (pitch bend outside
// the zone, +-2 semitones), the pedal (CC64). The rules are this class's; what follows from them
// is the host's (the host, any type with these members, besides NoteInput's):
//   bool zone_master(int channel)          an RPN 0 on this channel is the zone master's own range
//   void set_bend_range(float semitones)   the notes' new bend range
//   void zone_configured(int channel, int members)  an MCM came (the board: its MIDI out's mode)
//   void set_pedal(float)                  0 .. 1
//   bool bends_out_on(int channel)         the host sends its own notes' bends on this channel (a
//                                          pitch bend from it is an echo, not the instrument's)
//   void bend_moved(float delta)           the instrument's bend changes by delta (before the notes)
class InstrumentInput {
public:
	// a control change or pitch bend NoteInput didn't take: true if it was one of these
	template <class Host> bool message(Host& h, NoteInput& notes, uint8_t channel, uint8_t status, uint8_t data1, uint8_t data2) {
		if (status == 0xb0) {
			if (rpn_.control(channel, data1, data2)) return true;
			if (data1 == 6) { // data entry
				if (rpn_.is(channel, 0, 0)) {
					// the notes' bend range: at least 1 (0 divides by zero); an MPE host sends one
					// per channel, the master channel's is not the notes'
					if (data2 >= 1 && !h.zone_master(channel)) h.set_bend_range(data2);
				} else if (rpn_.is(channel, 0, 6)) {
					// the MPE Configuration Message: the lower zone (master channel 1) with data2
					// member channels (0: none); the upper zone (master 16) isn't supported
					if (channel == 0 && data2 <= 15) notes.set_members(data2);
					h.zone_configured(channel, data2);
				}
				return true;
			}
			if (data1 == 64) {
				h.set_pedal((float)data2 * (1.0f / 127.0f));
				return true;
			}
			return false;
		}
		if (status == 0xe0) {
			// a bend of the whole instrument, unless it is the echo of the host's own bends
			if (!h.bends_out_on(channel)) {
				const float bend = instrument_bend(data1, data2);
				h.bend_moved(bend - notes.bend());
				notes.set_bend(h, bend);
			}
			return true;
		}
		return false;
	}

	// the same in standard MIDI 2.0 (UMP type 4): the Registered Controllers 0 (the bend range:
	// semitones in the top 7 bits) and 6 (the MCM: members in the top 7 bits), CC64, the pitch
	// bend outside the zone (32-bit bipolar, x 2 semitones)
	template <class Host> bool message2(Host& h, NoteInput& notes, uint32_t w0, uint32_t w1) {
		if ((w0 >> 28) != 4) return false;
		const uint8_t status = (uint8_t)((w0 >> 16) & 0xf0), channel = (uint8_t)((w0 >> 16) & 0x0f);
		const uint8_t d1 = (uint8_t)((w0 >> 8) & 0x7f), d2 = (uint8_t)(w0 & 0x7f);
		if (status == 0x20) { // Registered Controller: bank d1, index d2
			const int top = (int)(w1 >> 25);
			if (d1 == 0 && d2 == 0) {
				if (top >= 1 && !h.zone_master(channel)) h.set_bend_range((float)((double)w1 / 33554432.0));
			} else if (d1 == 0 && d2 == 6) {
				if (channel == 0 && top <= 15) notes.set_members(top);
				h.zone_configured(channel, top);
			}
			return true;
		}
		if (status == 0xb0 && d1 == 64) {
			h.set_pedal(unsigned32(w1));
			return true;
		}
		if (status == 0xe0) {
			if (!h.bends_out_on(channel)) {
				const float bend = bipolar32(w1) * 2.0f;
				h.bend_moved(bend - notes.bend());
				notes.set_bend(h, bend);
			}
			return true;
		}
		return false;
	}

private:
	RpnSelect rpn_;
};

} // namespace striso::midi

#endif // STRISO_DSP_MIDI_IN_H_
