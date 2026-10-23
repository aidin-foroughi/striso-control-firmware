// Trumpet performance (voice_trumpet.h): a phrase that shows what a player does with the instrument.
// 31 s: tongued staccato, a slurred legato line (overlapping keys pressed softly), a pp -> ff ->
// pp swell on a long note (the brassiness blooms), a fall and a doit at releases (sideways tilt),
// a lip shake (motion), pedal vibrato, embouchure / mute wah (forward/back tilt), a half-valve bend
// and a three note chord.
#ifndef PERFORMANCE_H_
#define PERFORMANCE_H_
#include <math.h>

#define SAMPLERATE 48000
#define BLOCK 32
#define NVOICES_MAX 15
#define pmax(x, y) ((x) > (y) ? (x) : (y))
#define pmin(x, y) ((x) < (y) ? (x) : (y))

struct Ctl {
	float note, pres, vpres, but_x, but_y;
};

// key: voice, start, end, attack (s), release (s), note, pressure, tilt x at the end (fall/doit),
// tilt y amplitude (slow sweep), bend (semitones, reached at the end), swell (1: pp -> ff -> pp)
struct Key {
	int voice;
	double t0, t1, att, rel, note, pres, bx_end, ty, bend, swell;
};

static const Key keys[] = {
    // 1. tongued staccato (hard attacks), concert Bb major: Bb4 D5 F5 D5 Bb4 F4 Bb4
    {0, 0.20, 0.42, 0.004, 0.02, 70, 0.75, 0, 0, 0, 0},
    {1, 0.50, 0.70, 0.004, 0.02, 74, 0.75, 0, 0, 0, 0},
    {0, 0.80, 1.00, 0.004, 0.02, 77, 0.80, 0, 0, 0, 0},
    {1, 1.10, 1.30, 0.004, 0.02, 74, 0.75, 0, 0, 0, 0},
    {0, 1.40, 1.60, 0.004, 0.02, 70, 0.75, 0, 0, 0, 0},
    {1, 1.70, 1.90, 0.004, 0.02, 65, 0.70, 0, 0, 0, 0},
    {0, 2.00, 2.60, 0.006, 0.08, 70, 0.80, 0, 0, 0, 0},
    // 2. slurred legato (each key pressed softly while the previous is still down): F4 Bb4 D5 F5 Eb5 C5 A4 Bb4
    {2, 3.00, 3.62, 0.050, 0.03, 65, 0.55, 0, 0, 0, 0},
    {3, 3.55, 4.12, 0.060, 0.03, 70, 0.55, 0, 0, 0, 0},
    {2, 4.05, 4.62, 0.060, 0.03, 74, 0.60, 0, 0, 0, 0},
    {3, 4.55, 5.12, 0.060, 0.03, 77, 0.65, 0, 0, 0, 0},
    {2, 5.05, 5.62, 0.060, 0.03, 75, 0.60, 0, 0, 0, 0},
    {3, 5.55, 6.12, 0.060, 0.03, 72, 0.55, 0, 0, 0, 0},
    {2, 6.05, 6.62, 0.060, 0.03, 69, 0.55, 0, 0, 0, 0},
    {3, 6.55, 7.60, 0.060, 0.15, 70, 0.55, 0, 0, 0, 0},
    // 3. swell pp -> ff -> pp on G4 (the brassiness blooms at ff)
    {0, 8.00, 12.00, 0.030, 0.30, 67, 1.00, 0, 0, 0, 1},
    // 4. a fall (tilt left at the release) and a doit (tilt right)
    {1, 12.60, 13.60, 0.006, 0.05, 79, 0.85, -0.9, 0, 0, 0},
    {2, 14.20, 15.00, 0.006, 0.05, 74, 0.80, 0.9, 0, 0, 0},
    // 5. lip shake on a high C (motion) and 6. pedal vibrato on a held F5 (see the globals)
    {3, 15.60, 17.60, 0.010, 0.10, 84, 0.80, 0, 0, 0, 0},
    {0, 18.00, 20.60, 0.030, 0.25, 77, 0.60, 0, 0, 0, 0},
    // 7. embouchure / wah: slow forward-back tilt on a held D5
    {1, 21.00, 24.00, 0.020, 0.20, 74, 0.70, 0, 1.0, 0, 0},
    // 8. half-valve bend: Bb4 bent down a whole tone and back
    {2, 24.50, 26.50, 0.010, 0.10, 70, 0.70, 0, 0, -2.0, 0},
    // 9. a three note chord (a small section) and a low note
    {0, 27.00, 29.00, 0.020, 0.30, 65, 0.60, 0, 0, 0, 0},
    {1, 27.02, 29.00, 0.020, 0.30, 69, 0.55, 0, 0, 0, 0},
    {3, 27.04, 29.00, 0.020, 0.30, 72, 0.55, 0, 0, 0, 0},
    {2, 29.40, 30.60, 0.010, 0.20, 55, 0.70, 0, 0, 0, 0},
};

static double kgate(double t, const Key& k) {
	if (t < k.t0) return 0;
	const double a = pmin((t - k.t0) / k.att, 1.0);
	if (t < k.t1) return a;
	const double r = 1.0 - (t - k.t1) / k.rel;
	return r > 0 ? a * r : 0;
}

static void controls(double t, Ctl* v, int nv, float* g) {
	static float last_note[NVOICES_MAX] = {69, 69, 69, 69, 69, 69, 69, 69, 69, 69, 69, 69, 69, 69, 69};
	for (int i = 0; i < nv; i++) v[i] = {last_note[i], 0, 0, 0, 0};
	for (const Key& k : keys) {
		if (k.voice >= nv || t < k.t0) continue;
		const double e = kgate(t, k);
		if (e <= 0) continue;
		Ctl& c = v[k.voice];
		double p = k.pres * e;
		if (k.swell > 0) {
			const double u = (t - k.t0) / (k.t1 - k.t0);
			p = k.pres * e * (0.08 + 0.92 * sin(M_PI * pmin(u, 1.0)) * sin(M_PI * pmin(u, 1.0)));
		}
		c.pres = (float)p;
		// tilt x ramps in over the last 0.2 s for falls and doits
		const double ramp = k.bx_end != 0 ? pmin(pmax((t - (k.t1 - 0.2)) / 0.2, 0.0), 1.0) : 0.0;
		c.but_x = (float)(k.bx_end * ramp);
		c.but_y = (float)(k.ty * sin(2 * M_PI * (t - k.t0) / 1.5));
		double bend = 0;
		if (k.bend != 0) {
			const double u = (t - k.t0) / (k.t1 - k.t0);
			bend = k.bend * sin(M_PI * pmin(u, 1.0));
		}
		c.note = (float)(k.note + bend + 0.5 * c.but_x * c.but_x * c.but_x); // pitch bend as update_voice()
		last_note[k.voice] = c.note;
	}
	// globals: acc_abs, acc_x, acc_y, acc_z, rot_x, rot_y, rot_z, pedal
	g[0] = 1; g[1] = 0; g[2] = 0; g[3] = -1; g[4] = 0; g[5] = 0; g[6] = 0; g[7] = 3.0f / 16;
	if (t > 16.2 && t < 17.4) { // shaking the instrument: lip shake
		const double s = sin(2 * M_PI * 6.5 * t);
		g[0] = (float)(1 + 1.4 * s * s);
	}
	if (t > 18.6 && t < 20.6) g[7] = (float)(0.25 + 0.6 * pmin((t - 18.6) / 0.8, 1.0)); // pedal: vibrato
}

static void block_controls(int b, Ctl* v, int nv, float* g) {
	static float pres_old[NVOICES_MAX] = {0};
	controls((double)b * BLOCK / SAMPLERATE, v, nv, g);
	for (int i = 0; i < nv; i++) {
		v[i].vpres = pmax(-1.0f, pmin(1.0f, (v[i].pres - pres_old[i]) * (SAMPLERATE / BLOCK) * 0.0125f));
		pres_old[i] = v[i].pres;
	}
}

#endif
