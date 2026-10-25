// The modal engine's motor tremolo after its rate went to the extremes and back (the contract's
// parameter round trip, sdk/tests/contract.cpp): the rate reads back exactly and nothing is stuck;
// the one lasting difference is the tremolo's phase, the integral of the rate over the excursion
// (a vibraphone motor that ran faster for a moment is ahead by that much). White box: with the
// phase copied over from an untouched engine the phrase is the untouched one, bit for bit.
#include <cmath>
#include <cstring>
#include <vector>

#include "doctest/doctest.h"
#include "striso_dsp/voice_modal.h"

namespace {

constexpr int kSampleRate = 48000, kVoices = 10;

modal::Engine* fresh() {
	auto* e = new modal::Engine();
	modal::init(*e, kSampleRate, kVoices);
	modal::set_preset(*e, 0); // the vibraphone (motor tremolo)
	return e;
}

float* tremrate(modal::Engine& e) {
	using EP = strisy::EngineParams<modal::Engine>;
	for (int i = 0; i < EP::count; i++)
		if (std::strcmp(EP::desc(i)->name, "tremrate") == 0) return EP::ptr(e, i);
	return nullptr;
}

// n blocks; with the phrase: three keys pressed hard enough for the tremolo (above tremthr)
std::vector<float> play(modal::Engine& e, int n, bool phrase) {
	std::vector<float> out;
	for (int b = 0; b < n; b++) {
		if (phrase) {
			const float notes[3] = {48, 55, 64};
			for (int v = 0; v < 3; v++) {
				e.vc[v].note = notes[v];
				e.vc[v].pres = b < 900 ? 0.95f : 0.0f;
				e.vc[v].vpres = b == 0 ? 0.6f : 0.0f;
			}
		}
		float buf[modal::BLOCK];
		modal::process(e, buf);
		out.insert(out.end(), buf, buf + modal::BLOCK);
	}
	return out;
}

double diff_db(const std::vector<float>& a, const std::vector<float>& b) {
	double ea = 0, ed = 0;
	for (size_t i = 0; i < a.size(); i++) {
		ea += static_cast<double>(a[i]) * a[i];
		ed += (static_cast<double>(a[i]) - b[i]) * (a[i] - b[i]);
	}
	return 10.0 * std::log10(ed / ea + 1e-30);
}

} // namespace

TEST_CASE("modal tremrate: after its extremes only the tremolo's phase differs") {
	modal::Engine* ref = fresh();
	modal::Engine* moved = fresh();
	float* rate = tremrate(*moved);
	REQUIRE(rate != nullptr);
	const float v0 = *rate;
	play(*ref, 600, false);
	*rate = 10.0f;
	play(*moved, 40, false);
	*rate = 1.0f;
	play(*moved, 40, false);
	*rate = v0;
	play(*moved, 520, false);
	CHECK(*tremrate(*moved) == v0);
	CHECK(moved->trem_phase != ref->trem_phase); // the excursion moved the phase

	// as the contract hears it: the tremolo out of phase
	modal::Engine* a = new modal::Engine(*ref);
	modal::Engine* b = new modal::Engine(*moved);
	const std::vector<float> ra = play(*a, 1500, true), rb = play(*b, 1500, true);
	CHECK(diff_db(ra, rb) > -60.0);

	// the same with the phase copied over: bit for bit (nothing else was left behind)
	moved->trem_phase = ref->trem_phase;
	const std::vector<float> r1 = play(*ref, 1500, true), r2 = play(*moved, 1500, true);
	CHECK(std::memcmp(r1.data(), r2.data(), r1.size() * sizeof(float)) == 0);
	delete a;
	delete b;
	delete ref;
	delete moved;
}
