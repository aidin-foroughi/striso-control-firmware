// Stale vpres test: the firmware leaves a released voice's last vpres in place (pres = 0 but
// vpres stays positive). A voice must go silent and idle anyway.
// Voice 0 plays note 60 for 1 s, then is released with vpres stuck at 0.25 for the rest.
#ifndef PERFORMANCE_H_
#define PERFORMANCE_H_
#include <math.h>

#define SAMPLERATE 48000
#define BLOCK 32
#define NVOICES_MAX 15

struct Ctl { float note, pres, vpres, but_x, but_y; };

static void block_controls(int b, Ctl* v, int nv, float* g) {
	double t = (double)b * BLOCK / SAMPLERATE;
	for (int i = 0; i < nv; i++) v[i] = {60, 0, 0, 0, 0};
	if (t < 1.0) {
		v[0].pres = t < 0.02 ? (float)(t / 0.02 * 0.6) : 0.6f;
		v[0].vpres = t < 0.02 ? 0.5f : 0.0f;
	} else {
		v[0].vpres = 0.25f;  // stale value after release
	}
	g[0] = 1; g[1] = 0; g[2] = 0; g[3] = -1; g[4] = 0; g[5] = 0; g[6] = 0; g[7] = 3.0f / 16;
}

#endif
