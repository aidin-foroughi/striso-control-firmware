// Host test harness: Faust architecture file that renders a scripted, deterministic Striso performance.
// The generated DSP class is compiled for the PC so optimized synth.dsp variants can be compared
// numerically against the original before anything is flashed.
//
// Usage: render <out.f32> [blocks]   writes mono float32 at SAMPLERATE, 32-sample blocks like the firmware
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <chrono>

#define FAUSTFLOAT float
#include "performance.h"

#define max(x,y) (x>y?x:y)
#define min(x,y) (x<y?x:y)

class dsp {
	protected:
		int fSamplingFreq;
};

struct Meta {
	virtual void declare(const char*, const char*) {}
};

struct Soundfile;

// Records slider zones: global controls by name, voice controls by voice group "v<n>"
struct UI {
	std::vector<std::string> boxes;
	float* global[8] = {0};
	float* voice[5][NVOICES_MAX] = {{0}};
	int nvoices = 0;
	const char* gnames[8] = {"acc_abs", "acc_x", "acc_y", "acc_z", "rot_x", "rot_y", "rot_z", "pedal"};
	const char* vnames[5] = {"note", "pres", "vpres", "but_x", "but_y"};

	void open(const char* l) { boxes.push_back(l); }
	void openTabBox(const char* l) { open(l); }
	void openHorizontalBox(const char* l) { open(l); }
	void openVerticalBox(const char* l) { open(l); }
	void closeBox() { boxes.pop_back(); }
	int current_voice() {
		for (auto& b : boxes)
			if (b.size() >= 2 && b[0] == 'v' && b[1] >= '0' && b[1] <= '9') return atoi(b.c_str() + 1);
		return -1;
	}
	void add(const char* l, float* z) {
		for (int i = 0; i < 8; i++) if (!strcmp(l, gnames[i])) global[i] = z;
		int v = current_voice();
		if (v >= 0 && v < NVOICES_MAX) {
			for (int i = 0; i < 5; i++) if (!strcmp(l, vnames[i])) voice[i][v] = z;
			if (v + 1 > nvoices) nvoices = v + 1;
		}
	}
	void addButton(const char* l, float* z) { add(l, z); }
	void addCheckButton(const char* l, float* z) { add(l, z); }
	void addVerticalSlider(const char* l, float* z, float, float, float, float) { add(l, z); }
	void addHorizontalSlider(const char* l, float* z, float, float, float, float) { add(l, z); }
	void addNumEntry(const char* l, float* z, float, float, float, float) { add(l, z); }
	void addHorizontalBargraph(const char*, float*, float, float) {}
	void addVerticalBargraph(const char*, float*, float, float) {}
	void addSoundfile(const char*, const char*, Soundfile**) {}
	void declare(float*, const char*, const char*) {}
};

<<includeIntrinsic>>

<<includeclass>>

int main(int argc, char** argv) {
	if (argc < 2) { fprintf(stderr, "usage: render out.f32 [seconds]\n"); return 1; }
	double seconds = argc > 2 ? atof(argv[2]) : 17.5;
	static mydsp d;
	UI ui;
	d.init(SAMPLERATE);
	d.buildUserInterface(&ui);
	int nv = ui.nvoices;
	// controls the DSP doesn't use are optimized away by Faust; point them at a dummy like the firmware does
	static float dump;
	for (int i = 0; i < nv; i++)
		for (int k = 0; k < 5; k++)
			if (!ui.voice[k][i]) ui.voice[k][i] = &dump;

	int nblocks = (int)(seconds * SAMPLERATE / BLOCK);
	std::vector<float> out((size_t)nblocks * BLOCK);
	float buf[BLOCK];
	float* outputs[2] = {buf, buf};
	Ctl v[NVOICES_MAX];
	float g[8];
	double compute_s = 0;
	for (int b = 0; b < nblocks; b++) {
		block_controls(b, v, nv, g);
		for (int i = 0; i < nv; i++) {
			*ui.voice[0][i] = v[i].note;
			*ui.voice[1][i] = v[i].pres;
			*ui.voice[2][i] = v[i].vpres;
			*ui.voice[3][i] = v[i].but_x;
			*ui.voice[4][i] = v[i].but_y;
		}
		for (int k = 0; k < 8; k++) if (ui.global[k]) *ui.global[k] = g[k];
		auto t0 = std::chrono::steady_clock::now();
		d.compute(BLOCK, NULL, outputs);
		compute_s += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		memcpy(&out[(size_t)b * BLOCK], buf, sizeof(buf));
	}
	FILE* f = fopen(argv[1], "wb");
	fwrite(out.data(), sizeof(float), out.size(), f);
	fclose(f);
	fprintf(stderr, "%d voices, %.1f s audio, host compute %.3f s\n", nv, seconds, compute_s);
	return 0;
}
