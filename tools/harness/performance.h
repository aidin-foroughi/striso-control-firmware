// Scripted, deterministic Striso performance shared by the Faust and C++ renderers.
#ifndef PERFORMANCE_H_
#define PERFORMANCE_H_
#include <math.h>

#define SAMPLERATE 48828
#define BLOCK 32
#define NVOICES_MAX 15
#define pmax(x,y) ((x)>(y)?(x):(y))
#define pmin(x,y) ((x)<(y)?(x):(y))

// ---------------------------------------------------------------------------------------------
// Scripted performance. Everything is a deterministic function of time, evaluated once per block
// (the firmware updates controls asynchronously at ~1.3 kHz, close to the 1.5 kHz block rate).

struct Ctl { float note, pres, vpres, but_x, but_y; };

// gate envelope: 0 before t0, attack to 1, hold until t1, release to 0
static double gate(double t, double t0, double t1, double att, double rel) {
	if (t < t0) return 0;
	double a = pmin((t - t0) / att, 1.0);
	if (t < t1) return a;
	double r = 1.0 - (t - t1) / rel;
	return r > 0 ? a * r : 0;
}

struct Note { int voice; double t0, t1, att, rel, note, pres; double glide_to, glide_t; double vib, tilt; };

static const Note notes[] = {
	// A: single swelling note with onset strike
	{0, 0.10, 2.00, 0.020, 0.30, 60, 0.70, 0, 0, 0, 0},
	// B: six voice chord with vibrato through x (pitch bend) at different phases
	{0, 2.50, 6.00, 0.030, 0.40, 48, 0.50, 0, 0, 0.40, 0},
	{1, 2.60, 6.00, 0.030, 0.40, 55, 0.90, 0, 0, 0.40, 0},
	{2, 2.70, 6.00, 0.030, 0.40, 60, 0.30, 0, 0, 0.40, 0},
	{3, 2.80, 6.00, 0.030, 0.40, 64, 0.60, 0, 0, 0.40, 0},
	{4, 2.90, 6.00, 0.030, 0.40, 67, 0.80, 0, 0, 0.40, 0},
	{5, 3.00, 6.00, 0.030, 0.40, 72, 0.40, 0, 0, 0.40, 0},
	// C: tilt (y) sweeps: filter Q, low-pass damping and band-pass formant
	{0, 6.50, 9.30, 0.050, 0.20, 36, 0.60, 0, 0, 0, 1.0},
	{1, 6.50, 9.30, 0.050, 0.20, 79, 0.60, 0, 0, 0, 1.0},
	{2, 6.50, 9.30, 0.050, 0.20, 96, 0.60, 0, 0, 0, 1.0},
	// D: held notes under accelerometer shakes and gyro rotation (body filters)
	{3, 9.50, 12.00, 0.040, 0.30, 50, 0.50, 0, 0, 0, 0},
	{4, 9.50, 12.00, 0.040, 0.30, 62, 0.50, 0, 0, 0, 0},
	{5, 9.50, 12.00, 0.040, 0.30, 74, 0.50, 0, 0, 0, 0},
	// E: pedal sustain, retrigger, note jump on held voice, short strikes
	{0, 12.00, 12.50, 0.020, 0.05, 57, 0.70, 0, 0, 0, 0},
	{2, 12.00, 12.50, 0.020, 0.05, 64, 0.70, 0, 0, 0, 0},
	{0, 13.00, 13.80, 0.010, 0.10, 65, 0.80, 70, 13.30, 0, 0},
	{1, 13.50, 13.55, 0.005, 0.05, 72, 0.90, 0, 0, 0, 0},
	{1, 13.70, 13.75, 0.005, 0.05, 74, 0.90, 0, 0, 0, 0},
	{1, 13.90, 13.95, 0.005, 0.05, 76, 0.90, 0, 0, 0, 0},
	// F: extremes (very low note at wavetable limit, very high note) and a slow glide
	{2, 14.50, 16.00, 0.050, 0.30, 20, 0.80, 0, 0, 0, 0},
	{3, 14.50, 16.00, 0.050, 0.30, 115, 0.50, 0, 0, 0, 0},
	{4, 14.50, 16.20, 0.050, 0.30, 50, 0.60, 62, 15.00, 0, 0},
};

static void controls(double t, Ctl* v, int nv, float* g) {
	// released voices keep their last pitch while decaying, as in the firmware
	static float last_note[NVOICES_MAX] = {69, 69, 69, 69, 69, 69, 69, 69, 69, 69, 69, 69, 69, 69, 69};
	for (int i = 0; i < nv; i++) v[i] = {last_note[i], 0, 0, 0, 0};
	for (const Note& n : notes) {
		if (n.voice >= nv) continue;
		double e = gate(t, n.t0, n.t1, n.att, n.rel);
		if (e <= 0 && t > n.t0) continue;
		if (t < n.t0) continue;
		Ctl& c = v[n.voice];
		double note = n.note;
		if (n.glide_t > 0 && t > n.glide_t) note = n.glide_to - (n.glide_to - n.note) * exp(-(t - n.glide_t) * 10.0);
		double swell = 1.0 + 0.2 * sin(2 * M_PI * 0.7 * t + n.voice);
		c.pres = n.pres * e * swell;
		c.but_x = n.vib * sin(2 * M_PI * 5.0 * t + 1.3 * n.voice);
		c.but_y = n.tilt * sin(2 * M_PI * t / 1.5 + n.voice);
		c.note = note + 0.5 * c.but_x * c.but_x * c.but_x;  // pitch bend as in update_voice()
		last_note[n.voice] = c.note;
	}
	// globals: acc_abs, acc_x, acc_y, acc_z, rot_x, rot_y, rot_z, pedal
	g[0] = 1; g[1] = 0; g[2] = 0; g[3] = -1; g[4] = 0; g[5] = 0; g[6] = 0; g[7] = 3.0f / 16;
	if (t > 9.5 && t < 11.0) {
		double s = sin(2 * M_PI * 3 * t);
		g[0] = 1 + 2.5 * s * s * (s > 0);
		g[1] = sin(2 * M_PI * 0.5 * t);
	}
	if (t > 9.5 && t < 12.0) {
		g[4] = 0.8 * sin(2 * M_PI * 0.9 * t);
		g[5] = 0.8 * sin(2 * M_PI * 1.3 * t + 1);
		g[6] = 0.8 * sin(2 * M_PI * 1.7 * t + 2);
	}
	if (t > 12.0 && t < 14.0) g[7] = 1.0;
}

// controls for block b; vpres is the pressure velocity, scaled so a 20 ms attack to 0.8 gives ~0.5
static void block_controls(int b, Ctl* v, int nv, float* g) {
	static float pres_old[NVOICES_MAX] = {0};
	controls((double)b * BLOCK / SAMPLERATE, v, nv, g);
	for (int i = 0; i < nv; i++) {
		v[i].vpres = pmax(-1.0f, pmin(1.0f, (v[i].pres - pres_old[i]) * (SAMPLERATE / BLOCK) * 0.0125f));
		pres_old[i] = v[i].pres;
	}
}

#endif
