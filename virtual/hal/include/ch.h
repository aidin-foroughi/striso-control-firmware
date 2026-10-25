/*
 * The virtual board's ChibiOS/RT: the kernel API the firmware's own files use (threads, sleeps,
 * the system time, mutexes, events, the registry, the scheduler's suspend/ready), on the
 * deterministic virtual-time scheduler of virtual/hal/sched.cpp. Names, types and constants are
 * ChibiOS 20's (cfg/chconf.h: 2 kHz tick, tick mode, no time quantum); only the parts the
 * firmware uses are here.
 */
#ifndef SV_HAL_CH_H
#define SV_HAL_CH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* --- configuration (cfg/chconf.h) -------------------------------------------------------------- */
#define CH_CFG_ST_FREQUENCY 2000
#define CH_CFG_ST_TIMEDELTA 0
#define CH_CFG_TIME_QUANTUM 0
#define CH_CFG_USE_DYNAMIC FALSE
#define CH_DBG_ENABLE_STACK_CHECK TRUE
#define CH_DBG_FILL_THREADS TRUE
#ifndef CH_DBG_STATISTICS
#define CH_DBG_STATISTICS FALSE
#endif
#define CH_KERNEL_VERSION "virtual"

/* --- types ------------------------------------------------------------------------------------- */
typedef uint32_t systime_t;
typedef uint32_t sysinterval_t;
typedef uint32_t time_conv_t;
typedef uint32_t rtcnt_t;
typedef uint32_t tprio_t;
typedef int32_t msg_t;
typedef uint32_t eventmask_t;
typedef uint32_t eventflags_t;
typedef uint8_t tstate_t;
typedef uint8_t trefs_t;
typedef uint64_t stkalign_t;
typedef int32_t cnt_t;
typedef void (*tfunc_t)(void* p);

#define MSG_OK (msg_t)0
#define MSG_TIMEOUT (msg_t)-1
#define MSG_RESET (msg_t)-2

#define IDLEPRIO (tprio_t)1
#define LOWPRIO (tprio_t)2
#define NORMALPRIO (tprio_t)128
#define HIGHPRIO (tprio_t)255

#define TIME_IMMEDIATE ((sysinterval_t)0)
#define TIME_INFINITE ((sysinterval_t)-1)
#define TIME_MAX_INTERVAL ((sysinterval_t)-2)
#define TIME_S2I(secs) ((sysinterval_t)((time_conv_t)(secs) * (time_conv_t)CH_CFG_ST_FREQUENCY))
#define TIME_MS2I(msecs) \
	((sysinterval_t)((((time_conv_t)(msecs) * (time_conv_t)CH_CFG_ST_FREQUENCY) + (time_conv_t)999) / (time_conv_t)1000))
#define TIME_US2I(usecs) \
	((sysinterval_t)((((time_conv_t)(usecs) * (time_conv_t)CH_CFG_ST_FREQUENCY) + (time_conv_t)999999) / (time_conv_t)1000000))
#define TIME_I2MS(interval) \
	(time_msecs_t)((((time_conv_t)(interval) * (time_conv_t)1000) + (time_conv_t)CH_CFG_ST_FREQUENCY - (time_conv_t)1) / \
	               (time_conv_t)CH_CFG_ST_FREQUENCY)
typedef uint32_t time_msecs_t;

/* thread states (chschd.h) */
#define CH_STATE_READY (tstate_t)0
#define CH_STATE_CURRENT (tstate_t)1
#define CH_STATE_WTSTART (tstate_t)2
#define CH_STATE_SUSPENDED (tstate_t)3
#define CH_STATE_QUEUED (tstate_t)4
#define CH_STATE_WTSEM (tstate_t)5
#define CH_STATE_WTMTX (tstate_t)6
#define CH_STATE_WTCOND (tstate_t)7
#define CH_STATE_SLEEPING (tstate_t)8
#define CH_STATE_WTEXIT (tstate_t)9
#define CH_STATE_WTOREVT (tstate_t)10
#define CH_STATE_WTANDEVT (tstate_t)11
#define CH_STATE_SNDMSGQ (tstate_t)12
#define CH_STATE_SNDMSG (tstate_t)13
#define CH_STATE_WTMSG (tstate_t)14
#define CH_STATE_FINAL (tstate_t)15
#define CH_STATE_NAMES \
	"READY", "CURRENT", "WTSTART", "SUSPENDED", "QUEUED", "WTSEM", "WTMTX", "WTCOND", "SLEEPING", "WTEXIT", \
	    "WTOREVT", "WTANDEVT", "SNDMSGQ", "SNDMSG", "WTMSG", "FINAL"

/* A thread: the fields the firmware reads (console.cpp StcI, usb_audio.c) under their
 * ChibiOS names; the scheduler keeps the rest. */
typedef struct ch_thread thread_t;
struct ch_thread {
	struct {
		struct {
			tprio_t prio;
		} pqueue;
	} hdr;
	struct {
		void* sp;
	} ctx;
	stkalign_t* wabase;
	const char* name;
	tstate_t state;
	trefs_t refs;
	eventmask_t epending;
	eventmask_t ewmask;
	void* host; /* the scheduler's thread */
};

typedef struct ch_mutex {
	thread_t* owner;
	int32_t cnt;
	void* waiters; /* the scheduler's queue */
} mutex_t;

typedef struct ch_semaphore {
	cnt_t cnt;
	void* waiters;
} semaphore_t;

typedef thread_t* thread_reference_t;

/* a queue of waiting threads (chthreads.h threads_queue_t): FIFO, as ChibiOS' */
typedef struct ch_threads_queue {
	void* waiters; /* the scheduler's queue */
} threads_queue_t;

/* an event source (chevents.h): the firmware registers no listener on one (the USB drivers'
 * channels broadcast their flags to nobody), so it has none */
typedef struct event_source {
	void* next;
} event_source_t;

/* working areas: plain arrays (the virtual board's threads run on host stacks; cmd_threads
 * reads the fill pattern of the area to show what it never used) */
#define THD_WORKING_AREA_SIZE(n) (n)
#define THD_WORKING_AREA(s, n) stkalign_t s[((n) + sizeof(stkalign_t) - 1) / sizeof(stkalign_t)]
#define THD_FUNCTION(tname, arg) void tname(void* arg)

/* --- system ------------------------------------------------------------------------------------ */
void chSysInit(void);
/* one firmware thread runs at a time and interrupts come between them: the locks are no-ops */
static inline void chSysLock(void) {}
static inline void chSysUnlock(void) {}
static inline void chSysLockFromISR(void) {}
static inline void chSysUnlockFromISR(void) {}
void chSysHalt(const char* reason);
/* the realtime counter (DWT cycles at 400 MHz) follows virtual time: firmware code takes none */
rtcnt_t chSysGetRealtimeCounterX(void);

/* --- threads ----------------------------------------------------------------------------------- */
thread_t* chThdCreateStatic(void* wsp, size_t size, tprio_t prio, tfunc_t pf, void* arg);
thread_t* chThdGetSelfX(void);
void chThdSleep(sysinterval_t ticks);
#define chThdSleepS(t) chThdSleep(t)
#define chThdSleepSeconds(sec) chThdSleep(TIME_S2I(sec))
#define chThdSleepMilliseconds(msec) chThdSleep(TIME_MS2I(msec))
#define chThdSleepMicroseconds(usec) chThdSleep(TIME_US2I(usec))
void chThdSleepUntil(systime_t time);
void chThdYield(void);
bool chThdShouldTerminateX(void);
void chThdExit(msg_t msg);
tprio_t chThdGetPriorityX(void);
void chRegSetThreadName(const char* name);
thread_t* chRegFirstThread(void);
thread_t* chRegNextThread(thread_t* tp);

/* --- scheduler --------------------------------------------------------------------------------- */
thread_t* chSchReadyI(thread_t* tp);
void chSchGoSleepS(tstate_t newstate);
msg_t chSchGoSleepTimeoutS(tstate_t newstate, sysinterval_t timeout);
void chSchWakeupS(thread_t* ntp, msg_t msg);
void chSchRescheduleS(void);
msg_t chThdSuspendS(thread_reference_t* trp);
msg_t chThdSuspendTimeoutS(thread_reference_t* trp, sysinterval_t timeout);
void chThdResumeI(thread_reference_t* trp, msg_t msg);
void chThdResume(thread_reference_t* trp, msg_t msg);
void chThdQueueObjectInit(threads_queue_t* tqp);
msg_t chThdEnqueueTimeoutS(threads_queue_t* tqp, sysinterval_t timeout);
void chThdDequeueNextI(threads_queue_t* tqp, msg_t msg);
void chThdDequeueAllI(threads_queue_t* tqp, msg_t msg);

/* --- time -------------------------------------------------------------------------------------- */
systime_t chVTGetSystemTimeX(void);
#define chVTGetSystemTime() chVTGetSystemTimeX()
#define chTimeAddX(systime, interval) ((systime_t)((systime) + (interval)))
#define chTimeDiffX(start, end) ((sysinterval_t)((systime_t)((end) - (start))))
#define chVTTimeElapsedSinceX(start) chTimeDiffX((start), chVTGetSystemTimeX())

/* --- mutexes, semaphores, events --------------------------------------------------------------- */
void chMtxObjectInit(mutex_t* mp);
void chMtxLock(mutex_t* mp);
void chMtxUnlock(mutex_t* mp);
bool chMtxTryLock(mutex_t* mp);
void chSemObjectInit(semaphore_t* sp, cnt_t n);
msg_t chSemWait(semaphore_t* sp);
msg_t chSemWaitTimeout(semaphore_t* sp, sysinterval_t timeout);
void chSemSignal(semaphore_t* sp);
void chSemSignalI(semaphore_t* sp);
eventmask_t chEvtWaitOne(eventmask_t events);
eventmask_t chEvtWaitAny(eventmask_t events);
eventmask_t chEvtWaitOneTimeout(eventmask_t events, sysinterval_t timeout);
void chEvtSignalI(thread_t* tp, eventmask_t events);
void chEvtSignal(thread_t* tp, eventmask_t events);
eventmask_t chEvtAddEvents(eventmask_t events);
eventmask_t chEvtGetAndClearEvents(eventmask_t events);
static inline void chEvtObjectInit(event_source_t* esp) { esp->next = esp; }
/* no listener: the flags go nowhere, as with ChibiOS' broadcast to an empty source */
static inline void chEvtBroadcastFlagsI(event_source_t* esp, eventflags_t flags) {
	(void)esp;
	(void)flags;
}

/* debug */
#define chDbgCheck(c) ((void)0)
#define chDbgAssert(c, r) ((void)0)

#ifdef __cplusplus
}
#endif

#endif
