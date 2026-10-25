// What a voice engine is played with: the block size and the controls of one voice and of the
// whole instrument, as the board's synth_control writes them (src/synth/synth.h, synth_interface).
// Namespace strisy, where the engines have always found them; the engine contract
// (engine.h, namespace striso::dsp) builds on these.
#ifndef STRISO_DSP_CONTROLS_H_
#define STRISO_DSP_CONTROLS_H_

namespace strisy {

// samples per process() call: the codec's buffer (CHANNEL_BUFFER_SIZE), 0.65 ms at 48828 Hz
static constexpr int BLOCK = 32;

// Per voice controls, written asynchronously by synth_control (same meaning as the Faust sliders):
//   note   pitch in semitones, MIDI numbering (69 = A4), fractional: bends and tunings included
//   pres   pressure, 0 (released) .. 1 (firm; up to ~1.5 when pressed very hard)
//   vpres  pressure velocity, about -1 .. 1: > 0 while the key goes down (the strike), < 0 while
//          it comes up (the release); 0 when the pressure is steady
//   but_x  tilt along the row, -1 .. 1 (synth_control has already bent note by 0.5 x^3)
//   but_y  tilt across the row, -1 .. 1
struct VoiceControls {
	float note, pres, vpres, but_x, but_y;
};

// Global controls: accelerometer (g, acc_abs its magnitude: 1 at rest), gyroscope (rot_*,
// about -1 .. 1) and the pedal (0 .. 1)
struct GlobalControls {
	float acc_abs, acc_x, acc_y, acc_z, rot_x, rot_y, rot_z, pedal;
};

} // namespace strisy

#endif // STRISO_DSP_CONTROLS_H_
