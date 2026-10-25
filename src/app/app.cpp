/*
 * The application (was main.c, by Piers Titus van der Torren): the boot sequence, the threads
 * (app_threads.h: one table of every thread, started from here and read by the build's checks),
 * the blinker and the main thread's loop. main() itself, the .fwinfo record and config.h's
 * definitions are in start.c (C: they are byte for byte what main.c built).
 *
 * This file is part of Striso Control: free software under the GNU General Public License,
 * version 3 or later (see COPYING).
 */
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <type_traits>

#include "ch.h"
#include "hal.h"

extern "C" {
#include "aux_jack.h"
#include "button_read.h"
#include "codec.h"
#include "config.h"
#include "key_scanner.h"
#include "led.h"
#include "messaging.h"
#include "motionsensor.h"
#include "pconnection.h"
#include "synth.h"
}

#include "app.h"
#include "app_threads.h"

namespace {

// the LED flash thread
THD_WORKING_AREA(waBlinker, 128);
void Blinker(void* arg) {
	(void)arg;
	chRegSetThreadName("blinker");
	while (true) {
		chThdSleepMilliseconds(300);
		palSetLine(LINE_LED1);
		chThdSleepMilliseconds(300);
		palClearLine(LINE_LED1);
	}
}

void blinker_start() { chThdCreateStatic(waBlinker, sizeof(waBlinker), NORMALPRIO, Blinker, nullptr); }

struct ThreadSpec {
	const char* name;
	std::size_t stack;
	tprio_t prio;
	void (*start)(); // nullptr: the kernel's own
	bool kernel;     // (start is nullptr: known at compile time, unlike a function's address)
};

#define STRISO_THREAD_ID(id, ...) id,
enum class Thread : std::size_t { STRISO_THREADS(STRISO_THREAD_ID) };
#undef STRISO_THREAD_ID

#define STRISO_THREAD_SPEC(id, name, stack, prio, start, ...) ThreadSpec{name, stack, prio, start, std::is_same_v<decltype(start), std::nullptr_t>},
constexpr ThreadSpec kThreads[] = {STRISO_THREADS(STRISO_THREAD_SPEC)};
#undef STRISO_THREAD_SPEC

constexpr const ThreadSpec& spec(Thread t) { return kThreads[static_cast<std::size_t>(t)]; }

// every row the boot starts (all but the kernel's own)
constexpr uint32_t startable() {
	uint32_t m = 0;
	for (std::size_t i = 0; i < std::size(kThreads); i++) {
		if (!kThreads[i].kernel) m |= 1u << i;
	}
	return m;
}
static_assert(std::size(kThreads) <= 32);
uint32_t started = 0;

// one thread's start, from its row: each row once (the asserts are on in the board's build). A
// template, so the row's start is a direct call (no call through the table for the stack check)
template <Thread t>
void start() {
	static_assert(!spec(t).kernel, "app: the kernel starts this thread");
	constexpr uint32_t bit = 1u << static_cast<std::size_t>(t);
	chDbgAssert((started & bit) == 0, "app: a thread started twice");
	started |= bit;
	constexpr void (*fn)() = spec(t).start;
	fn();
}

// the working areas app.cpp owns are its rows' sizes
static_assert(sizeof(waBlinker) == THD_WORKING_AREA_SIZE(spec(Thread::blinker).stack));

} // namespace

// The boot, in the order the board has always started (the codec before the synth, the LED ramp
// before the jack, the synth control's settings before the keys), then the main loop.
void app_run(void) {
	// Enable the headphone amp to stop noise at boot with some amps
	palSetLine(LINE_HP_EN);

	led_init();
	led_rgb(0x000800);

	InitPConnection();

	start<Thread::control>(); // the keys to the synth and the binary stream (control_thread.cpp)
	start<Thread::blinker>();

	codec_init(SAMPLERATE);
	start<Thread::synth>();

	for (int i = 8; i < 168; i++) {
		led_rgb3(0, i, 0);
		chThdSleepMilliseconds(1);
	}

	start<Thread::auxjack>();
	synth_control_init();

	// Send initial configuration MIDI
	midi_config();

	start<Thread::scan_dump>(); // the raw recordings' dump (StcR), before the scanner as always
	start<Thread::read_buttons>();
	start<Thread::motion>();
	chDbgAssert(started == startable(), "app: a thread of app_threads.h was not started");

	while (true) {
		chThdSleepMilliseconds(2);
		synth_tick();
		led_tick();
		PExReceive();
	}
}
