// tools/harness/perf_stale.h (a released voice with its last vpres stuck) for the virtual board
#include "perf_stale.h"
#include "perfs.h"

void perf_stale(int b, int nv, float* vc, float* g) {
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
