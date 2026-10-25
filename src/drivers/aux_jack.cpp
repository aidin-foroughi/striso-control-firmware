/*
 * The aux jack's thread: the HAL under the detector (jack_detector.hpp). Every pass converts the
 * tip and ring levels, then the detector decides from them what is plugged in and sends the
 * pedals; this file only maps its calls to the pins, the ADC, the LED and the message queue.
 *
 * The thread owns the jack's state. A new jack setting (aux_jack_switch_mode_setting, from the
 * settings keys on the control thread) is a request it takes at the start of its next pass.
 */
#include <atomic>
#include <cstdint>

#include "ch.h"
#include "hal.h"

extern "C" {
#include "aux_jack.h"
#include "ccportab.h"
#include "codec.h"
#include "config.h"
#include "config_store.h"
#include "led.h"
#include "messaging.h"
#include "midi_serial.h"
#include "striso.h"
}

#include "jack_detector.hpp"

static_assert(static_cast<int>(jack::Mode::disabled) == JACK2_MODE_DISABLED && static_cast<int>(jack::Mode::midi) == JACK2_MODE_MIDI &&
        static_cast<int>(jack::Mode::pedal_expression) == JACK2_MODE_PEDAL_EXPRESSION &&
        static_cast<int>(jack::Mode::pedal_switch) == JACK2_MODE_PEDAL_SWITCH &&
        static_cast<int>(jack::Mode::linein) == JACK2_MODE_LINEIN && static_cast<int>(jack::Mode::autodetect) == JACK2_MODE_AUTODETECT &&
        static_cast<int>(jack::Mode::error) == JACK2_MODE_ERROR,
    "jack::Mode is jack2_mode_t");

extern "C" void aux_power_enable(void) {
	palClearLine(LINE_AUX_VDD); // aux power on the ring
}

extern "C" void aux_power_disable(void) {
	palSetLine(LINE_AUX_VDD);
}

namespace {

/* Total number of channels to be sampled by a single ADC operation.*/
#define ADC_GRP3_NUM_CHANNELS 2

/* Depth of the conversion buffer, channels are sampled one time each.*/
#define ADC_GRP3_BUF_DEPTH 1 // must be 1 or even.

const ADCConfig adccfg3 = {
    .difsel = 0U,
    .calibration = 0U};

// AUX jack Tip ADC: IN10
// AUX jack Ring ADC: IN11
const ADCConversionGroup adcgrpcfg3 = {
    .circular = FALSE,
    .num_channels = ADC_GRP3_NUM_CHANNELS,
    .end_cb = nullptr,
    .error_cb = nullptr,
    .cfgr = ADC_CFGR_RES_14BITS,
    .cfgr2 = ADC_CFGR2_ROVSE | ADC_CFGR2_OVSR_N(127) | ADC_CFGR2_OVSS_N(7), // 128x oversampling, gives around 80Hz sample rate
    .ccr = 0U,
    .pcsel = ADC_SELMASK_IN10 | ADC_SELMASK_IN11,
    .ltr1 = 0x00000000U,
    .htr1 = 0x03FFFFFFU,
    .ltr2 = 0x00000000U,
    .htr2 = 0x03FFFFFFU,
    .ltr3 = 0x00000000U,
    .htr3 = 0x03FFFFFFU,
    .smpr = {
        0U,
        ADC_SMPR2_SMP_AN10(ADC_SMPR_SMP_810P5) | ADC_SMPR2_SMP_AN11(ADC_SMPR_SMP_64P5)},
    .sqr = {ADC_SQR1_SQ1_N(ADC_CHANNEL_IN10) | ADC_SQR1_SQ2_N(ADC_CHANNEL_IN11), 0U, 0U, 0U}};

// ADC3 samples need to be in ram4! The CPU only reads them (a dirty cache line evicted after the
// next conversion would overwrite its data).
CC_SECTION(".ram4")
CC_ALIGN(CACHE_LINE_SIZE)
adcsample_t aux_adc_samples[CACHE_SIZE_ALIGN(adcsample_t, ADC_GRP3_NUM_CHANNELS* ADC_GRP3_BUF_DEPTH)];

iomode_t pal_mode(jack::Pin p) {
	switch (p) {
	case jack::Pin::input: return PAL_MODE_INPUT;
	case jack::Pin::pullup: return PAL_MODE_INPUT_PULLUP;
	case jack::Pin::pulldown: return PAL_MODE_INPUT_PULLDOWN;
	case jack::Pin::analog: return PAL_MODE_INPUT_ANALOG;
	}
	return PAL_MODE_INPUT_ANALOG;
}

// the detector's hardware
struct HalIo {
#ifdef USE_MIDI_SERIAL
	static constexpr bool kSerialMidi = true;
#else
	static constexpr bool kSerialMidi = false;
#endif
	jack::Sample convert() {
		adcConvert(&ADCD3, &adcgrpcfg3, aux_adc_samples, ADC_GRP3_BUF_DEPTH);
		cacheBufferInvalidate(aux_adc_samples, sizeof(aux_adc_samples) / sizeof(adcsample_t));
		return {aux_adc_samples[0], aux_adc_samples[1]};
	}
	void tx(jack::Pin p) { palSetLineMode(LINE_AUX_UART2_TX, pal_mode(p)); }
	void rx(jack::Pin p) { palSetLineMode(LINE_AUX_UART2_RX, pal_mode(p)); }
	void detect(bool high) {
		if (high) {
			palSetLine(LINE_AUX_JACK_DETECT);
		} else {
			palClearLine(LINE_AUX_JACK_DETECT);
		}
	}
	bool tx_high() { return palReadLine(LINE_AUX_UART2_TX) != 0; }
	void sleep_ms(int ms) { chThdSleepMilliseconds(ms); }
	void sleep_us(int us) { chThdSleepMicroseconds(us); }
	void power(bool on) {
		if (on) {
			aux_power_enable();
		} else {
			aux_power_disable();
		}
	}
	void blink(int r, int g, int b, int ms) { led_rgb3_blink(r, g, b, TIME_MS2I(ms)); }
	void pedal(int which, bool down) { event_send1(ID_CONTROL, which == 1 ? IDC_PEDAL_1 : IDC_PEDAL_2, down); }
	void pedal_expression(int tip, int ring) { event_send2(ID_CONTROL, IDC_PEDAL_EXP, tip, ring); }
	void serial_midi(bool on) {
#ifdef USE_MIDI_SERIAL
		if (on) {
			serial_midi_enable();
		} else {
			serial_midi_disable();
		}
#else
		(void)on;
#endif
	}
	void linein(bool on) {
		if (on) {
			codec_linein_enable();
		} else {
			codec_linein_disable();
		}
	}
	void publish(jack::Mode m) { config.jack2_mode = static_cast<jack2_mode_t>(m); }
};

HalIo io;
jack::JackDetector<HalIo> detector;
std::atomic<int> requested_setting{-1}; // a jack2_mode_t from aux_jack_switch_mode_setting, -1: none

THD_WORKING_AREA(waAuxJack, 320); // I2C to the codec with recovery (line-in)
THD_FUNCTION(AuxJack, arg) {
	(void)arg;
	chRegSetThreadName("auxjack");
	while (true) {
		const int r = requested_setting.exchange(-1);
		if (r >= 0) {
			config.jack2_mode_setting = static_cast<jack2_mode_t>(r);
			detector.switch_setting(io, static_cast<jack::Mode>(r));
		}
		detector.pass(io);
	}
}

} // namespace

extern "C" void aux_jack_init(void) {
	palSetLineMode(LINE_AUX_ADC_T, PAL_MODE_INPUT_ANALOG);
	palSetLineMode(LINE_AUX_ADC_R, PAL_MODE_INPUT_ANALOG);

	palSetLineMode(LINE_AUX_UART2_RX, PAL_MODE_INPUT_PULLDOWN);
	palSetLineMode(LINE_AUX_UART2_TX, PAL_MODE_INPUT_PULLDOWN);

	palSetLineMode(LINE_AUX_JACK_DETECT, PAL_MODE_OUTPUT_PUSHPULL);
	palSetLine(LINE_AUX_JACK_DETECT);

	palSetLineMode(LINE_AUX_VDD, PAL_MODE_OUTPUT_OPENDRAIN);
	aux_power_disable();

	adcStart(&ADCD3, &adccfg3);

	detector.mode = static_cast<jack::Mode>(config.jack2_mode);
	detector.setting = static_cast<jack::Mode>(config.jack2_mode_setting);
	// deprecated, keep loading if setting still exists (before the thread runs: no request needed)
	jack::Mode setting = jack::Mode::autodetect; // assume "auto"
	if (config_text_is("sGjack2 ", "pedal_ex")) {
		setting = jack::Mode::pedal_expression;
	} else if (config_text_is("sGjack2 ", "pedal_sw")) {
		setting = jack::Mode::pedal_switch;
	} else if (config_text_is("sGjack2 ", "midi")) {
		setting = jack::Mode::midi;
	} else if (config_text_is("sGjack2 ", "linein")) {
		setting = jack::Mode::linein;
	}
	config.jack2_mode_setting = static_cast<jack2_mode_t>(setting);
	detector.switch_setting(io, setting);

	chThdCreateStatic(waAuxJack, sizeof(waAuxJack), NORMALPRIO, AuxJack, nullptr);
}

// the control thread (a settings key): the jack thread switches at its next pass
extern "C" void aux_jack_switch_mode_setting(jack2_mode_t mode) {
	requested_setting.store(static_cast<int>(mode));
}
