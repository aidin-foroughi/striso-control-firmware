// Shared helpers of the host renderers and tests: WAV output (scaled like the board), a hash of a
// render (for golden checksums), parameter writes through the firmware's path, and the replay of
// recorded key streams (tools/record_keys.py CSV) as the board's synth_control.cpp would play them.
#ifndef STRISO_HARNESS_UTIL_H_
#define STRISO_HARNESS_UTIL_H_

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <span>
#include <string>
#include <vector>

#include "striso_dsp/strisy.h"

namespace harness {

// 16 bit WAV, x0.25 and hard clipped like the board at the usual volume ((volume/127)^2 ~ 0.25)
[[nodiscard]] inline bool write_wav(const char* path, std::span<const float> x, int channels, int sample_rate) {
	FILE* f = std::fopen(path, "wb");
	if (!f) return false;
	const uint32_t n = static_cast<uint32_t>(x.size());
	const uint32_t sr = static_cast<uint32_t>(sample_rate), br = sr * 2 * static_cast<uint32_t>(channels), data = n * 2;
	const uint16_t ch = static_cast<uint16_t>(channels), ba = static_cast<uint16_t>(2 * channels), bits = 16, fmt = 1;
	const uint32_t riff = 36 + data, fmtlen = 16;
	std::fwrite("RIFF", 1, 4, f);
	std::fwrite(&riff, 4, 1, f);
	std::fwrite("WAVEfmt ", 1, 8, f);
	std::fwrite(&fmtlen, 4, 1, f);
	std::fwrite(&fmt, 2, 1, f);
	std::fwrite(&ch, 2, 1, f);
	std::fwrite(&sr, 4, 1, f);
	std::fwrite(&br, 4, 1, f);
	std::fwrite(&ba, 2, 1, f);
	std::fwrite(&bits, 2, 1, f);
	std::fwrite("data", 1, 4, f);
	std::fwrite(&data, 4, 1, f);
	std::vector<int16_t> s(n);
	for (uint32_t k = 0; k < n; k++) {
		float v = x[k] * 0.25f;
		v = v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
		s[k] = static_cast<int16_t>(std::lrintf(v * 32767.0f));
	}
	std::fwrite(s.data(), 2, n, f);
	return std::fclose(f) == 0;
}

// FNV-1a 64 over the samples' bits: equal hashes mean bit-identical renders
[[nodiscard]] inline uint64_t hash_samples(std::span<const float> x) {
	uint64_t h = 1469598103934665603ull;
	for (float v : x) {
		uint32_t u;
		std::memcpy(&u, &v, 4);
		for (int k = 0; k < 4; k++) {
			h ^= (u >> (8 * k)) & 0xffu;
			h *= 1099511628211ull;
		}
	}
	return h;
}

// a parameter write as the firmware applies it (synth_strisy.cpp apply_params): NaN ignored,
// clamped to [min, max], rounded unless continuous, then changed()
template <class E> inline void param_write(E& e, int i, float value) {
	using P = strisy::EngineParams<E>;
	const strisy::ParamDesc* d = P::desc(i);
	float* p = P::ptr(e, i);
	if (!d || !p || value != value) return;
	float v = value < d->min ? d->min : (value > d->max ? d->max : value);
	if (d->kind != strisy::ParamKind::continuous) v = static_cast<float>(static_cast<int>(v + (v >= 0 ? 0.5f : -0.5f)));
	*p = v;
	P::changed(e, i);
}

// --- recorded key streams ------------------------------------------------------------------------
// tools/record_keys.py CSV (t,src,id,pres,velo,x,y; raw 14 bit values) to per-voice control events,
// as tools/harness/out/bowed_rec.py did: note = 62 + 12*c0 + 7*c1 + bend*x^3 (bend 0.5),
// voices by get_voice() (same button, else a free voice, else the earliest released one),
// velo clipped at +-8191 in the stream reconstructed from the pressure slope.
struct RecEvent {
	double t;
	int voice;
	float note, pres, vpres, but_x, but_y;
};

[[nodiscard]] inline std::vector<RecEvent> load_recording(const char* path, int nvoices) {
	static const int c0[68] = {6, 5, 5, 4, 4, 3, 2, 2, 1, 1, 0, 0, -1, -2, -2, -3, -3,
	    5, 4, 4, 3, 3, 2, 1, 1, 0, 0, -1, -1, -2, -3, -3, -4, -4,
	    4, 3, 3, 2, 2, 1, 0, 0, -1, -1, -2, -2, -3, -4, -4, -5, -5,
	    3, 2, 2, 1, 1, 0, -1, -1, -2, -2, -3, -3, -4, -5, -5, -6, -6};
	struct Row {
		double t;
		int id;
		double pres, velo, x, y;
	};
	std::vector<Row> rows;
	FILE* f = std::fopen(path, "r");
	if (!f) return {};
	char line[256];
	while (std::fgets(line, sizeof line, f)) {
		double t;
		int src, id, p, v, x, y;
		if (std::sscanf(line, "%lf,%d,%d,%d,%d,%d,%d", &t, &src, &id, &p, &v, &x, &y) == 7 && src == 0 && id >= 0 && id < 68)
			rows.push_back({t, id, (double)p, (double)v, (double)x, (double)y});
	}
	std::fclose(f);
	std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.t < b.t; });
	if (rows.empty()) return {};
	// pressure slope per button, fit velo = k * slope on unclipped rising samples
	std::vector<double> slope(rows.size());
	std::map<int, size_t> last;
	for (size_t j = 0; j < rows.size(); j++) {
		auto it = last.find(rows[j].id);
		const double dp = it == last.end() ? rows[j].pres : rows[j].pres - rows[it->second].pres;
		const double dt = it == last.end() ? 0.002 : std::max(rows[j].t - rows[it->second].t, 0.0);
		slope[j] = dp / std::max(dt, 0.001);
		last[rows[j].id] = j;
	}
	double num = 0, den = 0;
	for (size_t j = 0; j < rows.size(); j++)
		if (std::fabs(rows[j].velo) < 8000 && std::fabs(slope[j]) > 1e4 && rows[j].pres > 0) {
			num += rows[j].velo * slope[j];
			den += slope[j] * slope[j];
		}
	const double k = num / std::max(den, 1e-9);
	std::vector<int> voices(static_cast<size_t>(nvoices), -1);
	std::vector<double> rel_time(static_cast<size_t>(nvoices), -1e9);
	std::map<int, int> state; // button -> voice while held
	std::vector<RecEvent> out;
	const double t0 = rows[0].t;
	for (size_t j = 0; j < rows.size(); j++) {
		const Row& r = rows[j];
		double velo = r.velo;
		if (std::fabs(velo) >= 8191) velo = (velo > 0 ? 1 : -1) * std::max(std::fabs(velo), std::fabs(k * slope[j]));
		const double p = r.pres / 8191.0, vp = velo / 8191.0, bx = r.x / 8191.0;
		const double by = std::max(-1.0, std::min(1.0, r.y / 8191.0));
		const double note = 62 + 12 * c0[r.id] + 7 * ((r.id % 17) - 8) + 0.5 * bx * bx * bx;
		if (p > 0 && !state.count(r.id)) {
			int v = -1;
			for (int n = 0; n < nvoices && v < 0; n++)
				if (voices[n] == r.id) v = n;
			for (int n = 0; n < nvoices && v < 0; n++)
				if (voices[n] == -1) v = n;
			if (v < 0) {
				double best = 1e300;
				for (int n = 0; n < nvoices; n++)
					if (!state.count(voices[n]) && rel_time[n] < best) {
						best = rel_time[n];
						v = n;
					}
				if (v < 0) v = 0;
				state.erase(voices[v]);
			}
			voices[v] = r.id;
			state[r.id] = v;
		}
		auto it = state.find(r.id);
		if (it != state.end()) {
			const int v = it->second;
			out.push_back({r.t - t0, v, (float)note, (float)p, (float)vp, (float)bx, (float)by});
			if (p <= 0) {
				state.erase(it);
				rel_time[v] = r.t;
			}
		}
	}
	return out;
}

} // namespace harness

#endif
