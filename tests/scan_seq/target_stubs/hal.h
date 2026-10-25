/* The HAL for the scan sequence on QEMU (target_scan_seq.cpp): the GPIO ports as RAM with the
 * board's set/reset register write (palSetPort, palClearPort), each write also hashed while the
 * comparison runs; the ADC samples, the cycle counter. */
#pragma once
#include "ch.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct { volatile uint32_t BSRR; volatile uint32_t MODER[16]; } GPIO_TypeDef;
typedef GPIO_TypeDef *ioportid_t;
typedef uint32_t iopadid_t;
typedef uint32_t ioportmask_t;
typedef uint32_t ioline_t;
typedef uint16_t adcsample_t;
typedef struct { int id; } ADCDriver;
extern ADCDriver ADCD1;
extern GPIO_TypeDef target_gpio[9];
#define GPIOA (&target_gpio[1])
#define GPIOB (&target_gpio[2])
#define GPIOC (&target_gpio[3])
#define GPIOD (&target_gpio[4])
#define GPIOE (&target_gpio[5])
#define GPIOF (&target_gpio[6])
#define GPIOG (&target_gpio[7])
#define GPIOH (&target_gpio[8])
enum { LINE_BUTTON_PORT = 101, LINE_BUTTON_UP, LINE_BUTTON_DOWN, LINE_BUTTON_ALT };
#define PAL_MODE_OUTPUT_PUSHPULL 0x10
#define PAL_MODE_OUTPUT_OPENDRAIN 0x20
#define PAL_STM32_OSPEED_HIGHEST 0x03
extern int target_hashing;
extern uint32_t target_hash;
static inline void target_hash_op(uint32_t a, uint32_t b, uint32_t c) {
  target_hash = (target_hash ^ a) * 16777619u;
  target_hash = (target_hash ^ b) * 16777619u;
  target_hash = (target_hash ^ c) * 16777619u;
}
static inline void palSetPort(ioportid_t port, ioportmask_t mask) {
  port->BSRR = mask;
  if (target_hashing) target_hash_op(1, (uint32_t)(port - target_gpio), mask);
}
static inline void palClearPort(ioportid_t port, ioportmask_t mask) {
  port->BSRR = mask << 16;
  if (target_hashing) target_hash_op(2, (uint32_t)(port - target_gpio), mask);
}
static inline void palSetPadMode(ioportid_t port, iopadid_t pad, uint32_t mode) {
  port->MODER[pad & 15] = mode;
  if (target_hashing) target_hash_op(3, (uint32_t)(port - target_gpio), pad ^ (mode << 8));
}
static inline int palReadLine(ioline_t line) { return (int)(line & 1); }
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
