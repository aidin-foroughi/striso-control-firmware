/*
 * The LEDs' driver: the PWM and the pins under the presenter (led_presenter.hpp), which has the
 * logic. The PWM set-up and the dimmed LED's callbacks are as they were.
 *
 * The LEDs are set from several threads (the control thread's settings and keys, the jack
 * thread's blinks, main's tick and boot): a mutex keeps each change whole (the presenter's state
 * and the three PWM channels together).
 */
#include "ch.h"
#include "hal.h"

extern "C" {
#include "led.h"
}

#include "led_presenter.hpp"

int last_color[3] = {0}; // the presenter's colour, for the other files (led.h)
ioline_t led_dimmed = LINE_LED_UP;

namespace {

PWMConfig pwmcfg = {
    8000000, /* PWM clock frequency.   */
    2048,    /* Initial PWM period.    */
    nullptr,
    {{PWM_OUTPUT_ACTIVE_LOW, nullptr},
        {PWM_OUTPUT_ACTIVE_LOW, nullptr},
        {PWM_OUTPUT_ACTIVE_LOW, nullptr},
        {PWM_OUTPUT_DISABLED, nullptr}},
    0,
    0};

void pwm_dimmed_on(PWMDriver* pwmp) {
	(void)pwmp;
	palSetLine(led_dimmed);
}

void pwm_dimmed_off(PWMDriver* pwmp) {
	(void)pwmp;
	palClearLine(led_dimmed);
}

PWMConfig pwmcfg_updown = {
    125000, /* PWM clock frequency.   */
    2048,   /* Initial PWM period.    */
    pwm_dimmed_on,
    {{PWM_OUTPUT_ACTIVE_HIGH, pwm_dimmed_off},
        {PWM_OUTPUT_DISABLED, nullptr},
        {PWM_OUTPUT_DISABLED, nullptr},
        {PWM_OUTPUT_DISABLED, nullptr}},
    0,
    0};

led::Presenter presenter;
mutex_t lock;

// the presenter's state and the PWM, one change at a time
class Guard {
public:
	Guard() { chMtxLock(&lock); }
	~Guard() { chMtxUnlock(&lock); }
	Guard(const Guard&) = delete;
	Guard& operator=(const Guard&) = delete;
};

void show(const led::Duty& d) {
	pwmEnableChannel(&PWMD4, 0, d.r);
	pwmEnableChannel(&PWMD4, 2, d.g);
	pwmEnableChannel(&PWMD4, 1, d.b);
}

void publish_color() {
	for (int i = 0; i < 3; i++) last_color[i] = presenter.color[i];
}

// a non-dimmed up/down LED on or off; true: it is dimmed
bool updown_line(ioline_t line, led::Level level) {
	if (level == led::Level::on) {
		palSetLine(line);
	} else if (level == led::Level::off) {
		palClearLine(line);
	} else {
		return true;
	}
	return false;
}

} // namespace

extern "C" void led_init(void) {
	chMtxObjectInit(&lock);
	pwmStart(&PWMD4, &pwmcfg);
	palSetLineMode(LINE_LED_R, PAL_MODE_ALTERNATE(2));
	palSetLineMode(LINE_LED_G, PAL_MODE_ALTERNATE(2));
	palSetLineMode(LINE_LED_B, PAL_MODE_ALTERNATE(2));
	pwmStart(&PWMD3, &pwmcfg_updown);
	pwmEnableChannel(&PWMD3, 0, 768); // duty cycle of dimmed up/down leds
}

extern "C" void led_rgb(uint32_t rgb) {
	led_rgb3(rgb >> 16 & 0xff, rgb >> 8 & 0xff, rgb >> 0 & 0xff);
}

extern "C" void led_rgb3(int r, int g, int b) {
	const Guard g_;
	show(presenter.set(r, g, b));
	publish_color();
}

extern "C" void led_rgb3_blink(int r, int g, int b, sysinterval_t time) {
	const Guard g_;
	show(presenter.blink(r, g, b, chVTGetSystemTime(), time));
}

extern "C" void led_tick(void) {
	const Guard g_;
	if (const auto d = presenter.tick(chVTGetSystemTime())) {
		show(*d);
		publish_color();
	}
}

/**
 * Set up/down leds state
 *
 * state is 0xabcd with a=up2, b=up, c=down, d=down2
 * 0 = off, f = on, anything else = dimmed.
 * Only one led can be dimmed at the same time.
 */
extern "C" void led_updown(uint32_t state) {
	const Guard g_;
	const led::UpDown u = presenter.updown(state);

	// combine all led calls in one SysLock zone to not have previous dimmed led
	// turn on or off accidentally
	// checks from pwmEnableChannelNotification/pwmEnablePeriodicNotification
	osalDbgCheck(0 < PWMD3.channels); // (&PWMD3 is never NULL)

	osalSysLock();

	osalDbgAssert(PWMD3.state == PWM_READY, "not ready");
	osalDbgAssert(PWMD3.state == PWM_READY, "not ready");
	osalDbgAssert(PWMD3.config->callback != nullptr, "undefined periodic callback");
	osalDbgAssert((PWMD3.enabled & ((pwmchnmsk_t)1U << (pwmchnmsk_t)0)) != 0U,
	    "channel not enabled");
	osalDbgAssert(PWMD3.config->channels[0].callback != nullptr,
	    "undefined channel callback");

	const ioline_t lines[4] = {LINE_LED_UP2, LINE_LED_UP, LINE_LED_DOWN, LINE_LED_DOWN2};
	bool dim = false;
	for (int n = 0; n < 4; n++) dim = updown_line(lines[n], u.level[n]) || dim;
	if (dim) {
		led_dimmed = lines[u.dimmed];
		pwmEnablePeriodicNotificationI(&PWMD3);
		pwmEnableChannelNotificationI(&PWMD3, 0);
	} else {
		pwmDisableChannelNotificationI(&PWMD3, 0);
		pwmDisablePeriodicNotificationI(&PWMD3);
	}
	osalSysUnlock();
}

extern "C" uint32_t led_updown_get(void) {
	return presenter.updown_state;
}

extern "C" void led_updown_dial(int angle) {
	led_updown(led::Presenter::dial(angle));
}
