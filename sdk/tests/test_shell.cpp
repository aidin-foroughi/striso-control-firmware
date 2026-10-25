// The host shell (shell.h): MIDI in, any frame count per call; the MIDI file player.
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "doctest/doctest.h"
#include "striso_sdk/performance.h"
#include "striso_sdk/shell.h"
#include "striso_sdk/smf.h"

namespace sdk = striso::sdk;

namespace {

// frames of the shell in calls of the given sizes (cycled), MIDI note 60 on at 0, off at 0.5 s
// (with its offset in the call that frame is in, as a plugin host gives it)
std::vector<float> render(const std::vector<int>& sizes, int frames) {
	const sdk::EngineEntry* va = sdk::find_engine("va");
	REQUIRE(va != nullptr);
	sdk::Shell s(*va, 48000);
	s.midi(0x90, 60, 100);
	std::vector<float> l(static_cast<size_t>(frames)), r(static_cast<size_t>(frames));
	bool off = false;
	for (int n = 0, k = 0; n < frames; k++) {
		const int m = std::min(sizes[static_cast<size_t>(k) % sizes.size()], frames - n);
		if (!off && n + m > 24000) {
			s.midi(0x80, 60, 64, 24000 - n);
			off = true;
		}
		s.process(&l[static_cast<size_t>(n)], &r[static_cast<size_t>(n)], m);
		n += m;
	}
	l.insert(l.end(), r.begin(), r.end());
	return l;
}

} // namespace

TEST_CASE("shell: a MIDI note sounds and ends, the call size makes no difference") {
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
	// (the note off goes in before the block of frame 24000 either way: 750 blocks)
	CHECK(a == b);
}

TEST_CASE("shell: timed MIDI goes in before the block of its frame, late or untimed MIDI at once") {
	const sdk::EngineEntry* va = sdk::find_engine("va");
	REQUIRE(va != nullptr);
	// a note at frame 100 of a 1000-frame call is the note sent before a call that starts there
	sdk::Shell a(*va, 48000), b(*va, 48000);
	std::vector<float> la(2000), ra(2000), lb(2000), rb(2000);
	a.midi(0x90, 60, 100, 100);
	a.process(la.data(), ra.data(), 1000);
	a.process(la.data() + 1000, ra.data() + 1000, 1000);
	b.process(lb.data(), rb.data(), 96); // (the block of frame 100: frames 96..127)
	b.midi(0x90, 60, 100);
	b.process(lb.data() + 96, rb.data() + 96, 1904);
	CHECK(la == lb);
	float peak = 0;
	for (float x : la) peak = std::fmax(peak, std::fabs(x));
	CHECK(peak > 1e-3f);
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

TEST_CASE("players: a MIDI file plays an engine and says what it is") {
	const sdk::MidiEvent ev[] = {
	    {0.0, {0x90, 60, 100}, 3},
	    {0.0, {0x91, 64, 90}, 3},
	    {0.3, {0xe1, 0x00, 0x48}, 3},
	    {0.6, {0x80, 60, 0}, 3},
	    {0.6, {0x81, 64, 0}, 3},
	};
	const std::vector<uint8_t> bytes = sdk::make_midi(ev);
	const char* path = "striso_sdk_tests_players.mid";
	FILE* f = std::fopen(path, "wb");
	REQUIRE(f != nullptr);
	std::fwrite(bytes.data(), 1, bytes.size(), f);
	std::fclose(f);
	std::string error;
	const std::unique_ptr<sdk::Player> p = sdk::midi_player(path, 10, 48000, &error);
	REQUIRE(p != nullptr);
	CHECK(p->describe().find("striso_sdk_tests_players.mid") != std::string::npos);
	CHECK(p->describe().find("fnv") != std::string::npos);
	CHECK(p->seconds() == doctest::Approx(2.6));
	sdk::Engine e(*sdk::find_engine("ep"));
	e.init(48000, 10);
	std::vector<striso::dsp::VoiceControls> vc(10, striso::dsp::voice_at_rest);
	striso::dsp::GlobalControls g = striso::dsp::globals_at_rest;
	float peak = 0;
	for (int b = 0; b < static_cast<int>(p->seconds() * 48000 / 32); b++) {
		p->block(b, vc.data(), g);
		float out[32];
		float* const o[1] = {out};
		e.process(vc.data(), g, o);
		for (float x : out) peak = std::fmax(peak, std::fabs(x));
	}
	CHECK(peak > 0.01f);
	CHECK(e.active_voices() == 0);
	std::remove(path);
	CHECK(sdk::midi_player("no/such/file.mid", 10, 48000, &error) == nullptr);
	CHECK(!error.empty());
}
