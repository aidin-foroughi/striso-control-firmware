// single-note host driver for voice_trumpet.h (calibration, tools/trumpet/cal): tt out.f32 preset note pres seconds strike
// [param=value ...] [nopll= nocal=0/1/2 callip= caltrim= calpthr= rb= cb= qb= seed= norefl= trim0=]; env RAMP (key
// pressure ramp, s), TILT, PINT (print interval, blocks). Prints EARLY (early_db 20-60 ms re 250-400 ms, c25, worst
// |cents| 15-60 ms), JIT, PC50/PC70 (cup pressure rise), PCC (cup pressure pitch) and SUMMARY (t30/t10: within 30/12 c
// from the cup pressure, steady cents from the output, trim, partial). Writes pc/u/x.f32 (96 kHz, in time with the output).
// a note sequence: note may be "60,62,64" (each held seconds, legato overlap 20 ms if strike < 0)
#define TRUMPET_DEBUG
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
#include <cmath>
#include "striso_dsp/voice_trumpet.h"

int main(int argc, char** argv) {
	if (argc < 7) { fprintf(stderr, "usage\n"); return 1; }
	const char* outp = argv[1];
	int preset = atoi(argv[2]);
	std::vector<float> notes;
	{ std::string s = argv[3]; size_t a = 0; while (a < s.size()) { size_t b = s.find(',', a); if (b == std::string::npos) b = s.size(); notes.push_back((float)atof(s.substr(a, b - a).c_str())); a = b + 1; } }
	float pres = (float)atof(argv[4]);
	float secs = (float)atof(argv[5]);
	float strike = (float)atof(argv[6]);
	static trumpet::Engine e;
	trumpet::init(e, 48000, 10);
	trumpet::set_preset(e, preset);
	for (int a = 7; a < argc; a++) {
		const char* eq = strchr(argv[a], '=');
		if (!eq) continue;
		if (!strncmp(argv[a], "nopll=", 6)) { trumpet::dbg_nopll = atoi(eq + 1); continue; }
		if (!strncmp(argv[a], "trim0=", 6)) { for (auto& r : e.learn) for (auto& x : r) x = (float)atof(eq + 1); continue; }
		if (!strncmp(argv[a], "callip=", 7)) { trumpet::dbg_cal_lip = (float)atof(eq + 1); continue; }
		if (!strncmp(argv[a], "caltrim=", 8)) { trumpet::dbg_cal_trim = (float)atof(eq + 1); continue; }
		if (!strncmp(argv[a], "calpthr=", 8)) { trumpet::dbg_cal_pthr = (float)atof(eq + 1); continue; }
		if (!strncmp(argv[a], "seed=", 5)) { trumpet::dbg_seed = (float)atof(eq + 1); continue; }
		if (!strncmp(argv[a], "rb=", 3)) { trumpet::dbg_rb = (float)atof(eq + 1); continue; }
		if (!strncmp(argv[a], "cb=", 3)) { trumpet::dbg_cb = (float)atof(eq + 1); continue; }
		if (!strncmp(argv[a], "qb=", 3)) { trumpet::dbg_qb = (float)atof(eq + 1); continue; }
		if (!strncmp(argv[a], "norefl=", 7)) { trumpet::dbg_norefl = atoi(eq + 1); continue; }
		if (!strncmp(argv[a], "nocal=", 6)) { trumpet::dbg_nocal = atoi(eq + 1); continue; }
		for (int i = 0; i < trumpet::NUM_LIVE_PARAMS; i++)
			if (!strncmp(argv[a], trumpet::param_table[i].d.name, eq - argv[a]) && strlen(trumpet::param_table[i].d.name) == (size_t)(eq - argv[a]))
				e.p.*(trumpet::param_table[i].m) = (float)atof(eq + 1);
	}
	e.ps = e.p;
	trumpet::update_derived(e);
	const int B = 32;
	bool legato = strike < 0;
	float total = secs * notes.size() + 0.8f;
	int nb = (int)(total * 48000 / B);
	std::vector<float> out((size_t)nb * B);
	std::vector<float> pcap((size_t)nb * 64 * 2);
	trumpet::dbg_pc = pcap.data(); trumpet::dbg_len = (long)pcap.size();
	std::vector<float> ucap(pcap.size()), xcap(pcap.size());
	trumpet::dbg_u = ucap.data(); trumpet::dbg_x = xcap.data();
	float pp[10] = {0};
	for (int b = 0; b < nb; b++) {
		double t = (double)b * B / 48000;
		for (int v = 0; v < 10; v++) e.vc[v] = {69, 0, 0, 0, 0};
		for (size_t k = 0; k < notes.size(); k++) {
			double t0 = 0.05 + k * secs, t1 = t0 + secs - (legato ? -0.03 : 0.08);
			int v = (int)(k % 2);
			if (t >= t0 && t < t1) {
				static double ramp = getenv("RAMP") ? atof(getenv("RAMP")) : 0.01;
				float a = (float)std::min(1.0, (t - t0) / ramp);
				e.vc[v].note = notes[k];
				e.vc[v].pres = pres * a;
				static float tilt = getenv("TILT") ? (float)atof(getenv("TILT")) : 0.0f;
				e.vc[v].but_y = tilt;
			}
		}
		for (int v = 0; v < 10; v++) {
			float vp = (e.vc[v].pres - pp[v]) * (48000.0f / B) * 0.0125f;
			if (vp > 0) vp = std::min(1.0f, legato ? 0.1f * vp : std::fabs(strike) * (vp > 0.01f ? 1.0f : 0.0f) + 0.0f * vp);
			e.vc[v].vpres = vp;
			pp[v] = e.vc[v].pres;
		}
		static int sw = getenv("SWITCH") ? atoi(getenv("SWITCH")) : 0;
		if (sw && b > 0 && b % sw == 0) trumpet::set_preset(e, (e.preset + 1) % trumpet::NUM_PRESETS);
		float buf[32];
		trumpet::process(e, buf);
		if (!e.pl[0].active) { // the capture is in time with the output: player 0 was not processed in this block
			for (int n = 0; n < 64 && trumpet::dbg_n < trumpet::dbg_len; n++) pcap[trumpet::dbg_n] = ucap[trumpet::dbg_n] = xcap[trumpet::dbg_n] = 0.0f, trumpet::dbg_n++;
		}
		for (int n = 0; n < B; n++) out[(size_t)b * B + n] = buf[n];
		static int pint = getenv("PINT") ? atoi(getenv("PINT")) : 150;
		if (b % pint == 0) {
			const trumpet::Player& s = e.pl[0];
			const trumpet::Player& s1 = e.pl[1]; fprintf(stderr, "[active %d: %d%d%d%d] ", trumpet::active_voices(e), e.pl[0].active, e.pl[1].active, e.pl[2].active, e.pl[3].active);
			fprintf(stderr, "t %.2f p0: act %d note %.2f fmeas %7.2f (%+6.1f c) trim %6.1f lipc %5.0f c%d k%d pm %6.0f pc %7.0f x %.2e gate %.2f peak %.0f | p1 act %d fmeas %.1f\n", t, s.active, s.note,
			        s.fmeas, s.fmeas > 0 ? 1200 * std::log2(s.fmeas / (8.1757989 * std::pow(2.0, s.note / 12))) : 0.0, s.trim, s.lipc, s.combo, s.k, s.pm, s.pc, s.x, s.gate, s.peak, s1.active, s1.fmeas);
		}
	}
	{
		// summary over the last 0.3 s of the first note: cup pressure p2p, measured pitch (zero crossings of pc)
		long n0 = (long)((0.05 + secs - 0.4) * 96000), n1 = (long)((0.05 + secs - 0.1) * 96000);
		if (n1 > trumpet::dbg_n) n1 = trumpet::dbg_n;
		double mean = 0; for (long i = n0; i < n1; i++) mean += pcap[i]; mean /= (n1 - n0);
		float lo = 1e9f, hi = -1e9f; int nc = 0; double t0 = -1, t1 = -1;
		double ftl = 8.1757989 * std::pow(2.0, notes[0] / 12), al = 1 - std::exp(-2 * M_PI * 1.5 * ftl / 96000), l1 = 0, l2 = 0, l2p = 0;
		for (long i = n0 - 2000; i < n0; i++) { l1 += al * (pcap[i] - mean - l1); l2 += al * (l1 - l2); }
		l2p = l2;
		for (long i = n0 + 1; i < n1; i++) { lo = std::min(lo, pcap[i]); hi = std::max(hi, pcap[i]);
			l1 += al * (pcap[i] - mean - l1); l2 += al * (l1 - l2);
			double a = l2p, b2 = l2; l2p = l2;
			if (a < 0 && b2 >= 0) { double t = i - 1 + a / (a - b2); if (t0 < 0) t0 = t; else { t1 = t; nc++; } } }
		// rise: first time the pc envelope exceeds 50% of the final p2p
		long nr = -1; float h = 0.5f * (hi - lo);
		for (long i = (long)(0.05 * 96000); i < n0; i++) if (std::fabs(pcap[i] - mean) > 0.5f * h) { nr = i; break; }
		double fz = nc > 0 ? 96000.0 * nc / (t1 - t0) : 0;
		const double fzc = fz;
		{
			// pitch of the radiated sound: autocorrelation over the window (48 kHz output)
			long a0 = n0 / 2, a1 = n1 / 2, L = a1 - a0;
			int lmin = 48000 / 2600, lmax = std::min(48000 / 50, (int)(L / 2));
			std::vector<double> ac(lmax + 2, 0.0);
			double e0 = 0; for (long i = a0; i < a1; i++) e0 += (double)out[i] * out[i];
			for (int l = lmin - 1; l <= lmax + 1 && l < (int)ac.size(); l++) { double sacc = 0; for (long i = a0; i + l < a1; i++) sacc += (double)out[i] * out[i + l]; ac[l] = sacc; }
			int kb = lmin; for (int l = lmin; l <= lmax; l++) if (ac[l] > ac[kb]) kb = l;
			for (int d = 2; d <= 6; d++) { int kk = (int)std::lround((double)kb / d); if (kk < lmin) break; int j = kk; for (int q = std::max(lmin, kk - 3); q <= kk + 3; q++) if (ac[q] > ac[j]) j = q; if (ac[j] > 0.9 * ac[kb]) { kb = j; } }
			if (kb > lmin && kb < lmax && e0 > 1e-12) { double y0 = ac[kb - 1], y1 = ac[kb], y2 = ac[kb + 1]; double dd = 0.5 * (y0 - y2) / (y0 - 2 * y1 + y2); fz = 48000.0 / (kb + dd); } else fz = 0;
		}
		double ft = 8.1757989 * std::pow(2.0, notes[0] / 12);
		// attack pitch from the cup pressure: 2-period autocorrelation every 1 ms after the note start
		double t30 = -1, t10 = -1;
		{
			const double P = 96000.0 / ft;
			const int L = (int)(2 * P), l0 = (int)(0.6 * P), l1 = (int)(1.6 * P);
			int ok30 = 0, ok10 = 0;
			for (int ms = 1; ms < 300; ms++) {
				long e2 = (long)((0.05 + ms / 1000.0) * 96000);
				if (e2 - L - l1 < (long)(0.05 * 96000) || e2 >= trumpet::dbg_n) { ok30 = ok10 = 0; continue; }
				double best = -1; int bl = l0;
				for (int l = l0; l <= l1; l++) {
					double sxy = 0, sxx = 0, syy = 0;
					for (int i = 0; i < L; i++) { double a = pcap[e2 - i], b = pcap[e2 - i - l]; sxy += a * b; sxx += a * a; syy += b * b; }
					double c = sxy / (std::sqrt(sxx * syy) + 1e-12);
					if (c > best) { best = c; bl = l; }
				}
				double cents = 1200 * std::log2(P / bl);
				ok30 = (best > 0.8 && std::fabs(cents) < 30) ? ok30 + 1 : 0;
				ok10 = (best > 0.8 && std::fabs(cents) < 12) ? ok10 + 1 : 0;
				if (t30 < 0 && ok30 >= 3) t30 = ms - 2;
				if (t10 < 0 && ok10 >= 5) t10 = ms - 4;
				if (t30 >= 0 && t10 >= 0) break;
			}
		}
		// early pitch: worst |cents| between 15 and 60 ms (wrong partial / unlocked start), early level
		double wc = 0, c25 = 9999;
		{
			const double P = 96000.0 / ft;
			const int L = (int)(2 * P), l0 = (int)(0.45 * P), l1 = (int)(1.45 * P);
			for (int ms = 15; ms <= 60; ms += 5) {
				long e2 = (long)((0.05 + ms / 1000.0) * 96000);
				if (e2 - L - l1 < (long)(0.05 * 96000)) continue;
				double best = -1; int bl = l0;
				for (int l = l0; l <= l1; l++) {
					double sxy = 0, sxx = 0, syy = 0;
					for (int i = 0; i < L; i++) { double a = pcap[e2 - i], b = pcap[e2 - i - l]; sxy += a * b; sxx += a * a; syy += b * b; }
					double c = sxy / (std::sqrt(sxx * syy) + 1e-12);
					if (c > best) { best = c; bl = l; }
				}
				double cents = 1200 * std::log2(P / bl);
				if (ms == 25) c25 = cents;
				if (std::fabs(cents) > std::fabs(wc)) wc = cents;
			}
		}
		double eL = 0, sL = 0;
		{ long a = (long)(0.05 * 48000); for (long i = a + 960; i < a + 2880; i++) eL += (double)out[i] * out[i]; eL /= 1920;
		  for (long i = a + 12000; i < a + 19200; i++) sL += (double)out[i] * out[i]; sL /= 7200; }
		{ double t50 = -1, t70 = -1; for (int ms = 0; ms < 300; ms++) { long a = (long)((0.05 + ms / 1000.0) * 96000); float l = 1e9f, h2 = -1e9f; for (long i = a; i < a + 192 && i < trumpet::dbg_n; i++) { l = std::min(l, pcap[i]); h2 = std::max(h2, pcap[i]); } if (t50 < 0 && h2 - l >= 0.5f * (hi - lo)) t50 = ms + 2; if (t70 < 0 && h2 - l >= 0.7f * (hi - lo)) t70 = ms + 2; }
		  fprintf(stderr, "PC50 %.0f PC70 %.0f ", t50, t70); }
		{ // period jitter (cents std of the lip periods in the steady window)
			std::vector<double> per; double last = -1; double ml = 0; long cnt = 0;
			for (long i = n0; i < n1; i++) { ml += xcap[i]; cnt++; } ml /= std::max(cnt, 1L);
			for (long i = n0 + 1; i < n1; i++) { double a = xcap[i - 1] - ml, b2 = xcap[i] - ml; if (a < 0 && b2 >= 0) { double t = i - 1 + a / (a - b2); if (last >= 0) per.push_back(t - last); last = t; } }
			double m = 0, v = 0; for (double p : per) m += p; m /= std::max((size_t)1, per.size());
			for (double p : per) v += (1200 * std::log2(p / m)) * (1200 * std::log2(p / m)); v = std::sqrt(v / std::max((size_t)1, per.size()));
			fprintf(stderr, "JIT %.1f ", v); }
		fprintf(stderr, "PCC %.0f ", fzc > 0 ? 1200 * std::log2(fzc / (8.1757989 * std::pow(2.0, notes[0] / 12))) : 9999.0);
		fprintf(stderr, "EARLY early_db %.1f c25 %.0f worst %.0f ", 10 * std::log10((eL + 1e-20) / (sL + 1e-20)), c25, wc);
		fprintf(stderr, "SUMMARY t30 %.0f t10 %.0f note", t30, t10); fprintf(stderr, " %.2f p2p %.0f fz %.2f cents %.1f rise_ms %.1f trim %.1f lipc %.0f k %d combo %d\n", notes[0], hi - lo, fz, fz > 0 ? 1200 * std::log2(fz / ft) : 9999.0,
		        nr > 0 ? (nr / 96000.0 - 0.05) * 1000 : -1.0, e.pl[0].trim, e.pl[0].lipc, e.pl[0].k, e.pl[0].combo); fprintf(stderr, "xamp %.2e x %.2e%c", e.pl[0].xamp, e.pl[0].x, 10);
	}
	float pk = 0; size_t bad = 0;
	for (float x : out) { if (!std::isfinite(x)) bad++; else pk = std::max(pk, std::fabs(x)); }
	fprintf(stderr, "peak %.3f bad %zu\n", pk, bad);
	{ FILE* g = fopen("pc.f32", "wb"); fwrite(pcap.data(), 4, trumpet::dbg_n < (long)pcap.size() ? trumpet::dbg_n : pcap.size(), g); fclose(g);
	  g = fopen("u.f32", "wb"); fwrite(ucap.data(), 4, trumpet::dbg_n < (long)pcap.size() ? trumpet::dbg_n : pcap.size(), g); fclose(g);
	  g = fopen("x.f32", "wb"); fwrite(xcap.data(), 4, trumpet::dbg_n < (long)pcap.size() ? trumpet::dbg_n : pcap.size(), g); fclose(g); }
	FILE* f = fopen(outp, "wb");
	fwrite(out.data(), 4, out.size(), f);
	fclose(f);
}
