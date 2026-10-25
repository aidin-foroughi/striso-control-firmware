/*
 * The aux jack's detection state machine: what is plugged into the jack (nothing, a switch pedal,
 * an expression pedal, MIDI, line in) and the pedal messages, as aux_jack.c had it, pass for pass.
 * Pure C++20: the hardware is the Io the caller gives it (aux_jack.cpp: the HAL; the host tests:
 * an electrical model and the old code's recorded calls), so the same calls in the same order
 * reach the pins, the ADC and the sleeps as before (tests/aux_jack/test_jack_detector.cpp proves
 * it against the old aux_jack.c).
 *
 * Io:
 *   Sample convert()            one conversion of the tip (TX) and ring (RX) levels, 14 bit
 *   void tx(Pin) / rx(Pin)      the tip / ring pin's mode
 *   void detect(bool high)      the jack detect contact's output
 *   bool tx_high()              the tip pin's digital level
 *   void sleep_ms(int), sleep_us(int)
 *   void power(bool on)         aux power on the ring
 *   void blink(int r, int g, int b, int ms)
 *   void pedal(int which, bool down)          a switch pedal's contact: 1 (tip), 2 (ring)
 *   void pedal_expression(int tip, int ring)  an expression pedal's levels, 13 bit
 *   void serial_midi(bool on), linein(bool on)
 *   void publish(Mode)          the active mode, for the rest of the firmware (config.jack2_mode)
 *   static constexpr bool kSerialMidi   the firmware has serial MIDI (USE_MIDI_SERIAL)
 */
#pragma once

#include <cstdint>
#include <cstdlib>

namespace jack {

// config.h's jack2_mode_t, value for value (aux_jack.cpp checks)
enum class Mode : uint8_t { disabled = 0,
	midi,
	pedal_expression,
	pedal_switch,
	linein,
	autodetect,
	error };

// the pin modes the detector uses (PAL_MODE_INPUT, _PULLUP, _PULLDOWN, _ANALOG)
enum class Pin : uint8_t { input,
	pullup,
	pulldown,
	analog };

struct Sample {
	int tip = 0;  // TX: 0..16383
	int ring = 0; // RX
};

inline constexpr int kFull = 1 << 14;    // the 14 bit ADC's range
inline constexpr int kRingShort = 6000;  // ring below: shorted to the sleeve
inline constexpr int kLow = 500;         // tip below: pulled to the sleeve
inline constexpr int kUnplugged = 15000; // tip above: the detect contact (nothing plugged)
inline constexpr int kHigh = 16000;      // tip above with the pull-up: floating
inline constexpr int kStable = 100;      // the most two readings may differ in autodetect

// What one autodetect pass saw, from its four pin states: pulldown (tip with the pull-down),
// pullup (with the pull-up), floating (with neither, after the pull-down) and the ring.
enum class Seen : uint8_t {
	over_voltage,     // > 3.3 V on the tip or the ring: error
	ring_short,       // the ring on the sleeve: a mono plug or a switch pedal (after a few passes)
	tip_closed,       // the tip on the sleeve: a closed switch pedal (or an expression pedal at 0)
	tip_floating,     // the tip floats: a loose plug, or a switch pedal that has been closed
	maybe_unplugged,  // the tip high on the detect contact: check the contact
	expression_pedal, // the tip holds its level with either pull: a potentiometer's wiper
	midi,             // high with the pull-up, not low with the pull-down: an optocoupler
	nothing,
};

constexpr Seen classify(int pulldown, int pullup, int floating, int ring) {
	const bool downfixed = floating < 6 * pulldown;
	const bool upfixed = (kFull - floating) < 6 * (kFull - pullup);
	if (pulldown == kFull - 1 || ring == kFull - 1) return Seen::over_voltage;
	if (ring < kRingShort) return Seen::ring_short;
	if (pullup < kLow) return Seen::tip_closed;
	if (pulldown < kLow && pullup > kHigh) return Seen::tip_floating;
	if (pulldown > kUnplugged) return Seen::maybe_unplugged;
	if (upfixed && downfixed) return Seen::expression_pedal;
	if (pullup > kHigh && pulldown > 1000) return Seen::midi;
	return Seen::nothing;
}

template <class Io>
class JackDetector {
public:
	Mode mode = Mode::disabled;      // active (config.jack2_mode)
	Mode setting = Mode::autodetect; // from the settings (config.jack2_mode_setting)
	int short_count = 0;             // consecutive passes with the ring shorted (and the error blink)
	bool tip_was_closed = false;     // autodetect: the tip was on the sleeve since the plug went in
	int detect_level = 31;           // expression pedal: the tip level >> 9 that may be the contact
	bool pedal1 = false;             // the switch pedal's contacts as last sent
	bool pedal2 = true;

	// one pass of the thread: a conversion, then the active mode's checks
	void pass(Io& io) {
		const Sample s = io.convert();
		switch (mode) {
		case Mode::disabled:    // jack detect high, tx pull-down, rx pull-down, no aux power
			if (s.tip < kLow) { // the tip went low: something is plugged in
				tip_was_closed = false;
				switch_mode(io, setting);
			}
			break;
		case Mode::autodetect: autodetect(io, s); break;
		case Mode::midi: // jack detect high, tx alternate, rx analog, aux power
			if (s.tip > kUnplugged) switch_mode(io, Mode::disabled);
			ring_check(io, s);
			break;
		case Mode::linein: // jack detect high, tx and rx analog, no aux power
			if (s.tip > kUnplugged) switch_mode(io, Mode::disabled);
			break;
		case Mode::pedal_switch: pedal_switch(io, s); break;
		case Mode::pedal_expression: pedal_expression(io, s); break;
		case Mode::error: {
			if (short_count++ > 80) {
				io.blink(90, 0, 0, 200); // red blink, error
				short_count = 0;
			}
			if (unplugged(io, true)) switch_mode(io, Mode::disabled);
		} break;
		}
	}

	void switch_mode(Io& io, Mode m) {
		if (m == mode) return;
		io.power(false);
		io.detect(true);
		short_count = 0; // a count from the previous mode must not decide in the new one

		if (mode == Mode::midi && Io::kSerialMidi) io.serial_midi(false);
		if (mode == Mode::linein) io.linein(false);

		switch (m) {
		case Mode::disabled:
			io.tx(Pin::pulldown);
			io.rx(Pin::pulldown);
			publish(io, m);
			io.blink(90, 20, 0, 500); // red, unplug
			break;
		case Mode::autodetect:
			io.tx(Pin::pulldown);
			io.rx(Pin::analog);
			io.power(true);
			publish(io, m);
			io.blink(0, 186, 0, 1000); // green, autodetect
			break;
		case Mode::midi:
			if (Io::kSerialMidi) {
				io.serial_midi(true);
				publish(io, m);
				io.blink(0, 90, 90, 1000); // cyan, midi
			}
			break;
		case Mode::pedal_expression:
			io.power(true);
			io.rx(Pin::analog);
			io.tx(Pin::analog);
			publish(io, m);
			io.blink(90, 90, 0, 1000); // yellow, expression pedal
			break;
		case Mode::pedal_switch:
			io.rx(Pin::pullup);
			io.tx(Pin::pullup);
			publish(io, m);
			pedal2 = true;
			pedal1 = false;
			io.blink(90, 0, 90, 1000); // pink, switch pedal
			break;
		case Mode::linein:
			io.linein(true);
			io.rx(Pin::analog); // or pull-down if line out is through a capacitor?
			io.tx(Pin::analog);
			publish(io, m);
			io.blink(0, 0, 90, 1000); // blue, line in
			break;
		case Mode::error:
			io.rx(Pin::analog);
			io.tx(Pin::input);
			publish(io, m);
			io.blink(90, 0, 0, 200); // red, error
			break;
		}
	}

	// a new setting: the jack goes to disabled, so the next plug-in detects with it
	void switch_setting(Io& io, Mode m) {
		if (setting == m) return;
		setting = m;
		switch_mode(io, Mode::disabled);
	}

private:
	void publish(Io& io, Mode m) {
		mode = m;
		io.publish(m);
	}

	// toggle the detect contact up to 10 times: the tip follows it only with nothing plugged in
	// (both reads of a toggle are made, as before)
	bool unplugged(Io& io, bool start) {
		bool u = start;
		for (int i = 0; u && i < 10; i++) {
			io.detect(false);
			io.sleep_us(100);
			const bool low = !io.tx_high();
			io.detect(true);
			io.sleep_us(100);
			const bool high = io.tx_high();
			u = u && low && high;
		}
		return u;
	}

	// the ring shorted for more than a few passes: error
	void ring_check(Io& io, const Sample& s) {
		if (s.ring < kRingShort) {
			if (short_count++ > 5) switch_mode(io, Mode::error);
		} else {
			short_count = 0;
		}
	}

	// jack detect high, tx pull-down, rx analog, aux power: read the tip with each pull and none
	void autodetect(Io& io, const Sample& s) {
		const int pulldown = s.tip;
		io.tx(Pin::pullup);
		io.sleep_ms(1);
		const int pullup = io.convert().tip;

		// is the signal stable?
		io.tx(Pin::pulldown);
		io.sleep_ms(1);
		const int pulldown2 = io.convert().tip;
		io.tx(Pin::pullup);
		io.sleep_ms(1);
		const int pullup2 = io.convert().tip;
		if (std::abs(pulldown2 - pulldown) > kStable || std::abs(pullup2 - pullup) > kStable) {
			io.tx(Pin::pulldown); // not stable: no decision this pass
			return;
		}

		io.tx(Pin::analog);
		io.sleep_ms(1);
		const Sample f = io.convert();
		io.tx(Pin::pulldown);

		switch (classify(pulldown, pullup, f.tip, f.ring)) {
		case Seen::over_voltage: switch_mode(io, Mode::error); break;
		case Seen::ring_short:
			if (short_count++ > 5) {
				switch_mode(io, Mode::pedal_switch);
				return;
			}
			break;
		case Seen::tip_closed: tip_was_closed = true; break;
		case Seen::tip_floating:
			if (tip_was_closed) switch_mode(io, Mode::pedal_switch); // closed since insertion: a switch pedal
			break;
		case Seen::maybe_unplugged:
			// unplugged, or an expression pedal at its maximum, the tip on the ring, power on the
			// tip (MIDI in type B): check the contact
			if (unplugged(io, true)) switch_mode(io, Mode::disabled);
			break;
		case Seen::expression_pedal: switch_mode(io, Mode::pedal_expression); break;
		case Seen::midi: switch_mode(io, Mode::midi); break;
		case Seen::nothing: break;
		}
		// count consecutive passes with the ring shorted: reset only when it isn't
		if (f.ring >= kRingShort) short_count = 0;
	}

	// jack detect high, tx and rx pull-up, no aux power
	void pedal_switch(Io& io, const Sample& s) {
		io.tx(Pin::pulldown);
		io.sleep_us(100);
		const bool out = unplugged(io, io.tx_high());
		io.tx(Pin::pullup);

		int level1 = s.tip;
		int level2 = s.ring;
		if (out) {
			switch_mode(io, Mode::disabled);
			level1 = kFull; // pedal-off messages after the unplug
			level2 = kFull;
		}
		if (pedal1 != (level1 < kFull / 2)) {
			pedal1 = !pedal1;
			io.pedal(1, pedal1);
		}
		if (pedal2 != (level2 < kFull / 2)) {
			pedal2 = !pedal2;
			io.pedal(2, pedal2);
		}
		// TODO: detect a third button, which closes both contacts through a diode
	}

	// jack detect high, tx and rx analog, aux power
	void pedal_expression(Io& io, const Sample& s) {
		if (s.tip >> 9 == detect_level) {
			// maybe unplugged: check with the contact
			io.tx(Pin::input);
			const bool out = unplugged(io, true);
			io.tx(Pin::analog);
			if (out) {
				detect_level = 31;
				switch_mode(io, Mode::disabled);
				return;
			}
			if (detect_level != 0) {
				detect_level = 0;
				io.detect(false);
			} else {
				detect_level = 31;
			}
		}
		ring_check(io, s);
		io.pedal_expression(s.tip >> 1, s.ring >> 1);
	}
};

} // namespace jack
