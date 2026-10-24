/*
 * The virtual board's HAL: the ChibiOS HAL and CMSIS parts the firmware's own files use.
 *   streams     BaseSequentialStream / BaseAsynchronousChannel with their vmt, as ChibiOS has them
 *               (chprintf, streamWrite, chnReadTimeout work on the console as on the board)
 *   pal         lines (the real board.h) recorded as state: LEDs, the headphone amp, buttons
 *   pwm         the LED PWM channels recorded as duty values (led.c runs as is)
 *   usb         connect/disconnect of the virtual bus
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

/* --- streams (hal_streams.h, hal_channels.h) ---------------------------------------------------- */
#define _base_sequential_stream_methods \
	size_t (*write)(void* instance, const uint8_t* bp, size_t n); \
	size_t (*read)(void* instance, uint8_t* bp, size_t n); \
	msg_t (*put)(void* instance, uint8_t b); \
	msg_t (*get)(void* instance);
#define _base_sequential_stream_data
struct BaseSequentialStreamVMT {
	_base_sequential_stream_methods
};
typedef struct {
	const struct BaseSequentialStreamVMT* vmt;
	_base_sequential_stream_data
} BaseSequentialStream;
#define streamWrite(ip, bp, n) ((ip)->vmt->write(ip, bp, n))
#define streamRead(ip, bp, n) ((ip)->vmt->read(ip, bp, n))
#define streamPut(ip, b) ((ip)->vmt->put(ip, b))
#define streamGet(ip) ((ip)->vmt->get(ip))

#define _base_channel_methods \
	_base_sequential_stream_methods \
	msg_t (*putt)(void* instance, uint8_t b, sysinterval_t time); \
	msg_t (*gett)(void* instance, sysinterval_t time); \
	size_t (*writet)(void* instance, const uint8_t* bp, size_t n, sysinterval_t time); \
	size_t (*readt)(void* instance, uint8_t* bp, size_t n, sysinterval_t time); \
	msg_t (*ctl)(void* instance, unsigned int operation, void* arg);
#define _base_asynchronous_channel_methods _base_channel_methods
#define _base_asynchronous_channel_data
#define chnWriteTimeout(ip, bp, n, time) ((ip)->vmt->writet(ip, bp, n, time))
#define chnReadTimeout(ip, bp, n, time) ((ip)->vmt->readt(ip, bp, n, time))
#define chnPutTimeout(ip, b, time) ((ip)->vmt->putt(ip, b, time))
#define chnGetTimeout(ip, time) ((ip)->vmt->gett(ip, time))

/* --- buffer queues: what the firmware does with a driver's obqueue ------------------------------- */
typedef struct sv_obqueue output_buffers_queue_t;
typedef struct sv_obqueue input_buffers_queue_t;
struct sv_obqueue {
	int which; /* the virtual board's byte queue */
};
size_t obqWriteTimeout(output_buffers_queue_t* obqp, const uint8_t* bp, size_t n, sysinterval_t timeout);
bool obqIsEmptyI(output_buffers_queue_t* obqp);
void obqResetI(output_buffers_queue_t* obqp);
#define BQ_BUFFER_SIZE(n, size) ((size_t)(n) * (size_t)(size))

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

/* --- OSAL ------------------------------------------------------------------------------------------ */
#define osalSysLock() chSysLock()
#define osalSysUnlock() chSysUnlock()
#define osalSysLockFromISR() chSysLockFromISR()
#define osalSysUnlockFromISR() chSysUnlockFromISR()
#define osalDbgCheck(c) ((void)0)
#define osalDbgAssert(c, r) ((void)0)
#define osalThreadSleepMilliseconds(ms) chThdSleepMilliseconds(ms)

/* --- USB (hal_usb.h): the virtual bus ----------------------------------------------------------------- */
typedef uint8_t usbep_t;
typedef struct USBDriver USBDriver;
struct USBDriver {
	int connected;
};
typedef struct {
	int unused;
} USBConfig;
extern USBDriver USBD1;
void usbStart(USBDriver* usbp, const USBConfig* config);
void usbStop(USBDriver* usbp);
void usbConnectBus(USBDriver* usbp);
void usbDisconnectBus(USBDriver* usbp);

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
