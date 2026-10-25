// The small shared units of dsp/: dspmath.h, svf.h, params.h, engine.h's track_key, engine_set.h.
#include <algorithm>
#include <cmath>
#include <cstring>

#include "doctest/doctest.h"
#include "striso_dsp/dspmath.h"
#include "striso_dsp/engine_set.h"
#include "striso_dsp/engines/bowed.h"
#include "striso_dsp/engines/sine.h"
#include "striso_dsp/engines/strisy.h"
#include "striso_dsp/engines/trumpet.h"
#include "striso_dsp/params.h"
#include "striso_dsp/svf.h"

namespace dsp = striso::dsp;

TEST_CASE("dspmath: fastpow2 (p >= 0, as the engines use it) and tan0 approximate exp2 and tan") {
	for (int k = 0; k <= 87; k++) {
		const float p = 0.137f * static_cast<float>(k);
		CAPTURE(p);
		CHECK(std::fabs(strisy::fastpow2(p) / std::exp2(p) - 1.0f) < 1e-4f);
	}
	for (int k = 1; k < 30; k++) {
		const float a = 0.01f * static_cast<float>(k);
		CHECK(std::fabs(strisy::tan0(a) / std::tan(a) - 1.0f) < 1e-3f); // (K_f0: up to ~4 kHz at 48 kHz)
	}
	CHECK(strisy::rmax(1.0f, 2.0f) == 2.0f);
	CHECK(strisy::rmin(1.0f, 2.0f) == 1.0f);
}

TEST_CASE("svf: unity gain at DC, -3 dB at the cutoff for Q = 0.707, Pade tan") {
	const float sr = 48000.0f, fc = 1000.0f;
	const strisy::SvfCoef c = strisy::svf_from_g(std::tan(3.14159265f * fc / sr), 0.70710678f);
	// DC: the lowpass settles at the input
	strisy::SvfState s{0, 0};
	float v1 = 0, v2 = 0;
	for (int n = 0; n < 20000; n++) SVF_TICK(1.0f, s.ic1, s.ic2, c, v1, v2);
	CHECK(v2 == doctest::Approx(1.0f).epsilon(1e-4));
	// a sine at the cutoff: lowpass amplitude 1/sqrt(2)
	s = {0, 0};
	float peak = 0;
	for (int n = 0; n < 48000; n++) {
		SVF_TICK(std::sin(2 * 3.14159265f * fc * n / sr), s.ic1, s.ic2, c, v1, v2);
		if (n > 24000) peak = std::fmax(peak, std::fabs(v2));
	}
	CHECK(peak == doctest::Approx(0.70710678f).epsilon(2e-3));
	// the Pade coefficients are the tan coefficients (lowpass and bandpass terms)
	for (int k = 0; k < 11; k++) {
		const float f = 50.0f * std::pow(1.7f, static_cast<float>(k));
		const float x = 3.14159265f * f / sr;
		const strisy::SvfCoef a = strisy::svf_pade(x, 2.0f), b = strisy::svf_from_g(std::tan(x), 2.0f);
		CAPTURE(f);
		CHECK(a.a == doctest::Approx(b.a).epsilon(1e-5));
		CHECK(a.ga == doctest::Approx(b.ga).epsilon(1e-5));
		CHECK(a.g2a == doctest::Approx(b.g2a).epsilon(1e-4));
	}
}

TEST_CASE("params: write_param clamps, rounds unless continuous, ignores NaN") {
	const strisy::ParamDesc cont{"c", "g", -1.0f, 1.0f, strisy::ParamKind::continuous};
	const strisy::ParamDesc steps{"s", "g", 0.0f, 8.0f, strisy::ParamKind::steps};
	float v = 0.5f;
	CHECK(!strisy::write_param(&cont, &v, NAN));
	CHECK(v == 0.5f);
	CHECK(!strisy::write_param(nullptr, &v, 0.1f));
	CHECK(!strisy::write_param(&cont, nullptr, 0.1f));
	CHECK(strisy::write_param(&cont, &v, 0.25f));
	CHECK(v == 0.25f);
	CHECK(strisy::write_param(&cont, &v, 7.0f));
	CHECK(v == 1.0f);
	CHECK(strisy::write_param(&cont, &v, -INFINITY));
	CHECK(v == -1.0f);
	CHECK(strisy::write_param(&steps, &v, 2.4f));
	CHECK(v == 2.0f);
	CHECK(strisy::write_param(&steps, &v, 2.6f));
	CHECK(v == 3.0f);
	CHECK(strisy::write_param(&steps, &v, 100.0f));
	CHECK(v == 8.0f);
}

TEST_CASE("track_key: gate, strike and release from the key signals") {
	dsp::VoiceControls c = dsp::voice_at_rest;
	dsp::track_key(c);
	CHECK(c.gate == 0.0f);
	c.pres = 0.1f; // the key goes down
	c.vpres = 0.4f;
	dsp::track_key(c);
	CHECK(c.gate == 1.0f);
	CHECK(c.strike == 0.4f);
	c.pres = 0.5f; // faster
	c.vpres = 0.7f;
	dsp::track_key(c);
	CHECK(c.strike == 0.7f);
	c.vpres = 0.0f; // held
	dsp::track_key(c);
	CHECK(c.strike == 0.7f);
	c.pres = 0.0f; // released
	c.vpres = -0.3f;
	dsp::track_key(c);
	CHECK(c.gate == 0.0f);
	CHECK(c.release == 0.3f);
	CHECK(c.strike == 0.7f);
	c.vpres = -0.5f;
	dsp::track_key(c);
	CHECK(c.release == 0.5f);
	c.pres = 0.2f; // struck again
	c.vpres = 2.0f;
	dsp::track_key(c);
	CHECK(c.gate == 1.0f);
	CHECK(c.strike == 1.0f); // (clipped)
}

TEST_CASE("engine_set: visit reaches each engine with its own type and memory") {
	using Set = dsp::EngineSet<dsp::engines::strisy, dsp::engines::sine>;
	static_assert(Set::count == 2);
	static Set::States s;
	static Set::Fast f;
	static Set::Large l;
	for (int k = 0; k < Set::count; k++)
		Set::visit(k, s, f, l, [](auto& e, auto& fast, auto& large, auto) { e.init(fast, large, 48000, 6); });
	CHECK(std::strcmp(Set::info(0)->name, "strisy") == 0);
	CHECK(std::strcmp(Set::info(1)->name, "sine") == 0);
	CHECK(Set::info(2) == nullptr);
	int seen = 0;
	Set::visit(1, s, [&seen](auto& e, auto i) {
		seen = static_cast<int>(decltype(i)::value) + 10 * e.voices();
	});
	CHECK(seen == 61);
	seen = -1;
	Set::visit(5, s, [&seen](auto&, auto) { seen = 0; });
	CHECK(seen == -1);
}

namespace {
// bowed without its init_writes_memory: the arena's zeroing path (every engine with regions in
// the board's set says its init writes them, test_engine_memory.cpp proves it)
struct bowed_zeroed : dsp::engines::bowed {
	static constexpr bool init_writes_memory = false;
};
} // namespace

TEST_CASE("engine_set: one arena per region class, the largest engine's, and start gives the new engine its power-on state") {
	using B = bowed_zeroed;
	using T = dsp::engines::trumpet;
	using Set = dsp::EngineSet<B, T, dsp::engines::sine>;
	static_assert(Set::Fast::size == std::max(sizeof(B::FastMemory), sizeof(T::FastMemory)));
	static_assert(Set::Large::size == std::max(sizeof(B::LargeMemory), sizeof(T::LargeMemory)));
	static_assert(Set::Fast::size < sizeof(B::FastMemory) + sizeof(T::FastMemory), "shared, not added up");
	static_assert(!dsp::init_writes_memory<B> && dsp::init_writes_memory<T>);
	// the fit check: an arena smaller than a region doesn't hold it
	static_assert(!dsp::Arena<8, 4>::holds<B::FastMemory>());
	static_assert(dsp::Arena<sizeof(B::FastMemory), alignof(B::FastMemory)>::holds<B::FastMemory>());

	static Set::States s, s0;
	static Set::Fast fast, fast0;
	static Set::Large large, large0;
	// what ran before left its data in the arenas
	std::memset(fast.bytes, 0x5a, sizeof fast.bytes);
	std::memset(large.bytes, 0x5a, sizeof large.bytes);
	const auto zeroed = [](const void* p, size_t n) {
		const auto* b = static_cast<const unsigned char*>(p);
		for (size_t i = 0; i < n; i++)
			if (b[i] != 0) return false;
		return true;
	};
	for (int k : {1, 0}) { // trumpet (the smaller regions, init writes them), then bowed over it (zeroed)
		CAPTURE(k);
		bool before_init = false;
		size_t fast_n = 0, large_n = 0;
		Set::start(k, s, fast, large, [&](auto& e, auto& f, auto& l, auto) {
			CHECK(static_cast<void*>(&f) == static_cast<void*>(fast.bytes)); // the arena's start
			CHECK(static_cast<void*>(&l) == static_cast<void*>(large.bytes));
			before_init = zeroed(&f, sizeof f) && zeroed(&l, sizeof l);
			fast_n = sizeof f;
			large_n = sizeof l;
			e.init(f, l, 48000, 10);
		});
		// the engine without the trait gets its share zeroed before its init
		if (k == 0) CHECK(before_init);
		// either way, its regions are what init leaves over zeros (power on)
		std::memset(&fast0, 0, sizeof fast0);
		std::memset(&large0, 0, sizeof large0);
		Set::start(k, s0, fast0, large0, [](auto& e, auto& f, auto& l, auto) { e.init(f, l, 48000, 10); });
		CHECK(std::memcmp(fast.bytes, fast0.bytes, fast_n) == 0);
		CHECK(std::memcmp(large.bytes, large0.bytes, large_n) == 0);
	}
}
