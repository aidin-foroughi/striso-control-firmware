/* A stand-in for the HAL (hal.h) for led.cpp and the old led.c: the pins, the PWM and the OSAL
 * lock, recorded by test_led.cpp. */
#pragma once
#include "ch.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef uint32_t ioline_t;
typedef uint32_t pwmcnt_t;
typedef uint32_t pwmchannel_t;
typedef uint32_t pwmchnmsk_t;
typedef struct PWMDriver PWMDriver;
typedef void (*pwmcallback_t)(PWMDriver *pwmp);
typedef struct { uint32_t mode; pwmcallback_t callback; } PWMChannelConfig;
typedef struct {
  uint32_t frequency;
  pwmcnt_t period;
  pwmcallback_t callback;
  PWMChannelConfig channels[4];
  uint32_t cr2;
  uint32_t dier;
} PWMConfig;
enum { PWM_STOP, PWM_READY };
struct PWMDriver { int id; int state; uint32_t channels; pwmchnmsk_t enabled; const PWMConfig *config; };
extern PWMDriver PWMD3, PWMD4;
#define PWM_OUTPUT_DISABLED 0
#define PWM_OUTPUT_ACTIVE_HIGH 1
#define PWM_OUTPUT_ACTIVE_LOW 2
enum { LINE_LED_R = 1, LINE_LED_G, LINE_LED_B, LINE_LED_UP, LINE_LED_UP2, LINE_LED_DOWN, LINE_LED_DOWN2 };
#define PAL_MODE_ALTERNATE(n) (0x100 + (n))
void palSetLine(ioline_t l);
void palClearLine(ioline_t l);
void palSetLineMode(ioline_t l, uint32_t mode);
void pwmStart(PWMDriver *pwmp, const PWMConfig *config);
void pwmEnableChannel(PWMDriver *pwmp, pwmchannel_t channel, pwmcnt_t width);
void pwmEnablePeriodicNotificationI(PWMDriver *pwmp);
void pwmDisablePeriodicNotificationI(PWMDriver *pwmp);
void pwmEnableChannelNotificationI(PWMDriver *pwmp, pwmchannel_t channel);
void pwmDisableChannelNotificationI(PWMDriver *pwmp, pwmchannel_t channel);
void osalSysLock(void);
void osalSysUnlock(void);
void osal_check(bool ok, const char *what);
#define osalDbgCheck(c) osal_check((c), #c)
#define osalDbgAssert(c, r) osal_check((c), (r))
#ifdef __cplusplus
}
#endif
