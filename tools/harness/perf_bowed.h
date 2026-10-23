// Stress performance for the bowed string voice (voice_bowed.h): same interface as performance.h.
// Sections (17.5 s, the renderer's default length):
//  A 0-2.5      slow pressure swells 0.05 -> 1 -> 0.05 with vibrato, low and mid note together
//  B 2.5-5      pressure tremolo (8 Hz, 0.15..0.95), tilt sweeps -1..1, a third voice holding
//  C 5-6.5      extreme pressure: held at 1.5, and repeated instant 0 -> 1.4 attacks (vpres = 1)
//  D 6.5-8.5    fast strikes: 17 ms notes over the whole range on all voices, 2 ms attacks
//  E 8.5-11     note jumps on a held bow (20 <-> 115 and everything between), then fast glides
//  F 11-14.5    all (10) voices: extreme chord 20..115 at pressure 1.2, tilt extremes, shaking,
//               rotation, pedal 1; released all at once at 13.5 (pedal held: long ring)
//  G 14.5-16.5  bow bouncing: pressure flutter 0 <-> 1 at 30 Hz on all voices with random tilt
//  H 16.5-17.5  silence (tails, gating)
#ifndef PERFORMANCE_H_
#define PERFORMANCE_H_
#include <math.h>

#define SAMPLERATE 48000
#define BLOCK 32
#define NVOICES_MAX 15
#define pmax(x,y) ((x)>(y)?(x):(y))
#define pmin(x,y) ((x)<(y)?(x):(y))

struct Ctl { float note, pres, vpres, but_x, but_y; };

static double frac_(double x) { return x - floor(x); }
// deterministic pseudo random in [0,1) from an integer
static double hash01(int i) {
	unsigned int h = (unsigned int)i * 2654435761u;
	h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;
	return (h & 0xffffff) / 16777216.0;
}

#ifdef BOWED_DEMO
// A short musical demo (17.5 s), transposed by DEMO_BASE (the preset's register, MIDI note of
// the tonic): a legato melody with swells and vibrato through tilt-x, a held chord with a swell,
// accented (struck) notes, a quick lift-off that rings, a tilt sweep tasto -> ponticello.
#ifndef DEMO_BASE
#define DEMO_BASE 62
#endif
static void controls(double t, Ctl* v, int nv, float* g) {
	static float last_note[NVOICES_MAX] = {69, 69, 69, 69, 69, 69, 69, 69, 69, 69, 69, 69, 69, 69, 69};
	for (int i = 0; i < nv; i++) v[i] = {last_note[i], 0, 0, 0, 0};
	g[0] = 1; g[1] = 0; g[2] = 0; g[3] = -1; g[4] = 0; g[5] = 0; g[6] = 0; g[7] = 3.0f / 16;
	auto set = [&](int i, double note, double pres, double bx, double by) {
		if (i >= nv) return;
		v[i].note = (float)(note + 0.5 * bx * bx * bx);
		v[i].pres = (float)pmax(pres, 0.0);
		v[i].but_x = (float)bx; v[i].but_y = (float)by;
		last_note[i] = v[i].note;
	};
	const double B = DEMO_BASE;
	// 1: legato melody on one voice (0 - 6 s): note, start, length
	static const double mel[][3] = {{0, 0.2, 0.9}, {2, 1.1, 0.5}, {4, 1.6, 0.5}, {5, 2.1, 1.0}, {7, 3.1, 0.6},
		{9, 3.7, 0.4}, {7, 4.1, 0.4}, {4, 4.5, 1.4}};
	if (t < 6.2) {
		for (const auto& m : mel) {
			if (t >= m[1] && t < m[1] + m[2] + (m[1] + m[2] < 5.8 ? 0.0 : 0.3)) {
				double u = t - m[1], e = pmin(u / 0.08, 1.0);
				double fade = t > 5.6 ? pmax(0.0, 1.0 - (t - 5.6) / 0.35) : 1.0;
				double sw = 0.45 + 0.25 * sin(M_PI * pmin(u / m[2], 1.0));
				double vib = 0.28 * sin(2 * M_PI * 5.3 * t) * pmin(pmax(u - 0.25, 0.0) / 0.3, 1.0);
				set(0, B + m[0], e * sw * fade, vib, 0.0);
			}
		}
	}
	// 2: chord with a swell (6.3 - 9.8 s)
	if (t >= 6.3 && t < 9.8) {
		static const int ch[4] = {-12, -5, 0, 4};
		for (int i = 0; i < 4; i++) {
			double u = t - 6.3 - 0.06 * i;
			if (u < 0) continue;
			double e = pmin(u / 0.25, 1.0) * (t > 9.4 ? pmax(0.0, 1.0 - (t - 9.4) / 0.35) : 1.0);
			double sw = 0.2 + 0.45 * sin(M_PI * pmin(u / 3.2, 1.0));
			set(1 + i, B + ch[i], e * sw, 0.2 * sin(2 * M_PI * (5.0 + 0.3 * i) * t), 0);
		}
	}
	// 3: accented notes (10 - 12.4 s): fast strikes, held 0.25 s
	if (t >= 10.0 && t < 12.4) {
		static const int st[6] = {0, 7, 5, 4, 2, 0};
		int k = (int)((t - 10.0) / 0.4);
		double u = t - 10.0 - 0.4 * k;
		if (k < 6 && u < 0.28) set(5 + (k % 2), B + st[k], pmin(u / 0.004, 1.0) * (u > 0.24 ? 0.0 : 0.8), 0, 0);
	}
	// 4: held note, tilt sweep fingerboard -> bridge -> flat, then a quick lift-off (12.6 - 16.6 s)
	if (t >= 12.6 && t < 15.6) {
		double u = t - 12.6;
		double by = u < 1.2 ? -u / 1.2 : (u < 2.4 ? -1 + 2 * (u - 1.2) / 1.2 : 1 - (u - 2.4) / 0.6);
		set(7, B - 5, pmin(u / 0.15, 1.0) * 0.65, 0.15 * sin(2 * M_PI * 5.4 * t), by);
	}
	if (t >= 15.6 && t < 15.62) set(7, B - 5, 0.65 * (1.0 - (t - 15.6) / 0.02), 0, 0);  // lift-off
}
#else
static void controls(double t, Ctl* v, int nv, float* g) {
	static float last_note[NVOICES_MAX] = {69, 69, 69, 69, 69, 69, 69, 69, 69, 69, 69, 69, 69, 69, 69};
	for (int i = 0; i < nv; i++) v[i] = {last_note[i], 0, 0, 0, 0};
	// globals: acc_abs, acc_x, acc_y, acc_z, rot_x, rot_y, rot_z, pedal
	g[0] = 1; g[1] = 0; g[2] = 0; g[3] = -1; g[4] = 0; g[5] = 0; g[6] = 0; g[7] = 3.0f / 16;
	auto set = [&](int i, double note, double pres, double bx, double by) {
		if (i >= nv) return;
		v[i].note = (float)(note + 0.5 * bx * bx * bx);
		v[i].pres = (float)pmax(pres, 0.0);
		v[i].but_x = (float)bx;
		v[i].but_y = (float)by;
		last_note[i] = v[i].note;
	};
	if (t < 2.5) {  // A
		double sw = 0.5 - 0.5 * cos(2 * M_PI * t / 2.5);
		double vib = 0.35 * sin(2 * M_PI * 5.5 * t) * pmin(t / 0.8, 1.0);
		set(0, 45, 0.05 + 0.95 * sw, vib, 0);
		set(1, 69, 0.05 + 0.95 * sw * sw, 0.35 * sin(2 * M_PI * 5.1 * t + 1), -0.3);
	} else if (t < 5.0) {  // B
		double u = t - 2.5;
		set(0, 62, 0.55 + 0.4 * sin(2 * M_PI * 8 * u), 0, 0);
		set(1, 50, 0.6, 0, sin(2 * M_PI * u / 1.5));
		set(2, 81, 0.5 + 0.2 * sin(2 * M_PI * 0.5 * u), 0.2 * sin(2 * M_PI * 6 * u), -sin(2 * M_PI * u / 1.1));
	} else if (t < 6.5) {  // C
		double u = t - 5.0;
		set(0, 57, 1.5, 0, 0.5);
		double ph = frac_(u / 0.25);
		set(1, 76, ph < 0.6 ? 1.4 : 0.0, 0, -0.5);
		set(2, 33, 1.5 * pmin(u / 0.01, 1.0), 0, 1.0);
	} else if (t < 8.5) {  // D
		double u = t - 6.5;
		int k = (int)(u / 0.05);
		double ph = u - k * 0.05;
		int vi = k % nv;
		double note = 20 + 95 * hash01(k);
		if (ph < 0.017) set(vi, note, ph < 0.002 ? ph / 0.002 * 0.95 : 0.95, 0, 2 * hash01(k + 1000) - 1);
		else set(vi, note, 0, 0, 0);
	} else if (t < 11.0) {  // E
		double u = t - 8.5;
		static const double seq[] = {20, 115, 40, 96, 60, 108, 25, 84, 52, 71, 115, 20, 66, 67, 43};
		if (u < 1.75) {
			int k = (int)(u / 0.15);
			set(0, seq[k % 15], 0.7, 0, 0);
			set(1, seq[(k + 5) % 15] - 0.5, 0.4 + 0.5 * hash01(k + 7), 0, 0.8);
		} else {
			double w = u - 1.75;
			set(0, 40 + 60 * w / 0.75, 0.8, 0, 0);       // fast glide up
			set(1, 100 - 70 * w / 0.75, 0.6, 0, -0.8);   // fast glide down
		}
	} else if (t < 14.5) {  // F
		double u = t - 11.0;
		static const double ch[10] = {20, 35, 52, 71, 90, 115, 28, 44, 63, 83};
		double rel = u < 2.5 ? 1.0 : 0.0;
		for (int i = 0; i < 10; i++)
			set(i, ch[i], 1.2 * rel * pmin(u / 0.03, 1.0), 0.9 * sin(2 * M_PI * 6.3 * u + i), (i % 2 ? 1.0 : -1.0));
		double s = sin(2 * M_PI * 4 * u);
		g[0] = 1 + 2.8 * s * s;
		g[4] = sin(2 * M_PI * 0.9 * u);
		g[5] = sin(2 * M_PI * 1.3 * u + 1);
		g[6] = sin(2 * M_PI * 1.7 * u + 2);
		g[7] = 1.0;
	} else if (t < 16.5) {  // G
		double u = t - 14.5;
		for (int i = 0; i < 10; i++) {
			double ph = frac_(u * 30 + i * 0.17);
			set(i, 30 + 13 * i, ph < 0.5 ? 1.0 : 0.0, 0, 2 * hash01((int)(u * 30) + 50 * i) - 1);
		}
	}
}

#endif  // BOWED_DEMO

static void block_controls(int b, Ctl* v, int nv, float* g) {
	static float pres_old[NVOICES_MAX] = {0};
	controls((double)b * BLOCK / SAMPLERATE, v, nv, g);
	for (int i = 0; i < nv; i++) {
		v[i].vpres = pmax(-1.0f, pmin(1.0f, (v[i].pres - pres_old[i]) * (SAMPLERATE / BLOCK) * 0.0125f));
		pres_old[i] = v[i].pres;
	}
}


#ifdef BOWED_TEST_HOOK
// Called by bowed::process() every block: writes a random in-range value to a random live
// parameter every BOWED_LIVE_EVERY blocks (*ptr = value, then changed(), as the app does), and
// with BOWED_LIVE_PRESETS also switches presets now and then. On the first call it checks the
// parameter table: unique names, distinct pointers, every preset value inside its range.
#include <stdio.h>
#include <string.h>
#ifndef BOWED_LIVE_EVERY
#define BOWED_LIVE_EVERY 1
#endif
namespace bowed {
static void bowed_test_hook(Engine& e) {
	typedef strisy::EngineParams<Engine> EP;
	static uint32_t r = 2654435761u + 7u;
	static int b = 0;
	auto rnd = [&]() { r = r * 1664525u + 1013904223u; return (float)(r >> 8) * (1.0f / 16777216.0f); };
	if (b == 0) {
		int bad = 0;
		for (int i = 0; i < EP::count; i++) {
			const strisy::ParamDesc* d = EP::desc(i);
			for (int j = 0; j < i; j++)
				if (!strcmp(d->name, EP::desc(j)->name) || EP::ptr(e, i) == EP::ptr(e, j)) { fprintf(stderr, "!! duplicate param %s\n", d->name); bad++; }
			for (int k = 0; k < NUM_PRESETS; k++) {
				Params q = preset_params(k);
				float x = q.*(param_table[i].m);
				if (!(x >= d->min && x <= d->max)) { fprintf(stderr, "!! preset %s: %s = %g outside [%g, %g]\n", preset_name(k), d->name, x, d->min, d->max); bad++; }
			}
		}
		fprintf(stderr, "live params: %d, table check %s\n", EP::count, bad ? "FAILED" : "ok");
	}
	b++;
	if (b % BOWED_LIVE_EVERY == 0) {
		int i = (int)(rnd() * EP::count) % EP::count;
		const strisy::ParamDesc* d = EP::desc(i);
		float x = d->min + rnd() * (d->max - d->min);
		if (d->kind == 2) x = x > 0.5f * (d->min + d->max) ? d->max : d->min;
		*EP::ptr(e, i) = x;
		EP::changed(e, i);
	}
#ifdef BOWED_LIVE_PRESETS
	if (b % 700 == 0) set_preset(e, (int)(rnd() * NUM_PRESETS) % NUM_PRESETS);
#endif
}
}
#endif

#endif
