// Renders a performance through the virtual board, as tools/harness/render_strisy.cpp renders it
// straight on an engine, and checks the hash against the golden one (tests/golden/renders.txt).
//
// Usage: virtual_render --voice <engine> [--preset N] [--perf default|stale] [--rec file.csv]
//                       [--fx] [--expect HASH] [--switch-from N] [--out file.f32]
// The board boots, the motion thread hands the globals over, and at a block boundary the voice
// entry of the engine and preset is selected (synth_select_voice: engine_init and set_preset at
// the next block, as the console's voice command does). Then every block: the performance's
// controls go where synth_control writes them (synth_interface), one block renders, and the
// engine's output (before the effects; with --fx the effects' stereo output, the effects
// re-initialised first, as the renderer starts them) is kept. --switch-from N first plays the
// default performance 2 s on voice entry N, so the engine under test starts on a board that ran
// another voice. Prints "hash <hex>"; with --expect, exits 1 unless it matches.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "harness_util.h"
#include "perfs.h"
#include "striso_virtual.h"

namespace {

constexpr int NV = 10, BLOCK = 32, SR = 48000;

std::string console(sv_board* b, const std::string& cmd, int ms) {
	std::string out;
	uint8_t buf[8192];
	while (sv_console_read(b, buf, sizeof buf) > 0) {
	}
	const std::string bytes = "Stc" + cmd;
	sv_console_write(b, reinterpret_cast<const uint8_t*>(bytes.data()), static_cast<int32_t>(bytes.size()));
	sv_advance_ms(b, ms);
	int32_t n;
	while ((n = sv_console_read(b, buf, sizeof buf)) > 0) out.append(reinterpret_cast<char*>(buf), static_cast<size_t>(n));
	return out;
}

// the voice entry of an engine's preset, from the board's voice list (Stcv)
int entry_of(sv_board* b, const std::string& engine, int preset) {
	const std::string list = console(b, "v", 40);
	int k = 0;
	size_t p = 0;
	while (p < list.size()) {
		size_t e = list.find("\r\n", p);
		if (e == std::string::npos) e = list.size();
		const std::string line = list.substr(p, e - p);
		p = e + 2;
		if (line.size() < 4) continue;
		const int n = std::atoi(line.c_str() + 2);
		const std::string name = line.substr(line.find(' ', 2) + 1);
		if (name.substr(0, name.find(':')) != engine) continue;
		if (preset < 0 || k == preset) return n;
		k++;
	}
	return -1;
}

} // namespace

int main(int argc, char** argv) {
	std::string voice, perf = "default", rec, expect, out_path;
	int preset = -1, switch_from = -1;
	bool fx = false;
	for (int a = 1; a < argc; a++) {
		const std::string s = argv[a];
		auto next = [&]() -> std::string { return a + 1 < argc ? argv[++a] : ""; };
		if (s == "--voice") voice = next();
		else if (s == "--preset") preset = std::atoi(next().c_str());
		else if (s == "--perf") perf = next();
		else if (s == "--rec") {
			perf = "rec";
			rec = next();
		} else if (s == "--fx") fx = true;
		else if (s == "--expect") expect = next();
		else if (s == "--switch-from") switch_from = std::atoi(next().c_str());
		else if (s == "--out") out_path = next();
		else {
			std::fprintf(stderr, "unknown option %s\n", s.c_str());
			return 2;
		}
	}
	std::vector<harness::RecEvent> events;
	double seconds = 17.5;
	if (perf == "rec") {
		events = harness::load_recording(rec.c_str(), NV);
		if (events.empty()) {
			std::fprintf(stderr, "no key events in %s\n", rec.c_str());
			return 2;
		}
		seconds = events.back().t + 2.0;
	} else if (perf != "default" && perf != "stale") {
		std::fprintf(stderr, "unknown performance %s\n", perf.c_str());
		return 2;
	}

	sv_board* b = sv_create(nullptr);
	if (!b) {
		std::fprintf(stderr, "sv_create: %s\n", sv_last_error());
		return 2;
	}
	const int entry = entry_of(b, voice, preset);
	if (entry < 0) {
		std::fprintf(stderr, "the board has no voice %s preset %d\n", voice.c_str(), preset);
		sv_destroy(b);
		return 2;
	}
	sv_test(b, SV_TEST_MOTION_OFF, 0);
	float vc[NV * 5], g[8];
	const auto send = [&]() {
		for (int v = 0; v < NV; v++) sv_set_engine_controls(b, v, vc + 5 * v);
		sv_set_engine_controls(b, -1, g);
	};
	if (switch_from >= 0) {
		sv_select_voice(b, switch_from);
		for (int blk = 0; blk < 2 * SR / BLOCK; blk++) {
			perf_default(blk, NV, vc, g);
			send();
			sv_render_taps(b, BLOCK, nullptr, nullptr, nullptr);
		}
	}
	// to a block boundary: the next render's first frame is a block interrupt
	sv_state st{};
	st.size = sizeof st;
	sv_get_state(b, &st);
	if (st.to_block > 0) sv_render_taps(b, st.to_block, nullptr, nullptr, nullptr);
	sv_select_voice(b, entry);
	if (fx) sv_test(b, SV_TEST_FX_INIT, 0);

	const int nblocks = static_cast<int>(seconds * SR / BLOCK);
	const int ch = fx ? 2 : 1;
	std::vector<float> out(static_cast<size_t>(nblocks) * BLOCK * ch);
	size_t k = 0;
	for (int blk = 0; blk < nblocks; blk++) {
		if (perf == "rec") {
			// as render_strisy.cpp: the last event of each voice up to this block, the synth
			// thread's defaults at the start
			if (blk == 0) {
				for (int v = 0; v < NV; v++) {
					const float d[5] = {69.0f, 0.0f, 0.0f, 0.0f, 0.0f};
					std::memcpy(vc + 5 * v, d, sizeof d);
				}
				const float dg[8] = {1.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f, 0.0f, 3.0f / 16};
				std::memcpy(g, dg, sizeof dg);
			}
			const double t = static_cast<double>(blk) * BLOCK / SR;
			for (; k < events.size() && events[k].t <= t; k++) {
				const harness::RecEvent& x = events[k];
				const float c[5] = {x.note, x.pres, x.vpres, x.but_x, x.but_y};
				std::memcpy(vc + 5 * x.voice, c, sizeof c);
			}
		} else if (perf == "default") {
			perf_default(blk, NV, vc, g);
		} else {
			perf_stale(blk, NV, vc, g);
		}
		send();
		float* dst = out.data() + static_cast<size_t>(blk) * BLOCK * ch;
		if (sv_render_taps(b, BLOCK, nullptr, fx ? nullptr : dst, fx ? dst : nullptr) != BLOCK) {
			std::fprintf(stderr, "render failed at block %d\n", blk);
			sv_destroy(b);
			return 2;
		}
	}
	sv_destroy(b);

	size_t bad = 0;
	float peak = 0;
	for (float x : out) {
		if (!std::isfinite(x)) bad++;
		else peak = std::fmax(peak, std::fabs(x));
	}
	if (!out_path.empty()) {
		if (FILE* f = std::fopen(out_path.c_str(), "wb")) {
			std::fwrite(out.data(), sizeof(float), out.size(), f);
			std::fclose(f);
		}
	}
	const uint64_t h = harness::hash_samples(out);
	char hx[17];
	std::snprintf(hx, sizeof hx, "%016llx", static_cast<unsigned long long>(h));
	std::fprintf(stderr, "voice %s preset %d (entry %d), %s, %.1f s, peak %.3f%s\n", voice.c_str(), preset, entry,
	    perf.c_str(), seconds, peak, bad ? ", NON-FINITE" : "");
	std::printf("hash %s\n", hx);
	if (!expect.empty() && expect != hx) {
		std::fprintf(stderr, "hash %s, expected %s\n", hx, expect.c_str());
		return 1;
	}
	return bad ? 1 : 0;
}
