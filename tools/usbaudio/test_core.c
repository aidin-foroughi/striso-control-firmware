// Host unit test of usb_audio_core.h (the exact firmware code): packet sizing (47/48/49), drift
// tracking, ring under/overrun handling, sample integrity and packing, test tone.
// Run: tools/usbaudio/run_tests.sh   (compiles with gcc in the striso-build Docker image)
//
// Simulation: the synth commits a 32-frame block once per block period of the device's audio
// clock (48 kHz * (1 + ppm)), each at a random render delay after its DMA interrupt (5..90% of the
// period). The USB ISR builds one packet per host USB frame (1 ms of the host clock). Each frame
// written carries its index, so the test checks that the host receives every frame exactly once,
// in order, with no silence after the start (except around the planned faults).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "usb_audio_core.h"

static ua_core_t c;
static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("  FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static uint32_t rng = 12345;
static double urand(void) { rng = rng * 1664525u + 1013904223u; return (rng >> 8) * (1.0 / 16777216.0); }

// frame f: left carries the index, right a different pattern (never both zero)
static int32_t sample_l(uint32_t f) { return (int32_t)((f & 0xFFFFFF) << 8); }
static int32_t sample_r(uint32_t f) { return (int32_t)(((f ^ 0x5A5A5A) & 0xFFFFFF) << 8); }

typedef struct {
	double ppm;          // device audio clock vs host USB clock
	double seconds;
	int bps;             // 3 or 2
	double stall_at, stall_len;   // synth produces nothing for stall_len s (0: none)
	double pause_at, pause_len;   // host takes no packets for pause_len s (0: none)
	int fault;           // a fault is planned: allow underruns/overruns and gaps
} sim_t;

static void run(const char *name, sim_t s) {
	memset(&c, 0, sizeof c);
	ua_stats_reset(&c);
	ua_start(&c);
	const double tb = UA_BLOCK / (48000.0 * (1.0 + s.ppm * 1e-6));   // block period
	const double th = 1e-3;                                          // host frame
	uint32_t wframe = 0;               // next frame index the synth writes
	double next_dma = tb, next_commit = tb + (0.05 + 0.85 * urand()) * tb;
	double t_host = 0.37e-3;           // packet build times: arbitrary phase
	uint32_t expect = 0;
	int have_expect = 0;
	long hist[3] = {0, 0, 0}, steady_packets = 0, steady_frames = 0;
	long silent_after_start = 0, gaps = 0;
	int32_t lvl_lo = 1 << 30, lvl_hi = -1;
	uint8_t pkt[UA_PACKET_MAX + 8];
	const long npk = (long)(s.seconds / th);
	for (long k = 0; k < npk; k++, t_host += th) {
		// writer: every block committed before this packet is built
		while (next_commit <= t_host) {
			const double tc = next_commit;
			const int stalled = s.stall_len > 0 && tc >= s.stall_at && tc < s.stall_at + s.stall_len;
			if (!stalled) {
				int32_t *p = ua_write_ptr(&c);
				for (int i = 0; i < UA_BLOCK; i++) {
					p[2 * i] = sample_l(wframe + i);
					p[2 * i + 1] = sample_r(wframe + i);
				}
				ua_write_commit(&c, 0);
			}
			wframe += UA_BLOCK;   // the synth's timeline advances even when a block is lost
			next_dma += tb;
			next_commit = next_dma + (0.05 + 0.85 * urand()) * tb;
		}
		if (s.pause_len > 0 && t_host >= s.pause_at && t_host < s.pause_at + s.pause_len) continue;
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
			if (l == 0 && r == 0) {   // silence: priming or underrun padding
				if (have_expect) silent_after_start++;
				continue;
			}
			const uint32_t mask = s.bps == 3 ? 0xFFFFFF00u : 0xFFFF0000u;
			if (s.bps == 3) {
				const uint32_t f = l >> 8;
				CHECK(r == (uint32_t)sample_r(f), "%s: right channel does not match left", name);
				if (have_expect && f != (expect & 0xFFFFFF)) gaps++;
				expect = f + 1;
				have_expect = 1;
			} else {
				// 16 bit keeps the top 16 bits of the index: check it never jumps
				const uint32_t f = l >> 16;
				(void)mask;
				if (have_expect && f != expect && f != ((expect + 1) & 0xFFFF)) gaps++;
				expect = f;
				have_expect = 1;
			}
		}
		if (t_host > 5.0 && !was_priming && !c.priming) {   // steady state after the servo settled
			steady_packets++;
			steady_frames += frames;
			if ((int32_t)avail < lvl_lo) lvl_lo = (int32_t)avail;
			if ((int32_t)avail > lvl_hi) lvl_hi = (int32_t)avail;
		}
	}
	const double rate = steady_packets ? 1000.0 * steady_frames / steady_packets : 0;
	const double want = 48000.0 * (1.0 + s.ppm * 1e-6);
	const unsigned faults = c.underruns + c.overruns;
	printf("%-27s %.3f Hz (device %.3f: %+.2f ppm) 47:%-6ld 48:%-6ld 49:%-6ld level %3d..%3d "
	       "under %u over %u gaps %ld silent %ld\n", name, rate, want, (rate / want - 1) * 1e6,
	       hist[0], hist[1], hist[2], lvl_lo, lvl_hi, c.underruns, c.overruns, gaps, silent_after_start);
	if (!s.fault) {
		CHECK(faults == 0, "%s: %u underruns/overruns in a clean run", name, faults);
		CHECK(gaps == 0, "%s: %ld gaps in the frame sequence", name, gaps);
		CHECK(silent_after_start == 0, "%s: %ld silent frames after the start", name, silent_after_start);
		CHECK(fabs(rate / want - 1) < 2e-6, "%s: rate error %.2f ppm", name, (rate / want - 1) * 1e6);
		CHECK(lvl_lo >= UA_FRAMES_MAX && lvl_hi <= UA_OVERRUN_LEVEL, "%s: level %d..%d", name, lvl_lo, lvl_hi);
	} else {
		CHECK(faults >= 1 && faults <= 2, "%s: %u faults, expected 1..2", name, faults);
		CHECK(c.priming == 0, "%s: still priming at the end", name);
		CHECK(fabs(rate / want - 1) < 50e-6, "%s: rate error %.2f ppm after recovery", name, (rate / want - 1) * 1e6);
	}
}

// the 47/48/49 accumulator alone: a constant level offset gives a constant rate offset
static void test_accumulator(void) {
	memset(&c, 0, sizeof c);
	ua_rate_reset(&c);
	long sum = 0, n = 0, h[3] = {0, 0, 0};
	int run49 = 0, max_run48 = 0, run48 = 0;
	// level 26 frames above target -> +26/256 frame/packet once the level filter has settled
	for (int k = 0; k < 100000; k++) {
		const int f = ua_rate_next(&c, UA_TARGET_FRAMES + 26);
		CHECK(f >= 47 && f <= 49, "accumulator gave %d", f);
		if (k >= 1000) {
			sum += f; n++; h[f - 47]++;
			if (f == 49) { run49++; run48 = 0; } else if (++run48 > max_run48) max_run48 = run48;
		}
	}
	const double mean = (double)sum / n;
	printf("accumulator, level +26: mean %.5f frames/packet (expect %.5f) 47:%ld 48:%ld 49:%ld, "
	       "longest run of 48s %d (evenly spread: ~%d)\n", mean, 48 + 26 / 256.0, h[0], h[1], h[2],
	       max_run48, (int)(256 / 26));
	// the level filter's shift truncates: it settles up to 31/256 frame low, i.e. a rate offset of
	// at most 0.0005 frame/packet open loop (in the closed loop that is a level offset < 1 frame)
	CHECK(fabs(mean - (48 + 26 / 256.0)) < 1e-3, "accumulator mean %.5f", mean);
	CHECK(h[0] == 0, "no 47s expected above target");
	CHECK(max_run48 <= 10, "49s not evenly spread");
	// saturation: far above target -> +0.25 max; far below -> -0.25
	ua_rate_reset(&c);
	sum = 0; n = 0;
	for (int k = 0; k < 10000; k++) { int f = ua_rate_next(&c, 10000); if (k >= 1000) { sum += f; n++; } }
	printf("accumulator, level far above target: mean %.4f (limit 48.25)\n", (double)sum / n);
	CHECK(fabs((double)sum / n - 48.25) < 1e-3, "saturation high");
	ua_rate_reset(&c);
	sum = 0; n = 0;
	for (int k = 0; k < 10000; k++) { int f = ua_rate_next(&c, 0); if (k >= 1000) { sum += f; n++; } }
	printf("accumulator, level zero: mean %.4f (limit 47.75)\n", (double)sum / n);
	CHECK(fabs((double)sum / n - 47.75) < 1e-3, "saturation low");
	// exactly at target: always 48
	ua_rate_reset(&c);
	int bad = 0;
	for (int k = 0; k < 10000; k++) if (ua_rate_next(&c, UA_TARGET_FRAMES) != 48) bad++;
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
	for (int i = 0; i < UA_BLOCK; i++) { p[2 * i] = v[i % 4]; p[2 * i + 1] = v[(i + 1) % 4]; }
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

int main(void) {
	test_accumulator();
	test_tone();
	test_pack();
	const double ppms[] = {0, 20, -20, 100, -100, 500, -500, 2000, -2000};
	for (unsigned i = 0; i < sizeof ppms / sizeof ppms[0]; i++) {
		char name[64];
		snprintf(name, sizeof name, "24 bit %+5g ppm, 10 min", ppms[i]);
		run(name, (sim_t){ppms[i], 600, 3, 0, 0, 0, 0, 0});
	}
	run("16 bit   +50 ppm, 60 s", (sim_t){50, 60, 2, 0, 0, 0, 0, 0});
	run("synth stall 20 ms at 30 s", (sim_t){30, 60, 3, 30, 0.020, 0, 0, 1});
	run("host pause 50 ms at 30 s", (sim_t){-30, 60, 3, 0, 0, 30, 0.050, 1});
	printf(failures ? "FAILED: %d checks\n" : "all checks passed\n", failures);
	return failures != 0;
}
