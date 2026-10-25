#ifndef _LED_H_
#define _LED_H_
#include <hal.h>

extern int last_color[3]; /* the indicator's colour as last set (led_rgb3), r g b */
extern ioline_t led_dimmed; /* the dimmed up/down LED, which the PWM's callbacks toggle */

void led_init(void);

void led_rgb(uint32_t rgb);
void led_rgb3(int r, int g, int b);
/* Set light color and return to previous color after given time */
void led_rgb3_blink(int r, int g, int b, sysinterval_t time);

void led_tick(void);

void led_updown(uint32_t state);
/* the state of the last led_updown(), as given to it */
uint32_t led_updown_get(void);
void led_updown_dial(int angle);

#endif
