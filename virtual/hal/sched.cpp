// The virtual board's kernel: ChibiOS/RT's thread API on a deterministic scheduler in virtual time.
//
// Every firmware thread is a host thread, but only one runs at a time: the one the dispatcher
// (svh_run, called by the instance between interrupts) hands the baton to, the highest priority
// ready thread, first come first served among equals, as ChibiOS' ready list. A thread runs until
// it waits (sleep, event, mutex, suspend) and then gives the baton back; it is never preempted
// except where ChibiOS would switch at once (a thread creating or waking a higher priority one,
// a mutex handed to a higher priority waiter). Interrupts (the system tick, the codec's block,
// the USB frame) happen only between threads, when all of them wait: the firmware takes no
// virtual time. So a run is a pure function of what was injected and when.
//
// Ending: NVIC_SystemReset halts the scheduler (no thread runs again); svh_sched_shutdown wakes
// every thread with the order to end, and each one jumps out of the firmware back to its start
// (longjmp) and returns.
#include <algorithm>
#include <csetjmp>
#include <cstring>
#include <semaphore>
#include <thread>
#include <vector>

extern "C" {
#include "hal.h"
#include "sv_host.h"
}

namespace {

struct HThread;

struct WaitQueue {
	std::vector<HThread*> q; // by priority, FIFO among equals
};

struct HThread {
	thread_t tp{};
	std::thread os;
	std::binary_semaphore go{0};
	tfunc_t fn = nullptr;
	void* arg = nullptr;
	std::jmp_buf exit_jb{};
	bool started = false, finished = false, dummy = false;
	msg_t wake_msg = MSG_OK;
	bool timed = false;
	uint32_t wake_tick = 0;
	uint64_t timer_seq = 0;
	WaitQueue* waiting_on = nullptr;
};

struct Sched {
	std::vector<HThread*> all;   // registry order (creation)
	std::vector<HThread*> ready; // highest priority first, FIFO among equals
	HThread* current = nullptr;
	std::binary_semaphore host{0};
	uint64_t now = 0;
	uint32_t ticks = 0;
	uint64_t timer_seq = 0;
	bool halted = false, terminating = false;
	std::vector<WaitQueue*> queues; // of the firmware's mutexes and semaphores, freed at the end
};

Sched* S = nullptr;

HThread* of(thread_t* tp) { return tp ? static_cast<HThread*>(tp->host) : nullptr; }
tprio_t prio(const HThread* t) { return t->tp.hdr.pqueue.prio; }

void ready_insert(HThread* t) {
	t->timed = false;
	t->tp.state = CH_STATE_READY;
	auto it = std::find_if(S->ready.begin(), S->ready.end(), [&](HThread* r) { return prio(r) < prio(t); });
	S->ready.insert(it, t);
}

// ahead of the threads of its own priority (a thread preempted by a higher one)
void ready_insert_ahead(HThread* t) {
	t->tp.state = CH_STATE_READY;
	auto it = std::find_if(S->ready.begin(), S->ready.end(), [&](HThread* r) { return prio(r) <= prio(t); });
	S->ready.insert(it, t);
}

// the current thread waits in state st until it is made ready (or the scheduler ends)
void block_self(tstate_t st) {
	HThread* t = S->current;
	t->tp.state = st;
	S->host.release();
	t->go.acquire();
	if (S->terminating) std::longjmp(t->exit_jb, 1);
}

// a higher priority thread is ready: the current one steps aside (ChibiOS chSchRescheduleS)
void reschedule() {
	HThread* t = S->current;
	if (!t || S->ready.empty() || prio(S->ready.front()) <= prio(t)) return;
	ready_insert_ahead(t);
	block_self(CH_STATE_READY);
}

void entry(HThread* t) {
	t->go.acquire();
	if (!S->terminating && setjmp(t->exit_jb) == 0) t->fn(t->arg);
	t->finished = true;
	t->tp.state = CH_STATE_FINAL;
	S->host.release();
}

HThread* make_thread(void* wsp, size_t size, tprio_t pr, tfunc_t fn, void* arg) {
	auto* t = new HThread;
	t->tp.host = t;
	t->tp.hdr.pqueue.prio = pr;
	t->tp.wabase = static_cast<stkalign_t*>(wsp);
	t->tp.ctx.sp = static_cast<char*>(wsp) + size;
	t->tp.refs = 1;
	t->tp.state = CH_STATE_WTSTART;
	// CH_DBG_FILL_THREADS: the stack fill pattern. The thread runs on a host stack, so the area
	// stays unused but for its top, where ChibiOS keeps the thread's start frame (the end of
	// cmd_threads' scan for the pattern)
	if (wsp && size > 64) {
		std::memset(wsp, 0x55, size - 64);
		std::memset(static_cast<char*>(wsp) + size - 64, 0, 64);
	}
	t->fn = fn;
	t->arg = arg;
	S->all.push_back(t);
	t->os = std::thread(entry, t);
	t->started = true;
	return t;
}

void wq_insert(WaitQueue* q, HThread* t) {
	auto it = std::find_if(q->q.begin(), q->q.end(), [&](HThread* r) { return prio(r) < prio(t); });
	q->q.insert(it, t);
	t->waiting_on = q;
}

HThread* wq_pop(WaitQueue* q) {
	if (q->q.empty()) return nullptr;
	HThread* t = q->q.front();
	q->q.erase(q->q.begin());
	t->waiting_on = nullptr;
	return t;
}

WaitQueue* wq(void** p) {
	if (!*p) {
		*p = new WaitQueue;
		S->queues.push_back(static_cast<WaitQueue*>(*p));
	}
	return static_cast<WaitQueue*>(*p);
}

} // namespace

// --- the instance's side ----------------------------------------------------------------------------

extern "C" void svh_sched_init(void) {
	S = new Sched;
}

extern "C" void svh_run(void) {
	while (S && !S->halted && !S->ready.empty()) {
		HThread* t = S->ready.front();
		S->ready.erase(S->ready.begin());
		S->current = t;
		t->tp.state = CH_STATE_CURRENT;
		t->go.release();
		S->host.acquire();
		S->current = nullptr;
	}
}

extern "C" void svh_sched_shutdown(void) {
	if (!S) return;
	S->terminating = true;
	for (HThread* t : S->all) {
		if (t->dummy || t->finished) continue;
		t->go.release();
		S->host.acquire();
	}
	for (HThread* t : S->all)
		if (t->os.joinable()) t->os.join();
	for (HThread* t : S->all) delete t;
	for (WaitQueue* q : S->queues) delete q;
	delete S;
	S = nullptr;
}

extern "C" uint64_t svh_now(void) { return S->now; }
extern "C" void svh_set_now(uint64_t frames) { S->now = frames; }
extern "C" int svh_halted(void) { return S->halted; }

extern "C" void svh_tick(void) {
	S->ticks++;
	std::vector<HThread*> due;
	for (HThread* t : S->all)
		if (t->timed && static_cast<int32_t>(S->ticks - t->wake_tick) >= 0) due.push_back(t);
	std::sort(due.begin(), due.end(), [](HThread* a, HThread* b) {
		return a->wake_tick != b->wake_tick ? static_cast<int32_t>(a->wake_tick - b->wake_tick) < 0 : a->timer_seq < b->timer_seq;
	});
	for (HThread* t : due) {
		if (t->waiting_on) { // a wait with a timeout: leave the queue
			auto& q = t->waiting_on->q;
			q.erase(std::remove(q.begin(), q.end(), t), q.end());
			t->waiting_on = nullptr;
		}
		t->wake_msg = MSG_TIMEOUT;
		ready_insert(t);
	}
}

extern "C" thread_t* svh_dummy_thread(const char* name, tprio_t pr, tstate_t state) {
	static stkalign_t wa_dummy[256 / sizeof(stkalign_t)];
	std::memset(wa_dummy, 0x55, sizeof wa_dummy - 64);
	std::memset(reinterpret_cast<char*>(wa_dummy) + sizeof wa_dummy - 64, 0, 64);
	auto* t = new HThread;
	t->tp.host = t;
	t->tp.wabase = wa_dummy;
	t->tp.ctx.sp = reinterpret_cast<char*>(wa_dummy) + sizeof wa_dummy;
	t->tp.hdr.pqueue.prio = pr;
	t->tp.name = name;
	t->tp.refs = 1;
	t->tp.state = state;
	t->dummy = true;
	S->all.push_back(t);
	return &t->tp;
}

extern "C" thread_t* svh_start_main(tfunc_t fn, size_t stack) {
	static stkalign_t wa_main[1024 / sizeof(stkalign_t)];
	(void)stack;
	HThread* t = make_thread(wa_main, sizeof wa_main, NORMALPRIO, fn, nullptr);
	t->tp.name = "main";
	ready_insert(t);
	return &t->tp;
}

// --- ChibiOS -----------------------------------------------------------------------------------------

extern "C" void chSysInit(void) {}

extern "C" void chSysHalt(const char* reason) {
	(void)reason;
	S->halted = true;
	// in an interrupt (no thread runs): the interrupt ends, and no thread runs after it
	if (!S->current) return;
	for (;;) block_self(CH_STATE_FINAL);
}

extern "C" rtcnt_t chSysGetRealtimeCounterX(void) {
	return static_cast<rtcnt_t>(S->now * (STM32_SYS_CK / 1000u) / 48u); // 400 MHz cycles
}

extern "C" thread_t* chThdCreateStatic(void* wsp, size_t size, tprio_t pr, tfunc_t pf, void* arg) {
	HThread* t = make_thread(wsp, size, pr, pf, arg);
	ready_insert(t);
	reschedule(); // chSchWakeupS: a higher priority thread runs at once
	return &t->tp;
}

extern "C" thread_t* chThdGetSelfX(void) { return S->current ? &S->current->tp : nullptr; }
extern "C" tprio_t chThdGetPriorityX(void) { return S->current ? prio(S->current) : 0; }
extern "C" bool chThdShouldTerminateX(void) { return false; }

extern "C" void chThdSleep(sysinterval_t ticks) {
	HThread* t = S->current;
	if (!t || ticks == TIME_IMMEDIATE) return;
	t->timed = true;
	t->wake_tick = S->ticks + ticks;
	t->timer_seq = ++S->timer_seq;
	block_self(CH_STATE_SLEEPING);
}

extern "C" void chThdSleepUntil(systime_t time) {
	const sysinterval_t d = time - S->ticks;
	if (d != 0 && static_cast<int32_t>(d) > 0) chThdSleep(d);
}

extern "C" void chThdYield(void) {
	HThread* t = S->current;
	if (!t || S->ready.empty() || prio(S->ready.front()) < prio(t)) return;
	ready_insert(t);
	block_self(CH_STATE_READY);
}

extern "C" void chThdExit(msg_t msg) {
	(void)msg;
	for (;;) block_self(CH_STATE_FINAL);
}

extern "C" void chRegSetThreadName(const char* name) {
	if (S->current) S->current->tp.name = name;
}

extern "C" thread_t* chRegFirstThread(void) { return S->all.empty() ? nullptr : &S->all.front()->tp; }

extern "C" thread_t* chRegNextThread(thread_t* tp) {
	auto it = std::find(S->all.begin(), S->all.end(), of(tp));
	if (it == S->all.end() || ++it == S->all.end()) return nullptr;
	return &(*it)->tp;
}

extern "C" thread_t* chSchReadyI(thread_t* tp) {
	ready_insert(of(tp));
	return tp;
}

extern "C" void chSchGoSleepS(tstate_t newstate) { block_self(newstate); }

extern "C" msg_t chSchGoSleepTimeoutS(tstate_t newstate, sysinterval_t timeout) {
	HThread* t = S->current;
	if (timeout == TIME_IMMEDIATE) return MSG_TIMEOUT;
	if (timeout != TIME_INFINITE) {
		t->timed = true;
		t->wake_tick = S->ticks + timeout;
		t->timer_seq = ++S->timer_seq;
	}
	t->wake_msg = MSG_OK;
	block_self(newstate);
	return t->wake_msg;
}

extern "C" void chSchWakeupS(thread_t* ntp, msg_t msg) {
	HThread* t = of(ntp);
	t->wake_msg = msg;
	ready_insert(t);
	reschedule();
}

extern "C" void chSchRescheduleS(void) { reschedule(); }

extern "C" msg_t chThdSuspendS(thread_reference_t* trp) {
	*trp = &S->current->tp;
	S->current->wake_msg = MSG_OK;
	block_self(CH_STATE_SUSPENDED);
	return S->current->wake_msg;
}

extern "C" msg_t chThdSuspendTimeoutS(thread_reference_t* trp, sysinterval_t timeout) {
	if (timeout == TIME_IMMEDIATE) return MSG_TIMEOUT;
	*trp = &S->current->tp;
	const msg_t m = chSchGoSleepTimeoutS(CH_STATE_SUSPENDED, timeout);
	if (m == MSG_TIMEOUT) *trp = nullptr;
	return m;
}

extern "C" void chThdResumeI(thread_reference_t* trp, msg_t msg) {
	if (!*trp) return;
	HThread* t = of(*trp);
	*trp = nullptr;
	t->wake_msg = msg;
	ready_insert(t);
}

extern "C" void chThdResume(thread_reference_t* trp, msg_t msg) {
	chThdResumeI(trp, msg);
	reschedule();
}

// thread queues (chthreads.h): FIFO; a wait with a timeout leaves the queue when it times out
extern "C" void chThdQueueObjectInit(threads_queue_t* tqp) { tqp->waiters = nullptr; }

extern "C" msg_t chThdEnqueueTimeoutS(threads_queue_t* tqp, sysinterval_t timeout) {
	if (timeout == TIME_IMMEDIATE) return MSG_TIMEOUT;
	HThread* t = S->current;
	WaitQueue* q = wq(&tqp->waiters);
	q->q.push_back(t);
	t->waiting_on = q;
	return chSchGoSleepTimeoutS(CH_STATE_QUEUED, timeout);
}

extern "C" void chThdDequeueNextI(threads_queue_t* tqp, msg_t msg) {
	HThread* t = tqp->waiters ? wq_pop(static_cast<WaitQueue*>(tqp->waiters)) : nullptr;
	if (!t) return;
	t->wake_msg = msg;
	ready_insert(t);
}

extern "C" void chThdDequeueAllI(threads_queue_t* tqp, msg_t msg) {
	while (tqp->waiters && !static_cast<WaitQueue*>(tqp->waiters)->q.empty()) chThdDequeueNextI(tqp, msg);
}

extern "C" systime_t chVTGetSystemTimeX(void) { return S->ticks; }

// mutexes: no priority inheritance (the firmware's mutex is never held across a wait)
extern "C" void chMtxObjectInit(mutex_t* mp) {
	mp->owner = nullptr;
	mp->cnt = 0;
	mp->waiters = nullptr;
}

extern "C" void chMtxLock(mutex_t* mp) {
	if (!mp->owner) {
		mp->owner = &S->current->tp;
		return;
	}
	wq_insert(wq(&mp->waiters), S->current);
	block_self(CH_STATE_WTMTX); // the unlock made this thread the owner
}

extern "C" bool chMtxTryLock(mutex_t* mp) {
	if (mp->owner) return false;
	mp->owner = &S->current->tp;
	return true;
}

extern "C" void chMtxUnlock(mutex_t* mp) {
	HThread* next = mp->waiters ? wq_pop(static_cast<WaitQueue*>(mp->waiters)) : nullptr;
	if (!next) {
		mp->owner = nullptr;
		return;
	}
	mp->owner = &next->tp;
	ready_insert(next);
	reschedule();
}

extern "C" void chSemObjectInit(semaphore_t* sp, cnt_t n) {
	sp->cnt = n;
	sp->waiters = nullptr;
}

extern "C" msg_t chSemWaitTimeout(semaphore_t* sp, sysinterval_t timeout) {
	if (--sp->cnt >= 0) return MSG_OK;
	if (timeout == TIME_IMMEDIATE) {
		sp->cnt++;
		return MSG_TIMEOUT;
	}
	HThread* t = S->current;
	wq_insert(wq(&sp->waiters), t);
	const msg_t m = chSchGoSleepTimeoutS(CH_STATE_WTSEM, timeout);
	if (m == MSG_TIMEOUT) sp->cnt++;
	return m;
}

extern "C" msg_t chSemWait(semaphore_t* sp) { return chSemWaitTimeout(sp, TIME_INFINITE); }

extern "C" void chSemSignalI(semaphore_t* sp) {
	if (++sp->cnt <= 0) {
		HThread* t = sp->waiters ? wq_pop(static_cast<WaitQueue*>(sp->waiters)) : nullptr;
		if (t) {
			t->wake_msg = MSG_OK;
			ready_insert(t);
		}
	}
}

extern "C" void chSemSignal(semaphore_t* sp) {
	chSemSignalI(sp);
	reschedule();
}

// events (one thread's pending flags)
namespace {
bool evt_satisfied(const thread_t* tp) {
	if (tp->state == CH_STATE_WTOREVT) return (tp->epending & tp->ewmask) != 0;
	if (tp->state == CH_STATE_WTANDEVT) return (tp->epending & tp->ewmask) == tp->ewmask;
	return false;
}
} // namespace

extern "C" void chEvtSignalI(thread_t* tp, eventmask_t events) {
	tp->epending |= events;
	if (evt_satisfied(tp)) ready_insert(of(tp));
}

extern "C" void chEvtSignal(thread_t* tp, eventmask_t events) {
	chEvtSignalI(tp, events);
	reschedule();
}

extern "C" eventmask_t chEvtAddEvents(eventmask_t events) {
	thread_t* tp = &S->current->tp;
	return tp->epending |= events;
}

extern "C" eventmask_t chEvtGetAndClearEvents(eventmask_t events) {
	thread_t* tp = &S->current->tp;
	const eventmask_t m = tp->epending & events;
	tp->epending &= ~events;
	return m;
}

extern "C" eventmask_t chEvtWaitOneTimeout(eventmask_t events, sysinterval_t timeout) {
	thread_t* tp = &S->current->tp;
	eventmask_t m = tp->epending & events;
	if (m == 0) {
		if (timeout == TIME_IMMEDIATE) return 0;
		tp->ewmask = events;
		if (chSchGoSleepTimeoutS(CH_STATE_WTOREVT, timeout) < MSG_OK) return 0;
		m = tp->epending & events;
	}
	m ^= m & (m - 1); // the lowest one
	tp->epending &= ~m;
	return m;
}

extern "C" eventmask_t chEvtWaitOne(eventmask_t events) { return chEvtWaitOneTimeout(events, TIME_INFINITE); }

extern "C" eventmask_t chEvtWaitAny(eventmask_t events) {
	thread_t* tp = &S->current->tp;
	eventmask_t m = tp->epending & events;
	if (m == 0) {
		tp->ewmask = events;
		block_self(CH_STATE_WTOREVT);
		m = tp->epending & events;
	}
	tp->epending &= ~m;
	return m;
}

// --- CMSIS: the reset -------------------------------------------------------------------------------

extern "C" void NVIC_SystemReset(void) {
	S->halted = true;
	for (;;) block_self(CH_STATE_FINAL);
}
