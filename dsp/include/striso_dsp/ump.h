// The Striso UMP profile: how the per-note controls the engines read travel as MIDI 2.0 Universal
// MIDI Packets inside the instrument, and the one decoder from them to the engines' controls
// (doc/engines.md, "MIDI 2.0"). Everything that plays the internal synth goes this way: the
// board's keys (synth_control.cpp encodes them), MIDI 1.0 and MPE in (midi_in.h up-converts them),
// standard MIDI 2.0 in (midi_in.h, NoteInput::message2: an edge, converted to the Striso scale).
//
// Inside, the messages are standard MIDI 2.0 channel voice messages (type 4, group 0, channel 0)
// with Striso scaling, chosen so the board's values arrive exactly as the engines got them before
// (bit for bit): pressure 1.0 is the board's calibrated full press (code 8191), with room up to
// 4.0; velocity, x, y are signed with the same unit; pitch is signed 9.23 semitones (-256 .. 256,
// so MPE bends past MIDI's 0 .. 128 stay exact). The note number is the host's note id (the
// board: a key 0 .. 67, a MIDI in note's slot 68 .. 83), not a pitch.
//
//   engine control  message                                    Striso scale (inside)
//   note on/off     Note On 0x9 / Note Off 0x8, velocity 16    attribute 0x02 (profile): bit 0 =
//                                                              a strike (vpres fades, below)
//   note (pitch)    Assignable Per-Note Controller 0x1, 1      signed 9.23 semitones
//   pres            Poly Pressure 0xA                          pres x 8191 x 2^17 (unsigned)
//   vpres           Assignable Per-Note Controller 0x1, 2      vpres x 8191 x 2^16 (signed)
//   but_x           Assignable Per-Note Controller 0x1, 3      but_x x 8191 x 2^16 (signed)
//   but_y           Assignable Per-Note Controller 0x1, 4      but_y x 8191 x 2^16 (signed)
//   sound ends      Per-Note Management 0xF, flag D (2)        the id is taken by a newer note
//   note dropped    Per-Note Management 0xF, flag S (1)        it ends silently (all notes off)
//   (exact values)  Assignable Per-Note Controller 0x1, 0x11   the float32's bits, for a value
//                   .. 0x15: pitch, pres, vpres, x, y          the scale can't hold exactly
// The board's codes (code / 8191), MIDI 1.0's values and the MPE pitches are all exact in the
// scale. A value computed from several keys (the portamento and free transpose merges: weighted
// averages) can need more bits than the scale's 32: the encoder sends it as an exact value then,
// so the engines get what they always got. The edges convert those to the standard scale too.
// (The globals have their numbers too: the pedal CC64, the accelerometer and gyroscope
// Assignable Controllers 0x10 .. 0x16; the board writes them directly for now.)
//
// A note whose Note On says "strike" gets its vpres faded by the decoder: x0.6 per tick() (every
// TICK_SECONDS) while above 0.01, then 0, as the board's MIDI in always did; the board's keys
// send their measured vpres instead.
//
// The decoder applies the host's touch factors per note (the board: its keys' sensitivities,
// MIDI in notes 1) in the board's float expressions, and calls its host once per changed note at
// flush() (the host, any type with these members; the calls are templates, inlined):
//   Touch touch(int id)                            the note's pres, velo and y factors
//   void note_started(int id, const NoteSignals&)  a Note On: give it a voice
//   void note_changed(int id, const NoteSignals&)  its controls changed (to its voice)
//   void note_silenced(int id)                     its sound ends now (taken by a newer note)
//   void note_dropped(int id)                      it ends silently
#ifndef STRISO_DSP_UMP_H_
#define STRISO_DSP_UMP_H_

#include <stdint.h>

namespace striso::ump {

inline constexpr double TICK_SECONDS = 0.002; // the strike fade's step (the board's synth_tick)

// a note's key signals, as a key gives them (controls.h), and whether it is held
struct NoteSignals {
	float note, pres, vpres, but_x, but_y;
	bool on;
};

// a note's touch factors (multiplied in the decoder): pressure, velocity, y
struct Touch {
	float pres, velo, y;
};

// --- the profile ---------------------------------------------------------------------------------

enum Status : uint8_t {
	pnc_assignable = 0x1,
	note_off = 0x8,
	note_on = 0x9,
	poly_pressure = 0xa,
	control_change = 0xb,
	per_note_management = 0xf,
};
// the Assignable Per-Note Controllers of the profile
enum Controller : uint8_t {
	ctl_pitch = 1,
	ctl_vpres = 2,
	ctl_x = 3,
	ctl_y = 4,
	exact_pitch = 0x11, // the exact values (float32 bits)
	exact_pres = 0x12,
	exact_vpres = 0x13,
	exact_x = 0x14,
	exact_y = 0x15,
};
// the Assignable Controllers of the globals, and CC64
enum Global : uint8_t {
	glb_acc_abs = 0x10,
	glb_acc_x = 0x11,
	glb_acc_y = 0x12,
	glb_acc_z = 0x13,
	glb_rot_x = 0x14,
	glb_rot_y = 0x15,
	glb_rot_z = 0x16,
	glb_pedal = 64,
};
inline constexpr uint8_t ATTR_PROFILE = 0x02; // Note On attribute type: profile specific
inline constexpr uint16_t ATTR_STRIKE = 1;    // its data, bit 0: the velocity is a strike to fade
inline constexpr uint8_t PNM_DROP = 1;        // Per-Note Management flags: S
inline constexpr uint8_t PNM_SILENCE = 2;     //                            D

inline constexpr double PRES_ONE = 8191.0 * 131072.0;  // pres 1.0: the board's code 8191, << 17
inline constexpr double SIGNED_ONE = 8191.0 * 65536.0; // vpres, but_x, but_y 1.0: code 8191, << 16
inline constexpr double PITCH_ONE = 8388608.0;         // one semitone: 2^23

// one message: two words
struct Packet {
	uint32_t w0, w1;
};

// a Universal MIDI Packet's length in words, from its first word's message type (UMP 1.1)
[[nodiscard]] constexpr int words(uint32_t w0) {
	constexpr uint8_t n[16] = {1, 1, 1, 2, 2, 4, 1, 1, 2, 2, 2, 3, 3, 4, 4, 4};
	return n[w0 >> 28];
}

[[nodiscard]] constexpr uint32_t head(uint8_t status, uint8_t note, uint8_t index) {
	return 0x40000000u | ((uint32_t)status << 20) | ((uint32_t)(note & 0x7f) << 8) | index;
}

[[nodiscard]] inline double round_half_away(double x) { return x >= 0 ? (double)(int64_t)(x + 0.5) : -(double)(int64_t)(0.5 - x); }

// values in the Striso scale (inside): from the engines' floats, and back (one rounding, in double)
[[nodiscard]] inline uint32_t pres_value(float p) {
	const double v = round_half_away((double)p * PRES_ONE);
	return v <= 0 ? 0u : (v >= 4294967295.0 ? 0xffffffffu : (uint32_t)v);
}
[[nodiscard]] inline float pres_of(uint32_t v) { return (float)((double)v / PRES_ONE); }
[[nodiscard]] inline uint32_t signed_value(float x) {
	const double v = round_half_away((double)x * SIGNED_ONE);
	return (uint32_t)(int32_t)(v <= -2147483648.0 ? -2147483647.0 - 1.0 : (v >= 2147483647.0 ? 2147483647.0 : v));
}
[[nodiscard]] inline float signed_of(uint32_t v) { return (float)((double)(int32_t)v / SIGNED_ONE); }
[[nodiscard]] inline uint32_t pitch_value(float note) {
	const double v = round_half_away((double)note * PITCH_ONE);
	return (uint32_t)(int32_t)(v <= -2147483648.0 ? -2147483647.0 - 1.0 : (v >= 2147483647.0 ? 2147483647.0 : v));
}
[[nodiscard]] inline float pitch_of(uint32_t v) { return (float)((double)(int32_t)v / PITCH_ONE); }

[[nodiscard]] inline uint32_t float_bits(float x) {
	union {
		float f;
		uint32_t u;
	} c = {x};
	return c.u;
}
[[nodiscard]] inline float bits_float(uint32_t u) {
	union {
		uint32_t u;
		float f;
	} c = {u};
	return c.f;
}

// the messages of the profile
[[nodiscard]] inline Packet note_on_packet(int id, float strike, bool fades) {
	const double s = strike <= 0.0f ? 0.0 : (strike >= 1.0f ? 65535.0 : round_half_away((double)strike * 65535.0));
	return {head(note_on, (uint8_t)id, ATTR_PROFILE), ((uint32_t)s << 16) | (fades ? ATTR_STRIKE : 0u)};
}
[[nodiscard]] inline Packet note_off_packet(int id) { return {head(note_off, (uint8_t)id, 0), 0}; }
// (each in the scale if the scale holds it exactly, else as an exact value)
[[nodiscard]] inline Packet pres_packet(int id, float p) {
	const uint32_t v = pres_value(p);
	if (pres_of(v) == p) return {head(poly_pressure, (uint8_t)id, 0), v};
	return {head(pnc_assignable, (uint8_t)id, exact_pres), float_bits(p)};
}
[[nodiscard]] inline Packet pitch_packet(int id, float note) {
	const uint32_t v = pitch_value(note);
	if (pitch_of(v) == note) return {head(pnc_assignable, (uint8_t)id, ctl_pitch), v};
	return {head(pnc_assignable, (uint8_t)id, exact_pitch), float_bits(note)};
}
[[nodiscard]] inline Packet signed_packet(int id, Controller c, float x) {
	const uint32_t v = signed_value(x);
	if (signed_of(v) == x) return {head(pnc_assignable, (uint8_t)id, c), v};
	return {head(pnc_assignable, (uint8_t)id, (uint8_t)(c + 0x11)), float_bits(x)};
}
[[nodiscard]] inline Packet management_packet(int id, uint8_t flags) { return {head(per_note_management, (uint8_t)id, flags), 0}; }

// --- the decoder ---------------------------------------------------------------------------------

template <int Ids> class Decoder {
	static_assert(Ids <= 128, "a note id is a note number");

public:
	// one packet of the profile (others are ignored)
	void packet(uint32_t w0, uint32_t w1) {
		if ((w0 >> 28) != 4) return;
		const int status = (int)((w0 >> 20) & 0xf), id = (int)((w0 >> 8) & 0x7f), index = (int)(w0 & 0xff);
		if (id >= Ids) return;
		Note& n = note_[id];
		switch (status) {
		case note_on:
			n.on = true;
			n.strike = index == ATTR_PROFILE && (w1 & ATTR_STRIKE) != 0;
			if (n.strike) fading_ = true;
			n.pending |= STARTED | CHANGED;
			break;
		case note_off:
			n.on = false;
			n.pending |= CHANGED;
			break;
		case poly_pressure:
			n.pres = pres_of(w1);
			n.pending |= CHANGED;
			break;
		case pnc_assignable:
			switch (index) {
			case ctl_pitch: n.note = pitch_of(w1); break;
			case ctl_vpres: n.vpres = signed_of(w1); break;
			case ctl_x: n.x = signed_of(w1); break;
			case ctl_y: n.y = signed_of(w1); break;
			case exact_pitch: n.note = bits_float(w1); break;
			case exact_pres: n.pres = bits_float(w1); break;
			case exact_vpres: n.vpres = bits_float(w1); break;
			case exact_x: n.x = bits_float(w1); break;
			case exact_y: n.y = bits_float(w1); break;
			default: return;
			}
			n.pending |= CHANGED;
			break;
		case per_note_management:
			if (index & PNM_SILENCE) n.pending |= SILENCED;
			if (index & PNM_DROP) {
				n.on = false;
				n.pending |= DROPPED;
			}
			break;
		default: return;
		}
		if (!n.queued) {
			n.queued = true;
			queue_[queued_++] = (uint8_t)id;
		}
	}
	void packet(const Packet& p) { packet(p.w0, p.w1); }

	// the host hears of every note changed since the last flush, in the order they first changed
	template <class Host> void flush(Host& h) {
		for (int k = 0; k < queued_; k++) {
			const int id = queue_[k];
			Note& n = note_[id];
			const uint8_t p = n.pending;
			n.pending = 0;
			n.queued = false;
			if (p & SILENCED) h.note_silenced(id);
			if (p & DROPPED) {
				h.note_dropped(id);
				continue;
			}
			if (p & (STARTED | CHANGED)) {
				const NoteSignals s = signals(h, id);
				if (p & STARTED) h.note_started(id, s);
				h.note_changed(id, s);
			}
		}
		queued_ = 0;
	}

	// the strikes fade one step (then flush)
	template <class Host> void tick(Host& h) {
		bool fading = false;
		for (int id = 0; id < Ids; id++) {
			Note& n = note_[id];
			if (n.on && n.strike && n.vpres > 0.0f) {
				n.vpres = n.vpres > 0.01f ? n.vpres * 0.6f : 0.0f;
				fading |= n.vpres > 0.0f;
				n.pending |= CHANGED;
				if (!n.queued) {
					n.queued = true;
					queue_[queued_++] = (uint8_t)id;
				}
			}
		}
		fading_ = fading;
		flush(h);
	}
	// a strike is fading: tick() is due every TICK_SECONDS
	[[nodiscard]] bool fading() const { return fading_; }

	// a note's controls as decoded (before the touch factors)
	[[nodiscard]] float note(int id) const { return note_[id].note; }
	[[nodiscard]] float pres(int id) const { return note_[id].pres; }
	[[nodiscard]] float vpres(int id) const { return note_[id].vpres; }
	[[nodiscard]] bool on(int id) const { return note_[id].on; }

private:
	enum : uint8_t { STARTED = 1,
		CHANGED = 2,
		SILENCED = 4,
		DROPPED = 8 };
	struct Note {
		float note = 0, pres = 0, vpres = 0, x = 0, y = 0;
		bool on = false, strike = false, queued = false;
		uint8_t pending = 0;
	};

	[[nodiscard]] static float clampf(float x, float lo, float hi) { return x > hi ? hi : (x < lo ? lo : x); }

	// the engines' controls, as the board computed them: the touch factors on pres, vpres, y
	template <class Host> NoteSignals signals(Host& h, int id) const {
		const Note& n = note_[id];
		const Touch t = h.touch(id);
		return {n.note, n.pres * t.pres, n.vpres * t.velo, n.x, clampf(n.y * t.y, -1.0f, 1.0f), n.on};
	}

	Note note_[Ids];
	uint8_t queue_[Ids] = {};
	int queued_ = 0;
	volatile bool fading_ = false; // (read by the board's main loop: synth_tick)
};

} // namespace striso::ump

#endif // STRISO_DSP_UMP_H_
