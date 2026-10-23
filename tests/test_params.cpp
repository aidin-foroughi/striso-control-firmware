// Live parameters of every engine and of the master effects: the tables are sane, and random
// writes through the firmware's path (synth_strisy.cpp apply_params: NaN ignored, clamped to
// [min, max], rounded unless continuous, then changed()) while notes play never make the output
// non-finite or blow up. Every engine, every preset (presets switch as on the board: init, then
// set_preset).
#include <cmath>
#include <cstdint>
#include <cstring>
#include <set>
#include <string>

#include "doctest/doctest.h"
#include "harness_util.h"
#include "striso_engines.h"  // generated: every voice engine, STRISO_TEST_ENGINES(X, XP)
#include "striso_dsp/fx.h"

namespace {

constexpr int kSampleRate = 48000;
constexpr int kVoices = 10;
// a blow-up shows as NaN/Inf or as an absurd level; extreme but legal parameters may be loud
// (the firmware clips at +-1 after the x0.25 volume), so the bound only catches instability
constexpr float kFuzzPeakBound = 64.0f;

struct Rng {
	uint32_t s;
	float next() { s = s * 1664525u + 1013904223u; return static_cast<float>(s >> 8) * (1.0f / 16777216.0f); }
	float range(float lo, float hi) { return lo + next() * (hi - lo); }
};

// one random parameter value: mostly in range, often the extremes, sometimes far outside or NaN/Inf
float fuzz_value(Rng& r, const strisy::ParamDesc& d) {
	const float u = r.next(), span = d.max - d.min;
	if (u < 0.55f) return r.range(d.min, d.max);
	if (u < 0.70f) return d.min;
	if (u < 0.85f) return d.max;
	if (u < 0.92f) return r.next() < 0.5f ? d.min - 10.0f * span - 1.0f : d.max + 10.0f * span + 1.0f;
	if (u < 0.96f) return NAN;
	return r.next() < 0.5f ? INFINITY : -INFINITY;
}

// playing: notes on and off, glides, vibrato, tilt, strikes, pedal and motion
void fuzz_controls(Rng& r, strisy::VoiceControls* vc, strisy::GlobalControls& g, int nv) {
	for (int v = 0; v < nv; v++) {
		strisy::VoiceControls& c = vc[v];
		const float u = r.next();
		if (u < 0.03f) {  // press / release
			const bool on = c.pres <= 0.0f;
			c.pres = on ? r.range(0.05f, 1.3f) : 0.0f;
			c.vpres = on ? r.range(0.0f, 1.0f) : -r.range(0.0f, 1.0f);
			if (on) c.note = r.range(20.0f, 110.0f);
		} else if (c.pres > 0.0f) {
			c.pres = std::fmin(1.5f, std::fmax(0.01f, c.pres + r.range(-0.05f, 0.05f)));
			c.vpres = r.range(-0.2f, 0.2f);
			c.note += r.range(-0.1f, 0.1f);
			c.but_x = r.range(-1.0f, 1.0f);
			c.but_y = r.range(-1.0f, 1.0f);
		}
	}
	if (r.next() < 0.02f) {
		g.acc_abs = r.range(0.0f, 3.5f);
		g.acc_x = r.range(-1.0f, 1.0f);
		g.rot_x = r.range(-1.0f, 1.0f);
		g.rot_y = r.range(-1.0f, 1.0f);
		g.rot_z = r.range(-1.0f, 1.0f);
		g.pedal = r.range(0.0f, 1.0f);
	}
}

template <class E> void check_table(const char* engine) {
	using P = strisy::EngineParams<E>;
	static E e;
	std::set<std::string> names;
	for (int i = 0; i < P::count; i++) {
		const strisy::ParamDesc* d = P::desc(i);
		CAPTURE(engine);
		CAPTURE(i);
		REQUIRE(d != nullptr);
		REQUIRE(d->name != nullptr);
		CHECK(std::strlen(d->name) > 0);
		CHECK(d->group != nullptr);
		CHECK(names.insert(d->name).second);  // unique
		CHECK(d->min <= d->max);
		CHECK(static_cast<int>(d->kind) <= 2);
		CHECK(P::ptr(e, i) != nullptr);
	}
	CHECK(P::desc(P::count) == nullptr);
}

template <class E, class Init, class SetPreset, class Process>
void fuzz_engine(const char* engine, int preset, uint32_t seed, Init init, SetPreset set_preset, Process process) {
	using P = strisy::EngineParams<E>;
	static E e;
	init(e);
	set_preset(e, preset);
	for (int v = 0; v < kVoices; v++) e.vc[v] = {69.0f, 0.0f, 0.0f, 0.0f, 0.0f};
	e.g = {1.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f, 0.0f, 3.0f / 16};
	Rng r{seed};
	long bad = 0;
	float peak = 0.0f;
	const int blocks = 4 * kSampleRate / 32;  // 4 s
	for (int b = 0; b < blocks; b++) {
		fuzz_controls(r, e.vc, e.g, kVoices);
		if (P::count > 0) {
			const int writes = 1 + static_cast<int>(r.next() * 4);
			for (int k = 0; k < writes; k++) {
				const int i = static_cast<int>(r.next() * P::count) % P::count;
				harness::param_write(e, i, fuzz_value(r, *P::desc(i)));
			}
		}
		float out[32];
		process(e, out);
		for (float x : out) {
			if (!std::isfinite(x)) bad++;
			else peak = std::fmax(peak, std::fabs(x));
		}
	}
	CAPTURE(engine);
	CAPTURE(preset);
	CHECK(bad == 0);
	CHECK(peak <= kFuzzPeakBound);
	MESSAGE(std::string(engine) << " preset " << preset << ": fuzz peak " << peak);
}

}  // namespace

TEST_CASE("parameter tables are sane") {
#define STRISO_CHECK_TABLE(ns) check_table<ns::Engine>(#ns);
	STRISO_TEST_ENGINES(STRISO_CHECK_TABLE, STRISO_CHECK_TABLE)
	check_table<fx::Engine>("fx");
}

TEST_CASE("live parameter fuzz: every engine and preset stays finite and bounded") {
	const auto no_preset = [](auto&, int) {};
#define STRISO_FUZZ(ns) SUBCASE(#ns) {                                                         \
		fuzz_engine<ns::Engine>(#ns, -1, 1, [](ns::Engine& e) { ns::init(e, kSampleRate, kVoices); }, \
		                        no_preset, [](ns::Engine& e, float* o) { ns::process(e, o); });    \
	}
#define STRISO_FUZZ_PRESETS(ns) SUBCASE(#ns) {                                                 \
		for (int p = 0; p < ns::NUM_PRESETS; p++)                                                  \
			fuzz_engine<ns::Engine>(#ns, p, 1u + static_cast<uint32_t>(p),                         \
			                        [](ns::Engine& e) { ns::init(e, kSampleRate, kVoices); },      \
			                        [](ns::Engine& e, int q) { ns::set_preset(e, q); },            \
			                        [](ns::Engine& e, float* o) { ns::process(e, o); });           \
	}
	STRISO_TEST_ENGINES(STRISO_FUZZ, STRISO_FUZZ_PRESETS)
}

TEST_CASE("live parameter fuzz: master effects stay finite and bounded") {
	using P = strisy::EngineParams<fx::Engine>;
	static fx::Engine e;
	fx::init(e, kSampleRate);
	Rng r{7};
	long bad = 0;
	float peak = 0.0f;
	for (int b = 0; b < 8 * kSampleRate / 32; b++) {
		const int writes = 1 + static_cast<int>(r.next() * 4);
		for (int k = 0; k < writes; k++) {
			const int i = static_cast<int>(r.next() * P::count) % P::count;
			harness::param_write(e, i, fuzz_value(r, *P::desc(i)));
		}
		float in[32], l[32], rr[32];
		const bool loud = (b / 200) % 2 == 0;  // bursts of loud noise, then silence (tails)
		for (float& x : in) x = loud ? r.range(-4.0f, 4.0f) : 0.0f;
		fx::process(e, in, l, rr);
		for (int n = 0; n < 32; n++) {
			if (!std::isfinite(l[n]) || !std::isfinite(rr[n])) bad++;
			else peak = std::fmax(peak, std::fmax(std::fabs(l[n]), std::fabs(rr[n])));
		}
	}
	CHECK(bad == 0);
	CHECK(peak <= kFuzzPeakBound);
	MESSAGE("fx: fuzz peak " << peak);
}
