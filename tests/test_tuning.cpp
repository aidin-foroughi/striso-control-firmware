// The tuning math behind the StcN console command (src/synth/tuning.h): every key's pitch from its
// layout coordinates, and its name (put_button_name, key numbers via button_number_map) as
// synth_tuning_print() combines them, so tuning-aware labels in Striso Live are right.
#include <cmath>
#include <cstring>
#include <string>

#include "doctest/doctest.h"
#include "tuning.h"

extern "C" {
#include "config_store.h"
}

namespace {

using striso::tuning::KEY_COUNT;
using striso::tuning::Tuning;

// a key's name (e.g. "D_3") as 12-TET pitch: the 17 names per octave of put_button_name
double name_pitch_12tet(const char* name) {
	static const char* const names[17] = {"C_", "Db", "C#", "D_", "Eb", "D#", "E_", "F_", "Gb",
	    "F#", "G_", "Ab", "G#", "A_", "Bb", "A#", "B_"};
	static const int pc[17] = {0, 1, 1, 2, 3, 3, 4, 5, 6, 6, 7, 8, 8, 9, 10, 10, 11};
	for (int i = 0; i < 17; i++)
		if (std::strncmp(name, names[i], 2) == 0) return 12.0 * (name[2] - '1' + 3) + pc[i]; // D_3 = 62
	return -1;
}

float pitch_of(const Tuning& t, int n, float key_offset = 0.0f) {
	const int but = button_number_map[n];
	return striso::tuning::key_pitch(t, key_offset, striso::tuning::coord0[but], striso::tuning::coord1[but]);
}

} // namespace

TEST_CASE("tuning: in 12-TET every key plays the note its name says") {
	const Tuning t; // 12-TET, base 62 (D)
	for (int n = 0; n < KEY_COUNT; n++) {
		char name[4] = {};
		put_button_name(n, name);
		CAPTURE(n);
		CAPTURE(std::string(name, 3));
		CHECK(pitch_of(t, n) == static_cast<float>(name_pitch_12tet(name)));
	}
}

TEST_CASE("tuning: key numbers are 17-tone steps, n = 17 coord0 + 10 coord1 + 30") {
	for (int n = 0; n < KEY_COUNT; n++) {
		const int but = button_number_map[n];
		CAPTURE(n);
		CHECK(17 * striso::tuning::coord0[but] + 10 * striso::tuning::coord1[but] + 30 == n);
	}
	// so in 17-EDO (fifth 10 of 17 steps) the keys rise by 12/17 semitone from key to key
	Tuning t;
	t.fifth = 12.0f * 10.0f / 17.0f;
	for (int n = 0; n < KEY_COUNT; n++) CHECK(pitch_of(t, n) == doctest::Approx(62.0 + (n - 30) * 12.0 / 17.0).epsilon(1e-6));
}

TEST_CASE("tuning: transposition, the tuning offset and per-key offsets add to the pitch") {
	Tuning t;
	t.fifth = 6.96f; // a meantone-ish fifth
	t.base = 60.0f;
	t.offset = 0.25f;
	const float plain = pitch_of(t, 40);
	CHECK(pitch_of(t, 40, 0.1f) == doctest::Approx(plain + 0.1f));
	t.base = 61.0f;
	CHECK(pitch_of(t, 40) == doctest::Approx(plain + 1.0f));
	// the centre key D (n = 30) is base + offset in every tuning
	CHECK(pitch_of(t, 30) == doctest::Approx(61.25f));
}
