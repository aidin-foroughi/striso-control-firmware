// The keyboard's tuning math, shared by synth_control.cpp and the host tests (tests/test_tuning.cpp):
// the isomorphic layout's coordinates per button and the pitch a key plays.
//
// Console command StcN (pconnection.c -> synth_tuning_print() in synth_control.cpp): read only,
// for tuning-aware note labels (Striso Live). Floats are the 8 hex digits of their IEEE bits (no
// float printf), as in the other list commands:
//   tuning <cur_tuning> <fifth> <octave> <offset> <color> <base> <count>
//     cur_tuning  the tuning loaded by load_tuning() (0: 12-TET, 1.. fT<n>...), -1 when the
//                 fifth was changed since (set_notegen1: tuning-system settings key)
//     fifth       the fifth in semitones (notegen1: fT<n>fifth / 100, 12-TET 7.0)
//     octave      the octave in semitones (notegen0: fT<n>oct / 100, 12-TET 12.0)
//     offset      the tuning's global offset in semitones (note_offset: fT<n>off / 100, 12-TET 0)
//     color       the tuning's LED colour, 6 hex digits (hT<n>color)
//     base        the transposition: the pitch of the centre key D (start_note_offset, default 62)
//     count       the number of key lines that follow (61)
//   <n> <name> <pitch> <key_offset>   one line per key, n = 0..60 (key numbers as in the config
//                 and in Stck, names from put_button_name, e.g. D_3)
//     pitch       the pitch in semitones (MIDI note numbers, fractional) the key plays when
//                 pressed now: key_pitch() below, the formula synth_control.cpp uses for
//                 buttons[].note at note on, before pitch bend; a key held since before a
//                 transposition keeps the old base until it is released
//     key_offset  the key's own tuning offset in semitones (tuning_note_offset: fT<n><key> / 100,
//                 0 when the tuning has none)
#ifndef STRISO_TUNING_H_
#define STRISO_TUNING_H_

namespace striso::tuning {

inline constexpr int KEY_COUNT = 61;    // playing keys (n = 0..60)
inline constexpr int BUTTON_COUNT = 68; // + the aux buttons, in the key scanner's order

// The isomorphic layout: a button's pitch is octave * coord0 + fifth * coord1 from the centre D.
// In key numbers (config_store.cpp): n = 17 * coord0 + 10 * coord1 + 30.
inline constexpr int coord0[BUTTON_COUNT] = {
    6, 5, 5, 4, 4, 3, 2, 2, 1, 1, 0, 0, -1, -2, -2, -3, -3,
    5, 4, 4, 3, 3, 2, 1, 1, 0, 0, -1, -1, -2, -3, -3, -4, -4,
    4, 3, 3, 2, 2, 1, 0, 0, -1, -1, -2, -2, -3, -4, -4, -5, -5,
    3, 2, 2, 1, 1, 0, -1, -1, -2, -2, -3, -3, -4, -5, -5, -6, -6};
inline constexpr int coord1[BUTTON_COUNT] = {
    -8, -7, -6, -5, -4, -3, -2, -1, 0, 1, 2, 3, 4, 5, 6, 7, 8,
    -8, -7, -6, -5, -4, -3, -2, -1, 0, 1, 2, 3, 4, 5, 6, 7, 8,
    -8, -7, -6, -5, -4, -3, -2, -1, 0, 1, 2, 3, 4, 5, 6, 7, 8,
    -8, -7, -6, -5, -4, -3, -2, -1, 0, 1, 2, 3, 4, 5, 6, 7, 8};

struct Tuning {
	float octave = 12.0f; // notegen0
	float fifth = 7.0f;   // notegen1
	float offset = 0.0f;  // note_offset
	float base = 62.0f;   // start_note_offset
};

// the pitch of a button, as synth_control.cpp computes buttons[].note at note on:
// start_note_offset + tuning_note_offset + notegen0 * coord0 + notegen1 * coord1 + note_offset
[[nodiscard]] constexpr float key_pitch(const Tuning& t, float key_offset, int c0, int c1) {
	return t.base + key_offset + t.octave * static_cast<float>(c0) + t.fifth * static_cast<float>(c1) + t.offset;
}

} // namespace striso::tuning

#endif
