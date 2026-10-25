/*
 * The key scan on the board: the ScanSequencer (the ADC interrupt's callbacks) and the
 * ScanConsumer (the scanner thread's columns) of scan_sequencer.hpp over the HAL, joined by the
 * hand-over of key_scanner_link.h. The interrupt owns the sequencer and the frame it writes, the
 * scanner thread the consumer and its copy of the frame.
 */
#include <cstdint>

extern "C" {
#include "ch.h"
#include "hal.h"

#include "config.h"
#include "key_scanner.h"
#include "messaging.h"
#include "scan_thread.h"
#include "striso.h"
}

#include "scan_sequencer.hpp"

namespace {

thread_t* tpReadButtons = nullptr; // the scanner thread, while it sleeps

} // namespace

// the scheduler for the hand-over (key_scanner_link.h)
#define SCAN_OS_LOCK() chSysLock()
#define SCAN_OS_UNLOCK() chSysUnlock()
#define SCAN_OS_SLEEP_S() \
	do { \
		tpReadButtons = chThdGetSelfX(); \
		chSchGoSleepS(CH_STATE_SUSPENDED); \
	} while (0)
#define SCAN_OS_WINDOW() \
	do { \
	} while (0)
#include "key_scanner_link.h"

namespace {

scan_link_t link;

// the interrupt's hardware
struct IsrIo {
	static void open(int ch) { palSetPort(out_channels_port[ch], out_channels_portmask[ch]); }
	static void drain(int ch) { palClearPort(out_channels_port[ch], out_channels_portmask[ch]); }
	static void all_opendrain() {
		for (int n = 0; n < ks::kOutChannels; n++) {
			palSetPadMode(out_channels_port[n], out_channels_pad[n], PAL_MODE_OUTPUT_OPENDRAIN | PAL_STM32_OSPEED_HIGHEST);
		}
	}
	static void all_pushpull() {
		for (int n = 0; n < ks::kOutChannels; n++) {
			palSetPadMode(out_channels_port[n], out_channels_pad[n], PAL_MODE_OUTPUT_PUSHPULL | PAL_STM32_OSPEED_HIGHEST);
		}
	}
	static ScanFrame* begin(uint32_t seq) { return scan_link_begin(&link, seq); }
	static void publish(uint32_t ready) { scan_link_publish(&link, ready); }
	// the interrupt published columns: wake the thread
	static void wake(uint32_t ready) {
		scan_link_publish(&link, ready);
		chSysLockFromISR();
		if (tpReadButtons != nullptr) {
			chSchReadyI(tpReadButtons);
			tpReadButtons = nullptr;
		}
		chSysUnlockFromISR();
	}
	static uint32_t cycles() { return DWT->CYCCNT; }
};

#ifdef DETECT_STUCK_NOTES
constexpr int kBootFrames = 100; // the zero level pass
#else
constexpr int kBootFrames = 0;
#endif
#ifdef USE_AUX_BUTTONS
constexpr int kAuxDebounce = AUX_BUTTON_DEBOUNCE_TIME;
// #define LINE_BUTTON_PORT   PAL_LINE(GPIOI,  2U)
// #define LINE_BUTTON_UP     PAL_LINE(GPIOI,  1U)
// #define LINE_BUTTON_DOWN   PAL_LINE(GPIOA,  9U) // GPIOA_UART1_TX
// #define LINE_BUTTON_ALT    PAL_LINE(GPIOA, 10U) // GPIOA_UART1_RX
const ioline_t aux_buttons_line[4] = {LINE_BUTTON_PORT, LINE_BUTTON_UP, LINE_BUTTON_DOWN, LINE_BUTTON_ALT};
#else
constexpr int kAuxDebounce = 0;
const ioline_t aux_buttons_line[4] = {};
#endif
constexpr int aux_buttons_msg[4] = {IDC_PORTAMENTO, IDC_OCT_UP, IDC_OCT_DOWN, IDC_ALT};

// the scanner thread's
struct ThreadIo {
	static void boot_column(int col) { ks_boot_column(col); }
	static void column(int col) { ks_column(col); }
	static void lost() { ks_lost(); }
	static void frame() { scan_stats.frames = scan_stats.frames + 1; }
	static bool aux_line(int n) { return palReadLine(aux_buttons_line[n]) != 0; }
	static void aux_send(int n, bool on) { event_send1(ID_CONTROL, aux_buttons_msg[n], on); }
};

ks::ScanSequencer sequencer;
IsrIo isr_io;
ks::ScanConsumer<kBootFrames, kAuxDebounce> consumer;
ThreadIo thread_io;

} // namespace

extern "C" void scan_isr_init(void) { sequencer.start(isr_io); }

extern "C" void scan_isr_callback(const adcsample_t adc[4]) { sequencer.callback(isr_io, adc, scan_stats); }

extern "C" void scan_thread_loop(void) {
	scan_pos_t pos = {};
	pos.k = link.seq;
	ScanFrame* work = ks_work();
	uint32_t caught = 0;
	while (true) {
		// the next complete column of the scan, sleeping until there is one
		const int note_id = scan_link_wait(&link, &pos, work, &caught);
		scan_stats.overruns = pos.lost;
		scan_stats.seq_gaps = pos.skipped;
		scan_stats.lost_wakeups = caught;
		consumer.column(thread_io, note_id);
	}
}
