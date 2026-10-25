/* A stand-in for the HAL (hal.h) for the key scan's sequence (tests/scan_seq): the GPIO ports as
 * numbers, the pin calls, the ADC samples and the cycle counter, recorded by test_scan_seq.cpp. */
#pragma once
#include "ch.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef int ioportid_t;
typedef uint32_t iopadid_t;
typedef uint32_t ioportmask_t;
typedef uint32_t ioline_t;
typedef uint16_t adcsample_t;
typedef struct { int id; } ADCDriver;
extern ADCDriver ADCD1;
enum { GPIOA = 1, GPIOB, GPIOC, GPIOD, GPIOE, GPIOF, GPIOG, GPIOH };
enum { LINE_BUTTON_PORT = 101, LINE_BUTTON_UP, LINE_BUTTON_DOWN, LINE_BUTTON_ALT };
#define PAL_MODE_OUTPUT_PUSHPULL 0x10
#define PAL_MODE_OUTPUT_OPENDRAIN 0x20
#define PAL_STM32_OSPEED_HIGHEST 0x03
void palSetPort(ioportid_t port, ioportmask_t mask);
void palClearPort(ioportid_t port, ioportmask_t mask);
void palSetPadMode(ioportid_t port, iopadid_t pad, uint32_t mode);
int palReadLine(ioline_t line);
typedef struct { volatile uint32_t CYCCNT; } DWT_Type;
extern DWT_Type dwt;
#define DWT (&dwt)
#define cacheBufferInvalidate(p, n) ((void)(p), (void)(n))
#define CC_ALIGN(n) __attribute__((aligned(n)))
#define CACHE_LINE_SIZE 0
#define CACHE_SIZE_ALIGN(t, n) (n)
#ifdef __cplusplus
}
#endif
