/*
 * The virtual board's HAL: the ChibiOS HAL and CMSIS parts the firmware's own files use.
 *   osal        ChibiOS' OSAL on the virtual kernel (ch.h), with the board's debug checks on
 *   streams,    ChibiOS' own headers (hal_streams.h, hal_channels.h, hal_buffers.h; their code,
 *   channels,   hal_buffers.c, is compiled in): the USB console and MIDI drivers (src/usb
 *   buffers     bulk_usb.c, midi_usb.c) run on the board's buffer queues
 *   usb         ChibiOS' hal_usb.h and hal_usb.c on the virtual OTG low level driver
 *               (hal_usb_lld.h, usb_host.c: the bus and the host)
 *   pal         lines (the real board.h) recorded as state: LEDs, the headphone amp, buttons
 *   pwm         the LED PWM channels recorded as duty values (led.c runs as is)
 *   CMSIS       __USAT, NVIC_SystemReset (ends the firmware instance: a reset), DWT, PWR, RTC
 *               (the backup registers survive the reset), UID_BASE
 */
#ifndef SV_HAL_HAL_H
#define SV_HAL_HAL_H

#include "ch.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HAL_USE_PAL TRUE
#define HAL_USE_PWM TRUE
#define HAL_USE_USB TRUE
#define STM32H7XX
#define STM32H743xx
#define STM32_SYS_CK 400000000U

/* --- OSAL (os/hal/osal/rt-nil/osal.h) on the virtual kernel ------------------------------------- */
/* CH_DBG_ENABLE_CHECKS and CH_DBG_ENABLE_ASSERTS are TRUE on the board (cfg/chconf.h): a failed
 * check halts the board (chSysHalt: the virtual board stops running the firmware) */
#define osalDbgAssert(c, remark) \
	do { \
		if (!(c)) chSysHalt(remark); \
	} while (0)
#define osalDbgCheck(c) \
	do { \
		if (!(c)) chSysHalt(__func__); \
	} while (0)
#define osalDbgCheckClassI() ((void)0)
#define osalDbgCheckClassS() ((void)0)
#define osalSysLock() chSysLock()
#define osalSysUnlock() chSysUnlock()
#define osalSysLockFromISR() chSysLockFromISR()
#define osalSysUnlockFromISR() chSysUnlockFromISR()
#define osalOsRescheduleS() chSchRescheduleS()
#define osalThreadSleepMilliseconds(ms) chThdSleepMilliseconds(ms)
#define osalThreadQueueObjectInit(tqp) chThdQueueObjectInit(tqp)
#define osalThreadEnqueueTimeoutS(tqp, timeout) chThdEnqueueTimeoutS(tqp, timeout)
#define osalThreadDequeueNextI(tqp, msg) chThdDequeueNextI(tqp, msg)
#define osalThreadDequeueAllI(tqp, msg) chThdDequeueAllI(tqp, msg)
#define osalEventObjectInit(esp) chEvtObjectInit(esp)
#define osalEventBroadcastFlagsI(esp, flags) chEvtBroadcastFlagsI(esp, flags)

/* --- ChibiOS' streams, channels, buffer queues and USB driver (os/hal/include) ------------------- */
#include "hal_objects.h"
#include "hal_streams.h"
#include "hal_channels.h"
#include "hal_buffers.h"
#include "hal_usb.h"

/* --- PAL (hal_pal.h): ports and lines as numbers, board.h's LINE_ names as on the board ------------ */
typedef uint32_t ioline_t;
typedef uint32_t ioportid_t;
typedef uint32_t iopadid_t;
typedef uint32_t iomode_t;
typedef uint32_t ioportmask_t;
#define GPIOA 0u
#define GPIOB 1u
#define GPIOC 2u
#define GPIOD 3u
#define GPIOE 4u
#define GPIOF 5u
#define GPIOG 6u
#define GPIOH 7u
#define GPIOI 8u
#define GPIOJ 9u
#define GPIOK 10u
#define PAL_LINE(port, pad) ((ioline_t)(((uint32_t)(port) << 4) | (uint32_t)(pad)))
#define PAL_PORT(line) ((ioportid_t)((line) >> 4))
#define PAL_PAD(line) ((iopadid_t)((line) & 15u))
#define PAL_LOW 0
#define PAL_HIGH 1
#define PAL_MODE_RESET 0u
#define PAL_MODE_UNCONNECTED 1u
#define PAL_MODE_INPUT 2u
#define PAL_MODE_INPUT_PULLUP 3u
#define PAL_MODE_INPUT_PULLDOWN 4u
#define PAL_MODE_INPUT_ANALOG 5u
#define PAL_MODE_OUTPUT_PUSHPULL 6u
#define PAL_MODE_OUTPUT_OPENDRAIN 7u
#define PAL_MODE_ALTERNATE(n) (16u + (n))
#define PAL_STM32_OSPEED_HIGHEST 0u
void palSetLine(ioline_t line);
void palClearLine(ioline_t line);
void palToggleLine(ioline_t line);
void palWriteLine(ioline_t line, int value);
int palReadLine(ioline_t line);
void palSetLineMode(ioline_t line, iomode_t mode);
#define palSetPad(port, pad) palSetLine(PAL_LINE(port, pad))
#define palClearPad(port, pad) palClearLine(PAL_LINE(port, pad))
#define palReadPad(port, pad) palReadLine(PAL_LINE(port, pad))
#define palSetPadMode(port, pad, mode) palSetLineMode(PAL_LINE(port, pad), mode)
#include "board.h"

/* --- PWM (hal_pwm.h) --------------------------------------------------------------------------- */
#define PWM_CHANNELS 4
#define PWM_OUTPUT_DISABLED 0x00U
#define PWM_OUTPUT_ACTIVE_HIGH 0x01U
#define PWM_OUTPUT_ACTIVE_LOW 0x02U
typedef enum { PWM_UNINIT = 0, PWM_STOP = 1, PWM_READY = 2 } pwmstate_t;
typedef uint32_t pwmcnt_t;
typedef uint32_t pwmchannel_t;
typedef uint32_t pwmchnmsk_t;
typedef struct PWMDriver PWMDriver;
typedef void (*pwmcallback_t)(PWMDriver* pwmp);
typedef struct {
	uint32_t mode;
	pwmcallback_t callback;
} PWMChannelConfig;
typedef struct {
	uint32_t frequency;
	pwmcnt_t period;
	pwmcallback_t callback;
	PWMChannelConfig channels[PWM_CHANNELS];
	uint32_t cr2;
	uint32_t dier;
} PWMConfig;
struct PWMDriver {
	pwmstate_t state;
	const PWMConfig* config;
	pwmcnt_t period;
	pwmchnmsk_t enabled;
	pwmchannel_t channels;
	pwmcnt_t duty[PWM_CHANNELS];
	bool periodic_notification, channel_notification[PWM_CHANNELS];
};
extern PWMDriver PWMD3, PWMD4;
void pwmStart(PWMDriver* pwmp, const PWMConfig* config);
void pwmEnableChannel(PWMDriver* pwmp, pwmchannel_t channel, pwmcnt_t width);
void pwmDisableChannel(PWMDriver* pwmp, pwmchannel_t channel);
void pwmEnablePeriodicNotificationI(PWMDriver* pwmp);
void pwmDisablePeriodicNotificationI(PWMDriver* pwmp);
void pwmEnableChannelNotificationI(PWMDriver* pwmp, pwmchannel_t channel);
void pwmDisableChannelNotificationI(PWMDriver* pwmp, pwmchannel_t channel);
#define pwmEnablePeriodicNotification(p) pwmEnablePeriodicNotificationI(p)
#define pwmEnableChannelNotification(p, c) pwmEnableChannelNotificationI(p, c)

/* --- CMSIS ------------------------------------------------------------------------------------------ */
#define __ASM __asm
#define __STATIC_INLINE static inline
static inline uint32_t __USAT(int32_t val, uint32_t sat) {
	const int32_t max = (int32_t)((1U << sat) - 1U);
	return val < 0 ? 0U : (uint32_t)(val > max ? max : val);
}
static inline int32_t __SSAT(int32_t val, uint32_t sat) {
	const int32_t max = (int32_t)((1U << (sat - 1U)) - 1U);
	return val > max ? max : (val < -max - 1 ? -max - 1 : val);
}
/* the firmware resets the board: this firmware instance ends (the scheduler stops it), the
 * library boots a fresh one; the RTC backup registers say whether the bootloader stays */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noreturn))
#endif
void NVIC_SystemReset(void);
typedef struct {
	volatile uint32_t CTRL, CYCCNT;
} sv_dwt_t;
extern sv_dwt_t sv_dwt;
#define DWT (sv_dwt_update())
sv_dwt_t* sv_dwt_update(void);
typedef struct {
	volatile uint32_t CR1, CR2, CR3, CPUCR;
} sv_pwr_t;
extern sv_pwr_t sv_pwr;
#define PWR (&sv_pwr)
#define PWR_CR1_DBP (1U << 8)
typedef struct {
	volatile uint32_t BKP0R, BKP1R, BKP2R, BKP3R, BKP4R, BKP5R, BKP6R, BKP7R;
} sv_rtc_t;
extern sv_rtc_t* sv_rtc;
#define RTC sv_rtc
extern uint32_t* sv_uid;
#define UID_BASE ((uintptr_t)sv_uid)

void halInit(void);

#ifdef __cplusplus
}
#endif

#endif
