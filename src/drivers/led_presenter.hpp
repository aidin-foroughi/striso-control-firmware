/*
 * The LEDs' logic, above the PWM: the indicator's colour and its gamma, a blink and its end, and
 * which up/down LEDs are on, off or dimmed. Pure C++20 with no HAL: led.cpp drives the PWM and the
 * pins from it, the host tests run it as it is (tests/led/test_led.cpp, against the old led.c).
 */
#pragma once

#include <array>
#include <cstdint>
#include <optional>

namespace led {

// a blink that never ends (led.c's UINT32_MAX)
inline constexpr uint32_t kNever = UINT32_MAX;

// Gamma correction to make light response close to linear: 0..256 to 0..2048 (the PWM period)
constexpr int gamma8(int x) { return x < 33 ? x : ((x + 1) * (x + 1)) / 32 - 1; }

// the indicator's PWM widths, red green blue
struct Duty {
	int r, g, b;
	bool operator==(const Duty&) const = default;
};

// the up/down LEDs, in the order of led_updown's nibbles (bits 0-3 up2, 4-7 up, 8-11 down,
// 12-15 down2): 0 off, f on, anything else dimmed (the PWM toggles a dimmed one)
enum class Level : uint8_t { off,
	on,
	dimmed };
struct UpDown {
	std::array<Level, 4> level{}; // up2, up, down, down2
	int dimmed = -1;              // the dimmed LED (the last one, in that order), -1: none
	bool operator==(const UpDown&) const = default;
};

class Presenter {
public:
	std::array<int, 3> color{};    // the indicator's colour as last set, r g b
	uint32_t blink_until = kNever; // the system time the blink ends after
	uint32_t updown_state = 0;     // the last up/down state, as given

	// the colour, and its widths
	Duty set(int r, int g, int b) {
		color = {r, g, b};
		return duty(r, g, b);
	}
	// a colour for a while (time ticks from now); then tick() shows the colour again
	Duty blink(int r, int g, int b, uint32_t now, uint32_t time) {
		blink_until = now + time;
		return duty(r, g, b);
	}
	// the end of a blink: the colour's widths, once
	std::optional<Duty> tick(uint32_t now) {
		if (now <= blink_until) return std::nullopt;
		blink_until = kNever;
		return duty(color[0], color[1], color[2]);
	}

	UpDown updown(uint32_t state) {
		updown_state = state;
		UpDown u;
		for (int n = 0; n < 4; n++) {
			const uint32_t s = state >> (4 * n) & 0xf;
			u.level[n] = s == 0xf ? Level::on : s == 0 ? Level::off :
			                                             Level::dimmed;
			if (u.level[n] == Level::dimmed) u.dimmed = n;
		}
		return u;
	}

	// the dial: angle (mod 16) as a pattern of the four up/down LEDs
	static constexpr uint32_t dial(int angle) {
		angle = ((angle + 1024) % 16) - 8; // +1024 to make % work as modulo for negative numbers
		return angle >= 0 ? 0xffffu >> (2 * angle) : 0xffffu << (2 * -angle);
	}

private:
	static constexpr Duty duty(int r, int g, int b) { return {gamma8(r), gamma8(g), gamma8(b)}; }
};

} // namespace led
