// The voice allocator (striso_dsp/voice_allocator.h): the board's rule for which note plays
// which voice, on its own.
#include <cmath>

#include "doctest/doctest.h"
#include "striso_dsp/voice_allocator.h"

namespace {

using Alloc = striso::voices::VoiceAllocator<8, 3>;

// the notes' keys and level floors, as a host keeps them
struct Host {
	bool down[8] = {};
	float fl[8] = {};
	[[nodiscard]] bool held(int n) const { return down[n]; }
	[[nodiscard]] float floor(int n) const { return fl[n]; }
	void press(Alloc& a, int n, float level) {
		down[n] = true;
		fl[n] = level;
		a.set_level(n, level);
	}
	void release(int n) {
		down[n] = false;
		fl[n] = -1.0f;
	}
};

} // namespace

TEST_CASE("voice allocator: empty voices first, then the quietest released, then a quieter held one") {
	Alloc a;
	Host h;
	for (int n = 0; n < 3; n++) {
		h.press(a, n, 0.5f + 0.1f * n);
		CHECK(a.start(h, n) == n); // the empty voices, in order
	}
	CHECK(a.voice(0) == 0);
	CHECK(a.owner(2) == 2);
	h.release(1);
	h.release(2);
	a.raise_level(1, -1.0f); // (a floor below the level changes nothing)
	CHECK(a.level(1) == doctest::Approx(0.6f));
	h.press(a, 3, 0.9f);
	CHECK(a.start(h, 3) == 1); // released: 1 (0.6) is quieter than 2 (0.7)
	CHECK(a.voice(1) == -1);
	h.press(a, 4, 0.9f);
	CHECK(a.start(h, 4) == 2); // the other released one
	h.press(a, 5, 0.8f);
	CHECK(a.start(h, 5) == 0); // all held: 0 (0.5) is quieter than the new note
	CHECK(a.voice(0) == -1);
	h.press(a, 6, 0.1f);
	CHECK(a.start(h, 6) == -1); // quieter than every held note: none
	h.press(a, 5, 0.8f);
	CHECK(a.start(h, 5) == 0); // a repeated note keeps its voice
}

TEST_CASE("voice allocator: first voice, hand-over, the level decay, fewer voices") {
	Alloc a;
	Host h;
	h.press(a, 0, 1.0f);
	CHECK(a.start(h, 0, 1) == 1); // voice 0 is the host's (the board's Mono mode)
	a.take(7, 0);
	CHECK(a.voice(7) == 0);
	a.hand_over(0, 6); // 0's note goes on as 6
	CHECK(a.voice(0) == -1);
	CHECK(a.voice(6) == 1);
	h.press(a, 6, 1.0f);
	h.release(6);
	a.tick(h); // x 0.998 - 0.0005, not below the floor (-1 after the release)
	CHECK(a.level(6) == static_cast<float>(1.0f * 0.998 - 0.0005));
	for (int i = 0; i < 5000; i++) a.tick(h);
	CHECK(a.level(6) == doctest::Approx(-0.25f).epsilon(1e-3)); // (the decay's own limit: x = 0.998 x - 0.0005)
	a.set_voices(2);
	CHECK(a.voices() == 2);
	h.press(a, 5, 0.5f);
	CHECK(a.start(h, 5) == 1); // (voice 2 is out of use; 1 is released)
}
