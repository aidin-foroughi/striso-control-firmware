// Target renderer: a voice engine playing a scripted performance, compiled for the board's CPU
// with the firmware synth's flags and run on QEMU (mps2-an500, Cortex-M7 + FPv5-D16, FPSCR as on
// the board), so the output is the board's arithmetic: the arm compiler's own code (FMA
// contraction, instruction selection), not the host's. Used to compare toolchains (the host
// renders don't change with the arm compiler). Output: float32 mono, 48000 Hz, through
// semihosting. Engine/performance like render_strisy.cpp: SYNTH_VOICE_HEADER, SYNTH_VOICE_NS,
// SYNTH_VOICE_PRESETS, PERF_HEADER.
// Usage (QEMU semihosting arguments): render_target <out.f32> [preset] [voices] [seconds]
// Prints "hash <FNV-1a 64 of the float bits> peak <p> nonfinite <n>".
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "striso_dsp/strisy.h"
#ifdef SYNTH_VOICE_HEADER
#include SYNTH_VOICE_HEADER
namespace eng = SYNTH_VOICE_NS;
#else
namespace eng = strisy;
#endif
#ifndef PERF_HEADER
#define PERF_HEADER "performance.h"
#endif
#include PERF_HEADER

static eng::Engine e;

#ifdef TARGET_PROFILE
// -DTARGET_PROFILE with QEMU -icount shift=0 (1 ns per instruction): SysTick on the CPU clock
// (mps2: 25 MHz, a tick per 40 ns) counts the instructions eng::process() executes, /40
#define SYST_CSR (*reinterpret_cast<volatile uint32_t*>(0xE000E010u))
#define SYST_RVR (*reinterpret_cast<volatile uint32_t*>(0xE000E014u))
#define SYST_CVR (*reinterpret_cast<volatile uint32_t*>(0xE000E018u))
static uint64_t dsp_ticks;
#endif

int main(int argc, char** argv) {
	if (argc < 2) {
		std::fprintf(stderr, "usage: render_target out.f32 [preset] [voices] [seconds]\n");
		return 2;
	}
	const int preset = argc > 2 ? std::atoi(argv[2]) : -1;
	const int nv = argc > 3 ? std::atoi(argv[3]) : 10;
	const double seconds = argc > 4 ? std::atof(argv[4]) : 17.5;
	eng::init(e, SAMPLERATE, nv);
#ifdef SYNTH_VOICE_PRESETS
	if (preset >= 0 && preset < eng::NUM_PRESETS) eng::set_preset(e, preset);
#else
	(void)preset;
#endif
#ifdef TARGET_PROFILE
	SYST_RVR = 0xffffffu;
	SYST_CVR = 0;
	SYST_CSR = 5;  // enabled, CPU clock, no interrupt
#endif
	FILE* f = std::fopen(argv[1], "wb");
	if (!f) {
		std::fprintf(stderr, "cannot write %s\n", argv[1]);
		return 1;
	}
	static float out[64 * BLOCK];
	int k = 0;
	uint64_t hash = 1469598103934665603ull;  // FNV-1a 64 of the float bits, as harness_util.h
	float peak = 0.0f;
	long nonfinite = 0;
	const int nblocks = static_cast<int>(seconds * SAMPLERATE / BLOCK);
	Ctl v[NVOICES_MAX];
	float g[8];
	for (int b = 0; b < nblocks; b++) {
		block_controls(b, v, nv, g);
		for (int i = 0; i < nv; i++) e.vc[i] = {v[i].note, v[i].pres, v[i].vpres, v[i].but_x, v[i].but_y};
		e.g = {g[0], g[1], g[2], g[3], g[4], g[5], g[6], g[7]};
		eng::real buf[BLOCK];
#ifdef TARGET_PROFILE
		const uint32_t t0 = SYST_CVR;
#endif
		eng::process(e, buf);
#ifdef TARGET_PROFILE
		dsp_ticks += (t0 - SYST_CVR) & 0xffffffu;
#endif
		for (int n = 0; n < BLOCK; n++) {
			const float x = static_cast<float>(buf[n]);
			out[k * BLOCK + n] = x;
			uint32_t u;
			std::memcpy(&u, &x, 4);
			for (int i = 0; i < 4; i++) { hash ^= (u >> (8 * i)) & 0xffu; hash *= 1099511628211ull; }
			if (!std::isfinite(x)) nonfinite++;
			else peak = std::fabs(x) > peak ? std::fabs(x) : peak;
		}
		if (++k == 64 || b == nblocks - 1) {
			std::fwrite(out, sizeof(float), static_cast<size_t>(k) * BLOCK, f);
			k = 0;
		}
	}
	std::fclose(f);
	std::printf("hash %08lx%08lx peak %.3f nonfinite %ld\n", static_cast<unsigned long>(hash >> 32),
	            static_cast<unsigned long>(hash & 0xffffffffu), static_cast<double>(peak), nonfinite);
#ifdef TARGET_PROFILE
	std::printf("dsp_ticks %lu per_block %.1f\n", static_cast<unsigned long>(dsp_ticks),
	            static_cast<double>(dsp_ticks) / nblocks);
#endif
	return 0;
}
