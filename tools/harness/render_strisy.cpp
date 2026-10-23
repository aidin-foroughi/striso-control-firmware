// Host renderer for a C++ voice engine (strisy.h, or voice_<name>.h with SYNTH_VOICE_HEADER and
// SYNTH_VOICE_NS) playing a scripted performance (PERF_HEADER, default performance.h) or a
// recorded key stream, optionally through the master effects. Built by CMake (tools/harness:
// render_<voice>, render_<voice>_<perf>) and by tools/harness/render_strisy.sh.
//
// Usage: render_strisy [out.f32] [seconds] [options]
//   --preset N        engine preset (engines with presets; default: as init() leaves it)
//   --nv N            voices (default NV, 6; the firmware runs 10)
//   --rec FILE.csv    play a recorded key stream (tools/record_keys.py) instead of PERF_HEADER;
//                     seconds default to its length + 2
//   --fx              through the master effects (fx.h, default parameters): stereo output
//   --wav FILE.wav    also write a 16 bit WAV, x0.25 like the board
//   --check           fail (exit 1) on non-finite output, a peak over --max-peak (default 8), or
//                     with --silent-after T, any sample above 1e-4 after T seconds, or voices still
//                     active at the end
//   --expect HASH     fail unless the render's hash (FNV-1a 64 of the float bits) is HASH
//   --golden-name N   with the environment variable STRISO_GOLDEN_OUT=<dir>: write the hash to
//                     <dir>/N.hash instead of comparing it (tools/update_goldens.sh)
// Prints "<voices>, <seconds>, host compute, peak" (as before) and "hash <hex>".
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "harness_util.h"
#include "striso_dsp/strisy.h"
#ifdef SYNTH_VOICE_HEADER
#include SYNTH_VOICE_HEADER
namespace eng = SYNTH_VOICE_NS;
#else
namespace eng = strisy;
#endif
#include "striso_dsp/fx.h"
#ifndef PERF_HEADER
#define PERF_HEADER "performance.h"
#endif
#include PERF_HEADER

// engines with presets (NUM_PRESETS, set_preset(Engine&, int)): built with SYNTH_VOICE_PRESETS
static bool set_preset_checked(eng::Engine& e, int p) {
	if (p < 0) return true;
#ifdef SYNTH_VOICE_PRESETS
	if (p >= eng::NUM_PRESETS) return false;
	eng::set_preset(e, p);
	return true;
#else
	(void)e;
	return false;
#endif
}

int main(int argc, char** argv) {
	const char* out_path = nullptr;
	double seconds = -1;
	int preset = -1, positional = 0;
#ifndef NV
#define NV 6
#endif
	int nv = NV;
	const char* rec = nullptr;
	const char* wav = nullptr;
	const char* expect = nullptr;
	const char* golden_name = nullptr;
	bool use_fx = false, check = false;
	double max_peak = 8.0, silent_after = -1;
	for (int a = 1; a < argc; a++) {
		const std::string s = argv[a];
		auto next = [&]() -> const char* {
			if (a + 1 >= argc) { std::fprintf(stderr, "%s needs a value\n", s.c_str()); std::exit(2); }
			return argv[++a];
		};
		if (s == "--preset") preset = std::atoi(next());
		else if (s == "--nv") nv = std::atoi(next());
		else if (s == "--rec") rec = next();
		else if (s == "--fx") use_fx = true;
		else if (s == "--wav") wav = next();
		else if (s == "--check") check = true;
		else if (s == "--max-peak") max_peak = std::atof(next());
		else if (s == "--silent-after") silent_after = std::atof(next());
		else if (s == "--expect") expect = next();
		else if (s == "--golden-name") golden_name = next();
		else if (s.size() > 1 && s[0] == '-' && s[1] == '-') { std::fprintf(stderr, "unknown option %s\n", s.c_str()); return 2; }
		else if (positional == 0) { out_path = argv[a]; positional++; }
		else if (positional == 1) { seconds = std::atof(argv[a]); positional++; }
	}
	if (nv < 1 || nv > NVOICES_MAX || nv > eng::MAX_VOICES) { std::fprintf(stderr, "--nv out of range\n"); return 2; }

	std::vector<harness::RecEvent> events;
	if (rec) {
		events = harness::load_recording(rec, nv);
		if (events.empty()) { std::fprintf(stderr, "no key events in %s\n", rec); return 2; }
		if (seconds < 0) seconds = events.back().t + 2.0;
	}
	if (seconds < 0) seconds = 17.5;

	static eng::Engine e;
	eng::init(e, SAMPLERATE, nv);
	if (!set_preset_checked(e, preset)) { std::fprintf(stderr, "no preset %d for this engine\n", preset); return 2; }
	static fx::Engine fxe;
	if (use_fx) fx::init(fxe, SAMPLERATE);

	const int nblocks = static_cast<int>(seconds * SAMPLERATE / BLOCK);
	const int channels = use_fx ? 2 : 1;
	std::vector<float> out(static_cast<size_t>(nblocks) * BLOCK * channels);
	Ctl v[NVOICES_MAX];
	float g[8];
	double compute_s = 0;
	size_t k = 0;
	for (int b = 0; b < nblocks; b++) {
		if (rec) {
			// the firmware's controls: the last event of each voice up to this block, defaults as
			// the synth thread starts them (note 69, accelerometer at rest, pedal 3/16)
			if (b == 0) {
				for (int i = 0; i < nv; i++) e.vc[i] = {69.0f, 0.0f, 0.0f, 0.0f, 0.0f};
				e.g = {1.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f, 0.0f, 3.0f / 16};
			}
			const double t = static_cast<double>(b) * BLOCK / SAMPLERATE;
			for (; k < events.size() && events[k].t <= t; k++) {
				const harness::RecEvent& x = events[k];
				e.vc[x.voice] = {x.note, x.pres, x.vpres, x.but_x, x.but_y};
			}
		} else {
			block_controls(b, v, nv, g);
			for (int i = 0; i < nv; i++) e.vc[i] = {v[i].note, v[i].pres, v[i].vpres, v[i].but_x, v[i].but_y};
			e.g = {g[0], g[1], g[2], g[3], g[4], g[5], g[6], g[7]};
		}
		auto t0 = std::chrono::steady_clock::now();
		eng::real buf[BLOCK];
		eng::process(e, buf);
		if (use_fx) {
			float l[BLOCK], r[BLOCK], in[BLOCK];
			for (int n = 0; n < BLOCK; n++) in[n] = static_cast<float>(buf[n]);
			fx::process(fxe, in, l, r);
			for (int n = 0; n < BLOCK; n++) {
				out[(static_cast<size_t>(b) * BLOCK + n) * 2] = l[n];
				out[(static_cast<size_t>(b) * BLOCK + n) * 2 + 1] = r[n];
			}
		} else {
			for (int n = 0; n < BLOCK; n++) out[static_cast<size_t>(b) * BLOCK + n] = static_cast<float>(buf[n]);
		}
		compute_s += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	}

	size_t bad = 0, loud_tail = 0;
	float peak = 0;
	const size_t tail_from = silent_after < 0 ? out.size() : static_cast<size_t>(silent_after * SAMPLERATE) * channels;
	for (size_t i = 0; i < out.size(); i++) {
		const float x = out[i];
		if (!std::isfinite(x)) bad++;
		else {
			peak = std::max(peak, std::fabs(x));
			if (i >= tail_from && std::fabs(x) > 1e-4f) loud_tail++;
		}
	}
	if (bad) std::fprintf(stderr, "!! %zu NON-FINITE OUTPUT SAMPLES\n", bad);
	if (out_path) {
		FILE* f = std::fopen(out_path, "wb");
		if (!f) { std::perror(out_path); return 1; }
		std::fwrite(out.data(), sizeof(float), out.size(), f);
		std::fclose(f);
	}
	if (wav && !harness::write_wav(wav, out, channels, SAMPLERATE)) { std::perror(wav); return 1; }
	const uint64_t h = harness::hash_samples(out);
	std::fprintf(stderr, "%d voices, %.1f s audio, host compute %.3f s, peak %.3f\n", nv, seconds, compute_s, peak);
	std::printf("hash %016llx\n", static_cast<unsigned long long>(h));

	int fail = 0;
	if (check) {
		if (bad) fail = 1;
		if (peak > max_peak) { std::fprintf(stderr, "!! peak %.3f over %.3f\n", peak, max_peak); fail = 1; }
		if (silent_after >= 0) {
			const int active = eng::active_voices(e);
			if (loud_tail) { std::fprintf(stderr, "!! %zu samples above 1e-4 after %.2f s\n", loud_tail, silent_after); fail = 1; }
			if (active) { std::fprintf(stderr, "!! %d voices still active at the end\n", active); fail = 1; }
		}
	}
	const char* golden_out = std::getenv("STRISO_GOLDEN_OUT");
	if (golden_name && golden_out && *golden_out) {
		const std::string path = std::string(golden_out) + "/" + golden_name + ".hash";
		FILE* f = std::fopen(path.c_str(), "w");
		if (!f) { std::perror(path.c_str()); return 1; }
		std::fprintf(f, "%s %016llx\n", golden_name, static_cast<unsigned long long>(h));
		std::fclose(f);
		expect = nullptr;
	}
	if (expect) {
		char hs[32];
		std::snprintf(hs, sizeof hs, "%016llx", static_cast<unsigned long long>(h));
		if (std::strcmp(hs, expect) != 0) { std::fprintf(stderr, "!! render hash %s, expected %s (golden render changed)\n", hs, expect); fail = 1; }
	}
	return fail;
}
