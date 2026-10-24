/**
 * USB audio output: the hardware independent part (ring buffer, packet sizing, sample packing).
 * Plain C with no ChibiOS dependency, so the host unit test (tools/usbaudio/test_core.c) runs
 * exactly this code.
 *
 * Data path: the synth thread writes each 32-frame stereo block (the same int32 samples it gives
 * the codec: 24 bit, left-justified) into a ring, always, streaming or not; the USB ISR takes one
 * packet per 1 ms USB frame.
 *
 * Asynchronous source: the codec clock (exactly 48 kHz from the board's crystal) and the host's
 * USB frame clock are independent, so the number of frames per packet must follow the codec clock.
 * The ring level is the integral of the rate difference: a slow servo on the (low-passed) level
 * sets a fractional frames-per-packet rate around 48, and an accumulator turns it into a 47/48/49
 * sequence. The level stays bounded, so there is no drift however long the stream runs.
 *
 * Latency: the level the servo keeps before each packet (the target, runtime tunable) is the
 * ring's share of the device latency. It must cover the packet itself (up to 49 frames) plus the
 * level's ripple: the writer adds 32 frames at a time, the reader takes 48 per ms, and the synth's
 * render time moves each commit by up to almost a block (tools/usbaudio/test_core.c measures the
 * margin for each target).
 *
 * Fast start: the ring always holds the synth's most recent output, so a stream starts with the
 * read position one target behind the writer and its first packet is already audio (no priming
 * silence). Priming (silence until the ring holds a target again) remains for underruns.
 *
 * Concurrency: one writer (synth thread) owns w, one reader (USB ISR, which preempts the writer
 * on this single core) owns r. The writer never blocks and never looks at r; the reader only
 * reads committed frames, and detects an overrun (writer close to a ring ahead) and re-centres.
 * ua_set_target() runs in a thread with the system locked (the reader cannot run meanwhile).
 */
#ifndef USB_AUDIO_CORE_H
#define USB_AUDIO_CORE_H

#include <stdint.h>
#include <stddef.h>

#define UA_RATE 48000
#define UA_CHANNELS 2
#define UA_FRAMES_NOMINAL (UA_RATE / 1000)    // 48 frames per 1 ms USB frame
#define UA_FRAMES_MIN (UA_FRAMES_NOMINAL - 1) // 47
#define UA_FRAMES_MAX (UA_FRAMES_NOMINAL + 1) // 49 -> wMaxPacketSize 294 (24 bit)
#define UA_BLOCK 32                           // synth block (frames)
#define UA_RING_FRAMES 512                    // 10.7 ms, power of 2, multiple of UA_BLOCK
#define UA_RING_MASK (UA_RING_FRAMES - 1)
// Level kept before taking a packet (frames). Default: 2 ms, the smallest target with which the
// simulation (test_core.c) never underruns with the synth's render time anywhere in its block
// and the packet builds' timing jitter up to 0.25 ms; see the 'Stct' console command to try
// other values on the board.
#define UA_TARGET_DEFAULT 96
#define UA_TARGET_MIN UA_FRAMES_MAX        // a packet's worth: underruns at any jitter
#define UA_TARGET_MAX (UA_RING_FRAMES / 2) // 5.3 ms
// A (re)centred stream starts this much above the target: the level before a packet ripples by
// about +-30 frames with the phase of the synth blocks against the USB frames, so a start at
// exactly the target on a ripple peak would leave the mean up to 30 frames low until the servo
// catches up (the simulation's stream restart test underruns without it, and comes within one
// frame of it with half a block)
#define UA_START_HEADROOM UA_BLOCK
#define UA_LEVEL_SHIFT 5   // level low-pass: 2^5 = 32 packets (32 ms)
#define UA_CORR_MAX 0x4000 // rate correction limit: +-0.25 frame/packet
#define UA_OVERRUN_LEVEL (UA_RING_FRAMES - 2 * UA_BLOCK)
#define UA_PACKET_MAX (UA_FRAMES_MAX * UA_CHANNELS * 3) // 294 bytes

#if (UA_RING_FRAMES % UA_BLOCK) != 0 || (UA_RING_FRAMES & UA_RING_MASK) != 0
#error "ring size must be a power of 2 and a multiple of the synth block"
#endif

typedef struct {
  int32_t ring[UA_RING_FRAMES * UA_CHANNELS]; // interleaved L R, sample << 8 (24 bit)
  volatile uint32_t w;                        // frames written, free running (synth thread)
  volatile uint32_t r;                        // frames read, free running (USB ISR)
  int32_t target;                             // level kept before a packet (frames)
  int32_t lvl_q8;                             // low-passed level, frames * 256
  int32_t acc_q16;                            // packet size accumulator, frames * 65536
  uint8_t priming;                            // sending silence until the ring holds a target
  uint8_t recentre;                           // target changed: re-centre at the next packet
  uint32_t tone_ph;                           // test tone phase (frames, mod 96)
  // writer side: the codec's lead over the synth (blocks), its known part, blocks never rendered
  int32_t lag_perm;
  uint8_t lag_valid;
  uint32_t lost, repeats;
  // statistics (reader side)
  uint32_t packets, frames, underruns, overruns, primes, fast_starts, silent;
  uint32_t sizes[3];        // packets of 47, 48, 49 frames
  int32_t lvl_min, lvl_max; // raw level seen before a packet, since the last stats reset
} ua_core_t;

// --- packet sizing ----------------------------------------------------------------------------

static inline void ua_rate_reset(ua_core_t *c) {
  c->lvl_q8 = c->target << 8;
  c->acc_q16 = 0x8000;
}

// Frames for the next packet, given the ring level (frames available) before taking it.
// Rate = 48 + (filtered level - target) / 256 frames per packet, limited to 48 +- 0.25, so the
// result is always 47, 48 or 49 (a fraction in [0, 1) plus 47.75 .. 48.25 floors to one of them).
// Loop: level filter pole 1/32 per packet, rate gain 1/256 per packet: overdamped, ~0.3 s. A
// proportional loop leaves a level offset of 256 * 48 * ppm * 1e-6 frames: 0.25 frame at 20 ppm,
// 6 frames at the 500 ppm the USB spec allows the host's frame clock.
static inline int ua_rate_next(ua_core_t *c, int32_t avail) {
  c->lvl_q8 += ((avail << 8) - c->lvl_q8) >> UA_LEVEL_SHIFT;
  int32_t corr = c->lvl_q8 - (c->target << 8); // Q8 frames == Q16 frames/packet / 256
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
  __atomic_signal_fence(__ATOMIC_RELEASE); // samples before the index (single core: compiler only)
  c->w = c->w + UA_BLOCK;
}

#define UA_REPEAT_MAX 2 // blocks repeated for blocks the synth never rendered (more: a stall)

// Blocks the synth never rendered (overload: two codec interrupts before it waited again, the
// events merged): the codec repeated a stale buffer on the jack, and the ring got 32 frames less
// per block, which at a low target underruns (a gap of about the target in silence). Instead the
// last block is committed again, once per lost block (the jack's glitch, no more), up to
// UA_REPEAT_MAX; a longer stall underruns as before. lag: codec blocks minus blocks committed,
// not counting a block the synth is about to render (its wakeup event already pending).
static inline void ua_write_sync(ua_core_t *c, int32_t lag) {
  if (!c->lag_valid) {
    c->lag_valid = 1;
    c->lag_perm = lag;
    return;
  }
  if (lag <= c->lag_perm) {
    c->lag_perm = lag;
    return;
  }
  const int32_t n = lag - c->lag_perm;
  c->lag_perm = lag;
  c->lost += (uint32_t)n;
  for (int32_t k = 0; k < n && k < UA_REPEAT_MAX; k++) {
    const int32_t *src = &c->ring[((c->w - UA_BLOCK) & UA_RING_MASK) * UA_CHANNELS];
    int32_t *dst = ua_write_ptr(c);
    for (int i = 0; i < UA_BLOCK * UA_CHANNELS; i++) dst[i] = src[i];
    __atomic_signal_fence(__ATOMIC_RELEASE);
    c->w = c->w + UA_BLOCK;
    c->repeats++;
  }
}

// --- reader (USB ISR) -------------------------------------------------------------------------

static inline void ua_init(ua_core_t *c) {
  c->target = UA_TARGET_DEFAULT;
  ua_rate_reset(c);
}

// the level a (re)centred stream starts with
static inline uint32_t ua_start_level(const ua_core_t *c) {
  return (uint32_t)c->target + UA_START_HEADROOM;
}

// (re)start a stream. The ring holds the synth's latest output (the writer always writes), so
// the read position goes one target (plus the headroom) behind the writer and the first packet is
// audio. Only right after boot (fewer frames ever written) it primes: silence until the ring fills.
static inline void ua_start(ua_core_t *c) {
  const uint32_t w = c->w;
  c->recentre = 0;
  if (w >= ua_start_level(c)) {
    c->r = w - ua_start_level(c);
    c->priming = 0;
    c->fast_starts++;
  } else {
    c->r = w;
    c->priming = 1;
    c->primes++;
  }
  ua_rate_reset(c);
}

// change the target (thread, system locked); the reader re-centres at its next packet
static inline int32_t ua_set_target(ua_core_t *c, int32_t frames) {
  if (frames < UA_TARGET_MIN) frames = UA_TARGET_MIN;
  if (frames > UA_TARGET_MAX) frames = UA_TARGET_MAX;
  c->target = frames;
  c->recentre = 1;
  return frames;
}

static inline void ua_stats_reset(ua_core_t *c) {
  c->packets = c->frames = c->underruns = c->overruns = c->primes = c->fast_starts = c->silent = 0;
  c->sizes[0] = c->sizes[1] = c->sizes[2] = 0;
  c->lvl_min = 0x7FFFFFFF;
  c->lvl_max = -1;
}

static inline uint8_t *ua_pack(uint8_t *p, const int32_t *ring, uint32_t r, int frames, int bps) {
  if (bps == 3) {
    for (int i = 0; i < frames; i++, r++) {
      const int32_t *s = &ring[(r & UA_RING_MASK) * UA_CHANNELS];
      const uint32_t a = (uint32_t)s[0], b = (uint32_t)s[1];
      p[0] = (uint8_t)(a >> 8);
      p[1] = (uint8_t)(a >> 16);
      p[2] = (uint8_t)(a >> 24);
      p[3] = (uint8_t)(b >> 8);
      p[4] = (uint8_t)(b >> 16);
      p[5] = (uint8_t)(b >> 24);
      p += 6;
    }
  } else {
    for (int i = 0; i < frames; i++, r++) {
      const int32_t *s = &ring[(r & UA_RING_MASK) * UA_CHANNELS];
      const uint32_t a = (uint32_t)s[0], b = (uint32_t)s[1];
      p[0] = (uint8_t)(a >> 16);
      p[1] = (uint8_t)(a >> 24);
      p[2] = (uint8_t)(b >> 16);
      p[3] = (uint8_t)(b >> 24);
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
  const uint32_t start = ua_start_level(c);
  uint32_t avail = w - c->r;
  c->packets++;
  if (c->recentre) { // new target: skip ahead to it, or prime up to it
    c->recentre = 0;
    if (avail >= start) {
      c->r = w - start;
      avail = start;
    } else {
      c->priming = 1;
      c->primes++;
    }
    ua_rate_reset(c);
  }
  if (avail > UA_OVERRUN_LEVEL) {
    c->overruns++;
    c->r = w - start;
    avail = start;
    ua_rate_reset(c);
  }
  if (c->priming) {
    if (avail < start) {
      const size_t bytes = (size_t)UA_FRAMES_NOMINAL * UA_CHANNELS * bps;
      for (size_t i = 0; i < bytes; i++) pkt[i] = 0;
      c->silent++;
      return bytes;
    }
    c->priming = 0;
    c->r = w - start;
    avail = start;
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

// --- latency marker (console 'Stcl1') ----------------------------------------------------------
//
// A host measures the latency of the USB audio path electronically: the console asks for a
// marker, the USB ISR writes it over the first UA_MARK_FRAMES frames of the next packet it
// builds (the right channel only; the left keeps the synth), and the board reports which frame
// the packet went out in and how long after the command arrived. The marker is a 32-chip
// pseudo-random sequence (a 5-bit maximal-length LFSR, x^5 + x^3 + 1, plus one chip) at
// -20 dBFS: its aperiodic autocorrelation sidelobes are at most 5/32, so a host finds it by
// correlation in any audio, or by its exact values in a bit-exact capture. It goes into the
// packed packet only: the ring, and so the jack, never see it.
#define UA_MARK_FRAMES 32
#define UA_MARK_AMP24 838861 // 0.1 * 2^23: -20 dBFS
#define UA_MARK_AMP16 3277   // 0.1 * 2^15
static const int8_t ua_mark_pn[UA_MARK_FRAMES] = {1, -1, -1, -1, -1, 1, -1, -1, 1, -1, 1, 1, -1, -1, 1, 1,
    1, 1, 1, -1, -1, -1, 1, 1, -1, 1, 1, 1, -1, 1, -1, 1};

// Overwrite the right channel of the first frames of a packed packet (frames: frames in it; bps:
// 3 or 2 bytes per sample) with the marker. Returns the frames marked.
static inline int ua_mark_packet(uint8_t *pkt, int frames, int bps) {
  const int n = frames < UA_MARK_FRAMES ? frames : UA_MARK_FRAMES;
  for (int i = 0; i < n; i++) {
    uint8_t *s = pkt + (size_t)(2 * i + 1) * (size_t)bps; // right channel of frame i
    if (bps == 3) {
      const uint32_t v = (uint32_t)(ua_mark_pn[i] * UA_MARK_AMP24);
      s[0] = (uint8_t)v;
      s[1] = (uint8_t)(v >> 8);
      s[2] = (uint8_t)(v >> 16);
    } else {
      const uint32_t v = (uint32_t)(ua_mark_pn[i] * UA_MARK_AMP16);
      s[0] = (uint8_t)v;
      s[1] = (uint8_t)(v >> 8);
    }
  }
  return n;
}

#endif /* USB_AUDIO_CORE_H */
