// Stress performance for the modal voice (voice_modal.h), 17.5 s, same interface as performance.h.
// Sections:
//  A 0.0- 2.5  fast repeated hard strikes on one key (every 60 ms) while but_y sweeps all materials
//  B 2.5- 5.5  all 6 voices rubbed hard (pres up to 1.3, fast pressure wobble), fast full morph sweeps
//  C 5.5- 8.0  note jumps every 90 ms on all voices (strike at each), notes 0..127, held between
//  D 8.0-11.0  6-voice bell chords struck together in phase every 150 ms, pedal 1 (long ring)
//  E 11.0-14.0 bowed chord with extreme vibrato (but_x +-1 at 7 Hz), gyro + shaking, pedal flipping
//  F 14.0-16.0 random pres/vpres/but_y every block (vpres +-1), random notes, then release and ring out
#ifndef PERFORMANCE_H_
#define PERFORMANCE_H_
#include <math.h>
#include <stdint.h>

#define SAMPLERATE 48000
#define BLOCK 32
#define NVOICES_MAX 15
#define pmax(x,y) ((x)>(y)?(x):(y))
#define pmin(x,y) ((x)<(y)?(x):(y))

struct Ctl { float note, pres, vpres, but_x, but_y; };

static double prand(uint32_t a, uint32_t b) {  // deterministic hash -> [0, 1)
	uint32_t h = a * 2654435761u ^ (b + 0x9e3779b9u) * 2246822519u;
	h ^= h >> 15; h *= 2246822519u; h ^= h >> 13; h *= 3266489917u; h ^= h >> 16;
	return h / 4294967296.0;
}

// pulse: 1 during [0, len) of each period, with 3 ms attack
static double pulse(double t, double period, double len) {
	double p = fmod(t, period);
	if (p >= len) return 0;
	return pmin(p / 0.003, 1.0);
}

static void controls(double t, int blk, Ctl* v, int nv, float* g, bool* vp_override, float* vp) {
	static float last_note[NVOICES_MAX] = {69, 69, 69, 69, 69, 69, 69, 69, 69, 69, 69, 69, 69, 69, 69};
	for (int i = 0; i < nv; i++) { v[i] = {last_note[i], 0, 0, 0, 0}; vp_override[i] = false; vp[i] = 0; }
	g[0] = 1; g[1] = 0; g[2] = 0; g[3] = -1; g[4] = 0; g[5] = 0; g[6] = 0; g[7] = 3.0f / 16;
	const int chord[6] = {36, 43, 48, 55, 60, 64};
	if (t < 2.5) {
		v[0].note = 48;
		v[0].pres = 0.9 * pulse(t, 0.06, 0.03);
		v[0].but_y = -1 + 2 * (t / 2.5);
		v[1].note = 72;
		v[1].pres = 1.0 * pulse(t + 0.02, 0.045, 0.02);
		v[1].but_y = 1 - 2 * (t / 2.5);
	} else if (t < 5.5) {
		double tt = t - 2.5;
		for (int i = 0; i < nv && i < 6; i++) {
			double e = pmin(tt / 0.2, 1.0) * pmin((5.5 - t) / 0.1, 1.0);
			v[i].note = chord[i] + 12;
			v[i].pres = e * (0.9 + 0.4 * sin(2 * M_PI * 8 * t + i));
			v[i].but_y = sin(2 * M_PI * 1.5 * t + 0.7 * i);
		}
	} else if (t < 8.0) {
		double tt = t - 5.5;
		int k = (int)(tt / 0.09);
		for (int i = 0; i < nv && i < 6; i++) {
			v[i].note = (float)(int)(128 * prand(k, i));
			double p = fmod(tt, 0.09);
			v[i].pres = 0.4 + 0.6 * pmin(p / 0.004, 1.0) * prand(k, i + 10);
			v[i].but_y = 2 * prand(k, i + 20) - 1;
		}
	} else if (t < 11.0) {
		double tt = t - 8.0;
		g[7] = 1.0;
		for (int i = 0; i < nv && i < 6; i++) {
			v[i].note = chord[i] + 24;
			v[i].pres = 1.0 * pulse(tt, 0.15, 0.05);
			v[i].but_y = 1.0;
		}
	} else if (t < 14.0) {
		double tt = t - 11.0;
		for (int i = 0; i < nv && i < 6; i++) {
			double e = pmin(tt / 0.05, 1.0) * pmin((14.0 - t) / 0.05, 1.0);
			v[i].pres = e * (0.6 + 0.3 * sin(2 * M_PI * 0.5 * t + i));
			v[i].but_x = sin(2 * M_PI * 7 * t + 1.3 * i);
			v[i].note = chord[i] + 12 + 0.5 * v[i].but_x * v[i].but_x * v[i].but_x;
			v[i].but_y = -0.5 + 0.5 * i / 5.0 * 3;
		}
		double s = sin(2 * M_PI * 4 * t);
		g[0] = 1 + 3 * s * s;
		g[4] = sin(2 * M_PI * 1.1 * t); g[5] = sin(2 * M_PI * 1.7 * t + 1); g[6] = sin(2 * M_PI * 0.6 * t + 2);
		g[7] = fmod(t, 0.5) < 0.25 ? 0.0 : 1.0;
	} else if (t < 16.0) {
		for (int i = 0; i < nv && i < 6; i++) {
			v[i].note = 20 + (float)(95 * prand(blk / 37, i));
			v[i].pres = 1.5 * prand(blk, i + 30) * (prand(blk, i + 40) > 0.3);
			v[i].but_y = 2 * prand(blk, i + 50) - 1;
			v[i].but_x = 2 * prand(blk, i + 60) - 1;
			vp_override[i] = true;
			vp[i] = 2 * prand(blk, i + 70) - 1;
		}
		g[7] = prand(blk, 99);
		g[4] = 2 * prand(blk, 98) - 1;
	}
	for (int i = 0; i < nv; i++) last_note[i] = v[i].note;
}

static void block_controls(int b, Ctl* v, int nv, float* g) {
	static float pres_old[NVOICES_MAX] = {0};
	bool ovr[NVOICES_MAX];
	float vp[NVOICES_MAX];
	controls((double)b * BLOCK / SAMPLERATE, b, v, nv, g, ovr, vp);
	for (int i = 0; i < nv; i++) {
		v[i].vpres = ovr[i] ? vp[i] : pmax(-1.0f, pmin(1.0f, (v[i].pres - pres_old[i]) * (SAMPLERATE / BLOCK) * 0.0125f));
		pres_old[i] = v[i].pres;
	}
}

#endif
