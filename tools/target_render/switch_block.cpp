// The board's voice-switch block on its CPU: the engines of VOICE=all in one EngineSet with the
// board's shared arenas (synth_strisy.cpp), and for each engine the block the synth thread runs
// when it switches to it: EngineSet::start (the arenas handed over: the engine's share zeroed
// unless its init writes it all, then init()) and the engine's first process(). The engine before
// it played a chord for a while, so the arenas hold its data. Compiled with the synth DSP's flags
// and counted on QEMU (-icount shift=0, SysTick /40), as profile.sh: instructions, not cycles
// (caches and wait states are not modelled).
// Usage (QEMU semihosting arguments): switch_block
// Prints a line per engine: "switch <engine> start <instr> process <instr> block <instr>"
#include <cstdint>
#include <cstdio>

#include "striso_dsp/engine_set.h"
#include "striso_dsp/engines/bowed.h"
#include "striso_dsp/engines/brass.h"
#include "striso_dsp/engines/ep.h"
#include "striso_dsp/engines/modal.h"
#include "striso_dsp/engines/strisy.h"
#include "striso_dsp/engines/trumpet.h"
#include "striso_dsp/engines/va.h"
#include "striso_dsp/engines/wind.h"

#define SYST_CSR (*reinterpret_cast<volatile uint32_t*>(0xE000E010u))
#define SYST_RVR (*reinterpret_cast<volatile uint32_t*>(0xE000E014u))
#define SYST_CVR (*reinterpret_cast<volatile uint32_t*>(0xE000E018u))

namespace {

namespace E = striso::dsp::engines;
// synth_strisy.cpp's SYNTH_ENGINES_ALL
using Engines = striso::dsp::EngineSet<E::strisy, E::va, E::modal, E::bowed, E::wind, E::ep, E::brass, E::trumpet>;
constexpr int NV = 10, SR = 48000;

Engines::States engines;
Engines::Fast fast;
Engines::Large large;
striso::dsp::VoiceControls vc[NV];
striso::dsp::GlobalControls g;
float out[striso::dsp::BLOCK];

void init(int k) {
	Engines::start(k, engines, fast, large, [](auto& e, auto& f, auto& l, auto) { e.init(f, l, SR, NV); });
}
void process(int k) {
	for (int v = 0; v < NV; v++) striso::dsp::track_key(vc[v]);
	float* const outs[1] = {out};
	Engines::visit(k, engines, [&outs](auto& e, auto) { e.process(vc, g, outs); });
}
// a chord of every voice, held
void chord() {
	for (int v = 0; v < NV; v++) {
		vc[v] = striso::dsp::voice_at_rest;
		vc[v].note = 48.0f + 3.0f * static_cast<float>(v);
		vc[v].pres = 0.6f;
		vc[v].vpres = 0.5f;
	}
}
uint32_t ticks_since(uint32_t t0) { return (t0 - SYST_CVR) & 0xffffffu; }

} // namespace

int main(int /*argc*/, char** /*argv*/) {
	SYST_RVR = 0xffffffu;
	SYST_CVR = 0;
	SYST_CSR = 5; // enabled, CPU clock, no interrupt
	g = striso::dsp::globals_at_rest;
	for (int k = 0; k < Engines::count; k++) {
		const int before = (k + Engines::count - 1) % Engines::count;
		chord();
		init(before);
		for (int b = 0; b < 200; b++) process(before); // its data in the arenas
		const uint32_t t0 = SYST_CVR;
		init(k);
		const uint32_t t_start = ticks_since(t0);
		const uint32_t t1 = SYST_CVR;
		process(k);
		const uint32_t t_process = ticks_since(t1);
		std::printf("switch %s start %lu process %lu block %lu\n", Engines::info(k)->name,
		    static_cast<unsigned long>(t_start) * 40, static_cast<unsigned long>(t_process) * 40,
		    static_cast<unsigned long>(t_start + t_process) * 40);
	}
	return 0;
}
