// The bowed engine's index bounds (BOWED_ASSERT in voice_bowed.h: the note-on seeds and the delay
// line taps) under the brutal fuzz of tools/harness/out/bowed_fuzz.cpp and the recorded takes:
// random controls for all 10 voices (notes -50..200, NaN/Inf now and then, pres -1..3, vpres
// +-1e6, tilt +-10), chords of the lowest notes started in one block, out-of-range presets and
// direct parameter writes (in range, out of range, NaN). Checks: no bound violated, finite output,
// |out| <= 8.5.
#include <cmath>
#include <cstdint>

#include "doctest/doctest.h"

namespace {
long g_bound_failures = 0;
}
#define BOWED_ASSERT(c) \
	do { \
		if (!(c)) g_bound_failures++; \
	} while (0)
#include "harness_util.h"
#include "striso_dsp/engines/bowed.h"

namespace {

constexpr int kSampleRate = 48000, kVoices = 10, kBlock = 32;

uint32_t rs;
float rnd() {
	rs = rs * 1664525u + 1013904223u;
	return static_cast<float>(rs >> 8) * (1.0f / 16777216.0f);
}
float wild(float lo, float hi) {
	const float r = rnd();
	if (r < 0.01f) return NAN;
	if (r < 0.02f) return INFINITY;
	if (r < 0.03f) return -INFINITY;
	if (r < 0.05f) return (rnd() - 0.5f) * 2e6f;
	return lo + rnd() * (hi - lo);
}

struct Result {
	long bad = 0, big = 0;
	float peak = 0;
};

void collect(Result& res, const float* buf) {
	for (int n = 0; n < kBlock; n++) {
		if (!std::isfinite(buf[n])) res.bad++;
		else {
			res.peak = std::fmax(res.peak, std::fabs(buf[n]));
			if (std::fabs(buf[n]) > 8.5f) res.big++;
		}
	}
}

// the engine with its memory (the contract's init), then its native API: this is a white-box test
bowed::Engine& start() {
	static striso::dsp::engines::bowed a;
	static striso::dsp::Memory<striso::dsp::engines::bowed> mem;
	a.init(mem.fast, mem.large, kSampleRate, kVoices);
	return a.e;
}

Result fuzz(double secs, uint32_t seed) {
	bowed::Engine& e = start();
	rs = seed;
	Result res;
	const long nb = static_cast<long>(secs * kSampleRate / kBlock);
	for (long b = 0; b < nb; b++) {
		const int mode = static_cast<int>((b / 400) % 6); // phases of different nastiness
		for (int v = 0; v < kVoices; v++) {
			strisy::VoiceControls& c = e.vc[v];
			const float r = rnd();
			if (mode == 0) { // normal-ish playing with random note-ons/offs
				if (r < 0.05f) c.pres = c.pres > 0 ? 0.0f : rnd() * 1.5f;
				c.note = (r < 0.02f) ? 20 + rnd() * 95 : c.note;
				c.vpres = (rnd() - 0.5f) * 3;
				c.but_x = rnd() * 2 - 1;
				c.but_y = rnd() * 2 - 1;
			} else if (mode == 1) { // everything wild every block
				c.note = wild(-50, 200);
				c.pres = wild(-1, 3);
				c.vpres = wild(-10, 10);
				c.but_x = wild(-10, 10);
				c.but_y = wild(-10, 10);
			} else if (mode == 2) { // 10-voice chords of the lowest notes started in one block
				const bool on = (b % 20) < 3;
				c.note = rnd() * 20;
				c.pres = on ? 0.3f + rnd() * 1.7f : 0.0f;
				c.vpres = on ? 5.0f : -5.0f;
				c.but_x = 0;
				c.but_y = rnd() * 2 - 1;
			} else if (mode == 3) { // extreme bends far below and above the range
				c.pres = 0.8f;
				c.note = 60 + 8 * 0.5f * 1000.0f * std::sin(0.01f * static_cast<float>(b) + static_cast<float>(v)) * ((rnd() < 0.5f) ? 1 : 0.01f);
				c.vpres = wild(-2, 2);
				c.but_x = wild(-1, 1);
				c.but_y = wild(-1, 1);
			} else if (mode == 4) { // re-trigger every block (voice stealing / re-press)
				c.pres = (b + v) % 2 ? 0.0f : 1.0f;
				c.note = 10 + rnd() * 110;
				c.vpres = (b + v) % 2 ? -1.0f : 1.0f;
			} else { // pres > 0 with notes jumping every block
				c.pres = 0.2f + rnd();
				c.note = rnd() * 127;
				c.vpres = rnd() * 2;
			}
		}
		strisy::GlobalControls& g = e.g;
		if (rnd() < 0.05f) {
			g.acc_abs = wild(0, 5);
			g.rot_x = wild(-1, 1);
			g.rot_y = wild(-1, 1);
			g.rot_z = wild(-1, 1);
			g.pedal = wild(0, 1);
		}
		if (rnd() < 0.05f) bowed::set_preset(e, static_cast<int>(rnd() * 12) - 1);
		using EP = strisy::EngineParams<bowed::Engine>;
		for (int k = 0; k < 2; k++) {
			const int i = static_cast<int>(rnd() * EP::count) % EP::count;
			const strisy::ParamDesc* d = EP::desc(i);
			const float x = rnd() < 0.9f ? d->min + rnd() * (d->max - d->min) : wild(-1e3f, 1e3f);
			*EP::ptr(e, i) = x;
			EP::changed(e, i);
		}
		float buf[kBlock];
		bowed::process(e, buf);
		collect(res, buf);
	}
	return res;
}

Result replay(const char* csv, int preset) {
	bowed::Engine& e = start();
	bowed::set_preset(e, preset);
	const std::vector<harness::RecEvent> ev = harness::load_recording(csv, kVoices);
	Result res;
	if (ev.empty()) {
		res.bad = -1;
		return res;
	}
	const long nb = static_cast<long>((ev.back().t + 2.0) * kSampleRate / kBlock);
	size_t k = 0;
	for (long b = 0; b < nb; b++) {
		const double t = static_cast<double>(b) * kBlock / kSampleRate;
		for (; k < ev.size() && ev[k].t <= t; k++) e.vc[ev[k].voice] = {ev[k].note, ev[k].pres, ev[k].vpres, ev[k].but_x, ev[k].but_y};
		float buf[kBlock];
		bowed::process(e, buf);
		collect(res, buf);
	}
	return res;
}

} // namespace

TEST_CASE("bowed: seed and delay bounds hold under the brutal fuzz") {
	for (uint32_t seed : {1u, 2u, 3u}) {
		g_bound_failures = 0;
		const Result r = fuzz(20.0, seed);
		CAPTURE(seed);
		CHECK(g_bound_failures == 0);
		CHECK(r.bad == 0);
		CHECK(r.big == 0);
		MESSAGE("bowed fuzz seed " << seed << ": peak " << r.peak);
	}
}

TEST_CASE("bowed: seed and delay bounds hold on the recorded takes") {
	for (const char* take : {"staccato", "moderate", "legato", "a"}) {
		for (int preset : {0, 2, 3, 4, 8}) {
			g_bound_failures = 0;
			const std::string csv = std::string(STRISO_REC_DIR) + "/play_2026-10-22_" + take + ".csv";
			const Result r = replay(csv.c_str(), preset);
			CAPTURE(take);
			CAPTURE(preset);
			CHECK(r.bad == 0);
			CHECK(g_bound_failures == 0);
			CHECK(r.big == 0);
		}
	}
}
