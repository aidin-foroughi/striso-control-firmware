// The host shell (shell.h): MIDI in, any frame count per call, an engine change.
#include <cmath>
#include <vector>

#include "doctest/doctest.h"
#include "striso_sdk/shell.h"

namespace sdk = striso::sdk;

namespace {

// frames of the shell in calls of the given sizes (cycled), MIDI note 60 on at 0, off at 0.5 s
std::vector<float> render(const std::vector<int>& sizes, int frames) {
	const sdk::EngineEntry* va = sdk::find_engine("va");
	REQUIRE(va != nullptr);
	sdk::Shell s(*va, 48000);
	s.midi(0x90, 60, 100);
	std::vector<float> l(static_cast<size_t>(frames)), r(static_cast<size_t>(frames));
	bool off = false;
	for (int n = 0, k = 0; n < frames; k++) {
		if (!off && n >= 24000) {
			s.midi(0x80, 60, 64);
			off = true;
		}
		const int m = std::min(sizes[static_cast<size_t>(k) % sizes.size()], frames - n);
		s.process(&l[static_cast<size_t>(n)], &r[static_cast<size_t>(n)], m);
		n += m;
	}
	l.insert(l.end(), r.begin(), r.end());
	return l;
}

} // namespace

TEST_CASE("shell: a MIDI note sounds and ends; the call size makes no difference") {
	const int frames = 48000 * 3;
	const std::vector<float> a = render({32}, frames);
	const std::vector<float> b = render({1, 7, 64, 333, 13}, frames);
	float peak = 0, tail = 0;
	for (int n = 0; n < frames; n++) {
		peak = std::fmax(peak, std::fabs(a[static_cast<size_t>(n)]));
		if (n > 48000 * 5 / 2) tail = std::fmax(tail, std::fabs(a[static_cast<size_t>(n)]));
	}
	CHECK(peak > 0.01f);
	CHECK(tail < 1e-3f);
	// (the note off lands on the block boundary at or after 0.5 s either way: 24000 = 750 blocks)
	CHECK(a == b);
}

TEST_CASE("shell: every engine plays a MIDI note through the shell") {
	for (const sdk::EngineEntry* e : sdk::engines()) {
		CAPTURE(e->info->name);
		sdk::Shell s(*e, 48000, 10, false);
		s.midi(0x90, 57, 110);
		std::vector<float> l(9600), r(9600);
		s.process(l.data(), r.data(), 9600);
		float peak = 0;
		bool finite = true;
		for (float x : l) {
			finite &= std::isfinite(x);
			peak = std::fmax(peak, std::fabs(x));
		}
		CHECK(finite);
		CHECK(peak > 1e-3f);
	}
}
