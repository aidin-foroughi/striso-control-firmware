// Host unit test of usb_audio_core.h (the exact firmware code): packet sizing (47/48/49), drift
// tracking, ring under/overrun handling, sample integrity and packing, test tone, fast start,
// live target changes, and the latency/margin of each ring target.
// Run: tools/usbaudio/run_tests.sh   (or ctest --preset host -R usb_audio_core, in Docker)
//
// Simulation: the synth commits a 32-frame block once per block period of the device's audio
// clock (48 kHz * (1 + ppm)), each at a random render delay after its DMA interrupt (a fraction of
// the period, uniform in [dmin, dmax]). The USB ISR builds one packet per host USB frame (1 ms of
// the host clock), each at a random delay in [0, isr_jitter] after the host fetched the previous
// one; the packet goes on the bus at the next fetch, 1 ms after it was built. The host may skip
// frames (no fetch: the firmware resends the same packet in the next frame, so the next build is a
// frame later). Each frame written carries its index, so the test checks that the host receives
// every frame exactly once, in order, with no silence (except around the planned faults), and
// measures each frame's latency from its commit (the synth computed it) to the bus.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "usb_audio_core.h"

static ua_core_t c;
static int failures;
#define CHECK(cond, ...)                                                                           \
	do {                                                                                           \
		if (!(cond)) {                                                                             \
			failures++;                                                                            \
			printf("  FAIL: " __VA_ARGS__);                                                        \
			printf("\n");                                                                          \
		}                                                                                          \
	} while (0)

static uint32_t rng = 12345;
static double urand(void) {
	rng = rng * 1664525u + 1013904223u;
	return (rng >> 8) * (1.0 / 16777216.0);
}

// frame f: left carries the index, right a different pattern (never both zero)
static int32_t sample_l(uint32_t f) { return (int32_t)((f & 0xFFFFFF) << 8); }
static int32_t sample_r(uint32_t f) { return (int32_t)(((f ^ 0x5A5A5A) & 0xFFFFFF) << 8); }

typedef struct {
	double ppm; // device audio clock vs host USB clock
	double seconds;
	int bps;                    // 3 or 2
	int target;                 // ring target (0: UA_TARGET_DEFAULT)
	double dmin, dmax;          // synth render delay after its DMA interrupt, fraction of a block
	double isr_jitter;          // packet build delay after the host's fetch, 0 .. this (s)
	double skip_p;              // probability that the host skips a frame (no fetch)
	double burst_p;             // probability of a burst of 2..5 skipped frames
	double stall_at, stall_len; // synth produces nothing for stall_len s (0: none)
	double pause_at, pause_len; // host takes no packets for pause_len s (0: none)
	double lose_p;              // probability that the synth misses a block (overload)
	double retarget_at;         // change the target to retarget_to at this time (0: none)
	int retarget_to;
	int faults_max;  // underruns + overruns allowed (0: a clean run)
	int gaps_max;    // gaps in the frame sequence allowed
	int silent_ok;   // silence after the start allowed (priming after a fault)
	double prefill;  // the synth runs this long before the stream starts (s)
	double restart;  // the host restarts the stream (alt 0, alt 1) this often (s; 0: never)
	int quiet;       // no line printed (sweeps)
} sim_t;

typedef struct {
	unsigned underruns, overruns;
	long gaps, silent;
	int32_t lvl_lo, lvl_hi;
	double lat_avg, lat_max, rate_err_ppm;
} result_t;

#define TBLOCKS 4096 // commit times of the last blocks (power of 2)
static double t_commit[TBLOCKS];

static result_t run(const char *name, sim_t s) {
	memset(&c, 0, sizeof c);
	ua_init(&c);
	ua_stats_reset(&c);
	if (s.target) ua_set_target(&c, s.target);
	if (s.dmax == 0) s.dmin = 0.05, s.dmax = 0.9;
	const double tb = UA_BLOCK / (48000.0 * (1.0 + s.ppm * 1e-6)); // block period
	const double th = 1e-3;                                          // host frame
	uint32_t wframe = 0;           // next frame index the synth writes (its timeline)
	uint32_t dmas = 0, commits = 0; // codec DMA interrupts, blocks committed (ua_write_sync)
	double next_dma = tb;
	double next_commit = next_dma + (s.dmin + (s.dmax - s.dmin) * urand()) * tb;
	double t_fetch = s.prefill + 0.37e-3; // host fetch times: arbitrary phase
	uint32_t expect = 0;
	int have_expect = 0, started = 0, retargeted = 0;
	long hist[3] = {0, 0, 0}, steady_packets = 0, steady_frames = 0;
	long silent = 0, gaps = 0, lat_n = 0;
	double lat_sum = 0, lat_max = 0;
	int32_t lvl_lo = 1 << 30, lvl_hi = -1;
	uint8_t pkt[UA_PACKET_MAX + 8];
	int burst = 0, skipping = 0, restarts = 0;
	double next_restart = s.prefill + s.restart;
	long skip_runs = 0;
	const long npk = (long)(s.seconds / th);
	for (long k = 0; k < npk; k++, t_fetch += th) {
		const double t_build = t_fetch + s.isr_jitter * urand();
		// writer: every block committed before this packet is built
		while (next_commit <= t_build) {
			const double tc = next_commit;
			const int stalled = s.stall_len > 0 && tc >= s.stall_at && tc < s.stall_at + s.stall_len;
			const int lost = s.lose_p > 0 && urand() < s.lose_p;
			if (!stalled && !lost) {
				int32_t *p = ua_write_ptr(&c);
				for (int i = 0; i < UA_BLOCK; i++) {
					p[2 * i] = sample_l(wframe + i);
					p[2 * i + 1] = sample_r(wframe + i);
				}
				ua_write_commit(&c, 0);
				commits++;
				// the render ends before the next interrupt, so no wakeup is pending here
				ua_write_sync(&c, (int32_t)(dmas + 1 - commits));
				t_commit[(wframe / UA_BLOCK) & (TBLOCKS - 1)] = tc;
				wframe += UA_BLOCK;
			} else if (stalled) {
				wframe += UA_BLOCK; // the synth's timeline advances while it is stalled (a gap)
			} // a lost block (overload): the synth renders one block for two DMA periods, no gap
			dmas++;
			next_dma += tb;
			next_commit = next_dma + (s.dmin + (s.dmax - s.dmin) * urand()) * tb;
		}
		if (t_build < s.prefill) continue;
		if (!started) {
			ua_start(&c);
			started = 1;
		}
		if (s.restart > 0 && t_build >= next_restart) {
			ua_start(&c);
			next_restart += s.restart * (0.5 + urand());
			restarts++;
			have_expect = 0; // a new stream (the frames skipped meanwhile are not a gap)
		}
		if (s.retarget_at > 0 && !retargeted && t_build >= s.retarget_at) {
			ua_set_target(&c, s.retarget_to);
			retargeted = 1;
		}
		// the host does not fetch this frame's packet: the firmware drops it and builds the next
		// one (real time: the stream stays aligned, the host's missing frame is lost)
		int skip = s.pause_len > 0 && t_build >= s.pause_at && t_build < s.pause_at + s.pause_len;
		if (burst > 0) {
			burst--;
			skip = 1;
		} else if (s.burst_p > 0 && urand() < s.burst_p) {
			burst = 1 + (int)(4 * urand()); // this frame and 1..4 more
			skip = 1;
		} else if (s.skip_p > 0 && urand() < s.skip_p) {
			skip = 1;
		}
		if (skip) {
			ua_fill(&c, pkt, s.bps);
			if (!skipping) skip_runs++;
			skipping = 1;
			continue;
		}
		skipping = 0;
		const uint32_t avail = c.w - c.r;
		const int was_priming = c.priming;
		const size_t bytes = ua_fill(&c, pkt, s.bps);
		const int frames = (int)(bytes / (2 * s.bps));
		CHECK(bytes % (2 * s.bps) == 0, "%s: packet of %zu bytes", name, bytes);
		CHECK(bytes <= (size_t)UA_FRAMES_MAX * 2 * s.bps, "%s: packet too big: %zu", name, bytes);
		CHECK(frames >= UA_FRAMES_MIN && frames <= UA_FRAMES_MAX, "%s: %d frames", name, frames);
		if (frames < UA_FRAMES_MIN || frames > UA_FRAMES_MAX) break;
		hist[frames - UA_FRAMES_MIN]++;
		for (int i = 0; i < frames; i++) {
			const uint8_t *q = pkt + i * 2 * s.bps;
			uint32_t l, r;
			if (s.bps == 3) {
				l = (uint32_t)q[0] << 8 | (uint32_t)q[1] << 16 | (uint32_t)q[2] << 24;
				r = (uint32_t)q[3] << 8 | (uint32_t)q[4] << 16 | (uint32_t)q[5] << 24;
			} else {
				l = (uint32_t)q[0] << 16 | (uint32_t)q[1] << 24;
				r = (uint32_t)q[2] << 16 | (uint32_t)q[3] << 24;
			}
			if (l == 0 && r == 0) { // silence: priming or underrun padding
				silent++;
				continue;
			}
			if (s.bps == 3) {
				const uint32_t f = l >> 8;
				CHECK(r == (uint32_t)sample_r(f), "%s: right channel does not match left", name);
				if (have_expect && f != (expect & 0xFFFFFF)) gaps++;
				expect = f + 1;
				have_expect = 1;
				const double lat = t_fetch + th - t_commit[(f / UA_BLOCK) & (TBLOCKS - 1)];
				if (t_build > 5.0) {
					lat_sum += lat;
					lat_n++;
					if (lat > lat_max) lat_max = lat;
				}
			} else {
				// 16 bit keeps the top 16 bits of the index: check it never jumps
				const uint32_t f = l >> 16;
				if (have_expect && f != expect && f != ((expect + 1) & 0xFFFF)) gaps++;
				expect = f;
				have_expect = 1;
			}
		}
		if (t_build > s.prefill + 5.0 && !was_priming && !c.priming) { // steady, servo settled
			steady_packets++;
			steady_frames += frames;
			if ((int32_t)avail < lvl_lo) lvl_lo = (int32_t)avail;
			if ((int32_t)avail > lvl_hi) lvl_hi = (int32_t)avail;
		}
	}
	const double rate = steady_packets ? 1000.0 * steady_frames / steady_packets : 0;
	const double want = 48000.0 * (1.0 + s.ppm * 1e-6);
	result_t res = {c.underruns, c.overruns, gaps, silent, lvl_lo, lvl_hi,
	    lat_n ? lat_sum / lat_n : 0, lat_max, (rate / want - 1) * 1e6};
	const unsigned faults = c.underruns + c.overruns;
	if (!s.quiet)
		printf("%-34s %.3f Hz (%+.2f ppm) 47:%-6ld 48:%-6ld 49:%-6ld level %3d..%3d under %u over %u "
		       "gaps %ld (host skips %ld) silent %ld latency %.2f/%.2f ms\n",
		    name, rate, res.rate_err_ppm, hist[0], hist[1], hist[2], lvl_lo, lvl_hi, c.underruns,
		    c.overruns, gaps, skip_runs, silent, res.lat_avg * 1e3, res.lat_max * 1e3);
	if (s.quiet) return res;
	CHECK((int)faults <= s.faults_max, "%s: %u underruns/overruns (allowed %d)", name, faults, s.faults_max);
	CHECK(gaps <= s.gaps_max + skip_runs, "%s: %ld gaps in the frame sequence (allowed %d + %ld skipped by the host)", name,
	    gaps, s.gaps_max, skip_runs);
	if (!s.silent_ok) CHECK(silent == 0, "%s: %ld silent frames (fast start: none expected)", name, silent);
	CHECK(c.priming == 0, "%s: still priming at the end", name);
	if (s.restart > 0) {
		printf("  (%d stream starts)\n", restarts);
		CHECK(lvl_lo >= UA_FRAMES_MAX + 4, "%s: lowest level %d, margin < 4 frames", name, lvl_lo);
	}
	if (s.faults_max == 0 && s.gaps_max == 0 && s.restart == 0) { // (host skips leave the ring alone)
		CHECK(fabs(res.rate_err_ppm) < 2, "%s: rate error %.2f ppm", name, res.rate_err_ppm);
		CHECK(lvl_lo >= UA_FRAMES_MAX && lvl_hi <= UA_OVERRUN_LEVEL, "%s: level %d..%d", name, lvl_lo, lvl_hi);
	} else if (s.restart == 0 && s.lose_p == 0) { // (lost blocks really lower the rate)
		CHECK(fabs(res.rate_err_ppm) < 50, "%s: rate error %.2f ppm after recovery", name, res.rate_err_ppm);
	}
	return res;
}

// the 47/48/49 accumulator alone: a constant level offset gives a constant rate offset
static void test_accumulator(void) {
	memset(&c, 0, sizeof c);
	ua_init(&c);
	const int32_t t = c.target;
	long sum = 0, n = 0, h[3] = {0, 0, 0};
	int max_run48 = 0, run48 = 0;
	// level 26 frames above target -> +26/256 frame/packet once the level filter has settled
	for (int k = 0; k < 100000; k++) {
		const int f = ua_rate_next(&c, t + 26);
		CHECK(f >= 47 && f <= 49, "accumulator gave %d", f);
		if (k >= 1000) {
			sum += f;
			n++;
			h[f - 47]++;
			if (f == 49) run48 = 0;
			else if (++run48 > max_run48) max_run48 = run48;
		}
	}
	const double mean = (double)sum / n;
	printf("accumulator, level +26: mean %.5f frames/packet (expect %.5f) 47:%ld 48:%ld 49:%ld, "
	       "longest run of 48s %d (evenly spread: ~%d)\n",
	    mean, 48 + 26 / 256.0, h[0], h[1], h[2], max_run48, (int)(256 / 26));
	// the level filter's shift truncates: it settles up to 31/256 frame low, i.e. a rate offset of
	// at most 0.0005 frame/packet open loop (in the closed loop that is a level offset < 1 frame)
	CHECK(fabs(mean - (48 + 26 / 256.0)) < 1e-3, "accumulator mean %.5f", mean);
	CHECK(h[0] == 0, "no 47s expected above target");
	CHECK(max_run48 <= 10, "49s not evenly spread");
	// saturation: far above target -> +0.25 max; far below -> -0.25
	ua_rate_reset(&c);
	sum = 0;
	n = 0;
	for (int k = 0; k < 10000; k++) {
		int f = ua_rate_next(&c, 10000);
		if (k >= 1000) sum += f, n++;
	}
	printf("accumulator, level far above target: mean %.4f (limit 48.25)\n", (double)sum / n);
	CHECK(fabs((double)sum / n - 48.25) < 1e-3, "saturation high");
	ua_rate_reset(&c);
	sum = 0;
	n = 0;
	for (int k = 0; k < 10000; k++) {
		int f = ua_rate_next(&c, 0);
		if (k >= 1000) sum += f, n++;
	}
	printf("accumulator, level zero: mean %.4f (limit 47.75)\n", (double)sum / n);
	CHECK(fabs((double)sum / n - 47.75) < 1e-3, "saturation low");
	// exactly at target: always 48
	ua_rate_reset(&c);
	int bad = 0;
	for (int k = 0; k < 10000; k++)
		if (ua_rate_next(&c, t) != 48) bad++;
	CHECK(bad == 0, "at target: %d packets not 48", bad);
}

static void test_tone(void) {
	memset(&c, 0, sizeof c);
	double err = 0;
	for (int b = 0; b < 30; b++) {
		int32_t *p = ua_write_ptr(&c);
		const uint32_t base = c.w;
		ua_write_commit(&c, 1);
		for (int i = 0; i < UA_BLOCK; i++) {
			const double t = (base + i) / 48000.0;
			const double l = 0.5 * sin(2 * M_PI * 1000 * t), r = 0.5 * sin(2 * M_PI * 500 * t);
			const double el = fabs(p[2 * i] / 2147483648.0 - l), er = fabs(p[2 * i + 1] / 2147483648.0 - r);
			if (el > err) err = el;
			if (er > err) err = er;
		}
	}
	printf("test tone: max error vs sin %.2e over 30 blocks (phase continuous across blocks)\n", err);
	CHECK(err < 1e-6, "tone error %g", err);
}

// 24 and 16 bit packing of known values, little endian, signed
static void test_pack(void) {
	memset(&c, 0, sizeof c);
	int32_t *p = ua_write_ptr(&c);
	const int32_t v[4] = {0x123456 << 8, -(0x123456 << 8), (int32_t)0x7FFFFF00, (int32_t)0x80000000};
	for (int i = 0; i < UA_BLOCK; i++) {
		p[2 * i] = v[i % 4];
		p[2 * i + 1] = v[(i + 1) % 4];
	}
	ua_write_commit(&c, 0);
	uint8_t b[64];
	ua_pack(b, c.ring, 0, 2, 3);
	const uint8_t e3[12] = {0x56, 0x34, 0x12, 0xAA, 0xCB, 0xED, 0xAA, 0xCB, 0xED, 0xFF, 0xFF, 0x7F};
	CHECK(memcmp(b, e3, 12) == 0, "24 bit packing");
	ua_pack(b, c.ring, 0, 2, 2);
	const uint8_t e2[8] = {0x34, 0x12, 0xCB, 0xED, 0xCB, 0xED, 0xFF, 0x7F};
	CHECK(memcmp(b, e2, 8) == 0, "16 bit packing");
	printf("packing: 24 bit and 16 bit little endian checked\n");
}

// target and setting bounds; start right after boot (ring not yet filled) primes once
static void test_target_bounds(void) {
	memset(&c, 0, sizeof c);
	ua_init(&c);
	CHECK(c.target == UA_TARGET_DEFAULT, "default target");
	CHECK(ua_set_target(&c, 1) == UA_TARGET_MIN, "target clamps low");
	CHECK(ua_set_target(&c, 100000) == UA_TARGET_MAX, "target clamps high");
	CHECK(UA_TARGET_MAX + 2 * UA_BLOCK + UA_FRAMES_MAX < UA_OVERRUN_LEVEL, "headroom above the largest target");
	ua_init(&c);
	ua_start(&c); // w == 0: nothing written yet
	CHECK(c.priming == 1 && c.primes == 1, "start before the ring has a target's worth primes");
	memset(&c, 0, sizeof c);
	ua_init(&c);
	c.w = 10 * UA_BLOCK;
	ua_start(&c);
	CHECK(c.priming == 0 && c.r == c.w - UA_TARGET_DEFAULT - UA_START_HEADROOM, "fast start one target behind the writer");
	printf("target bounds %d..%d frames, default %d; fast start checked\n", UA_TARGET_MIN, UA_TARGET_MAX,
	    UA_TARGET_DEFAULT);
}

// The margin of each target: the lowest level seen before a packet, minus the packet (49), over
// 2 minutes with the phase between the codec blocks and the USB frames sweeping (37 ppm), for
// three render/ISR timing models. Picks the default: no underrun in the worst model.
static void target_sweep(void) {
	static const struct {
		const char *name;
		double dmin, dmax, isr;
	} models[3] = {
	    {"light synth (render 10..20%), ISR jitter 50 us", 0.10, 0.20, 50e-6},
	    {"any render time (5..95%), ISR jitter 100 us", 0.05, 0.95, 100e-6},
	    {"any render time (5..95%), ISR jitter 250 us", 0.05, 0.95, 250e-6},
	};
	printf("target sweep: margin = lowest level before a packet - 49 frames (underruns if < 0); "
	       "latency = commit to bus, avg/max ms\n");
	printf("  target  ");
	for (int m = 0; m < 3; m++) printf("| %-48s", models[m].name);
	printf("\n");
	for (int t = 56; t <= 144; t += 8) {
		printf("  %3d %4.2fms", t, t / 48.0);
		for (int m = 0; m < 3; m++) {
			sim_t s = {.ppm = 37, .seconds = 120, .bps = 3, .target = t, .dmin = models[m].dmin, .dmax = models[m].dmax, .isr_jitter = models[m].isr};
			s.quiet = 1;
			s.prefill = 0.05;
			const result_t r = run("sweep", s);
			printf("| margin %4d under %-3u lat %.2f/%.2f               ", r.lvl_lo - UA_FRAMES_MAX,
			    r.underruns, r.lat_avg * 1e3, r.lat_max * 1e3);
			if (t == UA_TARGET_DEFAULT && m == 2)
				CHECK(r.underruns == 0 && r.lvl_lo - UA_FRAMES_MAX >= 4,
				    "default target %d: margin %d, %u underruns in the worst timing model", t,
				    r.lvl_lo - UA_FRAMES_MAX, r.underruns);
		}
		printf("\n");
	}
}

int main(void) {
	test_accumulator();
	test_tone();
	test_pack();
	test_target_bounds();
	target_sweep();
	const double ppms[] = {0, 20, -20, 100, -100, 500, -500, 2000, -2000};
	for (unsigned i = 0; i < sizeof ppms / sizeof ppms[0]; i++) {
		char name[64];
		snprintf(name, sizeof name, "24 bit %+5g ppm, 10 min", ppms[i]);
		// +-2000 ppm (4x the USB limit) leaves a 25 frame level offset: that one gets 144
		run(name, (sim_t){.ppm = ppms[i], .seconds = 600, .bps = 3, .target = fabs(ppms[i]) > 500 ? 144 : 0, .dmin = 0, .dmax = 0, .isr_jitter = 100e-6, .prefill = 0.05});
	}
	run("16 bit   +50 ppm, 60 s", (sim_t){.ppm = 50, .seconds = 60, .bps = 2, .target = 0, .dmin = 0, .dmax = 0, .isr_jitter = 100e-6, .prefill = 0.05});
	run("restarts every 0.15..0.45 s, 5 min",
	    (sim_t){.ppm = 20, .seconds = 300, .bps = 3, .dmin = 0.05, .dmax = 0.95, .isr_jitter = 250e-6, .prefill = 0.05, .restart = 0.3});
	run("start at boot (primes once)", (sim_t){.ppm = 10, .seconds = 30, .bps = 3, .target = 0, .dmin = 0, .dmax = 0, .isr_jitter = 100e-6, .silent_ok = 1});
	run("host skips 1% of frames", (sim_t){.ppm = 30, .seconds = 120, .bps = 3, .target = 0, .dmin = 0, .dmax = 0, .isr_jitter = 100e-6, .skip_p = 0.01, .prefill = 0.05});
	run("host skip bursts of 2..5 frames", (sim_t){.ppm = -30, .seconds = 120, .bps = 3, .target = 0, .dmin = 0, .dmax = 0, .isr_jitter = 100e-6, .skip_p = 0, .burst_p = 0.002, .prefill = 0.05});
	run("ISR jitter up to 0.5 ms, target 144", (sim_t){.ppm = 20, .seconds = 120, .bps = 3, .target = 144, .dmin = 0, .dmax = 0, .isr_jitter = 500e-6, .prefill = 0.05});
	run("synth stall 20 ms at 30 s",
	    (sim_t){.ppm = 30, .seconds = 60, .bps = 3, .target = 0, .dmin = 0, .dmax = 0, .isr_jitter = 100e-6, .stall_at = 30, .stall_len = 0.020, .faults_max = 2, .gaps_max = 4, .silent_ok = 1, .prefill = 0.05});
	run("host pause 50 ms at 30 s (no fault)",
	    (sim_t){.ppm = -30, .seconds = 60, .bps = 3, .target = 0, .dmin = 0, .dmax = 0, .isr_jitter = 100e-6, .pause_at = 30, .pause_len = 0.050, .prefill = 0.05});
	// a lost block is repeated (as the jack repeats its buffer) at the next commit: fewer underruns
	run("synth loses a block now and then",
	    (sim_t){.ppm = 20, .seconds = 60, .bps = 3, .target = 0, .dmin = 0, .dmax = 0, .isr_jitter = 100e-6, .lose_p = 1e-4, .faults_max = 5, .gaps_max = 40, .silent_ok = 1, .prefill = 0.05});
	CHECK(c.lost >= 3 && c.repeats == c.lost, "lost blocks %u, repeated %u", c.lost, c.repeats);
	run("target 144 -> 96 at 20 s (one skip)",
	    (sim_t){.ppm = 20, .seconds = 60, .bps = 3, .target = 144, .dmin = 0, .dmax = 0, .isr_jitter = 100e-6, .retarget_at = 20, .retarget_to = UA_TARGET_DEFAULT, .gaps_max = 1, .prefill = 0.05});
	run("target 96 -> 144 at 20 s (primes)",
	    (sim_t){.ppm = 20, .seconds = 60, .bps = 3, .target = UA_TARGET_DEFAULT, .dmin = 0, .dmax = 0, .isr_jitter = 100e-6, .retarget_at = 20, .retarget_to = 144, .gaps_max = 1, .silent_ok = 1, .prefill = 0.05});
	printf(failures ? "FAILED: %d checks\n" : "all checks passed\n", failures);
	return failures != 0;
}
