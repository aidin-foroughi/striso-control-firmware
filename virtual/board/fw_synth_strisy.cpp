// src/synth/synth_strisy.cpp (the synth thread, every voice engine, the effects, the live
// parameters) as the board builds it, and the virtual board's taps into it: the engine's and the
// effects' output of the block just rendered and the controls the engine got, and the
// engine-level controls of sv_set_engine_controls.
#include "synth_strisy.cpp"

// The synth thread starts with its event already set (chEvtAddEvents(1) before its loop), so its
// first block runs before the codec's first interrupt has given it a buffer: on the board outbuf
// is still 0 and the block goes to address 0 (ITCM, unused there). Here it goes to a scratch buffer.
static int32_t svh_address0[PLAYBACK_BUFFER_SIZE];
static const bool svh_outbuf_set = (outbuf = svh_address0, inbuf = svh_address0, true);

extern "C" void svh_synth_taps(float* engine, float* fx_l, float* fx_r, float (*controls)[5], float* globals) {
	for (int n = 0; n < CHANNEL_BUFFER_SIZE; n++) {
		engine[n] = output0[n];
		fx_l[n] = out_l[n];
		fx_r[n] = out_r[n];
	}
	for (int v = 0; v < VOICECOUNT; v++) {
		const awva::VoiceControls& c = controls_vc[v];
		controls[v][0] = c.note;
		controls[v][1] = c.pres;
		controls[v][2] = c.vpres;
		controls[v][3] = c.but_x;
		controls[v][4] = c.but_y;
	}
	const awva::GlobalControls& g = controls_g;
	const float gv[8] = {g.acc_abs, g.acc_x, g.acc_y, g.acc_z, g.rot_x, g.rot_y, g.rot_z, g.pedal};
	for (int i = 0; i < 8; i++) globals[i] = gv[i];
}

extern "C" void svh_synth_fx_init(void) { fx_engine.init(fx_fast, fx_large, SAMPLINGFREQ); }

// the motion thread's writes go to a dummy from now on (the globals are the caller's)
static float motion_dummy;
extern "C" void svh_motion_off(void) {
	synth_interface.acc_abs = &motion_dummy;
	synth_interface.acc_x = &motion_dummy;
	synth_interface.acc_y = &motion_dummy;
	synth_interface.acc_z = &motion_dummy;
	synth_interface.rot_x = &motion_dummy;
	synth_interface.rot_y = &motion_dummy;
	synth_interface.rot_z = &motion_dummy;
}

extern "C" void svh_set_engine_globals(const float* g) { controls_g = {g[0], g[1], g[2], g[3], g[4], g[5], g[6], g[7]}; }

// the key signals of voice v (gate, strike and release follow from them, as from the keys)
extern "C" void svh_set_engine_voice(int v, const float* c) {
	awva::VoiceControls& d = controls_vc[v];
	d.note = c[0];
	d.pres = c[1];
	d.vpres = c[2];
	d.but_x = c[3];
	d.but_y = c[4];
}
