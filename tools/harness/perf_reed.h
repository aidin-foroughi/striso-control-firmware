// Stress performance for the reed voice (tools/harness/render_strisy.sh with PERF=perf_reed.h).
// Same structure as performance.h. 17.5 s:
//   A 0.0-3.0   breath swell 0 -> 1.4 -> 0 on one note while the tilt sweeps clarinet -> sax
//   B 3.0-5.5   fast tonguing: 10 strikes/s alternating notes, instant attacks
//   C 5.5-8.0   legato note jumps on a held voice (1 to 19 semitones) with vibrato, 2nd voice glide
//   D 8.0-11.5  all 6 voices, hard blowing (pressure up to 1.5), fast full-range tilt, shaking
//   E 11.5-14.0 extremes: notes 20, 21, 113, 115 hard blown, glide 20 -> 115, fast tilt
//   F 14.0-16.5 chaos: 6 voices, pseudo-random notes, instant 0 <-> 1.5 pressure steps
//   G 16.5-17.5 release tail (silence)
#ifndef PERFORMANCE_H_
#define PERFORMANCE_H_
#include <math.h>

#define SAMPLERATE 48828
#define BLOCK 32
#define NVOICES_MAX 15
#define pmax(x,y) ((x)>(y)?(x):(y))
#define pmin(x,y) ((x)<(y)?(x):(y))

struct Ctl { float note, pres, vpres, but_x, but_y; };

static double clamp1(double x) { return x < -1 ? -1 : (x > 1 ? 1 : x); }
static unsigned hash32(unsigned x) { x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16; return x; }

static void controls(double t, Ctl* v, int nv, float* g) {
	static float last_note[NVOICES_MAX] = {69, 69, 69, 69, 69, 69, 69, 69, 69, 69, 69, 69, 69, 69, 69};
	for (int i = 0; i < nv; i++) v[i] = {last_note[i], 0, 0, 0, 0};
	g[0] = 1; g[1] = 0; g[2] = 0; g[3] = -1; g[4] = 0; g[5] = 0; g[6] = 0; g[7] = 3.0f / 16;
	if (t < 3.0) {  // A
		double u = t / 3.0;
		v[0].note = 50;
		v[0].pres = (float)(1.4 * sin(M_PI * u) * sin(M_PI * u));
		v[0].but_y = (float)(-1 + 2 * u);
	} else if (t < 5.5) {  // B
		double tt = t - 3.0;
		int k = (int)(tt * 10);
		double ph = tt * 10 - k;
		v[0].note = (k % 2) ? 62 : 69;
		v[0].pres = ph < 0.6 ? 0.8f : 0.0f;  // instant on/off
		v[0].but_y = (float)sin(tt * 3);
		v[1].note = (k % 3) ? 45 : 57;
		v[1].pres = ph > 0.3 && ph < 0.5 ? 1.2f : 0.0f;
		v[1].but_y = -0.5f;
	} else if (t < 8.0) {  // C
		double tt = t - 5.5;
		static const int seq[] = {60, 61, 63, 67, 55, 74, 50, 69, 64, 76, 57, 45, 64, 52, 71, 60, 79};
		int k = (int)(tt / 0.15);
		v[0].note = (float)(seq[k % 17] + 0.4 * sin(2 * M_PI * 6 * tt));
		v[0].pres = 0.7f;
		v[0].but_x = (float)(0.4 * sin(2 * M_PI * 6 * tt));
		v[0].but_y = (float)(0.8 * sin(tt * 2));
		v[1].note = (float)(40 + 30 * tt / 2.5);
		v[1].pres = 0.5f;
		v[1].but_y = 0.7f;
	} else if (t < 11.5) {  // D
		double tt = t - 8.0;
		static const int ch[] = {36, 48, 55, 64, 70, 79};
		for (int i = 0; i < nv && i < 6; i++) {
			v[i].note = (float)(ch[i] + 0.3 * sin(2 * M_PI * 5.5 * tt + i));
			v[i].pres = (float)(pmin(tt / 0.05, 1.0) * (0.9 + 0.6 * sin(2 * M_PI * 0.8 * tt + i)));
			v[i].but_y = (float)clamp1(1.3 * sin(2 * M_PI * 1.7 * tt + 0.7 * i));
			v[i].but_x = (float)(0.3 * sin(2 * M_PI * 5.5 * tt + i));
		}
		double s = sin(2 * M_PI * 4 * tt);
		g[0] = (float)(1 + 2.5 * s * s);
		g[4] = (float)(0.9 * sin(2 * M_PI * 1.1 * tt));
		g[5] = (float)(0.9 * sin(2 * M_PI * 1.3 * tt + 1));
		g[7] = (float)(0.5 + 0.5 * sin(tt));
	} else if (t < 14.0) {  // E
		double tt = t - 11.5;
		v[0].note = 20; v[0].pres = 1.3f; v[0].but_y = (float)sin(2 * M_PI * 2 * tt);
		v[1].note = 115; v[1].pres = 1.3f; v[1].but_y = (float)-sin(2 * M_PI * 2 * tt);
		v[2].note = 21; v[2].pres = (float)(0.5 + 0.5 * sin(2 * M_PI * 3 * tt)); v[2].but_y = 1;
		v[3].note = 113; v[3].pres = (float)(0.5 + 0.5 * sin(2 * M_PI * 3 * tt)); v[3].but_y = -1;
		v[4].note = (float)(20 + 95 * pmin(tt / 1.2, 1.0)); v[4].pres = 1.0f; v[4].but_y = (float)sin(7 * tt);
		v[5].note = (float)(115 - 95 * pmin(tt / 1.2, 1.0)); v[5].pres = 0.6f; v[5].but_y = (float)cos(7 * tt);
	} else if (t < 16.5) {  // F
		double tt = t - 14.0;
		int k = (int)(tt * 23);
		for (int i = 0; i < nv && i < 6; i++) {
			unsigned h = hash32((unsigned)(k * 7 + i));
			v[i].note = (float)(20 + h % 96);
			v[i].pres = (h >> 8) % 3 ? (float)(((h >> 12) % 16) / 10.0) : 0.0f;
			v[i].but_y = (float)((((h >> 16) % 201) - 100) / 100.0);
			v[i].but_x = (float)((((h >> 24) % 21) - 10) / 50.0);
		}
		g[0] = (float)(1 + 2 * ((k % 5) == 0));
	}
	for (int i = 0; i < nv; i++) {
		v[i].note += 0.5f * v[i].but_x * v[i].but_x * v[i].but_x;
		if (v[i].pres > 0) last_note[i] = v[i].note;
	}
}

// controls for block b; vpres is the pressure velocity as in performance.h
static void block_controls(int b, Ctl* v, int nv, float* g) {
	static float pres_old[NVOICES_MAX] = {0};
	controls((double)b * BLOCK / SAMPLERATE, v, nv, g);
	for (int i = 0; i < nv; i++) {
		v[i].vpres = pmax(-1.0f, pmin(1.0f, (v[i].pres - pres_old[i]) * (SAMPLERATE / BLOCK) * 0.0125f));
		pres_old[i] = v[i].pres;
	}
}

#endif
