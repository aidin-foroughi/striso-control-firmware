/* Host stand-in for ChibiOS' hal.h (tests/synth_control) */
#ifndef SC_TEST_STUB_HAL_H
#define SC_TEST_STUB_HAL_H
#include "ch.h"
typedef struct BaseSequentialStream {
	int dummy;
} BaseSequentialStream;
#define LINE_BUTTON_PORT 0
#ifdef __cplusplus
extern "C" {
#endif
int palReadLine(int line);
#ifdef __cplusplus
}
#endif
/* CMSIS __USAT: saturate to an unsigned range of sat bits */
static inline uint32_t __USAT(int32_t val, uint32_t sat) {
	int32_t max = (int32_t)((1U << sat) - 1U);
	if (val > max) return (uint32_t)max;
	if (val < 0) return 0U;
	return (uint32_t)val;
}
#endif
