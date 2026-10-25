// The key path's cost on the board's CPU: the instructions per block that give 10 held, moving
// keys their engine controls, the way synth_control.cpp's update_synth_voice did it before the
// UMP profile (the floats straight to the voice) and the way it does now (striso_dsp/ump.h: the
// key's packets through the decoder, its host writing the voice). Compiled with the firmware's
// flags for synth_control.cpp and counted on QEMU (-icount shift=0, SysTick /40), as profile.sh.
// The keys play the default performance, quantised to the scanner's 14-bit codes; both paths
// must give the same floats, bit for bit, every block.
// Usage (QEMU semihosting arguments): key_path [seconds]
// Prints "before <instr/block> after <instr/block> differences <n>".
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "performance.h"
#include "striso_dsp/ump.h"

#define SYST_CSR (*reinterpret_cast<volatile uint32_t*>(0xE000E010u))
#define SYST_RVR (*reinterpret_cast<volatile uint32_t*>(0xE000E014u))
#define SYST_CVR (*reinterpret_cast<volatile uint32_t*>(0xE000E018u))

namespace {

constexpr int KEYS = 68, NV = 10;

// synth_control's state as update_synth_voice reads it
struct Button {
	float note, pres, vpres, but_x, but_y;
	bool on;
};
Button buttons[KEYS];
float pres_sensitivity = 1.0f, velo_sensitivity = 1.0f, y_sensitivity = 1.0f, bend_sensitivity = 0.0f;
bool pitch_bend_x = false;

// synth_interface: pointers to the voices' controls
struct Interface {
	float* note[NV];
	float* pres[NV];
	float* vpres[NV];
	float* but_x[NV];
	float* but_y[NV];
};
float vctl[2][NV][5];
Interface iface[2];
int voice_of[KEYS];

float clamp(float x, float lo, float hi) { return x > hi ? hi : (x < lo ? lo : x); }
#define pow3(x) ((x) * (x) * (x))

// before: the floats straight to the voice (synth_control.cpp before the profile)
__attribute__((noinline)) void update_before(int but) {
	const int voice = voice_of[but];
	const Interface* si = &iface[0];
	if (voice < 0) return;
	float pb = 0.0f;
	if (pitch_bend_x) pb = bend_sensitivity * pow3(buttons[but].but_x);
	*(si->note[voice]) = buttons[but].note + pb;
	*(si->pres[voice]) = buttons[but].pres * pres_sensitivity;
	*(si->vpres[voice]) = buttons[but].vpres * velo_sensitivity;
	*(si->but_x[voice]) = buttons[but].but_x;
	*(si->but_y[voice]) = clamp(buttons[but].but_y * y_sensitivity, -1.0f, 1.0f);
}

// after: the key's packets through the decoder (synth_control.cpp now)
struct Host {
	striso::ump::Decoder<KEYS + 16> synth_in;
	bool key_sounding[KEYS] = {};
	[[nodiscard]] striso::ump::Touch touch(int /*id*/) const { return {pres_sensitivity, velo_sensitivity, y_sensitivity}; }
	void note_started(int /*id*/, const striso::ump::NoteSignals& /*n*/) {}
	void note_changed(int id, const striso::ump::NoteSignals& n) {
		const int voice = voice_of[id];
		const Interface* si = &iface[1];
		if (voice < 0) return;
		*(si->note[voice]) = n.note;
		*(si->pres[voice]) = n.pres;
		*(si->vpres[voice]) = n.vpres;
		*(si->but_x[voice]) = n.but_x;
		*(si->but_y[voice]) = n.but_y;
	}
	void note_silenced(int /*id*/) {}
	void note_dropped(int /*id*/) {}
};
Host host;

__attribute__((noinline)) void update_after(int but) {
	const Button& b = buttons[but];
	float pb = 0.0f;
	if (pitch_bend_x) pb = bend_sensitivity * pow3(b.but_x);
	host.synth_in.packet(striso::ump::pitch_packet(but, b.note + pb));
	host.synth_in.packet(striso::ump::pres_packet(but, b.pres));
	host.synth_in.packet(striso::ump::signed_packet(but, striso::ump::ctl_vpres, b.vpres));
	host.synth_in.packet(striso::ump::signed_packet(but, striso::ump::ctl_x, b.but_x));
	host.synth_in.packet(striso::ump::signed_packet(but, striso::ump::ctl_y, b.but_y));
	if (b.on != host.key_sounding[but]) {
		host.synth_in.packet(b.on ? striso::ump::note_on_packet(but, b.vpres, false) : striso::ump::note_off_packet(but));
		host.key_sounding[but] = b.on;
	}
	host.synth_in.flush(host);
}

// a value as the key thread has it: its 14-bit code / 8191 (int2float)
float code(float x) {
	const float c = x * 8191.0f;
	return static_cast<float>(static_cast<int>(c + (c >= 0 ? 0.5f : -0.5f))) / 0x1fff;
}

} // namespace

int main(int argc, char** argv) {
	const double seconds = argc > 1 ? std::atof(argv[1]) : 17.5;
	for (int k = 0; k < 2; k++)
		for (int v = 0; v < NV; v++) {
			iface[k].note[v] = &vctl[k][v][0];
			iface[k].pres[v] = &vctl[k][v][1];
			iface[k].vpres[v] = &vctl[k][v][2];
			iface[k].but_x[v] = &vctl[k][v][3];
			iface[k].but_y[v] = &vctl[k][v][4];
		}
	// key 20 + v plays voice v (the allocator is the same on both paths)
	for (int i = 0; i < KEYS; i++) voice_of[i] = i >= 20 && i < 20 + NV ? i - 20 : -1;
	SYST_RVR = 0xffffffu;
	SYST_CVR = 0;
	SYST_CSR = 5; // enabled, CPU clock, no interrupt
	const int nblocks = static_cast<int>(seconds * SAMPLERATE / BLOCK);
	uint64_t ticks[2] = {0, 0};
	long differences = 0;
	Ctl c[NVOICES_MAX];
	float g[8];
	for (int b = 0; b < nblocks; b++) {
		block_controls(b, c, NV, g);
		for (int v = 0; v < NV; v++) {
			Button& k = buttons[20 + v];
			k.note = c[v].note;
			k.pres = code(c[v].pres);
			k.vpres = code(c[v].vpres);
			k.but_x = code(c[v].but_x);
			k.but_y = code(c[v].but_y);
			k.on = k.pres > 0.0f;
		}
		for (int path = 0; path < 2; path++) {
			const uint32_t t0 = SYST_CVR;
			for (int v = 0; v < NV; v++) path ? update_after(20 + v) : update_before(20 + v);
			ticks[path] += (t0 - SYST_CVR) & 0xffffffu;
		}
		differences += std::memcmp(vctl[0], vctl[1], sizeof vctl[0]) != 0;
	}
	std::printf("before %.0f after %.0f differences %ld\n", static_cast<double>(ticks[0]) * 40 / nblocks,
	    static_cast<double>(ticks[1]) * 40 / nblocks, differences);
	return 0;
}
