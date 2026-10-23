/* Bare-metal start for the target renderer on QEMU's mps2-an500 (Cortex-M7 with FPv5-D16):
 * the vector table; the FPU set up as ChibiOS' crt0_v7m.S leaves it on the board (CPACR full
 * access, FPSCR = FPDSCR = 0: IEEE round to nearest, no flush-to-zero, no default NaN); bss,
 * newlib's semihosting stdio (rdimon), constructors, argv from the semihosting command line, main,
 * exit. QEMU loads every section at its address, so .data needs no copy. */
#include <stdint.h>
#include <stdlib.h>

extern int main(int argc, char** argv);
extern void __libc_init_array(void);
extern void initialise_monitor_handles(void);
extern uint32_t __stack, __bss_start__, __bss_end__;

/* __libc_init_array calls these (normally from crti.o, not linked with -nostartfiles) */
__attribute__((used)) void _init(void) {}
__attribute__((used)) void _fini(void) {}

static int semihost(int op, void* arg) {
	register int r0 __asm("r0") = op;
	register void* r1 __asm("r1") = arg;
	__asm volatile("bkpt 0xab"
	               : "+r"(r0)
	               : "r"(r1)
	               : "memory");
	return r0;
}

static void c_start(void) __attribute__((noreturn, used));
static void c_start(void) {
	for (uint32_t* p = &__bss_start__; p < &__bss_end__; p++) *p = 0;
	initialise_monitor_handles();
	__libc_init_array();
	static char cmd[512];
	struct {
		char* buf;
		int len;
	} blk = {cmd, (int)sizeof cmd - 1};
	static char* argv[16];
	int argc = 0;
	if (semihost(0x15, &blk) == 0) { /* SYS_GET_CMDLINE */
		cmd[blk.len] = 0;
		for (char* s = cmd; *s && argc < 15;) {
			while (*s == ' ') *s++ = 0;
			if (!*s) break;
			argv[argc++] = s;
			while (*s && *s != ' ') s++;
		}
	}
	argv[argc] = 0;
	exit(main(argc, argv));
}

void Reset_Handler(void) __attribute__((naked, noreturn));
void Reset_Handler(void) {
	__asm volatile(
	    "ldr r0, =0xE000ED88  \n" /* CPACR: CP10/CP11 full access */
	    "ldr r1, [r0]         \n"
	    "orr r1, r1, #(0xF << 20) \n"
	    "str r1, [r0]         \n"
	    "dsb                  \n"
	    "isb                  \n"
	    "mov r0, #0           \n"
	    "vmsr fpscr, r0       \n"
	    "ldr r0, =0xE000EF3C  \n" /* FPDSCR = 0, as crt0_v7m.S */
	    "mov r1, #0           \n"
	    "str r1, [r0]         \n"
	    "b c_start            \n"
	    ".ltorg               \n"); /* the constants right here (with LTO there is no later pool) */
}

/* any fault ends the emulation with an error (semihosting SYS_EXIT, ADP_Stopped_RunTimeErrorUnknown) */
static void Fault_Handler(void) {
	semihost(0x18, (void*)0x20024);
	for (;;) {
	}
}

__attribute__((section(".vectors"), used)) const void* const vectors[16] = {
    &__stack,
    (const void*)Reset_Handler,
    (const void*)Fault_Handler,
    (const void*)Fault_Handler,
    (const void*)Fault_Handler,
    (const void*)Fault_Handler,
    (const void*)Fault_Handler,
    0,
    0,
    0,
    0,
    (const void*)Fault_Handler,
    (const void*)Fault_Handler,
    0,
    (const void*)Fault_Handler,
    (const void*)Fault_Handler,
};
