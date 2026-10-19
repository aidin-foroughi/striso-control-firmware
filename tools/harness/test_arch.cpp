// Runs a multi-output Faust DSP for N samples and writes interleaved float32. Usage: run out.f32 seconds
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <vector>
#define FAUSTFLOAT float
#define max(x,y) (x>y?x:y)
#define min(x,y) (x<y?x:y)
class dsp { protected: int fSamplingFreq; };
struct Meta { virtual void declare(const char*, const char*) {} };
struct Soundfile;
struct UI {
	void openTabBox(const char*) {} void openHorizontalBox(const char*) {} void openVerticalBox(const char*) {}
	void closeBox() {} void declare(float*, const char*, const char*) {}
	void addButton(const char*, float*) {} void addCheckButton(const char*, float*) {}
	void addVerticalSlider(const char*, float*, float, float, float, float) {}
	void addHorizontalSlider(const char*, float*, float, float, float, float) {}
	void addNumEntry(const char*, float*, float, float, float, float) {}
	void addHorizontalBargraph(const char*, float*, float, float) {} void addVerticalBargraph(const char*, float*, float, float) {}
	void addSoundfile(const char*, const char*, Soundfile**) {}
};
<<includeIntrinsic>>
<<includeclass>>
int main(int argc, char** argv) {
	static mydsp d; d.init(48828);
	int no = d.getNumOutputs(), n = (int)(atof(argv[2]) * 48828), B = 32;
	std::vector<float> buf(no * B), out, inb(3 * B); std::vector<float*> outs(no), ins(3);
	for (int c = 0; c < no; c++) outs[c] = &buf[c * B];
	for (int c = 0; c < 3; c++) ins[c] = &inb[c * B];
	unsigned s = 12345;
	for (int i = 0; i < n; i += B) {
		// identical float inputs for single and double builds: noise, cutoff sweep 200..16000 Hz, Q 1..9
		for (int k = 0; k < B; k++) {
			double t = (double)(i + k) / 48828;
			s = s * 1664525u + 1013904223u;
			inb[k] = (float)((int)s * (1.0 / 2147483648.0));
			inb[B + k] = (float)(200 + 7900 * (1 + sin(2 * M_PI * 0.37 * t)));
			inb[2 * B + k] = (float)(1 + 4 * (1 + sin(2 * M_PI * 0.23 * t)));
		}
		d.compute(B, ins.data(), outs.data());
		for (int k = 0; k < B; k++) for (int c = 0; c < no; c++) out.push_back(buf[c * B + k]);
	}
	FILE* f = fopen(argv[1], "wb"); fwrite(out.data(), 4, out.size(), f); fclose(f);
	return 0;
}
