// The engine contract (docs/engines.md) checked on one engine: this file is compiled once per
// engine and linked to that engine's library, striso_engine_<name> (STRISO_ENGINE_ENTRY names its
// entry), so every engine gets every check without a line of its own (sdk/CMakeLists.txt).
//   info              the declaration is complete and consistent
//   init              silent and idle with no input; the first note sounds
//   release           silent (< 1e-4) and idle within its declared tail after the last release
//   reset             back to silence, the preset and the live parameter values kept; afterwards
//                     it plays exactly as a fresh engine with that preset and those values
//   determinism       the same input gives the same output, bit for bit
//   independence      two instances don't share state (no globals)
//   fuzz              random playing and parameter writes, every preset: finite and bounded
//   parameters        the table is sane; writes are clamped, rounded, NaN ignored
//   presets           names, numbering; switching while notes sound stays finite and bounded
//   voice stealing    note jumps on held voices, instant re-strikes, every voice at once
//   inputs            an input it doesn't declare makes no difference to its output
//   cpu               a smoke check: the worst case (every voice, full pressure) runs much faster
//                     than real time on the PC
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "doctest/doctest.h"
#include "striso_sdk/engines.h"

const striso::sdk::EngineEntry& STRISO_ENGINE_ENTRY();

namespace {

namespace dsp = striso::dsp;
namespace sdk = striso::sdk;
using dsp::GlobalControls;
using dsp::VoiceControls;

constexpr int SR = 48000;
constexpr float SILENT = 1e-4f;      // the render checks' silence
constexpr float PEAK_PLAYING = 32.0f; // every voice at full pressure (the board clips at 4 after its x0.25 volume)
constexpr float PEAK_FUZZ = 64.0f;   // extreme but legal parameters may be loud: only blow-ups fail

[[nodiscard]] const sdk::EngineEntry& entry() { return STRISO_ENGINE_ENTRY(); }
[[nodiscard]] const dsp::EngineInfo& info() { return *entry().info; }
[[nodiscard]] int nvoices() { return std::min(10, info().budget.max_voices); }
[[nodiscard]] int blocks(double s) { return static_cast<int>(s * SR / dsp::BLOCK); }

// engines that are known not to keep the contract yet, and why (docs/engines.md); their check
// is expected to fail, and fails once they keep it, so the entry gets removed
[[nodiscard]] bool shares_static_memory() { return std::strcmp(info().name, "trumpet") == 0; }

// every preset, or -1 for an engine without presets
[[nodiscard]] std::vector<int> presets() {
	std::vector<int> p;
	for (int i = 0; i < info().num_presets; i++) p.push_back(i);
	if (p.empty()) p.push_back(-1);
	return p;
}

[[nodiscard]] sdk::Engine fresh(int preset) {
	sdk::Engine e(entry());
	e.init(SR, nvoices());
	if (preset >= 0) e.set_preset(preset);
	return e;
}

// The keys: pressure glides to its target at 1 per 20 ms, vpres is its speed as the harness
// performances compute it, gate/strike/release follow (track_key): every engine sees a real
// strike and release.
struct Keys {
	std::vector<VoiceControls> vc;
	std::vector<float> target;
	GlobalControls g = dsp::globals_at_rest;
	explicit Keys(int nv) : vc(static_cast<size_t>(nv), dsp::voice_at_rest), target(static_cast<size_t>(nv), 0.0f) {}
	void press(int v, float note, float pres) {
		vc[static_cast<size_t>(v)].note = note;
		target[static_cast<size_t>(v)] = pres;
	}
	void release(int v) { target[static_cast<size_t>(v)] = 0.0f; }
	void release_all() { std::fill(target.begin(), target.end(), 0.0f); }
	void step() {
		constexpr float rate = 1.0f / 30; // per block: 1 in 20 ms
		for (size_t v = 0; v < vc.size(); v++) {
			const float p = vc[v].pres, t = target[v];
			const float np = p < t ? std::min(t, p + rate) : std::max(t, p - rate);
			vc[v].vpres = std::clamp((np - p) * (static_cast<float>(SR) / dsp::BLOCK) * 0.0125f, -1.0f, 1.0f);
			vc[v].pres = np;
			dsp::track_key(vc[v]);
		}
	}
};

struct Stats {
	float peak = 0.0f;
	long nonfinite = 0;
	int last_loud = -1; // last block with a sample above SILENT
	int blocks = 0;
};

// n blocks of the keys (each block: keys.step(), then before(b) may change anything)
Stats play(sdk::Engine& e, Keys& k, int n, std::vector<float>* keep = nullptr, const std::function<void(int)>& before = {}) {
	Stats s;
	for (int b = 0; b < n; b++) {
		k.step();
		if (before) before(b);
		float out[dsp::BLOCK];
		float* const o[1] = {out};
		e.process(k.vc.data(), k.g, o);
		for (float x : out) {
			if (!std::isfinite(x)) s.nonfinite++;
			else {
				s.peak = std::max(s.peak, std::fabs(x));
				if (std::fabs(x) > SILENT) s.last_loud = s.blocks;
			}
		}
		if (keep) keep->insert(keep->end(), out, out + dsp::BLOCK);
		s.blocks++;
	}
	return s;
}

// a phrase: a three note chord with vibrato and tilt, a melody on top, all released at 1.5 s
void phrase(Keys& k, int b, float transpose = 0.0f) {
	const double t = static_cast<double>(b) * dsp::BLOCK / SR;
	const int nv = static_cast<int>(k.vc.size());
	if (b == 0) {
		k.press(0, 48 + transpose, 0.6f);
		if (nv > 1) k.press(1, 55 + transpose, 0.7f);
		if (nv > 2) k.press(2, 64 + transpose, 0.5f);
	}
	if (nv > 3) {
		const int step = static_cast<int>(t / 0.25);
		if (t < 1.5 && b == blocks(step * 0.25)) k.press(3, static_cast<float>(67 + 2 * (step % 4)) + transpose, 0.8f);
		if (t < 1.5 && b == blocks(step * 0.25 + 0.18)) k.release(3);
	}
	for (int v = 0; v < std::min(nv, 3); v++) {
		VoiceControls& c = k.vc[static_cast<size_t>(v)];
		c.but_x = static_cast<float>(0.3 * std::sin(2 * M_PI * 5 * t + v));
		c.but_y = static_cast<float>(0.6 * std::sin(2 * M_PI * 0.7 * t + v));
	}
	if (b == blocks(1.5)) k.release_all();
}

struct Rng {
	uint32_t s;
	float next() {
		s = s * 1664525u + 1013904223u;
		return static_cast<float>(s >> 8) * (1.0f / 16777216.0f);
	}
	float range(float lo, float hi) { return lo + next() * (hi - lo); }
};

// random playing: presses and releases, glides, tilt, strikes, pedal and motion, extremes included
void fuzz_controls(Rng& r, Keys& k) {
	for (size_t v = 0; v < k.vc.size(); v++) {
		VoiceControls& c = k.vc[v];
		const float u = r.next();
		if (u < 0.03f) { // press / release (instantly: the pressure jumps)
			const bool on = c.pres <= 0.0f;
			c.pres = k.target[v] = on ? r.range(0.05f, 1.3f) : 0.0f;
			c.vpres = on ? r.range(0.0f, 1.0f) : -r.range(0.0f, 1.0f);
			if (on) c.note = r.range(10.0f, 120.0f);
		} else if (c.pres > 0.0f) {
			c.pres = k.target[v] = std::clamp(c.pres + r.range(-0.05f, 0.05f), 0.01f, 1.5f);
			c.vpres = r.range(-0.2f, 0.2f);
			c.note += r.range(-0.1f, 0.1f);
			c.but_x = r.range(-1.0f, 1.0f);
			c.but_y = r.range(-1.0f, 1.0f);
		} else {
			c.vpres = 0.0f;
		}
		dsp::track_key(c);
	}
	if (r.next() < 0.02f) {
		k.g.acc_abs = r.range(0.0f, 3.5f);
		k.g.acc_x = r.range(-1.0f, 1.0f);
		k.g.acc_y = r.range(-1.0f, 1.0f);
		k.g.acc_z = r.range(-1.0f, 1.0f);
		k.g.rot_x = r.range(-1.0f, 1.0f);
		k.g.rot_y = r.range(-1.0f, 1.0f);
		k.g.rot_z = r.range(-1.0f, 1.0f);
		k.g.pedal = r.range(0.0f, 1.0f);
	}
}

// one random parameter value: mostly in range, often the extremes, sometimes far outside or NaN/Inf
[[nodiscard]] float fuzz_value(Rng& r, const dsp::ParamDesc& d) {
	const float u = r.next(), span = d.max - d.min;
	if (u < 0.55f) return r.range(d.min, d.max);
	if (u < 0.70f) return d.min;
	if (u < 0.85f) return d.max;
	if (u < 0.92f) return r.next() < 0.5f ? d.min - 10.0f * span - 1.0f : d.max + 10.0f * span + 1.0f;
	if (u < 0.96f) return NAN;
	return r.next() < 0.5f ? INFINITY : -INFINITY;
}

// a random performance with parameter writes, n blocks; the same seed plays the same
std::vector<float> fuzz_render(sdk::Engine& e, uint32_t seed, int n, Stats* st = nullptr) {
	Keys k(e.voices());
	Rng r{seed};
	std::vector<float> out;
	const int np = e.num_params();
	const Stats s = play(e, k, n, &out, [&](int) {
		fuzz_controls(r, k);
		if (np > 0)
			for (int w = 1 + static_cast<int>(r.next() * 4); w > 0; w--) {
				const int i = static_cast<int>(r.next() * np) % np;
				e.set_param(i, fuzz_value(r, *e.param_desc(i)));
			}
	});
	if (st) *st = s;
	return out;
}

[[nodiscard]] bool same_bits(const std::vector<float>& a, const std::vector<float>& b) {
	return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

} // namespace

TEST_CASE("info: the declaration is complete") {
	const dsp::EngineInfo& in = info();
	CAPTURE(in.name);
	REQUIRE(in.name != nullptr);
	CHECK(std::strlen(in.name) > 0);
	REQUIRE(in.summary != nullptr);
	CHECK(std::strlen(in.summary) > 10);
	CHECK(entry().header != nullptr);
	CHECK((in.channels == 1 || in.channels == 2));
	CHECK(in.budget.max_voices >= 1);
	CHECK(in.budget.instr_per_block > 0);
	CHECK(in.budget.instr_per_block < 314000); // a block's cycles on the board (480 MHz, 48828 Hz)
	CHECK(in.budget.tail_seconds > 0.0f);
	CHECK(in.budget.tail_seconds <= 30.0f);
	CHECK((in.inputs & dsp::in_pressure) != 0);
	CHECK(in.state_bytes > 0);
	CHECK(in.state_bytes < 128 * 1024); // the board's DTCM, for everything
	CHECK(in.num_presets >= 0);
	CHECK(in.num_params >= 0);
	sdk::Engine e(entry());
	e.init(SR, nvoices());
	CHECK(e.voices() == nvoices());
	CHECK((in.num_presets > 0 ? e.preset() >= 0 && e.preset() < in.num_presets : e.preset() == -1));
	MESSAGE(std::string(in.name) << ": " << in.num_presets << " presets, " << in.num_params << " parameters, state " << in.state_bytes
	                << " B, fast " << in.fast_bytes << " B, large " << in.large_bytes << " B");
}

TEST_CASE("init: silent and idle with no input; a note sounds") {
	for (int p : presets()) {
		CAPTURE(std::string(info().name));
		CAPTURE(p);
		sdk::Engine e = fresh(p);
		Keys k(e.voices());
		const Stats idle = play(e, k, blocks(1.0));
		CHECK(idle.nonfinite == 0);
		CHECK(idle.peak < 1e-5f);
		CHECK(e.active_voices() == 0);
		k.press(0, 60, 0.7f);
		const Stats note = play(e, k, blocks(1.0));
		CHECK(note.nonfinite == 0);
		CHECK(note.peak > 1e-3f);
		CHECK(note.peak < PEAK_PLAYING);
	}
}

TEST_CASE("release: silent and idle within the declared tail") {
	const float tail = info().budget.tail_seconds;
	float worst = 0.0f;
	for (int p : presets()) {
		CAPTURE(std::string(info().name));
		CAPTURE(p);
		sdk::Engine e = fresh(p);
		Keys k(e.voices());
		const Stats s = play(e, k, blocks(1.5 + tail + 0.5), nullptr, [&](int b) { phrase(k, b); });
		CHECK(s.nonfinite == 0);
		const float silent_after = static_cast<float>(s.last_loud + 1) * dsp::BLOCK / SR - 1.5f;
		worst = std::max(worst, silent_after);
		CHECK(silent_after <= tail);
		CHECK(e.active_voices() == 0);
	}
	MESSAGE(std::string(info().name) << ": silent " << worst << " s after the last release (declared " << tail << " s)");
}

TEST_CASE("reset: back to silence, preset and parameters kept, then as fresh") {
	for (int p : presets()) {
		CAPTURE(std::string(info().name));
		CAPTURE(p);
		sdk::Engine e = fresh(p);
		// live edits: every parameter to a point between its value and its maximum
		std::vector<float> want;
		for (int i = 0; i < e.num_params(); i++) {
			const dsp::ParamDesc* d = e.param_desc(i);
			e.set_param(i, *e.param(i) + 0.37f * (d->max - *e.param(i)));
			want.push_back(*e.param(i));
		}
		Keys k(e.voices());
		play(e, k, blocks(0.5), nullptr, [&](int b) { phrase(k, b); });
		e.reset();
		CHECK(e.preset() == (p >= 0 ? p : -1));
		for (int i = 0; i < e.num_params(); i++) {
			CAPTURE(e.param_desc(i)->name);
			CHECK(*e.param(i) == want[static_cast<size_t>(i)]);
		}
		Keys rest(e.voices());
		const Stats quiet = play(e, rest, blocks(0.2));
		CHECK(quiet.peak < 1e-5f);
		CHECK(e.active_voices() == 0);
		// and from here exactly as a fresh engine with that preset and those values (made after
		// this one is done: the independence of instances is a check of its own)
		Keys k1(e.voices());
		std::vector<float> a, b;
		play(e, k1, blocks(2.0), &a, [&](int n) { phrase(k1, n); });
		sdk::Engine f = fresh(p);
		for (int i = 0; i < f.num_params(); i++) f.set_param(i, want[static_cast<size_t>(i)]);
		Keys k2(f.voices());
		play(f, k2, blocks(0.2));
		play(f, k2, blocks(2.0), &b, [&](int n) { phrase(k2, n); });
		CHECK(same_bits(a, b));
	}
}

TEST_CASE("determinism: the same input, the same output bit for bit") {
	for (int p : presets()) {
		CAPTURE(std::string(info().name));
		CAPTURE(p);
		// (one after the other: two at once is the independence check)
		const auto once = [p]() {
			sdk::Engine e = fresh(p);
			return fuzz_render(e, 11u + static_cast<uint32_t>(p), blocks(2.0));
		};
		const std::vector<float> first = once();
		CHECK(same_bits(first, once()));
	}
}

TEST_CASE("independence: two instances share nothing" * doctest::should_fail(shares_static_memory())) {
	const int p = presets().back();
	CAPTURE(std::string(info().name));
	// alone, one after the other
	std::vector<float> alone_a, alone_b;
	{
		sdk::Engine a = fresh(p);
		Keys ka(a.voices());
		play(a, ka, blocks(2.0), &alone_a, [&](int b) { phrase(ka, b); });
	}
	{
		sdk::Engine b = fresh(p);
		Keys kb(b.voices());
		play(b, kb, blocks(2.0), &alone_b, [&](int n) { phrase(kb, n, 5.0f); });
	}
	// interleaved, block by block
	sdk::Engine c = fresh(p), d = fresh(p);
	Keys kc(c.voices()), kd(d.voices());
	std::vector<float> both_c, both_d;
	for (int n = 0; n < blocks(2.0); n++) {
		play(c, kc, 1, &both_c, [&](int) { phrase(kc, n); });
		play(d, kd, 1, &both_d, [&](int) { phrase(kd, n, 5.0f); });
	}
	CHECK(same_bits(alone_a, both_c));
	CHECK(same_bits(alone_b, both_d));
}

TEST_CASE("fuzz: random playing and parameter writes stay finite and bounded") {
	for (int p : presets()) {
		CAPTURE(std::string(info().name));
		CAPTURE(p);
		sdk::Engine e = fresh(p);
		Stats s;
		fuzz_render(e, 1u + static_cast<uint32_t>(p + 1), blocks(4.0), &s);
		CHECK(s.nonfinite == 0);
		CHECK(s.peak <= PEAK_FUZZ);
	}
}

TEST_CASE("parameters: a sane table; writes clamp, round and ignore NaN") {
	sdk::Engine e = fresh(presets().front());
	const int n = e.num_params();
	CHECK(e.param_desc(n) == nullptr);
	CHECK(e.param_desc(-1) == nullptr);
	std::set<std::string> names;
	for (int i = 0; i < n; i++) {
		const dsp::ParamDesc* d = e.param_desc(i);
		CAPTURE(std::string(info().name));
		CAPTURE(i);
		REQUIRE(d != nullptr);
		REQUIRE(d->name != nullptr);
		CAPTURE(d->name);
		CHECK(std::strlen(d->name) > 0);
		CHECK(d->group != nullptr);
		CHECK(names.insert(d->name).second); // unique
		CHECK(std::isfinite(d->min));
		CHECK(std::isfinite(d->max));
		CHECK(d->min <= d->max);
		CHECK(static_cast<int>(d->kind) <= 2);
		if (d->kind != dsp::ParamKind::continuous) {
			CHECK(d->min == std::round(d->min));
			CHECK(d->max == std::round(d->max));
		}
		REQUIRE(e.param(i) != nullptr);
		const float v0 = *e.param(i);
		CHECK(!e.set_param(i, NAN));
		CHECK(*e.param(i) == v0);
		CHECK(e.set_param(i, d->max + 1000.0f));
		CHECK(*e.param(i) == d->max);
		CHECK(e.set_param(i, d->min - 1000.0f));
		CHECK(*e.param(i) == d->min);
		if (d->kind != dsp::ParamKind::continuous && d->max - d->min >= 1.0f) {
			CHECK(e.set_param(i, d->min + 0.6f));
			CHECK(*e.param(i) == d->min + 1.0f);
		}
		e.set_param(i, v0);
	}
	// every preset's values are inside the ranges
	for (int p : presets()) {
		sdk::Engine f = fresh(p);
		for (int i = 0; i < n; i++) {
			const dsp::ParamDesc* d = f.param_desc(i);
			CAPTURE(p);
			CAPTURE(d->name);
			CHECK(*f.param(i) >= d->min);
			CHECK(*f.param(i) <= d->max);
		}
	}
}

TEST_CASE("presets: names and numbering; switching while notes sound") {
	const int np = info().num_presets;
	sdk::Engine e = fresh(-1);
	std::set<std::string> names;
	for (int i = 0; i < np; i++) {
		CAPTURE(i);
		REQUIRE(e.preset_name(i) != nullptr);
		CHECK(std::strlen(e.preset_name(i)) > 0);
		CHECK(names.insert(e.preset_name(i)).second);
		e.set_preset(i);
		CHECK(e.preset() == i);
	}
	CHECK(e.preset_name(np) == nullptr);
	CHECK(e.preset_name(-1) == nullptr);
	if (np == 0) return;
	// a chord held while the presets go round, twice, every 0.1 s
	Keys k(e.voices());
	int next = 0;
	const Stats s = play(e, k, blocks(0.2 * np + 1.0), nullptr, [&](int b) {
		phrase(k, b % blocks(1.4));
		if (b % blocks(0.1) == 0) e.set_preset(next++ % np);
	});
	CAPTURE(std::string(info().name));
	CHECK(s.nonfinite == 0);
	CHECK(s.peak <= PEAK_FUZZ);
}

TEST_CASE("voice stealing: note jumps, instant re-strikes, every voice at once") {
	const float tail = info().budget.tail_seconds;
	for (int p : presets()) {
		CAPTURE(std::string(info().name));
		CAPTURE(p);
		sdk::Engine e = fresh(p);
		const int nv = e.voices();
		Keys k(nv);
		const Stats s = play(e, k, blocks(4.0 + tail + 0.5), nullptr, [&](int b) {
			const double t = static_cast<double>(b) * dsp::BLOCK / SR;
			if (t < 1.0) { // a held voice jumps every 0.1 s across the range, never released
				if (b % blocks(0.1) == 0) k.press(0, static_cast<float>(20 + (b / blocks(0.1) * 37) % 91), 0.7f);
			} else if (t < 2.0) { // every voice: 30 ms notes, re-struck one block after the release at a new pitch
				k.release(0);
				const int phase = b % blocks(0.03);
				for (int v = 0; v < nv; v++) {
					VoiceControls& c = k.vc[static_cast<size_t>(v)];
					if (phase == 0) {
						c.pres = 0.0f; // released at once
						c.vpres = -0.8f;
						k.target[static_cast<size_t>(v)] = 0.0f;
					} else if (phase == 1) {
						c.note = static_cast<float>(30 + (v * 7 + b) % 70);
						c.pres = 0.9f; // struck at once
						c.vpres = 1.0f;
						k.target[static_cast<size_t>(v)] = 0.9f;
					}
				}
			} else if (t < 4.0) { // every voice at full pressure, a cluster, then all released
				if (b == blocks(2.0))
					for (int v = 0; v < nv; v++) k.press(v, static_cast<float>(40 + 5 * v), 1.2f);
				if (b == blocks(3.5)) k.release_all();
			}
		});
		CHECK(s.nonfinite == 0);
		CHECK(s.peak <= PEAK_PLAYING);
		const float silent_after = static_cast<float>(s.last_loud + 1) * dsp::BLOCK / SR - 3.5f;
		CHECK(silent_after <= tail + 0.1f);
		CHECK(e.active_voices() == 0);
	}
}

TEST_CASE("inputs: an input the engine doesn't declare makes no difference") {
	struct Probe {
		uint32_t bit;
		const char* name;
		std::function<void(Keys&, int)> modulate;
	};
	const auto t_of = [](int b) { return static_cast<double>(b) * dsp::BLOCK / SR; };
	const Probe probes[] = {
	    {dsp::in_velocity, "velocity", [](Keys& k, int) { for (VoiceControls& c : k.vc) c.vpres = 0.0f; }},
	    {dsp::in_tilt_x, "tilt x", [&](Keys& k, int b) { for (VoiceControls& c : k.vc) c.but_x = static_cast<float>(0.9 * std::sin(2 * M_PI * 3 * t_of(b))); }},
	    {dsp::in_tilt_y, "tilt y", [&](Keys& k, int b) { for (VoiceControls& c : k.vc) c.but_y = static_cast<float>(0.9 * std::sin(2 * M_PI * 2 * t_of(b))); }},
	    {dsp::in_note_events, "gate, strike, release", [](Keys& k, int) { for (VoiceControls& c : k.vc) c.gate = c.strike = c.release = 0.0f; }},
	    {dsp::in_shake, "shake", [&](Keys& k, int b) { const double s = std::sin(2 * M_PI * 3 * t_of(b)); k.g.acc_abs = static_cast<float>(1 + 2.5 * s * s); }},
	    {dsp::in_accel, "acceleration", [&](Keys& k, int b) {
		     k.g.acc_x = static_cast<float>(std::sin(2 * M_PI * 0.5 * t_of(b)));
		     k.g.acc_y = static_cast<float>(std::sin(2 * M_PI * 0.7 * t_of(b)));
		     k.g.acc_z = static_cast<float>(-std::cos(2 * M_PI * 0.3 * t_of(b)));
	     }},
	    {dsp::in_rotation, "rotation", [&](Keys& k, int b) {
		     k.g.rot_x = static_cast<float>(0.8 * std::sin(2 * M_PI * 0.9 * t_of(b)));
		     k.g.rot_y = static_cast<float>(0.8 * std::sin(2 * M_PI * 1.3 * t_of(b) + 1));
		     k.g.rot_z = static_cast<float>(0.8 * std::sin(2 * M_PI * 1.7 * t_of(b) + 2));
	     }},
	    {dsp::in_pedal, "pedal", [](Keys& k, int) { k.g.pedal = 1.0f; }},
	};
	for (const Probe& pr : probes) {
		if (info().inputs & pr.bit) continue;
		for (int p : presets()) {
			CAPTURE(std::string(info().name));
			CAPTURE(pr.name);
			CAPTURE(p);
			std::vector<float> plain, modulated;
			{
				sdk::Engine a = fresh(p);
				Keys ka(a.voices());
				play(a, ka, blocks(2.5), &plain, [&](int n) { phrase(ka, n); });
			}
			sdk::Engine b = fresh(p);
			Keys kb(b.voices());
			play(b, kb, blocks(2.5), &modulated, [&](int n) {
				phrase(kb, n);
				pr.modulate(kb, n);
			});
			CHECK(same_bits(plain, modulated));
		}
	}
}

TEST_CASE("cpu: the worst case runs much faster than real time on the PC") {
	sdk::Engine e = fresh(presets().back());
	Keys k(e.voices());
	for (int v = 0; v < e.voices(); v++) k.press(v, static_cast<float>(36 + 4 * v), 1.0f);
	const auto t0 = std::chrono::steady_clock::now();
	const Stats s = play(e, k, blocks(2.0), nullptr, [&](int b) { phrase(k, b % blocks(1.4)); });
	const double host = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	const double rt = host / 2.0;
	MESSAGE(std::string(info().name) << ": " << e.voices() << " voices at full pressure: " << 100.0 * rt << " % of real time on this PC (the board: "
	                    << 100.0 * info().budget.instr_per_block / 314000 << " % declared)");
	CHECK(s.nonfinite == 0);
#ifndef __SANITIZE_ADDRESS__
	CHECK(rt < 0.5); // (no timing under the sanitizers)
#endif
}
