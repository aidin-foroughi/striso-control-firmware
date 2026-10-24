// tools/harness/performance.h (the default scripted performance) for the virtual board's renders
#include "performance.h"
#include "perfs.h"

void perf_default(int b, int nv, float* vc, float* g) {
	Ctl v[NVOICES_MAX];
	block_controls(b, v, nv, g);
	for (int i = 0; i < nv; i++) {
		vc[5 * i] = v[i].note;
		vc[5 * i + 1] = v[i].pres;
		vc[5 * i + 2] = v[i].vpres;
		vc[5 * i + 3] = v[i].but_x;
		vc[5 * i + 4] = v[i].but_y;
	}
}
