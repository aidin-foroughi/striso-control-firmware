// Host renderer for the C++ engine (strisy.h) playing the same scripted performance.
// Usage: render_strisy out.f32 [seconds]
#include <stdio.h>
#include <stdlib.h>
#include <vector>
#include <chrono>
#include <cmath>
#include <algorithm>
#include "strisy.h"
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

int main(int argc, char** argv) {
	if (argc < 2) { fprintf(stderr, "usage: render_strisy out.f32 [seconds]\n"); return 1; }
	double seconds = argc > 2 ? atof(argv[2]) : 17.5;
	static eng::Engine e;
#ifndef NV
#define NV 6
#endif
	const int nv = NV;  // voices: -DNV=10 for the firmware's count
	eng::init(e, SAMPLERATE, nv);
	int nblocks = (int)(seconds * SAMPLERATE / BLOCK);
	std::vector<float> out((size_t)nblocks * BLOCK);
	Ctl v[NVOICES_MAX];
	float g[8];
	double compute_s = 0;
	for (int b = 0; b < nblocks; b++) {
		block_controls(b, v, nv, g);
		for (int i = 0; i < nv; i++) e.vc[i] = {v[i].note, v[i].pres, v[i].vpres, v[i].but_x, v[i].but_y};
		e.g = {g[0], g[1], g[2], g[3], g[4], g[5], g[6], g[7]};
		auto t0 = std::chrono::steady_clock::now();
		eng::real buf[BLOCK];
		eng::process(e, buf);
		for (int n = 0; n < BLOCK; n++) out[(size_t)b * BLOCK + n] = (float)buf[n];
		compute_s += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	}
	size_t bad = 0;
	float peak = 0;
	for (float x : out) { if (!std::isfinite(x)) bad++; else peak = std::max(peak, std::fabs(x)); }
	if (bad) fprintf(stderr, "!! %zu NON-FINITE OUTPUT SAMPLES\n", bad);
	FILE* f = fopen(argv[1], "wb");
	fwrite(out.data(), sizeof(float), out.size(), f);
	fclose(f);
	fprintf(stderr, "%d voices, %.1f s audio, host compute %.3f s, peak %.3f\n", nv, seconds, compute_s, peak);
	return 0;
}
