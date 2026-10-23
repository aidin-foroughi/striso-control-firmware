// Host test for the master effects (fx.h): a voice engine or a test signal through fx::process.
// Usage: fx_test <out-prefix> [key=value ...]
//   src=voice|impulse|sine|noise|silence  input (default voice: the engine compiled in, playing PERF)
//   seconds=S        length (default: 17.5)        tail=S   extra seconds of silence after the input
//   freq=F amp=A     test signal frequency / amplitude (sine, noise, impulse)
//   gain=G           input gain (voice)            preset=N  va preset
//   random=1         random parameter writes every block (stress)  seed=N
//   <param>=value    fx parameter (name from fx::PARAMS) at time 0
//   @t:<param>=value fx parameter written at time t (seconds)
//   ramp:<param>=a:b:t0:t1  parameter written every block, linear from a to b during [t0, t1]
// Writes <out>.f32 (stereo interleaved float32), <out>.wav (16 bit stereo x0.25 clipped like the
// board), with src=voice also <out>_dry.wav, and prints statistics.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <vector>
#include <string>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <xmmintrin.h>
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
#include "striso_dsp/fx.h"
#include PERF_HEADER

static int param_index(const char* name) {
	for (int i = 0; i < fx::NPARAM; i++)
		if (!strcmp(fx::PARAMS[i].d.name, name)) return i;
	fprintf(stderr, "unknown fx parameter %s\n", name);
	exit(1);
}

// writes go through the same path as the firmware: clamp to [min, max], round non-continuous
static void param_write(fx::Engine& e, int i, float value) {
	const strisy::ParamDesc* d = strisy::EngineParams<fx::Engine>::desc(i);
	float* p = strisy::EngineParams<fx::Engine>::ptr(e, i);
	if (!d || !p || value != value) return;
	float v = value < d->min ? d->min : (value > d->max ? d->max : value);
	if (d->kind != strisy::ParamKind::continuous) v = (float)(int)(v + (v >= 0 ? 0.5f : -0.5f));
	*p = v;
	strisy::EngineParams<fx::Engine>::changed(e, i);
}

struct Event { double t; int i; float v; };
struct Ramp { int i; float a, b; double t0, t1; };

static void write_wav(const char* path, const std::vector<float>& x, int channels) {
	FILE* f = fopen(path, "wb");
	if (!f) { perror(path); exit(1); }
	uint32_t n = (uint32_t)x.size();
	uint32_t sr = SAMPLERATE, br = sr * 2 * channels, data = n * 2;
	uint16_t ch = (uint16_t)channels, ba = (uint16_t)(2 * channels), bits = 16, fmt = 1;
	uint32_t riff = 36 + data, fmtlen = 16;
	fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
	fwrite(&fmtlen, 4, 1, f); fwrite(&fmt, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&sr, 4, 1, f);
	fwrite(&br, 4, 1, f); fwrite(&ba, 2, 1, f); fwrite(&bits, 2, 1, f);
	fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
	std::vector<int16_t> s(n);
	for (uint32_t k = 0; k < n; k++) {
		float v = x[k] * 0.25f;   // board: (volume/127)^2 ~ 0.25, then hard clip
		v = v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
		s[k] = (int16_t)lrintf(v * 32767.0f);
	}
	fwrite(s.data(), 2, n, f);
	fclose(f);
}

static uint32_t rng_state = 1;
static float frand() { rng_state = rng_state * 1664525u + 1013904223u; return (rng_state >> 8) * (1.0f / 16777216.0f); }

int main(int argc, char** argv) {
	_mm_setcsr(_mm_getcsr() | 0x8040);   // flush denormals (speed only; the board's FPU handles them in hardware)
	if (argc < 2) { fprintf(stderr, "usage: fx_test out-prefix [key=value ...]\n"); return 1; }
	std::string out = argv[1];
	std::string src = "voice";
	double seconds = 17.5, tail = 0, freq = 1000, amp = 1, gain = 1;
	int preset = -1, random = 0;
	double log0 = -1, log1 = -1;
	std::vector<Event> events;
	std::vector<Ramp> ramps;
	static fx::Engine fe;
	fx::init(fe, SAMPLERATE);
	for (int a = 2; a < argc; a++) {
		std::string s = argv[a];
		size_t eq = s.find('=');
		if (eq == std::string::npos) { fprintf(stderr, "bad argument %s\n", s.c_str()); return 1; }
		std::string k = s.substr(0, eq), v = s.substr(eq + 1);
		if (k == "src") src = v;
		else if (k == "seconds") seconds = atof(v.c_str());
		else if (k == "tail") tail = atof(v.c_str());
		else if (k == "freq") freq = atof(v.c_str());
		else if (k == "amp") amp = atof(v.c_str());
		else if (k == "gain") gain = atof(v.c_str());
		else if (k == "preset") preset = atoi(v.c_str());
		else if (k == "random") random = atoi(v.c_str());
		else if (k == "seed") rng_state = (uint32_t)atoi(v.c_str());
		else if (k == "log") sscanf(v.c_str(), "%lf:%lf", &log0, &log1);
		else if (k[0] == '@') {
			size_t c = k.find(':');
			events.push_back({atof(k.substr(1, c - 1).c_str()), param_index(k.substr(c + 1).c_str()), (float)atof(v.c_str())});
		} else if (k.compare(0, 5, "ramp:") == 0) {
			Ramp r;
			r.i = param_index(k.substr(5).c_str());
			if (sscanf(v.c_str(), "%f:%f:%lf:%lf", &r.a, &r.b, &r.t0, &r.t1) != 4) { fprintf(stderr, "bad ramp %s\n", s.c_str()); return 1; }
			ramps.push_back(r);
		} else param_write(fe, param_index(k.c_str()), (float)atof(v.c_str()));
	}
	// parameters given at time 0 apply from the start (no fade in)
	for (int i = 0; i < fx::NPARAM; i++) fe.s[i] = fe.sp[i] = fe.p[i];
	fx::update_stages(fe);

	static eng::Engine e;
	const int nv = 6;
	eng::init(e, SAMPLERATE, nv);
#ifdef SYNTH_VOICE_PRESETS
	if (preset >= 0) eng::set_preset(e, preset);
#else
	(void)preset;
#endif
	const int nin = (int)(seconds * SAMPLERATE / BLOCK);
	const int nblocks = nin + (int)(tail * SAMPLERATE / BLOCK);
	std::vector<float> dry((size_t)nblocks * BLOCK), wet((size_t)nblocks * BLOCK * 2);
	Ctl v[NVOICES_MAX];
	float g[8];
	double fx_s = 0;
	uint32_t noise = 12345;
	size_t ev = 0;
	std::sort(events.begin(), events.end(), [](const Event& a, const Event& b) { return a.t < b.t; });
	const int perf_blocks = (int)(17.5 * SAMPLERATE / BLOCK);
	for (int b = 0; b < nblocks; b++) {
		const double t = (double)b * BLOCK / SAMPLERATE;
		float in[BLOCK];
		if (src == "voice") {
			if (b < nin) {
				block_controls(b % perf_blocks, v, nv, g);
				for (int i = 0; i < nv; i++) e.vc[i] = {v[i].note, v[i].pres, v[i].vpres, v[i].but_x, v[i].but_y};
				e.g = {g[0], g[1], g[2], g[3], g[4], g[5], g[6], g[7]};
			} else {
				for (int i = 0; i < nv; i++) { e.vc[i].pres = 0; e.vc[i].vpres = 0; }
			}
			eng::process(e, in);
			for (int n = 0; n < BLOCK; n++) in[n] *= (float)gain;
		} else {
			for (int n = 0; n < BLOCK; n++) {
				const long k = (long)b * BLOCK + n;
				float x = 0;
				if (b < nin) {
					if (src == "sine") x = (float)(amp * sin(2 * M_PI * freq * k / SAMPLERATE));
					else if (src == "noise") { noise = noise * 1664525u + 1013904223u; x = (float)amp * ((int32_t)noise * (1.0f / 2147483648.0f)); }
					else if (src == "impulse") x = k == (long)(0.05 * SAMPLERATE) ? (float)amp : 0.0f;   // after the input fade-in
				}
				in[n] = x;
			}
		}
		// parameter writes between blocks, as apply_params() does in the firmware
		while (ev < events.size() && events[ev].t <= t) { param_write(fe, events[ev].i, events[ev].v); ev++; }
		for (const Ramp& r : ramps)
			if (t >= r.t0 && t <= r.t1) param_write(fe, r.i, (float)(r.a + (r.b - r.a) * (t - r.t0) / (r.t1 - r.t0)));
		if (random) {
			// stress: 1..4 random writes per block, sometimes extremes, sometimes bypass
			const int nw = 1 + (int)(frand() * 4);
			for (int k = 0; k < nw; k++) {
				const int i = (int)(frand() * (fx::NPARAM - 1));   // all but bypass
				const strisy::ParamDesc& d = fx::PARAMS[i].d;
				float u = frand();
				if (frand() < 0.2f) u = u < 0.5f ? 0.0f : 1.0f;
				param_write(fe, i, d.min + u * (d.max - d.min));
				if (t >= log0 && t <= log1) fprintf(stderr, "%.4f %s=%.3f\n", t, d.name, fe.p[i]);
			}
			if (frand() < 0.002f) param_write(fe, fx::P_BYPASS, fe.p[fx::P_BYPASS] > 0.5f ? 0.0f : 1.0f);
		}
		float l[BLOCK], r[BLOCK];
		auto t0 = std::chrono::steady_clock::now();
		fx::process(fe, in, l, r);
		fx_s += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		for (int n = 0; n < BLOCK; n++) {
			const size_t k = (size_t)b * BLOCK + n;
			dry[k] = in[n];
			wet[2 * k] = l[n];
			wet[2 * k + 1] = r[n];
		}
	}
	size_t bad = 0;
	double pk = 0, pkd = 0, sl = 0, sr = 0, sd = 0;
	for (size_t k = 0; k < dry.size(); k++) {
		const float a = wet[2 * k], c = wet[2 * k + 1];
		if (!std::isfinite(a) || !std::isfinite(c)) { bad++; continue; }
		pk = std::max(pk, (double)std::max(std::fabs(a), std::fabs(c)));
		pkd = std::max(pkd, (double)std::fabs(dry[k]));
		sl += a * a; sr += c * c; sd += dry[k] * dry[k];
	}
	const double N = (double)dry.size();
	if (bad) fprintf(stderr, "!! %zu NON-FINITE OUTPUT SAMPLES\n", bad);
	printf("%s: %.1f s, dry peak %.3f rms %.3f | wet peak %.3f rms L %.3f R %.3f | guard trips %u | host fx %.2f us/block\n",
	       out.c_str(), N / SAMPLERATE, pkd, sqrt(sd / N), pk, sqrt(sl / N), sqrt(sr / N), fe.guard_trips, 1e6 * fx_s / nblocks);
	FILE* f = fopen((out + ".f32").c_str(), "wb");
	fwrite(wet.data(), sizeof(float), wet.size(), f);
	fclose(f);
	f = fopen((out + "_in.f32").c_str(), "wb");
	fwrite(dry.data(), sizeof(float), dry.size(), f);
	fclose(f);
	write_wav((out + ".wav").c_str(), wet, 2);
	if (src == "voice") {
		std::vector<float> d2(dry.size() * 2);
		for (size_t k = 0; k < dry.size(); k++) d2[2 * k] = d2[2 * k + 1] = dry[k];
		write_wav((out + "_dry.wav").c_str(), d2, 2);
	}
	return bad ? 2 : 0;
}
