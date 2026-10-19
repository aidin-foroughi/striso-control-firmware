/**
 * Master effects after the voice engines: mono in, stereo out.
 *
 * This is the pass-through stub that defines the interface; the effects engine (chorus, delay,
 * reverb, tremolo/auto-pan) replaces it. API, in namespace fx:
 *   typedef float real; BLOCK (= 32)
 *   struct Engine (plain data, lives in DTCM; big buffers as separate static arrays)
 *   void init(Engine&, int sample_rate);
 *   void process(Engine&, const real* in, real* out_l, real* out_r);   // BLOCK samples each
 * and optionally live parameters through strisy::EngineParams<fx::Engine> (see strisy.h).
 */
#ifndef FX_H_
#define FX_H_

#include "strisy.h"

namespace fx {

typedef float real;
static const int BLOCK = strisy::BLOCK;

struct Engine {
	int unused;
};

static inline void init(Engine& e, int sample_rate) {
	(void)e;
	(void)sample_rate;
}

static inline void process(Engine& e, const real* __restrict__ in, real* __restrict__ out_l, real* __restrict__ out_r) {
	(void)e;
	for (int n = 0; n < BLOCK; n++) out_l[n] = out_r[n] = in[n];
}

} // namespace fx

#endif // FX_H_
