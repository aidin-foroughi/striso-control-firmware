/* ChibiOS' compiler portability macros (ccportab.h) for the host compilers (GCC, Clang). */
#ifndef SV_HAL_CCPORTAB_H
#define SV_HAL_CCPORTAB_H
#define CC_SECTION(s) __attribute__((section(s)))
#define CC_WEAK __attribute__((weak))
#define CC_USED __attribute__((used))
#define CC_ALIGN(n) __attribute__((aligned(n)))
#define CC_PACK __attribute__((packed))
#define CC_NO_INLINE __attribute__((noinline))
#define CC_FORCE_INLINE __attribute__((always_inline))
#define CC_NO_RETURN __attribute__((noreturn))
#define CC_LIKELY(x) __builtin_expect(!!(x), 1)
#define CC_UNLIKELY(x) __builtin_expect(!!(x), 0)
#endif
