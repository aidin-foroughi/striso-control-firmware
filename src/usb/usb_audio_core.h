/**
 * USB audio output: the hardware independent part (ring buffer, packet sizing, sample packing).
 * Plain C with no ChibiOS dependency, so the host unit test (tools/usbaudio/test_core.c) runs
 * exactly this code.
 *
 * Data path: the synth thread writes each 32-frame stereo block (the same int32 samples it gives
 * the codec: 24 bit, left-justified) into a ring; the USB ISR takes one packet per 1 ms USB frame.
 *
 * Asynchronous source: the codec clock (exactly 48 kHz from the board's crystal) and the host's
 * USB frame clock are independent, so the number of frames per packet must follow the codec clock.
 * The ring level is the integral of the rate difference: a slow servo on the (low-passed) level
 * sets a fractional frames-per-packet rate around 48, and an accumulator turns it into a 47/48/49
 * sequence. The level stays bounded, so there is no drift however long the stream runs.
 *
 * Concurrency: one writer (synth thread) owns w, one reader (USB ISR, which preempts the writer
 * on this single core) owns r. The writer never blocks and never looks at r; the reader only
 * reads committed frames, and detects an overrun (writer close to a ring ahead) and re-centres.
 */
#ifndef USB_AUDIO_CORE_H
#define USB_AUDIO_CORE_H

#include <stdint.h>
#include <stddef.h>

#define UA_RATE            48000
#define UA_CHANNELS        2
#define UA_FRAMES_NOMINAL  (UA_RATE / 1000)          // 48 frames per 1 ms USB frame
#define UA_FRAMES_MIN      (UA_FRAMES_NOMINAL - 1)   // 47
#define UA_FRAMES_MAX      (UA_FRAMES_NOMINAL + 1)   // 49 -> wMaxPacketSize 294 (24 bit)
#define UA_BLOCK           32                        // synth block (frames)
#define UA_RING_FRAMES     512                       // 10.7 ms, power of 2, multiple of UA_BLOCK
#define UA_RING_MASK       (UA_RING_FRAMES - 1)
#define UA_TARGET_FRAMES   144                       // level kept before taking a packet: 3 ms
#define UA_LEVEL_SHIFT     5                         // level low-pass: 2^5 = 32 packets (32 ms)
#define UA_CORR_MAX        0x4000                    // rate correction limit: +-0.25 frame/packet
#define UA_OVERRUN_LEVEL   (UA_RING_FRAMES - 2 * UA_BLOCK)
#define UA_PACKET_MAX      (UA_FRAMES_MAX * UA_CHANNELS * 3)   // 294 bytes

#if (UA_RING_FRAMES % UA_BLOCK) != 0 || (UA_RING_FRAMES & UA_RING_MASK) != 0
#error "ring size must be a power of 2 and a multiple of the synth block"
#endif

typedef struct {
	int32_t ring[UA_RING_FRAMES * UA_CHANNELS];  // interleaved L R, sample << 8 (24 bit)
	volatile uint32_t w;      // frames written, free running (synth thread)
	volatile uint32_t r;      // frames read, free running (USB ISR)
	int32_t lvl_q8;           // low-passed level, frames * 256
	int32_t acc_q16;          // packet size accumulator, frames * 65536
	uint8_t priming;          // sending silence until the ring holds UA_TARGET_FRAMES
	uint32_t tone_ph;         // test tone phase (frames, mod 96)
	// statistics (reader side)
	uint32_t packets, frames, underruns, overruns, primes;
	uint32_t sizes[3];        // packets of 47, 48, 49 frames
	int32_t lvl_min, lvl_max; // raw level seen before a packet, since the last stats reset
} ua_core_t;

// --- packet sizing ----------------------------------------------------------------------------

static inline void ua_rate_reset(ua_core_t *c) {
	c->lvl_q8 = UA_TARGET_FRAMES << 8;
	c->acc_q16 = 0x8000;
}

// Frames for the next packet, given the ring level (frames available) before taking it.
// Rate = 48 + (filtered level - target) / 256 frames per packet, limited to 48 +- 0.25, so the
// result is always 47, 48 or 49 (a fraction in [0, 1) plus 47.75 .. 48.25 floors to one of them).
// Loop: level filter pole 1/32 per packet, rate gain 1/256 per packet: overdamped, ~0.3 s.
static inline int ua_rate_next(ua_core_t *c, int32_t avail) {
	c->lvl_q8 += ((avail << 8) - c->lvl_q8) >> UA_LEVEL_SHIFT;
	int32_t corr = c->lvl_q8 - (UA_TARGET_FRAMES << 8);   // Q8 frames == Q16 frames/packet / 256
	if (corr > UA_CORR_MAX) corr = UA_CORR_MAX;
	if (corr < -UA_CORR_MAX) corr = -UA_CORR_MAX;
	c->acc_q16 += (UA_FRAMES_NOMINAL << 16) + corr;
	const int n = c->acc_q16 >> 16;
	c->acc_q16 -= n << 16;
	return n;
}

// --- writer (synth thread) --------------------------------------------------------------------

// where the next UA_BLOCK frames go (contiguous: w is always a multiple of UA_BLOCK)
static inline int32_t *ua_write_ptr(ua_core_t *c) {
	return &c->ring[(c->w & UA_RING_MASK) * UA_CHANNELS];
}

// test tone, -6 dBFS: sin(2 pi k / 96) * 2^22, left-justified (<< 8)
static const int32_t ua_sine96[96] = {
	0, 70226176, 140151552, 209476608, 277904896, 345143040, 410903296, 474903808, 536870912,
	596538880, 653652480, 707967232, 759250176, 807281920, 851856640, 892783616, 929887744, 963009792,
	992008192, 1016758528, 1037155072, 1053110272, 1064555776, 1071442944, 1073741824, 1071442944,
	1064555776, 1053110272, 1037155072, 1016758528, 992008192, 963009792, 929887744, 892783616,
	851856640, 807281920, 759250176, 707967232, 653652480, 596538880, 536870912, 474903808, 410903296,
	345143040, 277904896, 209476608, 140151552, 70226176, 0, -70226176, -140151552, -209476608,
	-277904896, -345143040, -410903296, -474903808, -536870912, -596538880, -653652480, -707967232,
	-759250176, -807281920, -851856640, -892783616, -929887744, -963009792, -992008192, -1016758528,
	-1037155072, -1053110272, -1064555776, -1071442944, -1073741824, -1071442944, -1064555776,
	-1053110272, -1037155072, -1016758528, -992008192, -963009792, -929887744, -892783616, -851856640,
	-807281920, -759250176, -707967232, -653652480, -596538880, -536870912, -474903808, -410903296,
	-345143040, -277904896, -209476608, -140151552, -70226176};

// Publish the block written at ua_write_ptr(). With tone != 0 the block is replaced by the test
// tone: left 1 kHz, right 500 Hz (exactly 48 and 96 frames per period), phase continuous, so
// any lost or repeated sample on the way to the host shows as a click in a pure tone.
static inline void ua_write_commit(ua_core_t *c, int tone) {
	if (tone) {
		int32_t *p = ua_write_ptr(c);
		uint32_t ph = c->tone_ph;
		for (int i = 0; i < UA_BLOCK; i++) {
			p[2 * i] = ua_sine96[(2 * ph) % 96];
			p[2 * i + 1] = ua_sine96[ph];
			ph = ph == 95 ? 0 : ph + 1;
		}
		c->tone_ph = ph;
	}
	__atomic_signal_fence(__ATOMIC_RELEASE);  // samples before the index (single core: compiler only)
	c->w = c->w + UA_BLOCK;
}

// --- reader (USB ISR) -------------------------------------------------------------------------

// (re)start a stream: wait for the writer to fill the ring to the target, sending silence
static inline void ua_start(ua_core_t *c) {
	c->r = c->w;
	c->priming = 1;
	c->primes++;
	ua_rate_reset(c);
}

static inline void ua_stats_reset(ua_core_t *c) {
	c->packets = c->frames = c->underruns = c->overruns = c->primes = 0;
	c->sizes[0] = c->sizes[1] = c->sizes[2] = 0;
	c->lvl_min = 0x7FFFFFFF;
	c->lvl_max = -1;
}

static inline uint8_t *ua_pack(uint8_t *p, const int32_t *ring, uint32_t r, int frames, int bps) {
	if (bps == 3) {
		for (int i = 0; i < frames; i++, r++) {
			const int32_t *s = &ring[(r & UA_RING_MASK) * UA_CHANNELS];
			const uint32_t a = (uint32_t)s[0], b = (uint32_t)s[1];
			p[0] = (uint8_t)(a >> 8); p[1] = (uint8_t)(a >> 16); p[2] = (uint8_t)(a >> 24);
			p[3] = (uint8_t)(b >> 8); p[4] = (uint8_t)(b >> 16); p[5] = (uint8_t)(b >> 24);
			p += 6;
		}
	} else {
		for (int i = 0; i < frames; i++, r++) {
			const int32_t *s = &ring[(r & UA_RING_MASK) * UA_CHANNELS];
			const uint32_t a = (uint32_t)s[0], b = (uint32_t)s[1];
			p[0] = (uint8_t)(a >> 16); p[1] = (uint8_t)(a >> 24);
			p[2] = (uint8_t)(b >> 16); p[3] = (uint8_t)(b >> 24);
			p += 4;
		}
	}
	return p;
}

// Build the next packet (bps: bytes per sample, 3 or 2) into pkt (>= UA_PACKET_MAX bytes) and
// return its size in bytes. Never blocks; on underrun the missing frames are silence and the
// stream re-primes; on overrun the read position jumps back to the target level.
static inline size_t ua_fill(ua_core_t *c, uint8_t *pkt, int bps) {
	const uint32_t w = c->w;
	uint32_t avail = w - c->r;
	c->packets++;
	if (avail > UA_OVERRUN_LEVEL) {
		c->overruns++;
		c->r = w - UA_TARGET_FRAMES;
		avail = UA_TARGET_FRAMES;
		ua_rate_reset(c);
	}
	if (c->priming) {
		if (avail < UA_TARGET_FRAMES) {
			const size_t bytes = (size_t)UA_FRAMES_NOMINAL * UA_CHANNELS * bps;
			for (size_t i = 0; i < bytes; i++) pkt[i] = 0;
			return bytes;
		}
		c->priming = 0;
		c->r = w - UA_TARGET_FRAMES;
		avail = UA_TARGET_FRAMES;
		ua_rate_reset(c);
	}
	if ((int32_t)avail < c->lvl_min) c->lvl_min = (int32_t)avail;
	if ((int32_t)avail > c->lvl_max) c->lvl_max = (int32_t)avail;
	const int n = ua_rate_next(c, (int32_t)avail);
	c->sizes[n - UA_FRAMES_MIN]++;
	int take = n;
	if (avail < (uint32_t)n) {
		take = (int)avail;
		c->underruns++;
		c->priming = 1;
		c->primes++;
	}
	uint8_t *p = ua_pack(pkt, c->ring, c->r, take, bps);
	for (uint8_t *end = pkt + (size_t)n * UA_CHANNELS * bps; p < end; p++) *p = 0;
	c->r = c->r + (uint32_t)take;
	c->frames += (uint32_t)take;
	return (size_t)n * UA_CHANNELS * bps;
}

#endif /* USB_AUDIO_CORE_H */
