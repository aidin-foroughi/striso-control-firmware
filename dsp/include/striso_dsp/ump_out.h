// MIDI 2.0 out: the Striso UMP profile's notes (ump.h, inside) as standard MIDI 2.0 (the edge,
// doc/engines.md "MIDI 2.0"). Fed the profile's packets of the notes that go out (the board: its
// keys), it sends what changed at flush(), in standard MIDI 2.0 channel voice messages on group 0,
// channel 1, every note on its own note number:
//
//   note on     Note On, velocity 16 (the strike, 0 .. 1), attribute 3: Pitch 7.9
//   pitch       Registered Per-Note Controller 3, Pitch 7.25 (absolute; right after the Note On)
//   pres        Poly Pressure, 32 bit: 1.0 (the Striso's full press) = 0xffffffff
//   y           Registered Per-Note Controller 74 (brightness, MPE's CC74), bipolar
//   x           Assignable Per-Note Controller 3, bipolar
//   vpres       Assignable Per-Note Controller 2, bipolar (the continuous velocity)
//   note off    Note Off, velocity 16 (the release, 0 .. 1)
//
// The edge converts: a value is clamped to the standard range (pres and the velocities 0 .. 1, x,
// y and vpres -1 .. 1, the pitch to Pitch 7.25's 0 .. 128), the exact values (the float32 escape)
// too, and only a changed 32-bit value is sent again. A note's number is the nearest one to its
// pitch that no other sounding note has (the Striso's C# and Db are two keys of one 12-TET note),
// kept until its Note Off; its pitch is the per-note pitch, so a receiver that follows Pitch 7.25
// (or the Note On's Pitch 7.9) plays it in tune, one that doesn't plays the nearest note.
// The sink: any type with void ump(uint32_t w0, uint32_t w1).
#ifndef STRISO_DSP_UMP_OUT_H_
#define STRISO_DSP_UMP_OUT_H_

#include <stdint.h>

#include <initializer_list>

#include "ump.h"

namespace striso::ump {

// standard MIDI 2.0 values (from the engines' floats; clamped)
[[nodiscard]] inline uint32_t std_unsigned(float x) {
	return !(x > 0.0f) ? 0u : (x >= 1.0f ? 0xffffffffu : round_u32((double)x * 4294967295.0));
}
[[nodiscard]] inline uint32_t std_bipolar(float x) {
	if (!(x > -1.0f)) return 0u;
	if (x >= 1.0f) return 0xffffffffu;
	return x <= 0.0f ? 0x80000000u - round_u32(-(double)x * 2147483648.0) : 0x80000000u + round_u32((double)x * 2147483647.0);
}
[[nodiscard]] inline uint16_t std_velocity(float x) {
	return !(x > 0.0f) ? (uint16_t)0 : (x >= 1.0f ? (uint16_t)0xffff : (uint16_t)round_u32((double)x * 65535.0));
}
[[nodiscard]] inline uint32_t std_pitch(float note) { // Pitch 7.25
	const double v = (double)note * 33554432.0;
	return !(v > 0) ? 0u : (v >= 4294967294.5 ? 0xffffffffu : round_u32(v));
}

inline constexpr uint8_t ATTR_PITCH_7_9 = 3;
inline constexpr uint8_t RPNC_PITCH = 3, RPNC_BRIGHTNESS = 74;
inline constexpr uint8_t STD_GROUP = 0, STD_CHANNEL = 0;

template <int Ids> class StandardOut {
	static_assert(Ids <= 128, "a note id is a note number");

public:
	// a packet of the profile (only the notes' own: Note On/Off and the controllers)
	void packet(uint32_t w0, uint32_t w1) {
		if ((w0 >> 28) != 4) return;
		const int status = (int)((w0 >> 20) & 0xf), id = (int)((w0 >> 8) & 0x7f), index = (int)(w0 & 0xff);
		if (id >= Ids) return;
		Note& n = note_[id];
		if (status == note_on) {
			n.on = true;
			n.strike = (float)((double)(w1 >> 16) / 65535.0);
		} else if (status == note_off) {
			n.on = false;
		} else if (!apply(n, status, index, w1)) {
			return;
		}
		if (!n.queued) {
			n.queued = true;
			queue_[queued_++] = (uint8_t)id;
		}
	}
	void packet(const Packet& p) { packet(p.w0, p.w1); }

	// the changes since the last flush, note by note in the order they changed
	template <class Sink> void flush(Sink& s) {
		for (int k = 0; k < queued_; k++) {
			Note& n = note_[queue_[k]];
			n.queued = false;
			if (n.on && !n.sounding) start(s, n);
			if (n.sounding) controllers(s, n);
			if (!n.on && n.sounding) stop(s, n);
		}
		queued_ = 0;
	}

	// the note number note id sounds on (-1: not sounding)
	[[nodiscard]] int number(int id) const { return note_[id].sounding ? note_[id].number : -1; }

private:
	struct Note : Controls {
		float strike = 0;
		bool on = false, sounding = false, queued = false, fresh = false;
		uint8_t number = 0;
		uint32_t sent[5] = {}; // pitch, pres, y, x, vpres as last sent
	};

	static constexpr uint32_t head2(uint8_t status, uint8_t number, uint8_t index) {
		return 0x40000000u | ((uint32_t)STD_GROUP << 24) | ((uint32_t)status << 20) | ((uint32_t)STD_CHANNEL << 16) |
		    ((uint32_t)number << 8) | index;
	}

	// the nearest free note number to the pitch
	uint8_t free_number(float note) {
		const float c = note < 0.0f ? 0.0f : (note > 127.0f ? 127.0f : note);
		const int want = (int)(c + 0.5f);
		for (int d = 0; d < 128; d++) {
			for (int k : {want - d, want + d}) {
				if (k >= 0 && k < 128 && !(used_[k >> 5] & (1u << (k & 31)))) return (uint8_t)k;
			}
		}
		return (uint8_t)want; // (more than 128 notes: shares one)
	}

	template <class Sink> void start(Sink& s, Note& n) {
		n.number = free_number(n.note);
		used_[n.number >> 5] |= 1u << (n.number & 31);
		n.sounding = true;
		const uint32_t pitch = std_pitch(n.note);
		s.ump(head2(note_on, n.number, ATTR_PITCH_7_9), (uint32_t)std_velocity(n.strike) << 16 | pitch >> 16);
		n.sent[0] = pitch;
		s.ump(head2(0x0, n.number, RPNC_PITCH), pitch);
		n.fresh = true; // (every controller goes out once)
	}

	template <class Sink> void controllers(Sink& s, Note& n) {
		const uint32_t v[5] = {std_pitch(n.note), std_unsigned(n.pres), std_bipolar(n.y), std_bipolar(n.x), std_bipolar(n.vpres)};
		const uint32_t h[5] = {head2(0x0, n.number, RPNC_PITCH), head2(poly_pressure, n.number, 0), head2(0x0, n.number, RPNC_BRIGHTNESS),
		    head2(pnc_assignable, n.number, ctl_x), head2(pnc_assignable, n.number, ctl_vpres)};
		for (int i = 0; i < 5; i++) {
			if (v[i] != n.sent[i] || (n.fresh && i > 0)) {
				s.ump(h[i], v[i]);
				n.sent[i] = v[i];
			}
		}
		n.fresh = false;
	}

	template <class Sink> void stop(Sink& s, Note& n) {
		s.ump(head2(note_off, n.number, 0), (uint32_t)std_velocity(-n.vpres) << 16);
		used_[n.number >> 5] &= ~(1u << (n.number & 31));
		n.sounding = false;
	}

	Note note_[Ids];
	uint8_t queue_[Ids] = {};
	int queued_ = 0;
	uint32_t used_[4] = {}; // the note numbers sounding
};

} // namespace striso::ump

#endif // STRISO_DSP_UMP_OUT_H_
