/* stand-in kernel for messaging.c: the mutex is a no-op (operations are atomic in the model), a
 * reader that would go to sleep jumps back to the model ("the queue looks empty") */
#ifndef MSG_STUB_CH_H
#define MSG_STUB_CH_H
#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>
typedef struct { int locked; } mutex_t;
typedef struct thread { int id; } thread_t;
#define CH_STATE_SUSPENDED 3
extern jmp_buf model_sleep_jmp;
extern int model_readied;
static thread_t model_reader;
static inline void chMtxObjectInit(mutex_t *m) { m->locked = 0; }
static inline void chMtxLock(mutex_t *m) { m->locked++; }
static inline void chMtxUnlock(mutex_t *m) { m->locked--; }
static inline void chSysLock(void) {}
static inline void chSysUnlock(void) {}
static inline thread_t *chThdGetSelfX(void) { return &model_reader; }
static inline void chSchReadyI(thread_t *tp) { (void)tp; model_readied++; }
static inline void chSchGoSleepS(int st) { (void)st; longjmp(model_sleep_jmp, 1); }
#endif
