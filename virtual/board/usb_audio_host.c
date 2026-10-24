/* The virtual board's USB audio, in place of src/usb/usb_audio.c (UAC streaming: hardware only).
 * The synth side is the board's: the ring and the test tone are usb_audio_core.h, so what a
 * block commits is what the board would stream (the synth, or the tone with Stcu). The virtual
 * board takes each committed block from the ring (the "USB" tap of sv_render), with the engine's
 * output, the effects' output and the controls the engine got (the other taps). There is no
 * isochronous stream, so StcU prints the board's lines with the stream counters at 0, and a latency
 * marker (Stcl1) is never sent: after 100 ms it reports "not streaming", as the board without a
 * stream. usbcfg.c's hooks: the SOF time (for Stcl0's echo), nothing else. */
#include "usb_audio.h"

#include "ch.h"
#include "chprintf.h"
#include "hal.h"
#include "sv_host.h"
#include "usb_audio_core.h"

static ua_core_t ua;
static volatile bool ua_tone_on;
static uint32_t ua_codec_blocks;
svh_block svh_last_block;

void svh_usb_audio_init(void);
void svh_usb_audio_init(void) {
	ua_init(&ua);
	ua_stats_reset(&ua);
	ua_tone_on = false;
}

void usb_audio_codec_tickI(void) { ua_codec_blocks = ua_codec_blocks + 1; }

int32_t* usb_audio_write_ptr(void) { return ua_write_ptr(&ua); }

void usb_audio_write_commit(void) {
	int32_t* p = ua_write_ptr(&ua);
	ua_write_commit(&ua, ua_tone_on);
	for (int i = 0; i < 2 * UA_BLOCK; i++) svh_last_block.usb[i] = p[i];
	svh_synth_taps(svh_last_block.engine, svh_last_block.fx, svh_last_block.fx + UA_BLOCK, svh_last_block.controls,
	    svh_last_block.globals);
	svh_last_block.valid = 1;
	ua.r = ua.w; /* nobody streams: keep the ring's reader with the writer */
}

void usb_audio_set_tone(bool on) { ua_tone_on = on; }
bool usb_audio_tone(void) { return ua_tone_on; }

int usb_audio_set_target(int frames) {
	chSysLock();
	const int t = (int)ua_set_target(&ua, frames);
	chSysUnlock();
	return t;
}

int usb_audio_target(void) { return (int)ua.target; }

/* the board's StcU lines (usb_audio.c usb_audio_print_status), for a board without a stream */
void usb_audio_print_status(void* chp) {
	BaseSequentialStream* out = (BaseSequentialStream*)chp;
	const int32_t target = ua.target;
	chprintf(out, "usb audio (UAC2): %s, source %s\r\n", "virtual, no stream (alt 0)",
	    ua_tone_on ? "test tone (L 1 kHz, R 500 Hz, -6 dBFS)" : "synth");
	chprintf(out, "since last U: packets sent %lu missed %lu (host skipped %lu, flushed early %lu); "
	              "built %lu (47:%lu 48:%lu 49:%lu)\r\n",
	    0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u);
	chprintf(out, "ring: level %lu now, %ld filtered, %ld..%ld before packets (target %ld = %ld us, of %d)%s\r\n",
	    0u, (int32_t)(ua.lvl_q8 >> 8), 0, 0, target, target * 1000 / 48, UA_RING_FRAMES, "");
	chprintf(out, "faults: underruns %lu overruns %lu primes %lu (silent packets %lu), fast starts %lu\r\n", 0u, 0u, 0u, 0u,
	    0u);
	chprintf(out, "frames: not ready at SOF %lu; SOF late (>1.1 ms) %lu lost %lu, max interval %lu us\r\n", 0u, 0u, 0u, 0u);
	chprintf(out, "builds: after fetch %lu, after miss %lu, at SOF %lu; fetch at %lu..%lu us (avg %lu) after SOF; "
	              "build avg %lu max %lu cycles\r\n",
	    0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u);
	chprintf(out, "synth: commit %lu us avg, %lu max after its DMA interrupt (block %lu us); off cadence %lu; "
	              "since boot: blocks lost %lu, repeated %lu\r\n",
	    0u, 0u, (uint32_t)((UA_BLOCK * 1000000u + UA_RATE / 2) / UA_RATE), 0u, ua.lost, ua.repeats);
	chprintf(out, "cpu: usb audio ISR hooks %lu cycles", 0u);
	chprintf(out, "; longest lock: thread %lu us, ISR %lu us\r\n", 0u, 0u);
	ua_stats_reset(&ua);
}

/* --- usbcfg.c's hooks: no isochronous endpoint; the SOF's time for usb_audio_echo ------------------ */
static uint32_t ua_sof_t;
void usb_audio_configured_hookI(USBDriver* usbp) { (void)usbp; }
void usb_audio_reset_hookI(void) {}
void usb_audio_suspend_hookI(void) {}
void usb_audio_wakeup_hookI(USBDriver* usbp) { (void)usbp; }
void usb_audio_sof_hookI(USBDriver* usbp) {
	(void)usbp;
	ua_sof_t = DWT->CYCCNT;
}
bool usb_audio_requests_hook(USBDriver* usbp) {
	(void)usbp;
	return false;
}

/* --- the latency marker (Stcl1) and echo (Stcl0), as usb_audio.c's without a stream ------------------ */
#define UA_CYC_US (STM32_SYS_CK / 1000000U)
#define UA_CYC_MS (STM32_SYS_CK / 1000U)
extern volatile uint32_t bdu_rx_cycles; /* bulk_usb.c: when the last console OUT packet arrived */
static bool ua_mk_wanted;
static uint32_t ua_mk_seq;
static systime_t ua_mk_asked;

static uint32_t ua_us(uint32_t cycles) { return (cycles + UA_CYC_US / 2) / UA_CYC_US; }

bool usb_audio_marker_request(void) {
	chSysLock();
	const bool idle = !ua_mk_wanted;
	if (idle) {
		ua_mk_seq++;
		ua_mk_asked = chVTGetSystemTimeX();
		ua_mk_wanted = true;
	}
	chSysUnlock();
	return idle;
}

void usb_audio_marker_poll(void* chp) {
	if (!ua_mk_wanted || chVTTimeElapsedSinceX(ua_mk_asked) <= TIME_MS2I(100)) return;
	ua_mk_wanted = false;
	chprintf((BaseSequentialStream*)chp, "usb audio marker %lu: not streaming\r\n", ua_mk_seq);
}

void usb_audio_echo(void* chp) {
	chSysLock();
	const uint32_t now = DWT->CYCCNT, rx = bdu_rx_cycles, sof = ua_sof_t;
	chSysUnlock();
	const uint32_t in_frame = now - sof;
	const uint32_t to_sof = in_frame < UA_CYC_MS ? UA_CYC_MS - in_frame : 0;
	chprintf((BaseSequentialStream*)chp, "usb audio echo: held %lu us\r\n", ua_us(now - rx + to_sof));
}
