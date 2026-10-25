/* A stand-in kernel for src/app/inbox.hpp: the kernel lock, the current thread, and suspending and
 * resuming the consumer. test_messaging_model.cpp implements it twice: as a model (sends and
 * receives are atomic steps, and a consumer that suspends jumps back to the model), and on real
 * threads (the lock a mutex, a suspend a condition variable wait under it). */
#ifndef MSG_STUB_CH_H
#define MSG_STUB_CH_H
#include <stdint.h>
typedef int32_t msg_t;
#define MSG_OK ((msg_t)0)
typedef struct thread {
	int id;
} thread_t;
typedef thread_t* thread_reference_t;
void chSysLock(void);
void chSysUnlock(void);
thread_t* chThdGetSelfX(void);
msg_t chThdSuspendS(thread_reference_t* trp);
void chThdResumeS(thread_reference_t* trp, msg_t msg);
#endif
