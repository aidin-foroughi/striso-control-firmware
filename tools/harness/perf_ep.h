// Performances for the ep (electric piano) voice, same interface as performance.h.
// Default: stress test, 17.5 s, written for 10 voices (render with tools/harness/out/ep_render.cpp,
// EP_NV=10; with fewer voices the higher voices are skipped):
//  A 0.0-2.5   single note 69 (440 Hz), medium strike, held (pitch check 0.4-2.3 s)
//  B 2.5-5.0   fast repeated strikes: one held key re-struck every 60 ms (pressure dips and
//              rises), plus 2 ms strikes cycling over all 10 voices, notes 28..103
//  C 5.0-7.5   hard chords on all 10 voices (pressure 1.2, 1 ms strikes), pedal down, repeated,
//              then pedal up at 6.8 s while the keys are already released (10 dampers at once)
//  D 7.5-10.0  10 held voices jumping to new notes every 90 ms, tilt sweeping +-1, pressure
//              swells to 1.5
//  E 10.0-12.0 out-of-range controls: notes -10..140, pressure 3, tilt +-3, vpres +-2, pedal
//              flapping, shaking
//  F 12.0-14.0 high note 96 hard alone (aliasing check 12.2-12.9 s), then note 28 hard (bass bark)
//  G 14.0-17.5 everything: dense random strikes on all voices, pedal toggling every 150 ms,
//              chords, vibrato, tilt
// -DEP_TONE: strike sweep for analysis: notes {28,40,52,64,76,88,100} x vpres {0.12,0.45,1.0},
//            each held 1.2 s (pres 0.6) then released 0.3 s, on voice 0 (31.5 s)
// -DEP_DEMO [-DEP_DEMO_TRANSPOSE=n]: musical demo (~16 s): chords, melody, soft to hard, pedal
// -DEP_FUZZ: random abrupt controls every block (some out of range)
#ifndef PERFORMANCE_H_
#define PERFORMANCE_H_
#include <math.h>
#include <stdint.h>

#define SAMPLERATE 48828
#define BLOCK 32
#define NVOICES_MAX 15
#define pmax(x,y) ((x)>(y)?(x):(y))
#define pmin(x,y) ((x)<(y)?(x):(y))

struct Ctl { float note, pres, vpres, but_x, but_y; };

static double gate(double t, double t0, double t1, double att, double rel) {
	if (t < t0) return 0;
	double a = pmin((t - t0) / att, 1.0);
	if (t < t1) return a;
	double r = 1.0 - (t - t1) / rel;
	return r > 0 ? a * r : 0;
}
static double hrand(int a, int b) {   // deterministic hash -> [0, 1)
	uint32_t h = (uint32_t)a * 2654435761u ^ (uint32_t)b * 2246822519u;
	h ^= h >> 13; h *= 3266489917u; h ^= h >> 16;
	return (h & 0xffffff) / 16777216.0;
}

static float last_note[NVOICES_MAX] = {69, 69, 69, 69, 69, 69, 69, 69, 69, 69, 69, 69, 69, 69, 69};

static void stress_controls(double t, Ctl* v, int nv, float* g) {
	for (int i = 0; i < nv; i++) v[i] = {last_note[i], 0, 0, 0, 0};
	g[0] = 1; g[1] = 0; g[2] = 0; g[3] = -1; g[4] = 0; g[5] = 0; g[6] = 0; g[7] = 3.0f / 16;
	auto set = [&](int i, double note, double pres, double bx, double by) {
		if (i >= nv) return;
		v[i].pres = (float)pres;
		v[i].but_x = (float)bx;
		v[i].but_y = (float)by;
		v[i].note = (float)(note + 0.5 * bx * bx * bx);
		last_note[i] = v[i].note;
	};
	// A
	{ double e = gate(t, 0.05, 2.4, 0.02, 0.05); if (e > 0) set(0, 69, 0.6 * e, 0, 0); }
	// B: repeated strikes on one held key (voice 0) + strikes over all voices
	if (t >= 2.5 && t < 5.0) {
		int k = (int)((t - 2.5) / 0.06);
		double u = t - 2.5 - 0.06 * k;
		double p = u < 0.003 ? 0.15 + 0.75 * u / 0.003 : (u < 0.04 ? 0.9 : 0.9 - 0.75 * pmin((u - 0.04) / 0.01, 1.0));
		set(0, 64, p, 0, 0);
		int j = (int)((t - 2.5) / 0.05);
		double t0 = 2.5 + 0.05 * j;
		static const int nn[9] = {28, 64, 52, 88, 71, 103, 47, 76, 40};
		double e = gate(t, t0, t0 + 0.03 + 0.02 * (j % 3), 0.002, 0.01);
		if (e > 0) set(1 + j % 9, nn[j % 9], e * (0.3 + 0.9 * hrand(j, 1)), 0, 0);
	}
	// C: hard chords on all voices, pedal down 5.0-6.8
	if (t >= 5.0 && t < 6.8) g[7] = 1.0;
	for (int r = 0; r < 3; r++) {
		static const int ch[10] = {29, 41, 48, 55, 60, 64, 67, 72, 76, 84};
		double t0 = 5.0 + 0.5 * r;
		for (int i = 0; i < 10; i++) {
			double e = gate(t, t0 + 0.002 * i, t0 + 0.3, 0.001, 0.02);
			if (e > 0) set(i, ch[i] + 2 * r, 1.2 * e, 0, 0);
		}
	}
	// D: jumps on held voices, tilt sweeps, pressure swells
	if (t >= 7.5 && t < 10.0) {
		for (int i = 0; i < nv && i < 10; i++) {
			double e = gate(t, 7.5, 9.9, 0.01, 0.1);
			if (e <= 0) continue;
			int k = (int)((t - 7.5) / 0.09) + 3 * i;
			static const int jj[7] = {0, 19, -12, 7, 24, -17, 5};
			set(i, 40 + 4 * i + jj[k % 7], e * (0.75 + 0.75 * sin(2 * M_PI * 2.3 * t + i)), 0.3 * sin(2 * M_PI * 5.5 * t + i),
			    sin(2 * M_PI * 1.5 * t + 1.1 * i));
		}
	}
	// E: out-of-range controls
	if (t >= 10.0 && t < 12.0) {
		int k = (int)((t - 10.0) / 0.04);
		for (int i = 0; i < nv && i < 10; i++) {
			double r1 = hrand(k, i), r2 = hrand(k, i + 20), r3 = hrand(k, i + 40);
			if (r1 < 0.3) continue;
			set(i, -10 + 150 * r2, 3.0 * r3, 2 * hrand(k, i + 60) - 1, 6 * hrand(k, i + 80) - 3);
			if (r1 > 0.9) v[i].vpres = 2.0f;   // overridden below unless EP_VPRES_RAW
		}
		g[7] = (k % 3) == 0 ? 1.0f : (k % 3 == 1 ? 0.0f : 3.0f);
		double s = sin(2 * M_PI * 5 * t);
		g[0] = 1 + 3 * s * s;
		g[4] = g[5] = g[6] = (float)sin(2 * M_PI * 2 * t);
	}
	// F: high note hard, then low note hard
	{ double e = gate(t, 12.0, 12.95, 0.001, 0.03); if (e > 0) set(0, 96, 1.0 * e, 0, 0); }
	{ double e = gate(t, 13.0, 13.95, 0.001, 0.03); if (e > 0) set(1, 28, 1.0 * e, 0, 0); }
	// G: everything
	if (t >= 14.0 && t < 17.0) {
		for (int i = 0; i < nv && i < 10; i++) {
			double per = 0.07 + 0.013 * i;
			int k = (int)((t - 14.0) / per);
			double t0 = 14.0 + per * k;
			double e = gate(t, t0, t0 + per * 0.6, 0.001, 0.005);
			double nt = 24 + 84 * hrand(k, i + 100);
			if (e > 0) set(i, nt, (0.2 + 1.3 * hrand(k, i + 120)) * e, sin(2 * M_PI * 6 * t + i), sin(2 * M_PI * 1.1 * t + i));
		}
		g[7] = ((int)((t - 14.0) / 0.15) % 2) ? 1.0f : 0.0f;
		double s = sin(2 * M_PI * 4 * t);
		g[0] = 1 + 2 * s * s;
	}
}

#ifdef EP_TONE
static const int tone_notes[7] = {28, 40, 52, 64, 76, 88, 100};
static const float tone_vel[3] = {0.12f, 0.45f, 1.0f};
#ifndef EP_TONE_Y
#define EP_TONE_Y 0.0f
#endif
// returns true when vpres is set explicitly
static bool tone_controls(int b, Ctl* v, int nv, float* g) {
	double t = (double)b * BLOCK / SAMPLERATE;
	for (int i = 0; i < nv; i++) v[i] = {last_note[i], 0, 0, 0, 0};
	g[0] = 1; g[1] = 0; g[2] = 0; g[3] = -1; g[4] = 0; g[5] = 0; g[6] = 0; g[7] = 3.0f / 16;
	int seg = (int)(t / 1.5);
	if (seg >= 21) return true;
	double u = t - 1.5 * seg;
	int bi = (int)(u * SAMPLERATE / BLOCK);
	v[0].note = last_note[0] = (float)tone_notes[seg / 3];
	if (u < 1.2) {
		v[0].pres = 0.6f;
		v[0].vpres = bi < 2 ? tone_vel[seg % 3] : 0.0f;
		v[0].but_y = EP_TONE_Y;
	} else {
		v[0].vpres = bi < 2 + (int)(1.2 * SAMPLERATE / BLOCK) ? -0.3f : 0.0f;
	}
	return true;
}
#endif

#ifdef EP_DEMO
#ifndef EP_DEMO_TRANSPOSE
#define EP_DEMO_TRANSPOSE 0
#endif
// voice, start, end, note, strike (vpres), hold pressure
struct DNote { int voice; double t0, t1; int note; double vel, pres; };
static const DNote demo[] = {
	// 1: soft chords, Fmaj9 -> Em9 (left hand + chord), pedal
	{0, 0.00, 1.90, 41, 0.18, 0.40}, {1, 0.02, 1.90, 52, 0.15, 0.35}, {2, 0.04, 1.90, 57, 0.15, 0.35},
	{3, 0.05, 1.90, 60, 0.16, 0.35}, {4, 0.06, 1.90, 64, 0.17, 0.35},
	{5, 2.00, 3.90, 40, 0.22, 0.40}, {6, 2.02, 3.90, 50, 0.20, 0.35}, {7, 2.04, 3.90, 55, 0.20, 0.35},
	{8, 2.05, 3.90, 59, 0.21, 0.35}, {9, 2.06, 3.90, 62, 0.22, 0.35},
	// 2: melody over Dm9 - G13, medium strikes, a hard accent
	{0, 4.00, 7.70, 38, 0.40, 0.50}, {1, 4.02, 7.70, 53, 0.30, 0.40}, {2, 4.03, 7.70, 57, 0.30, 0.40}, {3, 4.04, 7.70, 60, 0.30, 0.40},
	{4, 4.30, 4.60, 69, 0.45, 0.55}, {5, 4.60, 4.90, 72, 0.50, 0.55}, {6, 4.90, 5.60, 76, 0.75, 0.60},
	{7, 5.70, 5.95, 74, 0.45, 0.55}, {8, 5.95, 6.20, 72, 0.40, 0.50}, {9, 6.20, 7.60, 77, 1.00, 0.70},
	{4, 7.80, 9.40, 43, 0.55, 0.55}, {5, 7.82, 9.40, 53, 0.45, 0.45}, {6, 7.83, 9.40, 57, 0.45, 0.45},
	{7, 7.84, 9.40, 59, 0.45, 0.45}, {8, 7.85, 9.40, 64, 0.50, 0.50},
	// 3: hard, barking riff in the middle register (no pedal)
	{0, 9.60, 9.80, 48, 1.00, 0.80}, {1, 9.60, 9.80, 55, 0.90, 0.80}, {2, 9.60, 9.80, 58, 0.90, 0.80},
	{3, 9.95, 10.10, 48, 0.70, 0.70}, {0, 10.25, 10.60, 51, 1.00, 0.90}, {1, 10.25, 10.60, 58, 0.95, 0.90},
	{2, 10.25, 10.60, 61, 0.95, 0.90}, {3, 10.75, 10.90, 53, 0.80, 0.80}, {4, 11.00, 11.70, 55, 1.00, 1.00},
	{5, 11.00, 11.70, 62, 1.00, 1.00}, {6, 11.00, 11.70, 65, 1.00, 1.00},
	// 4: final chord soft -> pedal, high sparkle, long ring
	{0, 12.00, 15.00, 36, 0.35, 0.45}, {1, 12.02, 15.00, 52, 0.30, 0.40}, {2, 12.03, 15.00, 55, 0.30, 0.40},
	{3, 12.04, 15.00, 59, 0.30, 0.40}, {4, 12.05, 15.00, 62, 0.30, 0.40},
	{5, 12.60, 12.90, 79, 0.35, 0.50}, {6, 12.90, 13.20, 83, 0.40, 0.50}, {7, 13.20, 15.00, 86, 0.45, 0.50},
};
static bool demo_controls(double t, Ctl* v, int nv, float* g) {
	for (int i = 0; i < nv; i++) v[i] = {last_note[i], 0, 0, 0, 0};
	g[0] = 1; g[1] = 0; g[2] = 0; g[3] = -1; g[4] = 0; g[5] = 0; g[6] = 0; g[7] = 3.0f / 16;
	if (t < 3.95 || (t > 4.0 && t < 7.75) || (t > 7.8 && t < 9.45) || (t > 12.0 && t < 15.8)) g[7] = 1.0f;
	for (const DNote& n : demo) {
		if (n.voice >= nv || t < n.t0 || t > n.t1 + 0.02) continue;
		Ctl& c = v[n.voice];
		// pressure rises at the speed giving vpres = vel (see block_controls), holds, releases fast
		double att = n.pres * 19.07 / n.vel * BLOCK / SAMPLERATE;
		double e = gate(t, n.t0, n.t1, att, 0.012);
		if (e <= 0) continue;
		c.pres = (float)(n.pres * e * (1.0 + 0.1 * sin(2 * M_PI * 0.7 * (t - n.t0))));
		c.note = (float)(n.note + EP_DEMO_TRANSPOSE);
		last_note[n.voice] = c.note;
	}
	return false;
}
#endif

#ifdef EP_FUZZ
static void fuzz_controls(int b, Ctl* v, int nv, float* g) {
	static uint32_t r = 1;
	static float hold[NVOICES_MAX][5];
	auto rnd = [&]() { r = r * 1664525u + 1013904223u; return (float)(r >> 8) * (1.0f / 16777216.0f); };
	for (int i = 0; i < nv; i++) {
		if (b == 0 || rnd() < 0.05f) {
			float m = rnd();
			hold[i][0] = m < 0.05f ? -20.0f + 300.0f * rnd() : 127.0f * rnd();
			hold[i][1] = rnd() < 0.3f ? 0.0f : (rnd() < 0.05f ? 3.0f * rnd() : 1.6f * rnd());
			hold[i][2] = rnd() < 0.2f ? 4.0f * rnd() - 2.0f : 0.0f;
			hold[i][3] = 2.0f * rnd() - 1.0f;
			hold[i][4] = rnd() < 0.1f ? 6.0f * rnd() - 3.0f : 2.0f * rnd() - 1.0f;
		}
		hold[i][0] += 0.3f * (rnd() - 0.5f);
		v[i] = {hold[i][0], hold[i][1] * (rnd() < 0.02f ? 0.0f : 1.0f), hold[i][2], hold[i][3], hold[i][4]};
	}
	g[0] = rnd() < 0.3f ? 4.0f * rnd() : 1.0f;
	g[1] = 2 * rnd() - 1; g[2] = 2 * rnd() - 1; g[3] = 2 * rnd() - 1;
	g[4] = rnd() < 0.5f ? 0.0f : 2 * rnd() - 1; g[5] = 2 * rnd() - 1; g[6] = 2 * rnd() - 1;
	g[7] = rnd() < 0.3f ? 3.0f * rnd() - 1.0f : rnd();
}
#endif

static void block_controls(int b, Ctl* v, int nv, float* g) {
	static float pres_old[NVOICES_MAX] = {0};
	bool raw = false;
#if defined(EP_FUZZ)
	fuzz_controls(b, v, nv, g);
	raw = true;
#elif defined(EP_TONE)
	raw = tone_controls(b, v, nv, g);
#elif defined(EP_DEMO)
	raw = demo_controls((double)b * BLOCK / SAMPLERATE, v, nv, g);
#else
	stress_controls((double)b * BLOCK / SAMPLERATE, v, nv, g);
#endif
	for (int i = 0; i < nv; i++) {
		if (!raw) v[i].vpres = pmax(-1.0f, pmin(1.0f, (v[i].pres - pres_old[i]) * (SAMPLERATE / BLOCK) * 0.0125f));
		pres_old[i] = v[i].pres;
	}
#if !defined(EP_FUZZ) && !defined(EP_TONE) && !defined(EP_DEMO)
	// section E: some raw out-of-range vpres
	double t = (double)b * BLOCK / SAMPLERATE;
	if (t >= 10.0 && t < 12.0)
		for (int i = 0; i < nv; i++) if (hrand(b, i + 200) < 0.05) v[i].vpres = (float)(4 * hrand(b, i + 300) - 2);
#endif
}

#endif
