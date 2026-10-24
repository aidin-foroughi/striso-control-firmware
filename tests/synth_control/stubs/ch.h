/* Host stand-in for ChibiOS' ch.h for the synth_control tests (tests/synth_control): the types,
 * the system time (a variable the test advances) and the few calls synth_control.cpp makes. */
#ifndef SC_TEST_STUB_CH_H
#define SC_TEST_STUB_CH_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef uint32_t systime_t; /* CH_CFG_ST_RESOLUTION 32 (cfg/chconf.h) */
typedef uint32_t sysinterval_t;
typedef uint64_t time_conv_t;
#define CH_CFG_ST_FREQUENCY 2000
#define TIME_MS2I(msecs) ((sysinterval_t)((((time_conv_t)(msecs) * (time_conv_t)CH_CFG_ST_FREQUENCY) + (time_conv_t)999) / (time_conv_t)1000))
#define TRUE 1
#define FALSE 0
#ifdef __cplusplus
extern "C" {
#endif
extern systime_t host_now;
static inline systime_t chVTGetSystemTime(void) { return host_now; }
void chThdSleepMilliseconds(int ms);
void NVIC_SystemReset(void);
#ifdef __cplusplus
}
#endif
#endif
