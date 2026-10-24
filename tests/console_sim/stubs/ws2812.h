#ifndef SIM_WS2812_H
#define SIM_WS2812_H
#include <stdint.h>
typedef int ws2812_err_t;
static inline ws2812_err_t ws2812_write_led(uint32_t n, uint8_t r, uint8_t g, uint8_t b) { (void)n; (void)r; (void)g; (void)b; return 0; }
#endif
