/* Host simulation of the parts of ChibiOS (RT 6.1.4 / HAL 7.1.5) the Striso app core uses.
 * The real hal_buffers.c, hal_streams.h, hal_channels.h, chprintf.c, bulk_usb.c, midi_usb.c and
 * pconnection.c are compiled against this. Threads are not real: a blocking wait (TIME_INFINITE)
 * calls sim_block_hook(), which plays the scripted "other side" (USB ISR, other threads). */
#ifndef SIM_HAL_H
#define SIM_HAL_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ---- kernel types ---- */
typedef int32_t msg_t;
typedef uint32_t sysinterval_t;
typedef uint32_t systime_t;
typedef uint32_t eventflags_t;
typedef uint32_t eventmask_t;
typedef uint8_t tstate_t;
typedef int32_t cnt_t;
#define MSG_OK      ((msg_t)0)
#define MSG_TIMEOUT ((msg_t)-1)
#define MSG_RESET   ((msg_t)-2)
#define TIME_IMMEDIATE ((sysinterval_t)0)
#define TIME_INFINITE  ((sysinterval_t)-1)
#define TRUE 1
#define FALSE 0
#define NORMALPRIO 128
#define CH_CFG_ST_FREQUENCY 2000
#define TIME_MS2I(ms) ((sysinterval_t)((ms) * 2))
#define unlikely(x) (x)

typedef struct { int waiters; msg_t wake_msg; int woken; } threads_queue_t;
typedef struct { eventflags_t flags; } event_source_t;

/* ---- debug: CH_DBG_ENABLE_ASSERTS is TRUE on the board (cfg/chconf.h) ---- */
void sim_halt(const char *reason, const char *func);
#define osalDbgAssert(c, remark) do { if (!(c)) sim_halt(remark, __func__); } while (0)
#define osalDbgCheck(c) do { if (!(c)) sim_halt("check failed: " #c, __func__); } while (0)
#define osalDbgCheckClassI() do {} while (0)
#define osalDbgCheckClassS() do {} while (0)
#define chDbgAssert osalDbgAssert
#define chDbgCheck osalDbgCheck

/* ---- locking: single-threaded simulation ---- */
extern int sim_lock_depth;
#define osalSysLock() (sim_lock_depth++)
#define osalSysUnlock() (sim_lock_depth--)
#define osalSysLockFromISR() (sim_lock_depth++)
#define osalSysUnlockFromISR() (sim_lock_depth--)
#define chSysLock() osalSysLock()
#define chSysUnlock() osalSysUnlock()
#define chSysLockFromISR() osalSysLockFromISR()
#define chSysUnlockFromISR() osalSysUnlockFromISR()
#define osalOsRescheduleS() do {} while (0)
#define osalEventObjectInit(esp) ((esp)->flags = 0)
#define osalEventBroadcastFlagsI(esp, f) ((esp)->flags |= (f))
#define osalThreadQueueObjectInit(tqp) memset((tqp), 0, sizeof(threads_queue_t))
msg_t osalThreadEnqueueTimeoutS(threads_queue_t *tqp, sysinterval_t timeout);
void osalThreadDequeueNextI(threads_queue_t *tqp, msg_t msg);
void osalThreadDequeueAllI(threads_queue_t *tqp, msg_t msg);

void sim_sleep_ticks(sysinterval_t t);
#define chThdSleep(t) sim_sleep_ticks(t)
#define chThdSleepMilliseconds(ms) sim_sleep_ticks(TIME_MS2I(ms))
#define chThdSleepMicroseconds(us) sim_sleep_ticks(1)

/* ---- registry (cmd_threads) ---- */
#define CH_DBG_FILL_THREADS TRUE
#define CH_DBG_ENABLE_STACK_CHECK TRUE
#define CH_CFG_USE_DYNAMIC FALSE
#define CH_STATE_NAMES "READY", "CURRENT", "WTSTART", "SUSPENDED", "QUEUED", "WTSEM", "WTMTX", \
  "WTCOND", "SLEEPING", "WTEXIT", "WTOREVT", "WTANDEVT", "SNDMSGQ", "SNDMSG", "WTMSG", "FINAL"
typedef struct thread {
  void *wabase;
  struct { void *sp; } ctx;
  uint32_t refs;
  struct { struct { uint32_t prio; } pqueue; } hdr;
  tstate_t state;
  const char *name;
  struct thread *next;
} thread_t;
thread_t *chRegFirstThread(void);
thread_t *chRegNextThread(thread_t *tp);

/* ---- HAL streams, channels, buffers: the real ChibiOS headers ---- */
#define HAL_USE_USB TRUE
#include "hal_objects.h"
#include "hal_streams.h"
#include "hal_channels.h"
#include "hal_buffers.h"

/* ---- a fake USB device controller (the OTG LLD contract the drivers rely on) ---- */
typedef uint8_t usbep_t;
typedef enum { USB_UNINIT = 0, USB_STOP, USB_READY, USB_SELECTED, USB_ACTIVE, USB_SUSPENDED } usbstate_t;
typedef struct { size_t txsize; size_t txcnt; } USBInEndpointState;
typedef struct { size_t rxsize; size_t rxcnt; } USBOutEndpointState;
struct USBDriver;
typedef void (*usbepcallback_t)(struct USBDriver *usbp, usbep_t ep);
typedef struct {
  uint32_t ep_mode;
  void *setup_cb;
  usbepcallback_t in_cb;
  usbepcallback_t out_cb;
  uint16_t in_maxsize;
  uint16_t out_maxsize;
  USBInEndpointState *in_state;
  USBOutEndpointState *out_state;
  uint16_t in_multiplier;
  void *setup_buf;
} USBEndpointConfig;
typedef struct { uint32_t DSTS; } sim_otg_t;
typedef struct USBDriver {
  usbstate_t state;
  const USBEndpointConfig *epc[8];
  void *in_params[7];
  void *out_params[7];
  uint8_t setup[8];
  sim_otg_t *otg;
  uint16_t transmitting, receiving;
  /* simulation */
  const uint8_t *txbuf[8]; size_t txn[8];
  uint8_t *rxbuf[8]; size_t rxmax[8];
} USBDriver;
typedef struct { void *a, *b, *c, *d; } USBConfig;
#define USB_EP_MODE_TYPE_BULK 2
#define usbGetDriverStateI(usbp) ((usbp)->state)
#define usbGetTransmitStatusI(usbp, ep) ((bool)(((usbp)->transmitting >> (ep)) & 1U))
#define usbGetReceiveStatusI(usbp, ep) ((bool)(((usbp)->receiving >> (ep)) & 1U))
#define usbGetReceiveTransactionSizeX(usbp, ep) ((usbp)->epc[ep]->out_state->rxcnt)
void usbStartTransmitI(USBDriver *usbp, usbep_t ep, const uint8_t *buf, size_t n);
void usbStartReceiveI(USBDriver *usbp, usbep_t ep, uint8_t *buf, size_t n);
void usbDisconnectBus(USBDriver *usbp);
void usbConnectBus(USBDriver *usbp);
void usbStart(USBDriver *usbp, const USBConfig *cfg);
extern USBDriver USBD1;

/* ---- MCU registers the app core touches ---- */
typedef struct { volatile uint32_t CYCCNT; } sim_dwt_t;
typedef struct { volatile uint32_t CR1; } sim_pwr_t;
typedef struct { volatile uint32_t BKP0R; } sim_rtc_t;
extern sim_dwt_t sim_dwt; extern sim_pwr_t sim_pwr; extern sim_rtc_t sim_rtc;
extern uint32_t sim_uid[3];
#define DWT (&sim_dwt)
#define PWR (&sim_pwr)
#define RTC (&sim_rtc)
#define PWR_CR1_DBP (1U << 8)
#define UID_BASE ((uintptr_t)sim_uid)
#define STM32_SYS_CK 480000000U
void NVIC_SystemReset(void);
#define __ASM __asm__
#define palSetLine(l) ((void)0)
#define palClearLine(l) ((void)0)
#define palToggleLine(l) ((void)0)
#define CC_ALIGN(n) __attribute__((aligned(n)))

/* system time (a simulated clock the scenarios advance) */
extern systime_t sim_time;
#define chVTGetSystemTimeX() (sim_time)
#define chVTTimeElapsedSinceX(t) ((sysinterval_t)(sim_time - (t)))
#endif
