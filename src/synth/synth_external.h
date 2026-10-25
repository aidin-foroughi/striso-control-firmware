// Engines loaded at run time (the engine component ABI, awva/component/include/nsc/nsc.h), next to
// the compiled-in ones: the virtual board loads them from a folder (sv_options.engine_dir). Only a
// build that defines SYNTH_EXTERNAL_ENGINES has them (the board doesn't yet). synth_strisy.cpp
// lists them after the compiled-in engines (engine numbers Engines::count and up) and plays them
// through process_voices() with the board's own voice controls, so a loaded engine gets exactly
// what the compiled-in one would.
#pragma once

#include "nsc/nsc.h"
#include "awva/params.h"

struct SynthExternal {
	const nsc_component* c;
	void *state, *fast, *large; // its regions (the loader's)
	strisy::ParamDesc* params;  // its parameters as the board's ParamDesc
	float* values;              // the parameters' values, read back for engine_param_ptr
};

// the loaded engines (the loader's, constant while the synth runs)
#ifdef SYNTH_EXTERNAL_ENGINES
int synth_external_count(void);
SynthExternal* synth_external(int i);
#else
static inline int synth_external_count(void) { return 0; }
static inline SynthExternal* synth_external(int) { return nullptr; }
#endif
