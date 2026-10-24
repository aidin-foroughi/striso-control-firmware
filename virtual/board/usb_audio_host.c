/* The virtual board's USB audio, in place of src/usb/usb_audio.c (UAC streaming: hardware only).
 * The synth side is the board's: the ring and the test tone are usb_audio_core.h, so what a
 * block commits is what the board would stream (the synth, or the tone with Stcu). The virtual
 * board takes each committed block from the ring (the "USB" tap of sv_render), with the engine's
 * output, the effects' output and the controls the engine got (the other taps). There is no
 * isochronous stream, so StcU prints the board's lines with the stream counters at 0. */
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
	chprintf(out, "usb audio (UAC%d): %s, source %s\r\n", USB_AUDIO_UAC2 ? 2 : 1, "virtual, no stream (alt 0)",
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
