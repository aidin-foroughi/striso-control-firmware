// The firmware's message path: recorded key streams (tools/harness/rec) injected where the key
// scanner sends them (sv_inject_key: event_send, the control thread, synth_message, synth_control's
// voice allocation, pitch, sensitivities), rendered by the board's synth thread. The controls the
// engine got each block (the board's controls tap) are then replayed on the same engine compiled
// here, straight through its contract (awva::VoiceEngine), as the host renderers drive it:
// its output must be the board's, bit for bit.
// So the DSP on the board's path is exactly the DSP the goldens check; what differs from the
// goldens' recorded renders is only how controls come from keys (the harness's stand-in for
// synth_control, harness::load_recording, against the firmware's own code), shown as a message.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "board.h"
#include "doctest/doctest.h"
#include "harness_util.h"
#include "awva/engines/bowed.h"
#include "awva/engines/brass.h"
#include "awva/engines/ep.h"
#include "awva/engines/modal.h"
#include "striso_dsp/engines/strisy.h"
#include "awva/engines/va.h"
#include "awva/engines/wind.h"

namespace {

constexpr int NV = 10, BLOCK = 32, SR = 48000;

struct Row {
	double t;
	int src, id, pres, velo, x, y;
};

std::vector<Row> load_csv(const std::string& path) {
	std::vector<Row> rows;
	FILE* f = std::fopen(path.c_str(), "r");
	if (!f) return rows;
	char line[256];
	while (std::fgets(line, sizeof line, f)) {
		Row r{};
		if (std::sscanf(line, "%lf,%d,%d,%d,%d,%d,%d", &r.t, &r.src, &r.id, &r.pres, &r.velo, &r.x, &r.y) == 7) rows.push_back(r);
	}
	std::fclose(f);
	std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.t < b.t; });
	return rows;
}

struct BoardRender {
	std::vector<float> engine;          // the board's engine output
	std::vector<float> controls, globals; // per block: 10 x 5, 8
	int keys = 0;
};

int entry_of(vt::Board& bd, const std::string& engine) {
	for (const std::string& l : vt::lines(bd.cmd("v", 40))) {
		if (l.size() < 4) continue;
		const std::string name = l.substr(l.find(' ', 2) + 1);
		if (name.substr(0, name.find(':')) == engine) return std::atoi(l.c_str() + 2);
	}
	return -1;
}

BoardRender render_keys(const std::string& engine, const std::vector<Row>& rows, double seconds) {
	BoardRender out;
	vt::Board bd;
	const int entry = entry_of(bd, engine);
	REQUIRE(entry >= 0);
	REQUIRE(sv_select_voice(bd.b, entry) == 0);
	sv_state st{};
	st.size = sizeof st;
	sv_get_state(bd.b, &st);
	if (st.to_block > 0) sv_render_taps(bd.b, st.to_block, nullptr, nullptr, nullptr);
	const int nblocks = static_cast<int>(seconds * SR / BLOCK);
	out.engine.resize(static_cast<size_t>(nblocks) * BLOCK);
	const double t0 = rows.empty() ? 0 : rows.front().t;
	size_t k = 0;
	for (int b = 0; b < nblocks; b++) {
		const double t = static_cast<double>(b) * BLOCK / SR;
		for (; k < rows.size() && rows[k].t - t0 <= t; k++) {
			const Row& r = rows[k];
			sv_inject_key(bd.b, r.src, r.id, r.pres, r.velo, r.x, r.y);
			out.keys++;
		}
		REQUIRE(sv_render_taps(bd.b, BLOCK, nullptr, out.engine.data() + static_cast<size_t>(b) * BLOCK, nullptr) == BLOCK);
		sv_get_state(bd.b, &st);
		for (int v = 0; v < NV; v++)
			for (int c = 0; c < 5; c++) out.controls.push_back(st.controls[v][c]);
		for (int i = 0; i < 8; i++) out.globals.push_back(st.globals[i]);
	}
	return out;
}

// the same engine, straight, on the board's controls (its first preset, as the board's entry)
template <class E> std::vector<float> replay(const BoardRender& br) {
	static E e;
	static awva::Memory<E> mem;
	e.init(mem.fast, mem.large, SR, NV);
	if (E::num_presets() > 0) e.set_preset(0);
	const size_t nblocks = br.engine.size() / BLOCK;
	std::vector<float> out(br.engine.size());
	awva::VoiceControls vc[NV];
	for (awva::VoiceControls& c : vc) c = awva::voice_at_rest;
	for (size_t b = 0; b < nblocks; b++) {
		const float* c = &br.controls[b * NV * 5];
		for (int v = 0; v < NV; v++) {
			vc[v].note = c[5 * v];
			vc[v].pres = c[5 * v + 1];
			vc[v].vpres = c[5 * v + 2];
			vc[v].but_x = c[5 * v + 3];
			vc[v].but_y = c[5 * v + 4];
			awva::track_key(vc[v]);
		}
		const float* g = &br.globals[b * 8];
		const awva::GlobalControls gc = {g[0], g[1], g[2], g[3], g[4], g[5], g[6], g[7]};
		float buf[BLOCK];
		float* const o[1] = {buf};
		e.process(vc, gc, o);
		std::memcpy(out.data() + b * BLOCK, buf, sizeof buf);
	}
	return out;
}

// how far the firmware's controls are from the harness's stand-in for synth_control
std::string compare_harness(const std::string& rec, const BoardRender& br) {
	const std::vector<harness::RecEvent> ev = harness::load_recording(rec.c_str(), NV);
	const size_t nblocks = br.engine.size() / BLOCK;
	float vc[NV][5];
	for (auto& v : vc) {
		const float d[5] = {69, 0, 0, 0, 0};
		std::memcpy(v, d, sizeof d);
	}
	size_t k = 0, sounding = 0, both = 0;
	double dnote = 0, dpres = 0, npres = 0;
	for (size_t b = 0; b < nblocks; b++) {
		const double t = static_cast<double>(b) * BLOCK / SR;
		for (; k < ev.size() && ev[k].t <= t; k++) {
			const float c[5] = {ev[k].note, ev[k].pres, ev[k].vpres, ev[k].but_x, ev[k].but_y};
			std::memcpy(vc[ev[k].voice], c, sizeof c);
		}
		// pressure and pitch of what sounds, voice order apart: sort both by note
		std::vector<std::pair<float, float>> h, f;
		for (int v = 0; v < NV; v++) {
			if (vc[v][1] > 0) h.push_back({vc[v][0], vc[v][1]});
			const float* c = &br.controls[(b * NV + static_cast<size_t>(v)) * 5];
			if (c[1] > 0) f.push_back({c[0], c[1]});
		}
		std::sort(h.begin(), h.end());
		std::sort(f.begin(), f.end());
		sounding += f.size();
		if (h.size() == f.size()) {
			both += f.size();
			for (size_t i = 0; i < f.size(); i++) {
				dnote = std::fmax(dnote, std::fabs(h[i].first - f[i].first));
				dpres += std::fabs(h[i].second - f[i].second);
				npres += h[i].second;
			}
		}
	}
	char s[256];
	std::snprintf(s, sizeof s, "against the harness's replay: %zu of %zu sounding voice-blocks with the same voices, "
	                           "pitch up to %.3f semitones apart, pressure %.1f%% apart on average",
	    both, sounding, dnote, npres > 0 ? 100.0 * dpres / npres : 0.0);
	return s;
}

template <class E> void check_engine(const std::string& engine, const std::string& rec) {
	const std::vector<Row> rows = load_csv(rec);
	REQUIRE(!rows.empty());
	const double seconds = std::fmin(rows.back().t - rows.front().t + 2.0, 20.0);
	const BoardRender br = render_keys(engine, rows, seconds);
	const std::vector<float> ref = replay<E>(br);
	size_t diff = 0;
	float peak = 0;
	for (size_t i = 0; i < ref.size(); i++) {
		if (std::memcmp(&ref[i], &br.engine[i], 4) != 0) diff++;
		peak = std::fmax(peak, std::fabs(br.engine[i]));
	}
	CAPTURE(engine);
	CAPTURE(rec);
	CHECK(diff == 0);
	CHECK(peak > 0.01f); // the keys sounded
	MESSAGE(engine << ": " << br.keys << " key messages, " << ref.size() << " samples bit-identical: " << (diff == 0)
	               << "; " << compare_harness(rec, br));
}

const std::string staccato = std::string(STRISO_REC_DIR) + "/play_2026-10-22_staccato.csv";
const std::string legato = std::string(STRISO_REC_DIR) + "/play_2026-10-22_legato.csv";

} // namespace

#define ENGINE_CASE(ns) \
	TEST_CASE("message path: recorded keys through the firmware, " #ns " is the straight engine bit for bit") { \
		for (const std::string& rec : {staccato, legato}) check_engine<awva::engines::ns>(#ns, rec); \
	}

ENGINE_CASE(strisy)
ENGINE_CASE(va)
ENGINE_CASE(modal)
ENGINE_CASE(bowed)
ENGINE_CASE(wind)
ENGINE_CASE(ep)
ENGINE_CASE(brass)
