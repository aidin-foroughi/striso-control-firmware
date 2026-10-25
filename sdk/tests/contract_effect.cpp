// The effect contract (docs/engines.md) checked on the master effects (striso_effect_fx):
//   info, parameters (a sane table; writes clamp, round, ignore NaN), fuzz (random parameter
//   writes over bursts of loud noise and silence: finite and bounded), silence (silence in,
//   silence out after the tails), reset (silent, parameters kept), determinism, independence.
#include <cmath>
#include <cstdint>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "doctest/doctest.h"
#include "striso_sdk/engines.h"

namespace {

namespace dsp = striso::dsp;
namespace sdk = striso::sdk;
constexpr int SR = 48000;

struct Rng {
	uint32_t s;
	float next() {
		s = s * 1664525u + 1013904223u;
		return static_cast<float>(s >> 8) * (1.0f / 16777216.0f);
	}
	float range(float lo, float hi) { return lo + next() * (hi - lo); }
};

[[nodiscard]] float fuzz_value(Rng& r, const dsp::ParamDesc& d) {
	const float u = r.next(), span = d.max - d.min;
	if (u < 0.55f) return r.range(d.min, d.max);
	if (u < 0.70f) return d.min;
	if (u < 0.85f) return d.max;
	if (u < 0.92f) return r.next() < 0.5f ? d.min - 10.0f * span - 1.0f : d.max + 10.0f * span + 1.0f;
	if (u < 0.96f) return NAN;
	return r.next() < 0.5f ? INFINITY : -INFINITY;
}

// n blocks: noise bursts (or silence) in, the outputs kept; writes > 0: random parameter writes
std::vector<float> run(sdk::Effect& f, int n, uint32_t seed, bool noise, bool writes, float* peak = nullptr, long* bad = nullptr) {
	const dsp::EffectInfo& in = f.info();
	Rng r{seed};
	std::vector<float> out;
	std::vector<float> ib(static_cast<size_t>(in.inputs) * dsp::BLOCK), ob(static_cast<size_t>(in.outputs) * dsp::BLOCK);
	std::vector<const float*> ip;
	std::vector<float*> op;
	for (int c = 0; c < in.inputs; c++) ip.push_back(&ib[static_cast<size_t>(c) * dsp::BLOCK]);
	for (int c = 0; c < in.outputs; c++) op.push_back(&ob[static_cast<size_t>(c) * dsp::BLOCK]);
	for (int b = 0; b < n; b++) {
		if (writes && f.num_params() > 0)
			for (int w = 1 + static_cast<int>(r.next() * 4); w > 0; w--) {
				const int i = static_cast<int>(r.next() * f.num_params()) % f.num_params();
				f.set_param(i, fuzz_value(r, *f.param_desc(i)));
			}
		const bool loud = noise && (b / 200) % 2 == 0; // bursts of loud noise, then silence (tails)
		for (float& x : ib) x = loud ? r.range(-4.0f, 4.0f) : 0.0f;
		f.process(ip.data(), op.data());
		for (float x : ob) {
			if (!std::isfinite(x)) {
				if (bad) (*bad)++;
			} else if (peak) {
				*peak = std::fmax(*peak, std::fabs(x));
			}
		}
		out.insert(out.end(), ob.begin(), ob.end());
	}
	return out;
}

[[nodiscard]] bool same_bits(const std::vector<float>& a, const std::vector<float>& b) {
	return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

} // namespace

TEST_CASE("effect: info and parameters") {
	sdk::Effect f(sdk::fx_effect());
	f.init(SR);
	const dsp::EffectInfo& in = f.info();
	CHECK(std::strcmp(in.name, "fx") == 0);
	CHECK(std::strlen(in.summary) > 10);
	CHECK(in.inputs == 1);
	CHECK(in.outputs == 2);
	CHECK(in.instr_per_block > 0);
	CHECK(in.state_bytes > 0);
	CHECK(f.param_desc(f.num_params()) == nullptr);
	std::set<std::string> names;
	for (int i = 0; i < f.num_params(); i++) {
		const dsp::ParamDesc* d = f.param_desc(i);
		CAPTURE(i);
		REQUIRE(d != nullptr);
		CAPTURE(d->name);
		CHECK(names.insert(d->name).second);
		CHECK(d->group != nullptr);
		CHECK(d->min <= d->max);
		CHECK(*f.param(i) >= d->min);
		CHECK(*f.param(i) <= d->max);
		const float v0 = *f.param(i);
		CHECK(!f.set_param(i, NAN));
		CHECK(*f.param(i) == v0);
		CHECK(f.set_param(i, d->max + 100.0f));
		CHECK(*f.param(i) == d->max);
		f.set_param(i, v0);
	}
}

TEST_CASE("effect: random parameter writes over noise and silence stay finite and bounded") {
	sdk::Effect f(sdk::fx_effect());
	f.init(SR);
	float peak = 0.0f;
	long bad = 0;
	run(f, 8 * SR / dsp::BLOCK, 7u, true, true, &peak, &bad);
	CHECK(bad == 0);
	CHECK(peak <= 64.0f);
	MESSAGE("fx: fuzz peak " << peak);
}

TEST_CASE("effect: silence in, silence out; reset keeps the parameters") {
	sdk::Effect f(sdk::fx_effect());
	f.init(SR);
	for (int i = 0; i < f.num_params(); i++) // every stage on: the longest tails
		if (f.param_desc(i)->kind == dsp::ParamKind::continuous && std::strcmp(f.param_desc(i)->name, "bypass") != 0)
			f.set_param(i, *f.param(i) + 0.3f * (f.param_desc(i)->max - *f.param(i)));
	std::vector<float> want;
	for (int i = 0; i < f.num_params(); i++) want.push_back(*f.param(i));
	run(f, SR / dsp::BLOCK, 3u, true, false);
	f.reset();
	for (int i = 0; i < f.num_params(); i++) CHECK(*f.param(i) == want[static_cast<size_t>(i)]);
	float peak = 0.0f;
	run(f, SR / dsp::BLOCK, 3u, false, false, &peak);
	CHECK(peak < 1e-6f);
}

TEST_CASE("effect: deterministic and independent instances") {
	sdk::Effect a(sdk::fx_effect()), b(sdk::fx_effect());
	a.init(SR);
	b.init(SR);
	const std::vector<float> ra = run(a, 2 * SR / dsp::BLOCK, 5u, true, true);
	const std::vector<float> rb = run(b, 2 * SR / dsp::BLOCK, 5u, true, true);
	CHECK(same_bits(ra, rb));
	// interleaved with a third instance playing something else: still the same
	sdk::Effect c(sdk::fx_effect()), d(sdk::fx_effect());
	c.init(SR);
	d.init(SR);
	std::vector<float> rc;
	for (int n = 0; n < 2 * SR / dsp::BLOCK; n++) {
		(void)run(d, 1, 99u + static_cast<uint32_t>(n), true, false);
		const std::vector<float> one = run(c, 1, 5u, true, true);
		rc.insert(rc.end(), one.begin(), one.end());
	}
	// (c's random stream restarts every block: the same as a solo instance doing the same)
	sdk::Effect e(sdk::fx_effect());
	e.init(SR);
	std::vector<float> re;
	for (int n = 0; n < 2 * SR / dsp::BLOCK; n++) {
		const std::vector<float> one = run(e, 1, 5u, true, true);
		re.insert(re.end(), one.begin(), one.end());
	}
	CHECK(same_bits(rc, re));
}
