/* The old led.c (tests/ref/led_ref.c, as it was at cc47a4d) in the test, with its public names
 * prefixed ref_ so it links next to the new led.cpp. */
#define last_color ref_last_color
#define led_dimmed ref_led_dimmed
#define led_blink_timer ref_led_blink_timer
#define gamma8 ref_gamma8
#define led_init ref_led_init
#define led_rgb ref_led_rgb
#define led_rgb3 ref_led_rgb3
#define led_rgb3_blink ref_led_rgb3_blink
#define led_tick ref_led_tick
#define led_updown ref_led_updown
#define led_updown_get ref_led_updown_get
#define led_updown_dial ref_led_updown_dial
#include "../ref/led_ref.c"
