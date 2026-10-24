#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
typedef uint32_t sysinterval_t;
typedef uint32_t systime_t;
#define TRUE 1
#define FALSE 0
#define TIME_MS2I(x) ((sysinterval_t)(x) * 2)
#define NORMALPRIO 128
#define THD_WORKING_AREA(n, s) char n[s]
#define THD_FUNCTION(n, a) void n(void *a)
void chRegSetThreadName(const char *n);
void chThdSleepMilliseconds(int ms);
void chThdSleepMicroseconds(int us);
void *chThdCreateStatic(void *wa, size_t s, int prio, void (*f)(void *), void *arg);
