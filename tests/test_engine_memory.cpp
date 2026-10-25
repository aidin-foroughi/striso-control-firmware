// The engines' memory regions on a voice switch (engine_set.h, the board's shared arenas): an
// engine started in an arena another engine used starts from the same state as at power on.
// - init_writes_memory (engine.h): for each engine that declares it, init() over arbitrary data
//   gives the same region bytes and the same output as over zeros, so the host may skip zeroing.
// - EngineSet::start: for every engine of the board's VOICE=all set, a switch after another
//   engine played renders bit for bit what the engine renders from power on.
#include <cstdint>
#include <cstring>
#include <memory>
#include <type_traits>
#include <vector>

#include "doctest/doctest.h"
#include "awva/engine_set.h"
#include "awva/engines/accordion.h"
#include "awva/engines/bowed.h"
#include "awva/engines/brass.h"
#include "awva/engines/ep.h"
#include "awva/engines/modal.h"
#include "awva/engines/reed.h"
#include "striso_dsp/engines/strisy.h"
#include "awva/engines/trumpet.h"
#include "awva/engines/va.h"
#include "awva/engines/wind.h"

namespace dsp = awva;
namespace E = dsp::engines;

namespace {

constexpr int NV = 10, SR = 48000, BLOCKS = 300;

// a held chord with a strike, every voice, then the same controls every block
struct Performance {
	dsp::VoiceControls vc[NV];
	dsp::GlobalControls g = dsp::globals_at_rest;
	Performance() {
		for (int v = 0; v < NV; v++) {
			vc[v] = dsp::voice_at_rest;
			vc[v].note = 45.0f + 2.0f * static_cast<float>(v);
			vc[v].pres = 0.5f + 0.03f * static_cast<float>(v);
			vc[v].vpres = 0.8f;
		}
	}
	template <class F> std::vector<float> render(F&& process) {
		std::vector<float> out(BLOCKS * dsp::BLOCK);
		for (int b = 0; b < BLOCKS; b++) {
			for (int v = 0; v < NV; v++) dsp::track_key(vc[v]);
			float* const outs[1] = {out.data() + b * dsp::BLOCK};
			process(vc, g, outs);
		}
		return out;
	}
};

bool same_bits(const std::vector<float>& a, const std::vector<float>& b) {
	return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

// init over zeros and over each pattern: the regions' bytes after init, and the renders
template <class A> void check_init_writes_memory() {
	using F = typename A::FastMemory;
	using L = typename A::LargeMemory;
	const auto f0 = std::make_unique<F>(); // value-initialised: zeros
	const auto l0 = std::make_unique<L>();
	const auto e0 = std::make_unique<A>();
	e0->init(*f0, *l0, SR, NV);
	// the regions as init over zeros leaves them (the render below plays e0 on)
	std::vector<unsigned char> fz(sizeof(F)), lz(sizeof(L));
	std::memcpy(fz.data(), f0.get(), sizeof(F));
	std::memcpy(lz.data(), l0.get(), sizeof(L));
	for (const unsigned char pattern : {0xa5u, 0xffu, 0x01u}) { // junk, NaN floats, denormals
		CAPTURE(static_cast<int>(pattern));
		const auto f1 = std::make_unique<F>();
		const auto l1 = std::make_unique<L>();
		std::memset(f1.get(), pattern, sizeof(F));
		std::memset(l1.get(), pattern, sizeof(L));
		const auto e1 = std::make_unique<A>();
		e1->init(*f1, *l1, SR, NV);
		const auto report = [](const char* what, const void* x, const void* y, size_t n) {
			const auto* p = static_cast<const unsigned char*>(x);
			const auto* q = static_cast<const unsigned char*>(y);
			size_t first = n, last = 0, count = 0;
			for (size_t i = 0; i < n; i++)
				if (p[i] != q[i]) {
					first = first < i ? first : i;
					last = i;
					count++;
				}
			if (count) MESSAGE(what << ": " << count << " of " << n << " bytes differ, " << first << ".." << last);
			return count == 0;
		};
		if (!std::is_empty_v<F>) CHECK(report("fast", fz.data(), f1.get(), sizeof(F)));
		if (!std::is_empty_v<L>) CHECK(report("large", lz.data(), l1.get(), sizeof(L)));
		if (pattern == 0xa5u) {
			const auto a = Performance().render([&](auto* vc, auto& g, auto* outs) { e0->process(vc, g, outs); });
			const auto b = Performance().render([&](auto* vc, auto& g, auto* outs) { e1->process(vc, g, outs); });
			CHECK(same_bits(a, b));
		}
	}
}

} // namespace

TEST_CASE("engine memory: the engines with regions say their init writes them all, and it does (init_writes_memory)") {
	SUBCASE("bowed") {
		static_assert(dsp::init_writes_memory<E::bowed>);
		check_init_writes_memory<E::bowed>();
	}
	SUBCASE("brass") {
		static_assert(dsp::init_writes_memory<E::brass>);
		check_init_writes_memory<E::brass>();
	}
	SUBCASE("wind") {
		static_assert(dsp::init_writes_memory<E::wind>);
		check_init_writes_memory<E::wind>();
	}
	SUBCASE("reed") {
		static_assert(dsp::init_writes_memory<E::reed>);
		check_init_writes_memory<E::reed>();
	}
	SUBCASE("trumpet") {
		static_assert(dsp::init_writes_memory<E::trumpet>);
		check_init_writes_memory<E::trumpet>();
	}
}

TEST_CASE("engine memory: a switch starts every VOICE=all engine from its power-on state, after another") {
	using Set = dsp::EngineSet<E::strisy, E::va, E::modal, E::bowed, E::wind, E::ep, E::brass, E::trumpet, E::accordion>;
	static Set::States states, fresh_states;
	static Set::Fast fast, fresh_fast;
	static Set::Large large, fresh_large;
	const auto init = [](auto& e, auto& f, auto& l, auto) { e.init(f, l, SR, NV); };
	for (int k = 0; k < Set::count; k++) {
		CAPTURE(Set::info(k)->name);
		// from power on: zeroed objects and arenas
		std::memset(&fresh_states, 0, sizeof fresh_states);
		std::memset(&fresh_fast, 0, sizeof fresh_fast);
		std::memset(&fresh_large, 0, sizeof fresh_large);
		Set::start(k, fresh_states, fresh_fast, fresh_large, init);
		const auto want = Performance().render(
		    [&](auto* vc, auto& g, auto* outs) { Set::visit(k, fresh_states, [&](auto& e, auto) { e.process(vc, g, outs); }); });
		// after each other engine played in the same arenas
		for (int before = 0; before < Set::count; before++) {
			if (before == k) continue;
			CAPTURE(Set::info(before)->name);
			Set::start(before, states, fast, large, init);
			(void)Performance().render(
			    [&](auto* vc, auto& g, auto* outs) { Set::visit(before, states, [&](auto& e, auto) { e.process(vc, g, outs); }); });
			Set::start(k, states, fast, large, init);
			const auto got = Performance().render(
			    [&](auto* vc, auto& g, auto* outs) { Set::visit(k, states, [&](auto& e, auto) { e.process(vc, g, outs); }); });
			CHECK(same_bits(want, got));
		}
	}
}
// an engine without regions doesn't claim it
static_assert(!dsp::init_writes_memory<E::va>);
