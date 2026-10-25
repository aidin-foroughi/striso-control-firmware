// Target renderer: a voice engine playing a scripted performance, compiled for the board's CPU
// with the firmware synth's flags and run on QEMU (mps2-an500, Cortex-M7 + FPv5-D16, FPSCR as on
// the board), so the output is the board's arithmetic: the arm compiler's own code (FMA
// contraction, instruction selection), not the host's. Used to compare toolchains (the host
// renders don't change with the arm compiler). Output: float32 mono, 48000 Hz, through
// semihosting. The engine through its contract (striso::dsp::VoiceEngine: SYNTH_ENGINE_HEADER,
// SYNTH_VOICE_NS, as the firmware's VOICE=<name>), the performance PERF_HEADER.
// Usage (QEMU semihosting arguments): render_target <out.f32> [preset] [voices] [seconds] [fx]
// Prints "hash <FNV-1a 64 of the float bits> peak <p> nonfinite <n>".
// With -DTARGET_FX the master effects (fx.h) run after the engine, as in the firmware: fx 0 off,
// 1 defaults, 2 every stage on (fx_suite.sh's "all"); prints "fxhash <hash of L,R interleaved>".
// The engine's output and hash are the same as without it.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "striso_dsp/engine.h"
#include SYNTH_ENGINE_HEADER
using Engine = striso::dsp::engines::SYNTH_VOICE_NS;
#ifdef TARGET_FX
#include "striso_dsp/effects/fx.h" // before the performance (its BLOCK macro)
#endif
#ifndef PERF_HEADER
#define PERF_HEADER "performance.h"
#endif
#include PERF_HEADER

static Engine e;
static striso::dsp::Memory<Engine> mem;
#ifdef TARGET_FX
using Fx = striso::dsp::effects::fx;
static Fx fxe;
static striso::dsp::Memory<Fx> fxmem;
static void fx_set(const char* name, float v) {
	for (int i = 0; i < Fx::num_params; i++)
		if (!std::strcmp(Fx::param_desc(i)->name, name)) (void)fxe.set_param(i, v);
}
#endif

#ifdef TARGET_PROFILE
// -DTARGET_PROFILE with QEMU -icount shift=0 (1 ns per instruction): SysTick on the CPU clock
// (mps2: 25 MHz, a tick per 40 ns) counts the instructions e.process() executes (the engine, and its copy of the controls), /40
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
	e.init(mem.fast, mem.large, SAMPLERATE, nv);
#ifdef TARGET_FX
	const int fx_mode = argc > 5 ? std::atoi(argv[5]) : 1;
	fxe.init(fxmem.fast, fxmem.large, SAMPLERATE);
	if (fx_mode == 2) {
		fx_set("drive", 0.3f);
		fx_set("phmix", 0.6f);
		fx_set("chmix", 0.5f);
		fx_set("trdepth", 0.5f);
		fx_set("dlmix", 0.3f);
		fx_set("rvmix", 0.3f);
	}
	uint64_t fxhash = 1469598103934665603ull;
#ifdef TARGET_PROFILE
	uint64_t fx_ticks = 0;
#endif
#endif
	if (preset >= 0 && preset < Engine::num_presets()) e.set_preset(preset);
#ifdef TARGET_PROFILE
	SYST_RVR = 0xffffffu;
	SYST_CVR = 0;
	SYST_CSR = 5; // enabled, CPU clock, no interrupt
#endif
	FILE* f = std::fopen(argv[1], "wb");
	if (!f) {
		std::fprintf(stderr, "cannot write %s\n", argv[1]);
		return 1;
	}
	static float out[64 * BLOCK];
	int k = 0;
	uint64_t hash = 1469598103934665603ull; // FNV-1a 64 of the float bits, as harness_util.h
	float peak = 0.0f;
	long nonfinite = 0;
	const int nblocks = static_cast<int>(seconds * SAMPLERATE / BLOCK);
	Ctl v[NVOICES_MAX];
	float g[8];
	static striso::dsp::VoiceControls vc[NVOICES_MAX];
	for (int i = 0; i < nv; i++) vc[i] = striso::dsp::voice_at_rest;
	for (int b = 0; b < nblocks; b++) {
		block_controls(b, v, nv, g);
		for (int i = 0; i < nv; i++) {
			vc[i].note = v[i].note;
			vc[i].pres = v[i].pres;
			vc[i].vpres = v[i].vpres;
			vc[i].but_x = v[i].but_x;
			vc[i].but_y = v[i].but_y;
			striso::dsp::track_key(vc[i]);
		}
		const striso::dsp::GlobalControls gc = {g[0], g[1], g[2], g[3], g[4], g[5], g[6], g[7]};
		float buf[BLOCK];
		float* const outs[1] = {buf};
#ifdef TARGET_PROFILE
		const uint32_t t0 = SYST_CVR;
#endif
		e.process(vc, gc, outs);
#ifdef TARGET_PROFILE
		dsp_ticks += (t0 - SYST_CVR) & 0xffffffu;
#endif
#ifdef TARGET_FX
		if (fx_mode) {
			float l[BLOCK], r[BLOCK];
			const float* const fx_in[1] = {buf};
			float* const fx_out[2] = {l, r};
#ifdef TARGET_PROFILE
			const uint32_t t1 = SYST_CVR;
#endif
			fxe.process(fx_in, fx_out);
#ifdef TARGET_PROFILE
			fx_ticks += (t1 - SYST_CVR) & 0xffffffu;
#endif
			for (int n = 0; n < 2 * BLOCK; n++) {
				uint32_t u;
				std::memcpy(&u, n & 1 ? &r[n / 2] : &l[n / 2], 4);
				for (int i = 0; i < 4; i++) {
					fxhash ^= (u >> (8 * i)) & 0xffu;
					fxhash *= 1099511628211ull;
				}
			}
		}
#endif
		for (int n = 0; n < BLOCK; n++) {
			const float x = static_cast<float>(buf[n]);
			out[k * BLOCK + n] = x;
			uint32_t u;
			std::memcpy(&u, &x, 4);
			for (int i = 0; i < 4; i++) {
				hash ^= (u >> (8 * i)) & 0xffu;
				hash *= 1099511628211ull;
			}
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
#ifdef TARGET_FX
	std::printf("fxhash %08lx%08lx\n", static_cast<unsigned long>(fxhash >> 32), static_cast<unsigned long>(fxhash & 0xffffffffu));
#ifdef TARGET_PROFILE
	std::printf("fx_ticks %lu per_block %.1f\n", static_cast<unsigned long>(fx_ticks), static_cast<double>(fx_ticks) / nblocks);
#endif
#endif
	return 0;
}
