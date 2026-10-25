// LedView (src/synth/led_view.hpp): what the leds show for a state. The firmware's led calls are
// checked call for call by control_replay.settings.leds (recorded before the unit).
#include "led_view.hpp"

#include <initializer_list>

#include "doctest/doctest.h"

static_assert(led_view::octave_leds(1, 62.0f) == 0x0000);
static_assert(led_view::generator_color(7.0f) == 0x00aa00);

TEST_CASE("led view: the up/down leds show the transposition, two octaves each way, the layout's way") {
	struct Case {
		float start;
		uint32_t leds;
	};
	// semitones from the middle (62): 0, 1..11, 12, 13..23, 24+ and the same down
	for (const Case& c : {Case{62, 0x0000}, Case{63, 0x00a0}, Case{73, 0x00a0}, Case{74, 0x00f0}, Case{75, 0x00fa},
	         Case{85, 0x00fa}, Case{86, 0x00ff}, Case{92, 0x00ff}, Case{61, 0x0a00}, Case{51, 0x0a00}, Case{50, 0x0f00},
	         Case{49, 0xaf00}, Case{39, 0xaf00}, Case{38, 0xff00}, Case{32, 0xff00}}) {
		CAPTURE(c.start);
		CHECK(led_view::octave_leds(1, c.start) == c.leds);
	}
	// flipped: up is down
	CHECK(led_view::octave_leds(-1, 74.0f) == 0x0f00);
	CHECK(led_view::octave_leds(-1, 50.0f) == 0x00f0);
	// rounded to the nearest semitone (another tuning's octave isn't 12)
	CHECK(led_view::octave_leds(1, 62.4f) == 0x0000);
	CHECK(led_view::octave_leds(1, 62.6f) == 0x00a0);
}

TEST_CASE("led view: the indicator is the tuning's colour, brighter with Settings or glissando held") {
	CHECK(led_view::brightness(0, 0) == 64);
	CHECK(led_view::brightness(1, 0) == 80);
	CHECK(led_view::brightness(1, 3) == 96); // glissando: its lowest bit
	CHECK(led_view::indicator(0x00aa00, 8, 64) == 0xaa);
	CHECK(led_view::indicator(0x00aa00, 8, 96) == 0xaa * 96 / 64);
	CHECK(led_view::indicator(0x00aa00, 16, 96) == 0);
	CHECK(led_view::settings_shown(2));
	CHECK_FALSE(led_view::settings_shown(1));
}

TEST_CASE("led view: a fifth's tuning system colour") {
	CHECK(led_view::generator_color(6.86f) == 0x550055);  // 7-TET
	CHECK(led_view::generator_color(6.95f) == 0x0000aa);  // 19-TET
	CHECK(led_view::generator_color(6.968f) == 0x005555); // 31-TET, meantone
	CHECK(led_view::generator_color(7.0f) == 0x00aa00);   // 12-TET
	CHECK(led_view::generator_color(7.02f) == 0x555500);  // Pythagorean
	CHECK(led_view::generator_color(7.2f) == 0xaa0000);   // 5-TET
}

TEST_CASE("led view: the settings keys' dials stay on the 16 leds") {
	CHECK(led_view::pressure_dial(3.0f) == 15);
	CHECK(led_view::pressure_dial(-1.0f) == 0);
	CHECK(led_view::pressure_level(5.0f) == 255);
	CHECK(led_view::tilt_dial(-2.0f) == 0); // a negative tilt (SC-27)
	CHECK(led_view::tilt_blue(-2.0f) == 0);
	CHECK(led_view::velocity_dial(2.0f) == 15);
	CHECK(led_view::bend_dial(4.0f) == 15);
	CHECK(led_view::threshold_dial(16 * (1 << 24) / 256) == 15);
	CHECK(led_view::threshold_dial(-5) == 0);
	CHECK(led_view::volume_dial(127.0f) == 15);
	CHECK(led_view::rate_dial(1) == 15);
	CHECK(led_view::bend_range_dial(3) == 8);
	CHECK(led_view::voice_dial(2, 4) == 3);
	CHECK(led_view::voice_dial(0, 1) == 0); // one preset: no dial
}
