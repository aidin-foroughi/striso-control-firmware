// capture the steady oscillation of one note (all breaths of trumpet_seed.h) for the warm start:
//   cap preset note [secs]   -> prints "CAP preset note dyn ok per trim um combo k ext dc0..3 re/im..." lines
// per breath: a natural tongued start; where it doesn't reach the right regime, continuation from a
// neighbouring breath's capture (seeded); then one refinement pass (seeded from its own capture)
#define TRUMPET_DEBUG
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "striso_dsp/voice_trumpet.h"

using cd = std::complex<double>;
using trumpet::seed::NDYN;
using trumpet::seed::NH;
using trumpet::seed::State;

struct Res {
	bool ok = false;
	State st{};
	double cents = 9999, drift = 99, amp = 0;
	const char* why = "";
};

static trumpet::Engine e;
static std::vector<float> A[7]; // f, r, pc, x (outward), y (inward), opening, u
using trumpet::seed::NSIG;

static double hz(double n) { return 8.1757989 * std::pow(2.0, n / 12); }

static Res run(int preset, float note, float pres, const State* ovr, double secs) {
	trumpet::init(e, 48000, 10);
	trumpet::set_preset(e, preset);
	const long ns = (long)(secs * 96000) + 4096;
	for (auto& a : A) a.assign(ns, 0.0f);
	trumpet::dbg_f = A[0].data(); trumpet::dbg_r = A[1].data(); trumpet::dbg_pc = A[2].data(); trumpet::dbg_xo = A[3].data(); trumpet::dbg_y = A[4].data(); trumpet::dbg_x = A[5].data(); trumpet::dbg_u = A[6].data();
	trumpet::dbg_n = 0; trumpet::dbg_len = ns;
	trumpet::dbg_seed_ovr = ovr;
	trumpet::dbg_force_dyn = ovr ? pres : -1.0f;
	trumpet::dbg_bright = 1.0f;
	trumpet::dbg_nowarm = ovr ? 0 : 1;
	const long nb = (long)(secs * 48000 / 32);
	float buf[32];
	std::vector<float> out((size_t)nb * 32);
	for (long b = 0; b < nb; b++) {
		const double t = (double)b * 32 / 48000;
		for (int v = 0; v < 10; v++) e.vc[v] = {69, 0, 0, 0, 0};
		if (t >= 0.05) {
			const float a = (float)std::min(1.0, (t - 0.05) / 0.01);
			e.vc[0] = {note, pres * a, a < 1.0f ? 0.5f : 0.0f, 0, 0};
		}
		trumpet::process(e, buf);
		for (int n = 0; n < 32; n++) out[(size_t)b * 32 + n] = buf[n];
	}
	trumpet::dbg_seed_ovr = nullptr; trumpet::dbg_force_dyn = -1; trumpet::dbg_bright = -1; trumpet::dbg_nowarm = 0;
	Res r;
	const trumpet::Player& s = e.pl[0];
	const double ft = hz(trumpet::fold_note(e, note));
	if (!s.active || s.fmeas <= 0) { r.why = "silent"; return r; }
	r.cents = 1200 * std::log2(s.fmeas / ft);
	const double P = 96000.0 / s.fmeas, W = 2 * M_PI / P;
	const long n1 = trumpet::dbg_n;
	const long M = std::lround(4 * P);
	// harmonics over the last 4 periods; pc and x arrays hold the value at the start of each sample
	cd H[NSIG][NH + 1];
	double dc[NSIG] = {0};
	for (int q = 0; q < NSIG; q++) {
		const long nend = q < 2 ? n1 : n1 + 0; // f, r: value written at sample n; pc, x: before sample n
		for (int k = 0; k <= NH; k++) H[q][k] = 0;
		for (long m = 1; m <= M; m++) {
			const double v = A[q][nend - m];
			double tt = -(double)m;
			if (q >= 2) tt += 0.0; // dbg_pc[n] = state after sample n-1 = "t" of the state convention
			dc[q] += v;
			for (int k = 1; k <= NH; k++) H[q][k] += v * std::exp(cd(0, -k * W * tt));
		}
		dc[q] /= M;
		for (int k = 1; k <= NH; k++) H[q][k] *= 2.0 / M;
	}
	// pc/x: dbg arrays at index n hold the value before sample n, so index n1 - m is the state value
	// at t = -m + ... the seed convention: state after the last sample = signal at t = 0, with
	// arr[n1 + t]; the fit above used arr[n1 - m] at t = -m: consistent.
	// phase reference: the lips' fundamental at cos phase 0 at t = 0
	const double ph = std::arg(H[3][1] + H[4][1]);
	for (int q = 0; q < NSIG; q++)
		for (int k = 1; k <= NH; k++) H[q][k] *= std::exp(cd(0, -k * ph));
	// time shift: the reference moved by -ph / W samples; the state is the signal at that time, so
	// the seed restarts the oscillation at the lips' crest (any phase is a valid state of the cycle)
	State& st = r.st;
	st.per = (float)P;
	st.trim = s.trim;
	st.um = s.um;
	for (int q = 0; q < NSIG; q++) {
		st.dc[q] = (float)dc[q];
		for (int k = 0; k < NH; k++) { st.re[q][k] = (float)H[q][k + 1].real(); st.im[q][k] = (float)H[q][k + 1].imag(); }
	}
	st.combo = (int8_t)s.combo; st.k = (int8_t)s.k; st.ext = (int8_t)s.ext; st.ok = 1;
	// amplitude drift: lip amplitude, last 60 ms vs the 60 ms before (dB)
	auto rmsx = [&](long a, long b) { double m = 0, q = 0; for (long i = a; i < b; i++) m += A[5][i]; m /= (b - a); for (long i = a; i < b; i++) q += (A[5][i] - m) * (A[5][i] - m); return std::sqrt(q / (b - a)); };
	const long w6 = (long)(0.06 * 96000);
	const double a1 = rmsx(n1 - w6, n1), a0 = rmsx(n1 - 2 * w6, n1 - w6);
	r.amp = a1;
	r.drift = 20 * std::log10((a1 + 1e-12) / (a0 + 1e-12));
	const double x1 = std::abs(H[3][1] + H[4][1]);
	if (a1 < 2e-5) r.why = "weak";
	else if (std::fabs(r.cents) > 15) r.why = "pitch";
	else if (std::fabs(r.drift) > 1.0) r.why = "drift";
	else if (x1 < 0.5 * a1) r.why = "subharm";
	else r.ok = true;
	if (!r.ok) st.ok = 0;
	return r;
}

static void print(int preset, int note, int d, const Res& r) {
	const State& st = r.st;
	printf("CAP %d %d %d %d %.3f %.2f %.6g %d %d %d", preset, note, d, (int)st.ok, st.per, st.trim, st.um, st.combo, st.k, st.ext);
	for (int q = 0; q < NSIG; q++) printf(" %.6g", st.dc[q]);
	for (int q = 0; q < NSIG; q++)
		for (int k = 0; k < NH; k++) printf(" %.6g %.6g", st.re[q][k], st.im[q][k]);
	printf("%c", 10);
	fprintf(stderr, "preset %d note %d dyn %.2f: %s %s cents %.1f drift %.2f amp %.2e trim %.1f k %d c %d e %d\n", preset, note, trumpet::seed::DYN[d], r.ok ? "ok" : "--", r.why, r.cents, r.drift, r.amp, st.trim, st.k, st.combo, st.ext);
}

int main(int argc, char** argv) {
	const int preset = atoi(argv[1]);
	const int note = atoi(argv[2]);
	const double secs = argc > 3 ? atof(argv[3]) : 0.8;
	Res R[NDYN];
	for (int d = 0; d < NDYN; d++) R[d] = run(preset, (float)note, trumpet::seed::DYN[d], nullptr, secs);
	// continuation from the nearest good breath (louder first: a louder seed settles down safely)
	for (int pass = 0; pass < 2; pass++)
		for (int d = 0; d < NDYN; d++) {
			if (R[d].ok) continue;
			for (int dd = 1; dd < NDYN; dd++)
				for (int sgn : {+1, -1}) {
					const int j = d + sgn * dd;
					if (R[d].ok || j < 0 || j >= NDYN || !R[j].ok) continue;
					Res t = run(preset, (float)note, trumpet::seed::DYN[d], &R[j].st, secs);
					if (t.ok) R[d] = t;
				}
		}
	// refinement: the state the seeded note itself settles in
	for (int d = 0; d < NDYN; d++) {
		if (!R[d].ok) continue;
		Res t = run(preset, (float)note, trumpet::seed::DYN[d], &R[d].st, secs);
		if (t.ok) R[d] = t;
	}
	for (int d = 0; d < NDYN; d++) print(preset, note, d, R[d]);
	return 0;
}
