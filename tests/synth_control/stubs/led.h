/* Host stand-in for src/drivers/led.h (tests/synth_control): the test records the calls */
#ifndef SC_TEST_STUB_LED_H
#define SC_TEST_STUB_LED_H
#include "hal.h"
void led_rgb(uint32_t rgb);
void led_rgb3(int r, int g, int b);
void led_rgb3_blink(int r, int g, int b, sysinterval_t time);
void led_updown(uint32_t state);
void led_updown_dial(int angle);
#endif
