//----------------------------------------------------------
// name: "synth.dsp.tmp"
//
// Code generated with Faust 2.37.3 (https://faust.grame.fr)
//----------------------------------------------------------

/* link with : "" */
#include "fastpow.h"
#include <math.h>
#ifndef FAUSTPOWER
#define FAUSTPOWER
//#include <cmath>
template <int N> inline int faustpower(int x)              { return faustpower<N/2>(x) * faustpower<N-N/2>(x); } 
template <> 	 inline int faustpower<0>(int x)            { return 1; }
template <> 	 inline int faustpower<1>(int x)            { return x; }
template <> 	 inline int faustpower<2>(int x)            { return x*x; }
template <int N> inline float faustpower(float x)            { return faustpower<N/2>(x) * faustpower<N-N/2>(x); } 
template <> 	 inline float faustpower<0>(float x)          { return 1; }
template <> 	 inline float faustpower<1>(float x)          { return x; }
template <> 	 inline float faustpower<2>(float x)          { return x*x; }
#endif

extern "C" {
    #include "ch.h"
    #include "hal.h"
    #include "synth.h"
    #include "usb_audio.h"
    #include "chprintf.h"
}

#define max(x,y) (x>y?x:y)
#define min(x,y) (x<y?x:y)

#define MAX_VAL (1<<23)
#define VOLUME_FILTER 0.99f

class dsp {
	protected:
		int fSamplingFreq;
};

struct Meta {
    virtual void declare(const char* key, const char* value) = 0;
};

// variables
synth_interface_t synth_interface;

static float dump;
float volume = 1.0f;
volatile synth_load_t synth_load;

static int32_t *outbuf;
static int32_t *inbuf;
static float output0[CHANNEL_BUFFER_SIZE] = {0.0};
static float output1[CHANNEL_BUFFER_SIZE] = {0.0};
static float* output[2] = {output0, output1};

// Intrinsics

// Class
#ifndef FAUSTFLOAT
#define FAUSTFLOAT float
#endif 


#ifndef FAUSTCLASS 
#define FAUSTCLASS mydsp
#endif

class mydsp : public dsp {
  private:
	class SIG0 {
	  private:
		int fSampleRate;
	  public:
		int getNumInputs() { return 0; }
		int getNumOutputs() { return 1; }
		void init(int sample_rate) {
			fSampleRate = sample_rate;
		}
		void fill(int count, float output[]) {
			for (int i=0; i<count; i++) {
				output[i] = (9.3132257504915938e-10f * rand_hoaglin());
			}
		}
	};


	float 	fConst4;
	float 	fConst6;
	float 	fConst8;
	float 	fConst10;
	float 	fConst12;
	FAUSTFLOAT 	fslider0;
	static float 	ftbl0[4096];
	FAUSTFLOAT 	fslider1;
	float 	fVec0[2];
	float 	fConst13;
	float 	fConst14;
	float 	fRec2[2];
	float 	fRec1[3];
	float 	fConst16;
	float 	fConst18;
	FAUSTFLOAT 	fslider2;
	float 	fRec3[3];
	float 	fConst20;
	float 	fConst22;
	FAUSTFLOAT 	fslider3;
	float 	fRec4[3];
	float 	fConst26;
	float 	fConst28;
	float 	fConst30;
	FAUSTFLOAT 	fslider4;
	float 	fRec5[3];
	float 	fConst31;
	FAUSTFLOAT 	fslider5;
	float 	fConst32;
	float 	fConst33;
	float 	fConst34;
	float 	fConst35;
	float 	fConst36;
	float 	fConst37;
	float 	fConst38;
	float 	fRec6[2];
	float 	fConst39;
	float 	fRec7[2];
	float 	fConst40;
	float 	fConst44;
	float 	fConst46;
	float 	fConst48;
	FAUSTFLOAT 	fslider6;
	FAUSTFLOAT 	fslider7;
	float 	fConst49;
	float 	fRec13[2];
	float 	fRec12[3];
	float 	fConst50;
	float 	fConst54;
	float 	fConst56;
	float 	fConst58;
	float 	fConst59;
	float 	fConst61;
	FAUSTFLOAT 	fslider8;
	float 	fRec16[3];
	float 	fConst62;
	float 	fRec15[3];
	float 	fConst63;
	float 	fConst64;
	float 	fRec14[2];
	float 	fRec9[2];
	float 	fRec10[2];
	FAUSTFLOAT 	fslider9;
	float 	fVec1[2];
	float 	fRec18[2];
	float 	fRec17[3];
	float 	fRec19[3];
	float 	fRec20[3];
	FAUSTFLOAT 	fslider10;
	float 	fRec21[3];
	FAUSTFLOAT 	fslider11;
	float 	fRec22[2];
	float 	fRec23[2];
	FAUSTFLOAT 	fslider12;
	float 	fRec29[2];
	float 	fRec28[3];
	float 	fRec25[2];
	float 	fRec26[2];
	FAUSTFLOAT 	fslider13;
	float 	fVec2[2];
	float 	fRec31[2];
	float 	fRec30[3];
	float 	fRec32[3];
	float 	fRec33[3];
	FAUSTFLOAT 	fslider14;
	float 	fRec34[3];
	FAUSTFLOAT 	fslider15;
	float 	fRec35[2];
	float 	fRec36[2];
	FAUSTFLOAT 	fslider16;
	float 	fRec42[2];
	float 	fRec41[3];
	float 	fRec38[2];
	float 	fRec39[2];
	FAUSTFLOAT 	fslider17;
	float 	fVec3[2];
	float 	fRec44[2];
	float 	fRec43[3];
	float 	fRec45[3];
	float 	fRec46[3];
	FAUSTFLOAT 	fslider18;
	float 	fRec47[3];
	FAUSTFLOAT 	fslider19;
	float 	fRec48[2];
	float 	fRec49[2];
	FAUSTFLOAT 	fslider20;
	float 	fRec55[2];
	float 	fRec54[3];
	float 	fRec51[2];
	float 	fRec52[2];
	FAUSTFLOAT 	fslider21;
	float 	fVec4[2];
	float 	fRec57[2];
	float 	fRec56[3];
	float 	fRec58[3];
	float 	fRec59[3];
	FAUSTFLOAT 	fslider22;
	float 	fRec60[3];
	FAUSTFLOAT 	fslider23;
	float 	fRec61[2];
	float 	fRec62[2];
	FAUSTFLOAT 	fslider24;
	float 	fRec68[2];
	float 	fRec67[3];
	float 	fRec64[2];
	float 	fRec65[2];
	FAUSTFLOAT 	fslider25;
	float 	fVec5[2];
	float 	fRec70[2];
	float 	fRec69[3];
	float 	fRec71[3];
	float 	fRec72[3];
	FAUSTFLOAT 	fslider26;
	float 	fRec73[3];
	FAUSTFLOAT 	fslider27;
	float 	fRec74[2];
	float 	fRec75[2];
	FAUSTFLOAT 	fslider28;
	float 	fRec81[2];
	float 	fRec80[3];
	float 	fRec77[2];
	float 	fRec78[2];
	float 	fConst65;
	float 	fRec0[3];
	float 	fConst66;
	int fSampleRate;

  public:
	virtual void metadata(Meta* m) { 
		m->declare("fast.lib/author", "Piers Titus van der Torren (pierstitus@toverlamp.org)");
		m->declare("fast.lib/licence", "Apache-2.0");
		m->declare("fast.lib/name", "Faust Fast Approximations Library");
		m->declare("filename", "synth.dsp.tmp");
		m->declare("maths.lib/author", "GRAME");
		m->declare("maths.lib/copyright", "GRAME");
		m->declare("maths.lib/license", "LGPL with exception");
		m->declare("maths.lib/name", "Faust Math Library");
		m->declare("maths.lib/version", "2.5");
		m->declare("name", "synth.dsp.tmp");
		m->declare("platform.lib/name", "Generic Platform Library");
		m->declare("platform.lib/version", "0.2");
	}

	virtual int getNumInputs() { return 0; }
	virtual int getNumOutputs() { return 1; }
	static void classInit(int sample_rate) {
		SIG0 sig0;
		sig0.init(sample_rate);
		sig0.fill(4096,ftbl0);
	}
	virtual void instanceConstants(int sample_rate) {
		fSampleRate = sample_rate;
		float 	fConst0 = min(192000.0f, max(1.0f, (float)fSampleRate));
		float 	fConst1 = faustpower<2>(fConst0);
		float 	fConst2 = faustpower<2>((251.32741228718345f / fConst0));
		float 	fConst3 = ((fConst2 * ((0.20330000000000001f * fConst2) + 0.31755f)) + 1.0f);
		fConst4 = (2.0f * ((63165.468166971892f * (faustpower<2>(fConst3) / fConst1)) + -1.0f));
		float 	fConst5 = (63165.468166971892f * (fConst3 / fConst0));
		fConst6 = (((fConst3 * (fConst5 + -191.85298647876598f)) / fConst0) + 1.0f);
		float 	fConst7 = (((fConst3 * (fConst5 + 191.85298647876598f)) / fConst0) + 1.0f);
		fConst8 = (1.0f / fConst7);
		float 	fConst9 = faustpower<2>((4084.0704496667308f / fConst0));
		fConst10 = (4084.0704496667308f * (((fConst9 * ((0.20330000000000001f * fConst9) + 0.31755f)) + 1.0f) / fConst0));
		float 	fConst11 = faustpower<2>((8168.1408993334617f / fConst0));
		fConst12 = (8168.1408993334617f * (((fConst11 * ((0.20330000000000001f * fConst11) + 0.31755f)) + 1.0f) / fConst0));
		fConst13 = (0.061156102924877762f * fConst0);
		fConst14 = (8.1757989159999997f / fConst0);
		float 	fConst15 = faustpower<2>((942.47779607693792f / fConst0));
		fConst16 = (942.47779607693792f * (((fConst15 * ((0.20330000000000001f * fConst15) + 0.31755f)) + 1.0f) / fConst0));
		float 	fConst17 = faustpower<2>((1884.9555921538758f / fConst0));
		fConst18 = (1884.9555921538758f * (((fConst17 * ((0.20330000000000001f * fConst17) + 0.31755f)) + 1.0f) / fConst0));
		float 	fConst19 = faustpower<2>((2827.4333882308138f / fConst0));
		fConst20 = (2827.4333882308138f * (((fConst19 * ((0.20330000000000001f * fConst19) + 0.31755f)) + 1.0f) / fConst0));
		float 	fConst21 = faustpower<2>((5340.7075111026479f / fConst0));
		fConst22 = (5340.7075111026479f * (((fConst21 * ((0.20330000000000001f * fConst21) + 0.31755f)) + 1.0f) / fConst0));
		float 	fConst23 = faustpower<2>((62.831853071795862f / fConst0));
		float 	fConst24 = ((fConst23 * ((0.20330000000000001f * fConst23) + 0.31755f)) + 1.0f);
		float 	fConst25 = faustpower<2>(fConst24);
		fConst26 = (2.0f * ((3947.8417604357433f * (fConst25 / fConst1)) + -1.0f));
		float 	fConst27 = (3947.8417604357433f * (fConst24 / fConst0));
		fConst28 = (((fConst24 * (fConst27 + -88.495567706754741f)) / fConst0) + 1.0f);
		float 	fConst29 = (((fConst24 * (fConst27 + 88.495567706754741f)) / fConst0) + 1.0f);
		fConst30 = (1.0f / fConst29);
		fConst31 = (fConst25 / (fConst1 * fConst29));
		fConst32 = (3000.0f * fConst31);
		fConst33 = (276.34892323050201f / fConst1);
		fConst34 = (9.869604401089358f / fConst1);
		fConst35 = (9.869604401089358f / fConst0);
		fConst36 = (1.0f / fConst0);
		fConst37 = (3.1415926535897931f / fConst0);
		fConst38 = (6.2831853071795862f / fConst0);
		fConst39 = (19.739208802178716f / fConst0);
		fConst40 = (0.80000000000000004f * fConst31);
		float 	fConst41 = faustpower<2>((314.15926535897933f / fConst0));
		float 	fConst42 = ((fConst41 * ((0.20330000000000001f * fConst41) + 0.31755f)) + 1.0f);
		float 	fConst43 = faustpower<2>(fConst42);
		fConst44 = (2.0f * ((98696.044010893587f * (fConst43 / fConst1)) + -1.0f));
		float 	fConst45 = (98696.044010893587f * (fConst42 / fConst0));
		fConst46 = (((fConst42 * (fConst45 + -314.15926535897933f)) / fConst0) + 1.0f);
		float 	fConst47 = (((fConst42 * (fConst45 + 314.15926535897933f)) / fConst0) + 1.0f);
		fConst48 = (1.0f / fConst47);
		fConst49 = (0.010937499999999999f / fConst0);
		fConst50 = (fConst43 / (fConst1 * fConst47));
		float 	fConst51 = faustpower<2>((125.66370614359172f / fConst0));
		float 	fConst52 = ((fConst51 * ((0.20330000000000001f * fConst51) + 0.31755f)) + 1.0f);
		float 	fConst53 = faustpower<2>(fConst52);
		fConst54 = (2.0f * ((15791.367041742973f * (fConst53 / fConst1)) + -1.0f));
		float 	fConst55 = (15791.367041742973f * (fConst52 / fConst0));
		fConst56 = (((fConst52 * (fConst55 + -232.71056693257725f)) / fConst0) + 1.0f);
		float 	fConst57 = (((fConst52 * (fConst55 + 232.71056693257725f)) / fConst0) + 1.0f);
		fConst58 = (1.0f / fConst57);
		fConst59 = (((fConst52 * (fConst55 + -95.926493239382992f)) / fConst0) + 1.0f);
		float 	fConst60 = (((fConst52 * (fConst55 + 95.926493239382992f)) / fConst0) + 1.0f);
		fConst61 = (1.0f / fConst60);
		fConst62 = (fConst53 / (fConst1 * fConst60));
		fConst63 = (fConst53 / (fConst1 * fConst57));
		fConst64 = powf(0.5f,(10.0f / fConst0));
		fConst65 = (1.3700000000000001f * fConst50);
		fConst66 = (0 - (2.0f / fConst7));
	}
	virtual void instanceResetUserInterface() {
		fslider0 = 0.0f;
		fslider1 = 69.0f;
		fslider2 = 0.0f;
		fslider3 = 0.0f;
		fslider4 = 0.0f;
		fslider5 = 0.0f;
		fslider6 = 0.0f;
		fslider7 = 0.1875f;
		fslider8 = 1.0f;
		fslider9 = 69.0f;
		fslider10 = 0.0f;
		fslider11 = 0.0f;
		fslider12 = 0.0f;
		fslider13 = 69.0f;
		fslider14 = 0.0f;
		fslider15 = 0.0f;
		fslider16 = 0.0f;
		fslider17 = 69.0f;
		fslider18 = 0.0f;
		fslider19 = 0.0f;
		fslider20 = 0.0f;
		fslider21 = 69.0f;
		fslider22 = 0.0f;
		fslider23 = 0.0f;
		fslider24 = 0.0f;
		fslider25 = 69.0f;
		fslider26 = 0.0f;
		fslider27 = 0.0f;
		fslider28 = 0.0f;
	}
	virtual void instanceClear() {
		for (int i=0; i<2; i++) fVec0[i] = 0;
		for (int i=0; i<2; i++) fRec2[i] = 0;
		for (int i=0; i<3; i++) fRec1[i] = 0;
		for (int i=0; i<3; i++) fRec3[i] = 0;
		for (int i=0; i<3; i++) fRec4[i] = 0;
		for (int i=0; i<3; i++) fRec5[i] = 0;
		for (int i=0; i<2; i++) fRec6[i] = 0;
		for (int i=0; i<2; i++) fRec7[i] = 0;
		for (int i=0; i<2; i++) fRec13[i] = 0;
		for (int i=0; i<3; i++) fRec12[i] = 0;
		for (int i=0; i<3; i++) fRec16[i] = 0;
		for (int i=0; i<3; i++) fRec15[i] = 0;
		for (int i=0; i<2; i++) fRec14[i] = 0;
		for (int i=0; i<2; i++) fRec9[i] = 0;
		for (int i=0; i<2; i++) fRec10[i] = 0;
		for (int i=0; i<2; i++) fVec1[i] = 0;
		for (int i=0; i<2; i++) fRec18[i] = 0;
		for (int i=0; i<3; i++) fRec17[i] = 0;
		for (int i=0; i<3; i++) fRec19[i] = 0;
		for (int i=0; i<3; i++) fRec20[i] = 0;
		for (int i=0; i<3; i++) fRec21[i] = 0;
		for (int i=0; i<2; i++) fRec22[i] = 0;
		for (int i=0; i<2; i++) fRec23[i] = 0;
		for (int i=0; i<2; i++) fRec29[i] = 0;
		for (int i=0; i<3; i++) fRec28[i] = 0;
		for (int i=0; i<2; i++) fRec25[i] = 0;
		for (int i=0; i<2; i++) fRec26[i] = 0;
		for (int i=0; i<2; i++) fVec2[i] = 0;
		for (int i=0; i<2; i++) fRec31[i] = 0;
		for (int i=0; i<3; i++) fRec30[i] = 0;
		for (int i=0; i<3; i++) fRec32[i] = 0;
		for (int i=0; i<3; i++) fRec33[i] = 0;
		for (int i=0; i<3; i++) fRec34[i] = 0;
		for (int i=0; i<2; i++) fRec35[i] = 0;
		for (int i=0; i<2; i++) fRec36[i] = 0;
		for (int i=0; i<2; i++) fRec42[i] = 0;
		for (int i=0; i<3; i++) fRec41[i] = 0;
		for (int i=0; i<2; i++) fRec38[i] = 0;
		for (int i=0; i<2; i++) fRec39[i] = 0;
		for (int i=0; i<2; i++) fVec3[i] = 0;
		for (int i=0; i<2; i++) fRec44[i] = 0;
		for (int i=0; i<3; i++) fRec43[i] = 0;
		for (int i=0; i<3; i++) fRec45[i] = 0;
		for (int i=0; i<3; i++) fRec46[i] = 0;
		for (int i=0; i<3; i++) fRec47[i] = 0;
		for (int i=0; i<2; i++) fRec48[i] = 0;
		for (int i=0; i<2; i++) fRec49[i] = 0;
		for (int i=0; i<2; i++) fRec55[i] = 0;
		for (int i=0; i<3; i++) fRec54[i] = 0;
		for (int i=0; i<2; i++) fRec51[i] = 0;
		for (int i=0; i<2; i++) fRec52[i] = 0;
		for (int i=0; i<2; i++) fVec4[i] = 0;
		for (int i=0; i<2; i++) fRec57[i] = 0;
		for (int i=0; i<3; i++) fRec56[i] = 0;
		for (int i=0; i<3; i++) fRec58[i] = 0;
		for (int i=0; i<3; i++) fRec59[i] = 0;
		for (int i=0; i<3; i++) fRec60[i] = 0;
		for (int i=0; i<2; i++) fRec61[i] = 0;
		for (int i=0; i<2; i++) fRec62[i] = 0;
		for (int i=0; i<2; i++) fRec68[i] = 0;
		for (int i=0; i<3; i++) fRec67[i] = 0;
		for (int i=0; i<2; i++) fRec64[i] = 0;
		for (int i=0; i<2; i++) fRec65[i] = 0;
		for (int i=0; i<2; i++) fVec5[i] = 0;
		for (int i=0; i<2; i++) fRec70[i] = 0;
		for (int i=0; i<3; i++) fRec69[i] = 0;
		for (int i=0; i<3; i++) fRec71[i] = 0;
		for (int i=0; i<3; i++) fRec72[i] = 0;
		for (int i=0; i<3; i++) fRec73[i] = 0;
		for (int i=0; i<2; i++) fRec74[i] = 0;
		for (int i=0; i<2; i++) fRec75[i] = 0;
		for (int i=0; i<2; i++) fRec81[i] = 0;
		for (int i=0; i<3; i++) fRec80[i] = 0;
		for (int i=0; i<2; i++) fRec77[i] = 0;
		for (int i=0; i<2; i++) fRec78[i] = 0;
		for (int i=0; i<3; i++) fRec0[i] = 0;
	}
	virtual void init(int sample_rate) {
		classInit(sample_rate);
		instanceInit(sample_rate);
	}
	virtual void instanceInit(int sample_rate) {
		instanceConstants(sample_rate);
		instanceResetUserInterface();
		instanceClear();
	}

	virtual int getSampleRate() {
		return fSampleRate;
	}
	virtual void buildUserInterfaceEmbedded() {
		synth_interface.acc_abs = &fslider8;
		synth_interface.pedal = &fslider7;
		synth_interface.rot_x = &fslider2;
		synth_interface.rot_y = &fslider3;
		synth_interface.rot_z = &fslider0;
		synth_interface.note[0] = &fslider25;
		synth_interface.pres[0] = &fslider27;
		synth_interface.vpres[0] = &fslider28;
		synth_interface.but_y[0] = &fslider26;
		synth_interface.note[1] = &fslider21;
		synth_interface.pres[1] = &fslider23;
		synth_interface.vpres[1] = &fslider24;
		synth_interface.but_y[1] = &fslider22;
		synth_interface.note[2] = &fslider17;
		synth_interface.pres[2] = &fslider19;
		synth_interface.vpres[2] = &fslider20;
		synth_interface.but_y[2] = &fslider18;
		synth_interface.note[3] = &fslider13;
		synth_interface.pres[3] = &fslider15;
		synth_interface.vpres[3] = &fslider16;
		synth_interface.but_y[3] = &fslider14;
		synth_interface.note[4] = &fslider9;
		synth_interface.pres[4] = &fslider11;
		synth_interface.vpres[4] = &fslider12;
		synth_interface.but_y[4] = &fslider10;
		synth_interface.note[5] = &fslider1;
		synth_interface.pres[5] = &fslider5;
		synth_interface.vpres[5] = &fslider6;
		synth_interface.but_y[5] = &fslider4;
	}
	virtual void compute (int count, FAUSTFLOAT** input, FAUSTFLOAT** output) {
		//zone1
		//zone2
		float 	fSlow0 = float(fslider0);
		float 	fSlow1 = ((int((fSlow0 > 0.0f)))?fConst12:fConst10);
		float 	fSlow2 = fabsf(fSlow0);
		float 	fSlow3 = ((0.5f * fSlow2) + 1.0f);
		float 	fSlow4 = (fSlow3 * fSlow1);
		float 	fSlow5 = ((fSlow3 * (fSlow1 * (fSlow4 + 0.125f))) + 1.0f);
		float 	fSlow6 = ((faustpower<2>(fSlow3) * faustpower<2>(fSlow1)) + -1.0f);
		float 	fSlow7 = ((fSlow3 * (fSlow1 * (fSlow4 + -0.125f))) + 1.0f);
		float 	fSlow8 = float(fslider1);
		float 	fSlow9 = fastpow2((0.083333333333333329f * fSlow8));
		int 	iSlow10 = int(min((fConst13 / fSlow9), (float)4096));
		float 	fSlow11 = float(iSlow10);
		float 	fSlow12 = (fConst14 * (fSlow9 * fSlow11));
		float 	fSlow13 = (fSlow2 * fSlow3);
		float 	fSlow14 = float(fslider2);
		float 	fSlow15 = ((int((fSlow14 > 0.0f)))?fConst18:fConst16);
		float 	fSlow16 = fabsf(fSlow14);
		float 	fSlow17 = ((0.5f * fSlow16) + 1.0f);
		float 	fSlow18 = (fSlow17 * fSlow15);
		float 	fSlow19 = ((fSlow17 * (fSlow15 * (fSlow18 + 0.125f))) + 1.0f);
		float 	fSlow20 = ((faustpower<2>(fSlow17) * faustpower<2>(fSlow15)) + -1.0f);
		float 	fSlow21 = ((fSlow17 * (fSlow15 * (fSlow18 + -0.125f))) + 1.0f);
		float 	fSlow22 = (fSlow16 * fSlow17);
		float 	fSlow23 = float(fslider3);
		float 	fSlow24 = ((int((fSlow23 > 0.0f)))?fConst22:fConst20);
		float 	fSlow25 = fabsf(fSlow23);
		float 	fSlow26 = ((0.5f * fSlow25) + 1.0f);
		float 	fSlow27 = (fSlow26 * fSlow24);
		float 	fSlow28 = ((fSlow26 * (fSlow24 * (fSlow27 + 0.20000000000000001f))) + 1.0f);
		float 	fSlow29 = ((faustpower<2>(fSlow26) * faustpower<2>(fSlow24)) + -1.0f);
		float 	fSlow30 = ((fSlow26 * (fSlow24 * (fSlow27 + -0.20000000000000001f))) + 1.0f);
		float 	fSlow31 = (0.20000000000000001f * (fSlow25 * fSlow26));
		float 	fSlow32 = float(fslider4);
		float 	fSlow33 = float(fslider5);
		float 	fSlow34 = (fConst32 * fSlow33);
		float 	fSlow35 = (fConst40 * fSlow33);
		float 	fSlow36 = max((float(fslider6) + -0.02f), (float)0);
		float 	fSlow37 = (fSlow36 / (fSlow36 + 0.5f));
		float 	fSlow38 = ((0.20000000000000001f * float(fslider7)) + 0.01f);
		float 	fSlow39 = (1.0f - (fConst49 * (fSlow8 / max(min((16.0f * fSlow33), (0.5f * (fSlow33 + 1.0f))), fSlow38))));
		float 	fSlow40 = faustpower<2>(fSlow33);
		float 	fSlow41 = float(fslider8);
		float 	fSlow42 = max((8.1757989159999997f * fSlow9), (float)200);
		float 	fSlow43 = float(fslider9);
		float 	fSlow44 = fastpow2((0.083333333333333329f * fSlow43));
		int 	iSlow45 = int(min((fConst13 / fSlow44), (float)4096));
		float 	fSlow46 = float(iSlow45);
		float 	fSlow47 = (fConst14 * (fSlow44 * fSlow46));
		float 	fSlow48 = float(fslider10);
		float 	fSlow49 = float(fslider11);
		float 	fSlow50 = (fConst32 * fSlow49);
		float 	fSlow51 = (fConst40 * fSlow49);
		float 	fSlow52 = max((float(fslider12) + -0.02f), (float)0);
		float 	fSlow53 = (fSlow52 / (fSlow52 + 0.5f));
		float 	fSlow54 = (1.0f - (fConst49 * (fSlow43 / max(min((16.0f * fSlow49), (0.5f * (fSlow49 + 1.0f))), fSlow38))));
		float 	fSlow55 = faustpower<2>(fSlow49);
		float 	fSlow56 = max((8.1757989159999997f * fSlow44), (float)200);
		float 	fSlow57 = float(fslider13);
		float 	fSlow58 = fastpow2((0.083333333333333329f * fSlow57));
		int 	iSlow59 = int(min((fConst13 / fSlow58), (float)4096));
		float 	fSlow60 = float(iSlow59);
		float 	fSlow61 = (fConst14 * (fSlow58 * fSlow60));
		float 	fSlow62 = float(fslider14);
		float 	fSlow63 = float(fslider15);
		float 	fSlow64 = (fConst32 * fSlow63);
		float 	fSlow65 = (fConst40 * fSlow63);
		float 	fSlow66 = max((float(fslider16) + -0.02f), (float)0);
		float 	fSlow67 = (fSlow66 / (fSlow66 + 0.5f));
		float 	fSlow68 = (1.0f - (fConst49 * (fSlow57 / max(min((16.0f * fSlow63), (0.5f * (fSlow63 + 1.0f))), fSlow38))));
		float 	fSlow69 = faustpower<2>(fSlow63);
		float 	fSlow70 = max((8.1757989159999997f * fSlow58), (float)200);
		float 	fSlow71 = float(fslider17);
		float 	fSlow72 = fastpow2((0.083333333333333329f * fSlow71));
		int 	iSlow73 = int(min((fConst13 / fSlow72), (float)4096));
		float 	fSlow74 = float(iSlow73);
		float 	fSlow75 = (fConst14 * (fSlow72 * fSlow74));
		float 	fSlow76 = float(fslider18);
		float 	fSlow77 = float(fslider19);
		float 	fSlow78 = (fConst32 * fSlow77);
		float 	fSlow79 = (fConst40 * fSlow77);
		float 	fSlow80 = max((float(fslider20) + -0.02f), (float)0);
		float 	fSlow81 = (fSlow80 / (fSlow80 + 0.5f));
		float 	fSlow82 = (1.0f - (fConst49 * (fSlow71 / max(min((16.0f * fSlow77), (0.5f * (fSlow77 + 1.0f))), fSlow38))));
		float 	fSlow83 = faustpower<2>(fSlow77);
		float 	fSlow84 = max((8.1757989159999997f * fSlow72), (float)200);
		float 	fSlow85 = float(fslider21);
		float 	fSlow86 = fastpow2((0.083333333333333329f * fSlow85));
		int 	iSlow87 = int(min((fConst13 / fSlow86), (float)4096));
		float 	fSlow88 = float(iSlow87);
		float 	fSlow89 = (fConst14 * (fSlow86 * fSlow88));
		float 	fSlow90 = float(fslider22);
		float 	fSlow91 = float(fslider23);
		float 	fSlow92 = (fConst32 * fSlow91);
		float 	fSlow93 = (fConst40 * fSlow91);
		float 	fSlow94 = max((float(fslider24) + -0.02f), (float)0);
		float 	fSlow95 = (fSlow94 / (fSlow94 + 0.5f));
		float 	fSlow96 = (1.0f - (fConst49 * (fSlow85 / max(min((16.0f * fSlow91), (0.5f * (fSlow91 + 1.0f))), fSlow38))));
		float 	fSlow97 = faustpower<2>(fSlow91);
		float 	fSlow98 = max((8.1757989159999997f * fSlow86), (float)200);
		float 	fSlow99 = float(fslider25);
		float 	fSlow100 = fastpow2((0.083333333333333329f * fSlow99));
		int 	iSlow101 = int(min((fConst13 / fSlow100), (float)4096));
		float 	fSlow102 = float(iSlow101);
		float 	fSlow103 = (fConst14 * (fSlow100 * fSlow102));
		float 	fSlow104 = float(fslider26);
		float 	fSlow105 = float(fslider27);
		float 	fSlow106 = (fConst32 * fSlow105);
		float 	fSlow107 = (fConst40 * fSlow105);
		float 	fSlow108 = max((float(fslider28) + -0.02f), (float)0);
		float 	fSlow109 = (fSlow108 / (fSlow108 + 0.5f));
		float 	fSlow110 = (1.0f - (fConst49 * (fSlow99 / max(min((16.0f * fSlow105), (0.5f * (fSlow105 + 1.0f))), fSlow38))));
		float 	fSlow111 = faustpower<2>(fSlow105);
		float 	fSlow112 = max((8.1757989159999997f * fSlow100), (float)200);
		//zone2b
		//zone3
		FAUSTFLOAT* output0 = output[0];
		//LoopGraphScalar
		for (int i=0; i<count; i++) {
			fVec0[0] = fSlow8;
			fRec2[0] = (fSlow12 + (fRec2[1] - float((iSlow10 * ((fSlow12 + fRec2[1]) > fSlow11)))));
			int 	iTemp0 = int(fRec2[0]);
			float 	fTemp1 = ftbl0[iTemp0];
			float 	fTemp2 = (fTemp1 + ((fRec2[0] - float(iTemp0)) * (ftbl0[(iTemp0 + (1 - (iSlow10 * ((iTemp0 + 1) >= iSlow10))))] - fTemp1)));
			fRec1[0] = (fTemp2 - (((fSlow7 * fRec1[2]) + (2.0f * (fSlow6 * fRec1[1]))) / fSlow5));
			fRec3[0] = (fTemp2 - (((fSlow21 * fRec3[2]) + (2.0f * (fSlow20 * fRec3[1]))) / fSlow19));
			fRec4[0] = (fTemp2 - (((fSlow30 * fRec4[2]) + (2.0f * (fSlow29 * fRec4[1]))) / fSlow28));
			fRec5[0] = (fSlow32 - (fConst30 * ((fConst28 * fRec5[2]) + (fConst26 * fRec5[1]))));
			float 	fTemp3 = (((3947.8417604357433f * fRec5[0]) + (7895.6835208714865f * fRec5[1])) + (3947.8417604357433f * fRec5[2]));
			float 	fTemp4 = (fConst31 * fTemp3);
			float 	fTemp5 = max(fTemp4, (float)0);
			float 	fTemp6 = ((fSlow34 * (fTemp3 * fTemp5)) + 200.0f);
			float 	fTemp7 = faustpower<2>(fTemp6);
			float 	fTemp8 = ((fConst34 * (fTemp7 * (3150.0f - (fConst33 * fTemp7)))) + -62370.0f);
			float 	fTemp9 = ((fConst34 * (fTemp7 * ((fConst34 * (fTemp7 * (378.0f - (fConst34 * fTemp7)))) + -17325.0f))) + 135135.0f);
			float 	fTemp10 = (fTemp6 * fTemp9);
			float 	fTemp11 = ((fConst34 * (fTemp7 * fTemp8)) + 135135.0f);
			float 	fTemp12 = ((2.0f * faustpower<2>(fTemp11)) + (fConst37 * (fTemp10 * ((fConst36 * (fTemp6 * ((6.2831853071795862f * fTemp9) + (fConst35 * (fTemp6 * fTemp8))))) + 135135.0f))));
			float 	fTemp13 = (fTemp10 * (fTemp2 - fRec7[1]));
			float 	fTemp14 = (fTemp11 * fRec6[1]);
			float 	fTemp15 = ((fTemp11 * ((2.0f * fTemp14) + (fConst38 * fTemp13))) / fTemp12);
			fRec6[0] = ((2.0f * fTemp15) - fRec6[1]);
			float 	fTemp16 = (fRec7[1] + (fConst36 * ((fTemp10 * ((6.2831853071795862f * fTemp14) + (fConst39 * fTemp13))) / fTemp12)));
			fRec7[0] = ((2.0f * fTemp16) - fRec7[1]);
			float 	fRec8 = fTemp15;
			float 	fTemp17 = max((-1 * fTemp4), (float)0);
			fRec13[0] = (fSlow39 * (max(fRec13[1], fSlow37) * float((fabsf((fSlow8 - fVec0[1])) < 1.0f))));
			fRec12[0] = (fSlow40 + (fRec13[0] - (fConst48 * ((fConst46 * fRec12[2]) + (fConst44 * fRec12[1])))));
			float 	fTemp18 = (((98696.044010893587f * fRec12[0]) + (197392.08802178717f * fRec12[1])) + (98696.044010893587f * fRec12[2]));
			fRec16[0] = (fSlow41 - (fConst61 * ((fConst59 * fRec16[2]) + (fConst54 * fRec16[1]))));
			fRec15[0] = ((fConst62 * (((15791.367041742973f * fRec16[0]) + (31582.734083485946f * fRec16[1])) + (15791.367041742973f * fRec16[2]))) - (fConst58 * ((fConst56 * fRec15[2]) + (fConst54 * fRec15[1]))));
			fRec14[0] = (fConst64 * max(fRec14[1], min((fConst63 * (((15791.367041742973f * fRec15[0]) + (31582.734083485946f * fRec15[1])) + (15791.367041742973f * fRec15[2]))), (float)2)));
			float 	fTemp19 = max((fRec14[0] + -1.0f), (float)0);
			float 	fTemp20 = min((fSlow42 * (faustpower<2>((fTemp19 + (fConst50 * (fTemp18 * (1.0f - fTemp17))))) + 1.0f)), (float)16000);
			float 	fTemp21 = faustpower<2>(fTemp20);
			float 	fTemp22 = ((fConst34 * (fTemp21 * ((fConst34 * (fTemp21 * (378.0f - (fConst34 * fTemp21)))) + -17325.0f))) + 135135.0f);
			float 	fTemp23 = ((8.0f * fTemp17) + 1.0f);
			float 	fTemp24 = ((fConst34 * (fTemp21 * (3150.0f - (fConst33 * fTemp21)))) + -62370.0f);
			float 	fTemp25 = (fTemp20 * fTemp22);
			float 	fTemp26 = ((fConst34 * (fTemp21 * fTemp24)) + 135135.0f);
			float 	fTemp27 = ((fTemp23 * faustpower<2>(fTemp26)) + (fConst37 * (fTemp25 * ((fConst36 * (fTemp20 * ((fConst35 * (fTemp20 * fTemp24)) + (3.1415926535897931f * (fTemp23 * fTemp22))))) + 135135.0f))));
			float 	fTemp28 = (fTemp25 * (fTemp2 - fRec10[1]));
			float 	fTemp29 = (fTemp26 * fRec9[1]);
			float 	fTemp30 = (((fTemp23 * fTemp26) * (fTemp29 + (fConst37 * fTemp28))) / fTemp27);
			fRec9[0] = ((2.0f * fTemp30) - fRec9[1]);
			float 	fTemp31 = (fRec10[1] + (fConst36 * ((((fTemp23 * fTemp20) * fTemp22) * ((3.1415926535897931f * fTemp29) + (fConst35 * fTemp28))) / fTemp27)));
			fRec10[0] = ((2.0f * fTemp31) - fRec10[1]);
			float 	fRec11 = fTemp31;
			fVec1[0] = fSlow43;
			fRec18[0] = (fSlow47 + (fRec18[1] - float((iSlow45 * ((fSlow47 + fRec18[1]) > fSlow46)))));
			int 	iTemp32 = int(fRec18[0]);
			float 	fTemp33 = ftbl0[iTemp32];
			float 	fTemp34 = (fTemp33 + ((fRec18[0] - float(iTemp32)) * (ftbl0[(iTemp32 + (1 - (iSlow45 * ((iTemp32 + 1) >= iSlow45))))] - fTemp33)));
			fRec17[0] = (fTemp34 - (((fSlow7 * fRec17[2]) + (2.0f * (fSlow6 * fRec17[1]))) / fSlow5));
			fRec19[0] = (fTemp34 - (((fSlow21 * fRec19[2]) + (2.0f * (fSlow20 * fRec19[1]))) / fSlow19));
			fRec20[0] = (fTemp34 - (((fSlow30 * fRec20[2]) + (2.0f * (fSlow29 * fRec20[1]))) / fSlow28));
			fRec21[0] = (fSlow48 - (fConst30 * ((fConst28 * fRec21[2]) + (fConst26 * fRec21[1]))));
			float 	fTemp35 = (((3947.8417604357433f * fRec21[0]) + (7895.6835208714865f * fRec21[1])) + (3947.8417604357433f * fRec21[2]));
			float 	fTemp36 = (fConst31 * fTemp35);
			float 	fTemp37 = max(fTemp36, (float)0);
			float 	fTemp38 = ((fSlow50 * (fTemp35 * fTemp37)) + 200.0f);
			float 	fTemp39 = faustpower<2>(fTemp38);
			float 	fTemp40 = ((fConst34 * (fTemp39 * (3150.0f - (fConst33 * fTemp39)))) + -62370.0f);
			float 	fTemp41 = ((fConst34 * (fTemp39 * ((fConst34 * (fTemp39 * (378.0f - (fConst34 * fTemp39)))) + -17325.0f))) + 135135.0f);
			float 	fTemp42 = (fTemp38 * fTemp41);
			float 	fTemp43 = ((fConst34 * (fTemp39 * fTemp40)) + 135135.0f);
			float 	fTemp44 = ((2.0f * faustpower<2>(fTemp43)) + (fConst37 * (fTemp42 * ((fConst36 * (fTemp38 * ((6.2831853071795862f * fTemp41) + (fConst35 * (fTemp38 * fTemp40))))) + 135135.0f))));
			float 	fTemp45 = (fTemp42 * (fTemp34 - fRec23[1]));
			float 	fTemp46 = (fTemp43 * fRec22[1]);
			float 	fTemp47 = ((fTemp43 * ((2.0f * fTemp46) + (fConst38 * fTemp45))) / fTemp44);
			fRec22[0] = ((2.0f * fTemp47) - fRec22[1]);
			float 	fTemp48 = (fRec23[1] + (fConst36 * ((fTemp42 * ((6.2831853071795862f * fTemp46) + (fConst39 * fTemp45))) / fTemp44)));
			fRec23[0] = ((2.0f * fTemp48) - fRec23[1]);
			float 	fRec24 = fTemp47;
			float 	fTemp49 = max((-1 * fTemp36), (float)0);
			fRec29[0] = (fSlow54 * (max(fRec29[1], fSlow53) * float((fabsf((fSlow43 - fVec1[1])) < 1.0f))));
			fRec28[0] = (fSlow55 + (fRec29[0] - (fConst48 * ((fConst46 * fRec28[2]) + (fConst44 * fRec28[1])))));
			float 	fTemp50 = (((98696.044010893587f * fRec28[0]) + (197392.08802178717f * fRec28[1])) + (98696.044010893587f * fRec28[2]));
			float 	fTemp51 = min((fSlow56 * (faustpower<2>((fTemp19 + (fConst50 * (fTemp50 * (1.0f - fTemp49))))) + 1.0f)), (float)16000);
			float 	fTemp52 = faustpower<2>(fTemp51);
			float 	fTemp53 = ((fConst34 * (fTemp52 * ((fConst34 * (fTemp52 * (378.0f - (fConst34 * fTemp52)))) + -17325.0f))) + 135135.0f);
			float 	fTemp54 = ((8.0f * fTemp49) + 1.0f);
			float 	fTemp55 = ((fConst34 * (fTemp52 * (3150.0f - (fConst33 * fTemp52)))) + -62370.0f);
			float 	fTemp56 = (fTemp51 * fTemp53);
			float 	fTemp57 = ((fConst34 * (fTemp52 * fTemp55)) + 135135.0f);
			float 	fTemp58 = ((fTemp54 * faustpower<2>(fTemp57)) + (fConst37 * (fTemp56 * ((fConst36 * (fTemp51 * ((fConst35 * (fTemp51 * fTemp55)) + (3.1415926535897931f * (fTemp54 * fTemp53))))) + 135135.0f))));
			float 	fTemp59 = (fTemp56 * (fTemp34 - fRec26[1]));
			float 	fTemp60 = (fTemp57 * fRec25[1]);
			float 	fTemp61 = (((fTemp54 * fTemp57) * (fTemp60 + (fConst37 * fTemp59))) / fTemp58);
			fRec25[0] = ((2.0f * fTemp61) - fRec25[1]);
			float 	fTemp62 = (fRec26[1] + (fConst36 * ((((fTemp54 * fTemp51) * fTemp53) * ((3.1415926535897931f * fTemp60) + (fConst35 * fTemp59))) / fTemp58)));
			fRec26[0] = ((2.0f * fTemp62) - fRec26[1]);
			float 	fRec27 = fTemp62;
			fVec2[0] = fSlow57;
			fRec31[0] = (fSlow61 + (fRec31[1] - float((iSlow59 * ((fSlow61 + fRec31[1]) > fSlow60)))));
			int 	iTemp63 = int(fRec31[0]);
			float 	fTemp64 = ftbl0[iTemp63];
			float 	fTemp65 = (fTemp64 + ((fRec31[0] - float(iTemp63)) * (ftbl0[(iTemp63 + (1 - (iSlow59 * ((iTemp63 + 1) >= iSlow59))))] - fTemp64)));
			fRec30[0] = (fTemp65 - (((fSlow7 * fRec30[2]) + (2.0f * (fSlow6 * fRec30[1]))) / fSlow5));
			fRec32[0] = (fTemp65 - (((fSlow21 * fRec32[2]) + (2.0f * (fSlow20 * fRec32[1]))) / fSlow19));
			fRec33[0] = (fTemp65 - (((fSlow30 * fRec33[2]) + (2.0f * (fSlow29 * fRec33[1]))) / fSlow28));
			fRec34[0] = (fSlow62 - (fConst30 * ((fConst28 * fRec34[2]) + (fConst26 * fRec34[1]))));
			float 	fTemp66 = (((3947.8417604357433f * fRec34[0]) + (7895.6835208714865f * fRec34[1])) + (3947.8417604357433f * fRec34[2]));
			float 	fTemp67 = (fConst31 * fTemp66);
			float 	fTemp68 = max(fTemp67, (float)0);
			float 	fTemp69 = ((fSlow64 * (fTemp66 * fTemp68)) + 200.0f);
			float 	fTemp70 = faustpower<2>(fTemp69);
			float 	fTemp71 = ((fConst34 * (fTemp70 * (3150.0f - (fConst33 * fTemp70)))) + -62370.0f);
			float 	fTemp72 = ((fConst34 * (fTemp70 * ((fConst34 * (fTemp70 * (378.0f - (fConst34 * fTemp70)))) + -17325.0f))) + 135135.0f);
			float 	fTemp73 = (fTemp69 * fTemp72);
			float 	fTemp74 = ((fConst34 * (fTemp70 * fTemp71)) + 135135.0f);
			float 	fTemp75 = ((2.0f * faustpower<2>(fTemp74)) + (fConst37 * (fTemp73 * ((fConst36 * (fTemp69 * ((6.2831853071795862f * fTemp72) + (fConst35 * (fTemp69 * fTemp71))))) + 135135.0f))));
			float 	fTemp76 = (fTemp73 * (fTemp65 - fRec36[1]));
			float 	fTemp77 = (fTemp74 * fRec35[1]);
			float 	fTemp78 = ((fTemp74 * ((2.0f * fTemp77) + (fConst38 * fTemp76))) / fTemp75);
			fRec35[0] = ((2.0f * fTemp78) - fRec35[1]);
			float 	fTemp79 = (fRec36[1] + (fConst36 * ((fTemp73 * ((6.2831853071795862f * fTemp77) + (fConst39 * fTemp76))) / fTemp75)));
			fRec36[0] = ((2.0f * fTemp79) - fRec36[1]);
			float 	fRec37 = fTemp78;
			float 	fTemp80 = max((-1 * fTemp67), (float)0);
			fRec42[0] = (fSlow68 * (max(fRec42[1], fSlow67) * float((fabsf((fSlow57 - fVec2[1])) < 1.0f))));
			fRec41[0] = (fSlow69 + (fRec42[0] - (fConst48 * ((fConst46 * fRec41[2]) + (fConst44 * fRec41[1])))));
			float 	fTemp81 = (((98696.044010893587f * fRec41[0]) + (197392.08802178717f * fRec41[1])) + (98696.044010893587f * fRec41[2]));
			float 	fTemp82 = min((fSlow70 * (faustpower<2>((fTemp19 + (fConst50 * (fTemp81 * (1.0f - fTemp80))))) + 1.0f)), (float)16000);
			float 	fTemp83 = faustpower<2>(fTemp82);
			float 	fTemp84 = ((fConst34 * (fTemp83 * ((fConst34 * (fTemp83 * (378.0f - (fConst34 * fTemp83)))) + -17325.0f))) + 135135.0f);
			float 	fTemp85 = ((8.0f * fTemp80) + 1.0f);
			float 	fTemp86 = ((fConst34 * (fTemp83 * (3150.0f - (fConst33 * fTemp83)))) + -62370.0f);
			float 	fTemp87 = (fTemp82 * fTemp84);
			float 	fTemp88 = ((fConst34 * (fTemp83 * fTemp86)) + 135135.0f);
			float 	fTemp89 = ((fTemp85 * faustpower<2>(fTemp88)) + (fConst37 * (fTemp87 * ((fConst36 * (fTemp82 * ((fConst35 * (fTemp82 * fTemp86)) + (3.1415926535897931f * (fTemp85 * fTemp84))))) + 135135.0f))));
			float 	fTemp90 = (fTemp87 * (fTemp65 - fRec39[1]));
			float 	fTemp91 = (fTemp88 * fRec38[1]);
			float 	fTemp92 = (((fTemp85 * fTemp88) * (fTemp91 + (fConst37 * fTemp90))) / fTemp89);
			fRec38[0] = ((2.0f * fTemp92) - fRec38[1]);
			float 	fTemp93 = (fRec39[1] + (fConst36 * ((((fTemp85 * fTemp82) * fTemp84) * ((3.1415926535897931f * fTemp91) + (fConst35 * fTemp90))) / fTemp89)));
			fRec39[0] = ((2.0f * fTemp93) - fRec39[1]);
			float 	fRec40 = fTemp93;
			fVec3[0] = fSlow71;
			fRec44[0] = (fSlow75 + (fRec44[1] - float((iSlow73 * ((fSlow75 + fRec44[1]) > fSlow74)))));
			int 	iTemp94 = int(fRec44[0]);
			float 	fTemp95 = ftbl0[iTemp94];
			float 	fTemp96 = (fTemp95 + ((fRec44[0] - float(iTemp94)) * (ftbl0[(iTemp94 + (1 - (iSlow73 * ((iTemp94 + 1) >= iSlow73))))] - fTemp95)));
			fRec43[0] = (fTemp96 - (((fSlow7 * fRec43[2]) + (2.0f * (fSlow6 * fRec43[1]))) / fSlow5));
			fRec45[0] = (fTemp96 - (((fSlow21 * fRec45[2]) + (2.0f * (fSlow20 * fRec45[1]))) / fSlow19));
			fRec46[0] = (fTemp96 - (((fSlow30 * fRec46[2]) + (2.0f * (fSlow29 * fRec46[1]))) / fSlow28));
			fRec47[0] = (fSlow76 - (fConst30 * ((fConst28 * fRec47[2]) + (fConst26 * fRec47[1]))));
			float 	fTemp97 = (((3947.8417604357433f * fRec47[0]) + (7895.6835208714865f * fRec47[1])) + (3947.8417604357433f * fRec47[2]));
			float 	fTemp98 = (fConst31 * fTemp97);
			float 	fTemp99 = max(fTemp98, (float)0);
			float 	fTemp100 = ((fSlow78 * (fTemp97 * fTemp99)) + 200.0f);
			float 	fTemp101 = faustpower<2>(fTemp100);
			float 	fTemp102 = ((fConst34 * (fTemp101 * (3150.0f - (fConst33 * fTemp101)))) + -62370.0f);
			float 	fTemp103 = ((fConst34 * (fTemp101 * ((fConst34 * (fTemp101 * (378.0f - (fConst34 * fTemp101)))) + -17325.0f))) + 135135.0f);
			float 	fTemp104 = (fTemp100 * fTemp103);
			float 	fTemp105 = ((fConst34 * (fTemp101 * fTemp102)) + 135135.0f);
			float 	fTemp106 = ((2.0f * faustpower<2>(fTemp105)) + (fConst37 * (fTemp104 * ((fConst36 * (fTemp100 * ((6.2831853071795862f * fTemp103) + (fConst35 * (fTemp100 * fTemp102))))) + 135135.0f))));
			float 	fTemp107 = (fTemp104 * (fTemp96 - fRec49[1]));
			float 	fTemp108 = (fTemp105 * fRec48[1]);
			float 	fTemp109 = ((fTemp105 * ((2.0f * fTemp108) + (fConst38 * fTemp107))) / fTemp106);
			fRec48[0] = ((2.0f * fTemp109) - fRec48[1]);
			float 	fTemp110 = (fRec49[1] + (fConst36 * ((fTemp104 * ((6.2831853071795862f * fTemp108) + (fConst39 * fTemp107))) / fTemp106)));
			fRec49[0] = ((2.0f * fTemp110) - fRec49[1]);
			float 	fRec50 = fTemp109;
			float 	fTemp111 = max((-1 * fTemp98), (float)0);
			fRec55[0] = (fSlow82 * (max(fRec55[1], fSlow81) * float((fabsf((fSlow71 - fVec3[1])) < 1.0f))));
			fRec54[0] = (fSlow83 + (fRec55[0] - (fConst48 * ((fConst46 * fRec54[2]) + (fConst44 * fRec54[1])))));
			float 	fTemp112 = (((98696.044010893587f * fRec54[0]) + (197392.08802178717f * fRec54[1])) + (98696.044010893587f * fRec54[2]));
			float 	fTemp113 = min((fSlow84 * (faustpower<2>((fTemp19 + (fConst50 * (fTemp112 * (1.0f - fTemp111))))) + 1.0f)), (float)16000);
			float 	fTemp114 = faustpower<2>(fTemp113);
			float 	fTemp115 = ((fConst34 * (fTemp114 * ((fConst34 * (fTemp114 * (378.0f - (fConst34 * fTemp114)))) + -17325.0f))) + 135135.0f);
			float 	fTemp116 = ((8.0f * fTemp111) + 1.0f);
			float 	fTemp117 = ((fConst34 * (fTemp114 * (3150.0f - (fConst33 * fTemp114)))) + -62370.0f);
			float 	fTemp118 = (fTemp113 * fTemp115);
			float 	fTemp119 = ((fConst34 * (fTemp114 * fTemp117)) + 135135.0f);
			float 	fTemp120 = ((fTemp116 * faustpower<2>(fTemp119)) + (fConst37 * (fTemp118 * ((fConst36 * (fTemp113 * ((fConst35 * (fTemp113 * fTemp117)) + (3.1415926535897931f * (fTemp116 * fTemp115))))) + 135135.0f))));
			float 	fTemp121 = (fTemp118 * (fTemp96 - fRec52[1]));
			float 	fTemp122 = (fTemp119 * fRec51[1]);
			float 	fTemp123 = (((fTemp116 * fTemp119) * (fTemp122 + (fConst37 * fTemp121))) / fTemp120);
			fRec51[0] = ((2.0f * fTemp123) - fRec51[1]);
			float 	fTemp124 = (fRec52[1] + (fConst36 * ((((fTemp116 * fTemp113) * fTemp115) * ((3.1415926535897931f * fTemp122) + (fConst35 * fTemp121))) / fTemp120)));
			fRec52[0] = ((2.0f * fTemp124) - fRec52[1]);
			float 	fRec53 = fTemp124;
			fVec4[0] = fSlow85;
			fRec57[0] = (fSlow89 + (fRec57[1] - float((iSlow87 * ((fSlow89 + fRec57[1]) > fSlow88)))));
			int 	iTemp125 = int(fRec57[0]);
			float 	fTemp126 = ftbl0[iTemp125];
			float 	fTemp127 = (fTemp126 + ((fRec57[0] - float(iTemp125)) * (ftbl0[(iTemp125 + (1 - (iSlow87 * ((iTemp125 + 1) >= iSlow87))))] - fTemp126)));
			fRec56[0] = (fTemp127 - (((fSlow7 * fRec56[2]) + (2.0f * (fSlow6 * fRec56[1]))) / fSlow5));
			fRec58[0] = (fTemp127 - (((fSlow21 * fRec58[2]) + (2.0f * (fSlow20 * fRec58[1]))) / fSlow19));
			fRec59[0] = (fTemp127 - (((fSlow30 * fRec59[2]) + (2.0f * (fSlow29 * fRec59[1]))) / fSlow28));
			fRec60[0] = (fSlow90 - (fConst30 * ((fConst28 * fRec60[2]) + (fConst26 * fRec60[1]))));
			float 	fTemp128 = (((3947.8417604357433f * fRec60[0]) + (7895.6835208714865f * fRec60[1])) + (3947.8417604357433f * fRec60[2]));
			float 	fTemp129 = (fConst31 * fTemp128);
			float 	fTemp130 = max(fTemp129, (float)0);
			float 	fTemp131 = ((fSlow92 * (fTemp128 * fTemp130)) + 200.0f);
			float 	fTemp132 = faustpower<2>(fTemp131);
			float 	fTemp133 = ((fConst34 * (fTemp132 * (3150.0f - (fConst33 * fTemp132)))) + -62370.0f);
			float 	fTemp134 = ((fConst34 * (fTemp132 * ((fConst34 * (fTemp132 * (378.0f - (fConst34 * fTemp132)))) + -17325.0f))) + 135135.0f);
			float 	fTemp135 = (fTemp131 * fTemp134);
			float 	fTemp136 = ((fConst34 * (fTemp132 * fTemp133)) + 135135.0f);
			float 	fTemp137 = ((2.0f * faustpower<2>(fTemp136)) + (fConst37 * (fTemp135 * ((fConst36 * (fTemp131 * ((6.2831853071795862f * fTemp134) + (fConst35 * (fTemp131 * fTemp133))))) + 135135.0f))));
			float 	fTemp138 = (fTemp135 * (fTemp127 - fRec62[1]));
			float 	fTemp139 = (fTemp136 * fRec61[1]);
			float 	fTemp140 = ((fTemp136 * ((2.0f * fTemp139) + (fConst38 * fTemp138))) / fTemp137);
			fRec61[0] = ((2.0f * fTemp140) - fRec61[1]);
			float 	fTemp141 = (fRec62[1] + (fConst36 * ((fTemp135 * ((6.2831853071795862f * fTemp139) + (fConst39 * fTemp138))) / fTemp137)));
			fRec62[0] = ((2.0f * fTemp141) - fRec62[1]);
			float 	fRec63 = fTemp140;
			float 	fTemp142 = max((-1 * fTemp129), (float)0);
			fRec68[0] = (fSlow96 * (max(fRec68[1], fSlow95) * float((fabsf((fSlow85 - fVec4[1])) < 1.0f))));
			fRec67[0] = (fSlow97 + (fRec68[0] - (fConst48 * ((fConst46 * fRec67[2]) + (fConst44 * fRec67[1])))));
			float 	fTemp143 = (((98696.044010893587f * fRec67[0]) + (197392.08802178717f * fRec67[1])) + (98696.044010893587f * fRec67[2]));
			float 	fTemp144 = min((fSlow98 * (faustpower<2>((fTemp19 + (fConst50 * (fTemp143 * (1.0f - fTemp142))))) + 1.0f)), (float)16000);
			float 	fTemp145 = faustpower<2>(fTemp144);
			float 	fTemp146 = ((fConst34 * (fTemp145 * ((fConst34 * (fTemp145 * (378.0f - (fConst34 * fTemp145)))) + -17325.0f))) + 135135.0f);
			float 	fTemp147 = ((8.0f * fTemp142) + 1.0f);
			float 	fTemp148 = ((fConst34 * (fTemp145 * (3150.0f - (fConst33 * fTemp145)))) + -62370.0f);
			float 	fTemp149 = (fTemp144 * fTemp146);
			float 	fTemp150 = ((fConst34 * (fTemp145 * fTemp148)) + 135135.0f);
			float 	fTemp151 = ((fTemp147 * faustpower<2>(fTemp150)) + (fConst37 * (fTemp149 * ((fConst36 * (fTemp144 * ((fConst35 * (fTemp144 * fTemp148)) + (3.1415926535897931f * (fTemp147 * fTemp146))))) + 135135.0f))));
			float 	fTemp152 = (fTemp149 * (fTemp127 - fRec65[1]));
			float 	fTemp153 = (fTemp150 * fRec64[1]);
			float 	fTemp154 = (((fTemp147 * fTemp150) * (fTemp153 + (fConst37 * fTemp152))) / fTemp151);
			fRec64[0] = ((2.0f * fTemp154) - fRec64[1]);
			float 	fTemp155 = (fRec65[1] + (fConst36 * ((((fTemp147 * fTemp144) * fTemp146) * ((3.1415926535897931f * fTemp153) + (fConst35 * fTemp152))) / fTemp151)));
			fRec65[0] = ((2.0f * fTemp155) - fRec65[1]);
			float 	fRec66 = fTemp155;
			fVec5[0] = fSlow99;
			fRec70[0] = (fSlow103 + (fRec70[1] - float((iSlow101 * ((fSlow103 + fRec70[1]) > fSlow102)))));
			int 	iTemp156 = int(fRec70[0]);
			float 	fTemp157 = ftbl0[iTemp156];
			float 	fTemp158 = (fTemp157 + ((fRec70[0] - float(iTemp156)) * (ftbl0[(iTemp156 + (1 - (iSlow101 * ((iTemp156 + 1) >= iSlow101))))] - fTemp157)));
			fRec69[0] = (fTemp158 - (((fSlow7 * fRec69[2]) + (2.0f * (fSlow6 * fRec69[1]))) / fSlow5));
			fRec71[0] = (fTemp158 - (((fSlow21 * fRec71[2]) + (2.0f * (fSlow20 * fRec71[1]))) / fSlow19));
			fRec72[0] = (fTemp158 - (((fSlow30 * fRec72[2]) + (2.0f * (fSlow29 * fRec72[1]))) / fSlow28));
			fRec73[0] = (fSlow104 - (fConst30 * ((fConst28 * fRec73[2]) + (fConst26 * fRec73[1]))));
			float 	fTemp159 = (((3947.8417604357433f * fRec73[0]) + (7895.6835208714865f * fRec73[1])) + (3947.8417604357433f * fRec73[2]));
			float 	fTemp160 = (fConst31 * fTemp159);
			float 	fTemp161 = max(fTemp160, (float)0);
			float 	fTemp162 = ((fSlow106 * (fTemp159 * fTemp161)) + 200.0f);
			float 	fTemp163 = faustpower<2>(fTemp162);
			float 	fTemp164 = ((fConst34 * (fTemp163 * (3150.0f - (fConst33 * fTemp163)))) + -62370.0f);
			float 	fTemp165 = ((fConst34 * (fTemp163 * ((fConst34 * (fTemp163 * (378.0f - (fConst34 * fTemp163)))) + -17325.0f))) + 135135.0f);
			float 	fTemp166 = (fTemp162 * fTemp165);
			float 	fTemp167 = ((fConst34 * (fTemp163 * fTemp164)) + 135135.0f);
			float 	fTemp168 = ((2.0f * faustpower<2>(fTemp167)) + (fConst37 * (fTemp166 * ((fConst36 * (fTemp162 * ((6.2831853071795862f * fTemp165) + (fConst35 * (fTemp162 * fTemp164))))) + 135135.0f))));
			float 	fTemp169 = (fTemp166 * (fTemp158 - fRec75[1]));
			float 	fTemp170 = (fTemp167 * fRec74[1]);
			float 	fTemp171 = ((fTemp167 * ((2.0f * fTemp170) + (fConst38 * fTemp169))) / fTemp168);
			fRec74[0] = ((2.0f * fTemp171) - fRec74[1]);
			float 	fTemp172 = (fRec75[1] + (fConst36 * ((fTemp166 * ((6.2831853071795862f * fTemp170) + (fConst39 * fTemp169))) / fTemp168)));
			fRec75[0] = ((2.0f * fTemp172) - fRec75[1]);
			float 	fRec76 = fTemp171;
			float 	fTemp173 = max((-1 * fTemp160), (float)0);
			fRec81[0] = (fSlow110 * (max(fRec81[1], fSlow109) * float((fabsf((fSlow99 - fVec5[1])) < 1.0f))));
			fRec80[0] = (fSlow111 + (fRec81[0] - (fConst48 * ((fConst46 * fRec80[2]) + (fConst44 * fRec80[1])))));
			float 	fTemp174 = (((98696.044010893587f * fRec80[0]) + (197392.08802178717f * fRec80[1])) + (98696.044010893587f * fRec80[2]));
			float 	fTemp175 = min((fSlow112 * (faustpower<2>(((fConst50 * (fTemp174 * (1.0f - fTemp173))) + fTemp19)) + 1.0f)), (float)16000);
			float 	fTemp176 = faustpower<2>(fTemp175);
			float 	fTemp177 = ((fConst34 * (fTemp176 * ((fConst34 * (fTemp176 * (378.0f - (fConst34 * fTemp176)))) + -17325.0f))) + 135135.0f);
			float 	fTemp178 = ((8.0f * fTemp173) + 1.0f);
			float 	fTemp179 = ((fConst34 * (fTemp176 * (3150.0f - (fConst33 * fTemp176)))) + -62370.0f);
			float 	fTemp180 = (fTemp175 * fTemp177);
			float 	fTemp181 = ((fConst34 * (fTemp176 * fTemp179)) + 135135.0f);
			float 	fTemp182 = ((fTemp178 * faustpower<2>(fTemp181)) + (fConst37 * (fTemp180 * ((fConst36 * (fTemp175 * ((fConst35 * (fTemp175 * fTemp179)) + (3.1415926535897931f * (fTemp178 * fTemp177))))) + 135135.0f))));
			float 	fTemp183 = (fTemp180 * (fTemp158 - fRec78[1]));
			float 	fTemp184 = (fTemp181 * fRec77[1]);
			float 	fTemp185 = (((fTemp178 * fTemp181) * (fTemp184 + (fConst37 * fTemp183))) / fTemp182);
			fRec77[0] = ((2.0f * fTemp185) - fRec77[1]);
			float 	fTemp186 = (fRec78[1] + (fConst36 * ((((fTemp178 * fTemp175) * fTemp177) * ((3.1415926535897931f * fTemp184) + (fConst35 * fTemp183))) / fTemp182)));
			fRec78[0] = ((2.0f * fTemp186) - fRec78[1]);
			float 	fRec79 = fTemp186;
			fRec0[0] = ((fConst65 * ((((((fTemp174 * (((fRec79 * faustpower<2>((1.0f - (0.5f * fTemp173)))) + (fSlow107 * ((fRec76 * fTemp159) * fTemp161))) + (6.0f * ((fSlow31 * ((fSlow24 * (fRec72[0] - fRec72[2])) / fSlow28)) + (0.125f * ((fSlow22 * ((fSlow15 * (fRec71[0] - fRec71[2])) / fSlow19)) + (fSlow13 * ((fSlow1 * (fRec69[0] - fRec69[2])) / fSlow5)))))))) + (fTemp143 * (((fRec66 * faustpower<2>((1.0f - (0.5f * fTemp142)))) + (fSlow93 * ((fRec63 * fTemp128) * fTemp130))) + (6.0f * ((fSlow31 * ((fSlow24 * (fRec59[0] - fRec59[2])) / fSlow28)) + (0.125f * ((fSlow22 * ((fSlow15 * (fRec58[0] - fRec58[2])) / fSlow19)) + (fSlow13 * ((fSlow1 * (fRec56[0] - fRec56[2])) / fSlow5))))))))) + (fTemp112 * (((fRec53 * faustpower<2>((1.0f - (0.5f * fTemp111)))) + (fSlow79 * ((fRec50 * fTemp97) * fTemp99))) + (6.0f * ((fSlow31 * ((fSlow24 * (fRec46[0] - fRec46[2])) / fSlow28)) + (0.125f * ((fSlow22 * ((fSlow15 * (fRec45[0] - fRec45[2])) / fSlow19)) + (fSlow13 * ((fSlow1 * (fRec43[0] - fRec43[2])) / fSlow5))))))))) + (fTemp81 * (((fRec40 * faustpower<2>((1.0f - (0.5f * fTemp80)))) + (fSlow65 * ((fRec37 * fTemp66) * fTemp68))) + (6.0f * ((fSlow31 * ((fSlow24 * (fRec33[0] - fRec33[2])) / fSlow28)) + (0.125f * ((fSlow22 * ((fSlow15 * (fRec32[0] - fRec32[2])) / fSlow19)) + (fSlow13 * ((fSlow1 * (fRec30[0] - fRec30[2])) / fSlow5))))))))) + (fTemp50 * (((fRec27 * faustpower<2>((1.0f - (0.5f * fTemp49)))) + (fSlow51 * ((fRec24 * fTemp35) * fTemp37))) + (6.0f * ((fSlow31 * ((fSlow24 * (fRec20[0] - fRec20[2])) / fSlow28)) + (0.125f * ((fSlow22 * ((fSlow15 * (fRec19[0] - fRec19[2])) / fSlow19)) + (fSlow13 * ((fSlow1 * (fRec17[0] - fRec17[2])) / fSlow5))))))))) + (fTemp18 * (((fRec11 * faustpower<2>((1.0f - (0.5f * fTemp17)))) + (fSlow35 * ((fRec8 * fTemp3) * fTemp5))) + (6.0f * ((fSlow31 * ((fSlow24 * (fRec4[0] - fRec4[2])) / fSlow28)) + (0.125f * ((fSlow22 * ((fSlow15 * (fRec3[0] - fRec3[2])) / fSlow19)) + (fSlow13 * ((fSlow1 * (fRec1[0] - fRec1[2])) / fSlow5)))))))))) - (fConst8 * ((fConst6 * fRec0[2]) + (fConst4 * fRec0[1]))));
			output0[i] = (FAUSTFLOAT)((fConst66 * fRec0[1]) + (fConst8 * (fRec0[0] + fRec0[2])));
			// post processing
			fRec0[2] = fRec0[1]; fRec0[1] = fRec0[0];
			fRec78[1] = fRec78[0];
			fRec77[1] = fRec77[0];
			fRec80[2] = fRec80[1]; fRec80[1] = fRec80[0];
			fRec81[1] = fRec81[0];
			fRec75[1] = fRec75[0];
			fRec74[1] = fRec74[0];
			fRec73[2] = fRec73[1]; fRec73[1] = fRec73[0];
			fRec72[2] = fRec72[1]; fRec72[1] = fRec72[0];
			fRec71[2] = fRec71[1]; fRec71[1] = fRec71[0];
			fRec69[2] = fRec69[1]; fRec69[1] = fRec69[0];
			fRec70[1] = fRec70[0];
			fVec5[1] = fVec5[0];
			fRec65[1] = fRec65[0];
			fRec64[1] = fRec64[0];
			fRec67[2] = fRec67[1]; fRec67[1] = fRec67[0];
			fRec68[1] = fRec68[0];
			fRec62[1] = fRec62[0];
			fRec61[1] = fRec61[0];
			fRec60[2] = fRec60[1]; fRec60[1] = fRec60[0];
			fRec59[2] = fRec59[1]; fRec59[1] = fRec59[0];
			fRec58[2] = fRec58[1]; fRec58[1] = fRec58[0];
			fRec56[2] = fRec56[1]; fRec56[1] = fRec56[0];
			fRec57[1] = fRec57[0];
			fVec4[1] = fVec4[0];
			fRec52[1] = fRec52[0];
			fRec51[1] = fRec51[0];
			fRec54[2] = fRec54[1]; fRec54[1] = fRec54[0];
			fRec55[1] = fRec55[0];
			fRec49[1] = fRec49[0];
			fRec48[1] = fRec48[0];
			fRec47[2] = fRec47[1]; fRec47[1] = fRec47[0];
			fRec46[2] = fRec46[1]; fRec46[1] = fRec46[0];
			fRec45[2] = fRec45[1]; fRec45[1] = fRec45[0];
			fRec43[2] = fRec43[1]; fRec43[1] = fRec43[0];
			fRec44[1] = fRec44[0];
			fVec3[1] = fVec3[0];
			fRec39[1] = fRec39[0];
			fRec38[1] = fRec38[0];
			fRec41[2] = fRec41[1]; fRec41[1] = fRec41[0];
			fRec42[1] = fRec42[0];
			fRec36[1] = fRec36[0];
			fRec35[1] = fRec35[0];
			fRec34[2] = fRec34[1]; fRec34[1] = fRec34[0];
			fRec33[2] = fRec33[1]; fRec33[1] = fRec33[0];
			fRec32[2] = fRec32[1]; fRec32[1] = fRec32[0];
			fRec30[2] = fRec30[1]; fRec30[1] = fRec30[0];
			fRec31[1] = fRec31[0];
			fVec2[1] = fVec2[0];
			fRec26[1] = fRec26[0];
			fRec25[1] = fRec25[0];
			fRec28[2] = fRec28[1]; fRec28[1] = fRec28[0];
			fRec29[1] = fRec29[0];
			fRec23[1] = fRec23[0];
			fRec22[1] = fRec22[0];
			fRec21[2] = fRec21[1]; fRec21[1] = fRec21[0];
			fRec20[2] = fRec20[1]; fRec20[1] = fRec20[0];
			fRec19[2] = fRec19[1]; fRec19[1] = fRec19[0];
			fRec17[2] = fRec17[1]; fRec17[1] = fRec17[0];
			fRec18[1] = fRec18[0];
			fVec1[1] = fVec1[0];
			fRec10[1] = fRec10[0];
			fRec9[1] = fRec9[0];
			fRec14[1] = fRec14[0];
			fRec15[2] = fRec15[1]; fRec15[1] = fRec15[0];
			fRec16[2] = fRec16[1]; fRec16[1] = fRec16[0];
			fRec12[2] = fRec12[1]; fRec12[1] = fRec12[0];
			fRec13[1] = fRec13[0];
			fRec7[1] = fRec7[0];
			fRec6[1] = fRec6[0];
			fRec5[2] = fRec5[1]; fRec5[1] = fRec5[0];
			fRec4[2] = fRec4[1]; fRec4[1] = fRec4[0];
			fRec3[2] = fRec3[1]; fRec3[1] = fRec3[0];
			fRec1[2] = fRec1[1]; fRec1[1] = fRec1[0];
			fRec2[1] = fRec2[0];
			fVec0[1] = fVec0[0];
		}
	}
};


float 	mydsp::ftbl0[4096];

FAUSTCLASS dsp;

// 4 KB: the Faust compute() frame alone can approach 1 KB with register spills (upstream used 1024)
static THD_WORKING_AREA(waSynthThread, 4096);
static thread_t* pThreadDSP = 0;
static void synthThread(void *arg) {  // THE SYNTH THREAD
	(void)arg;
	chRegSetThreadName("SYNTH");

	int32_t tmp;
	int count = CHANNEL_BUFFER_SIZE;
	float volume_filtered = 0.0f;

	//codec_pwrCtl(1);    // POWER ON
	//codec_muteCtl(0);   // MUTE OFF

	chEvtAddEvents(1);

	// initialize interface with dump, for when controls are not used
	synth_interface.acc_abs = &dump;
	synth_interface.acc_x = &dump;
	synth_interface.acc_y = &dump;
	synth_interface.acc_z = &dump;
	synth_interface.rot_x = &dump;
	synth_interface.rot_y = &dump;
	synth_interface.rot_z = &dump;
	synth_interface.pedal = &dump;

	// initialization
	dsp.init(SAMPLINGFREQ);

	// initialize interface
	dsp.buildUserInterfaceEmbedded();

	// computation loop
	while (true) {
		// palClearLine(LINE_LED_ALT);
		chEvtWaitOne(1);
		// palSetLine(LINE_LED_ALT);
		rtcnt_t t_start = chSysGetRealtimeCounterX();

		dsp.compute(count, NULL, output);

		volume_filtered = VOLUME_FILTER * volume_filtered + (1 - VOLUME_FILTER) * volume;

		// USB audio: the same samples, NULL if no host is streaming
		int32_t* usb = usb_audio_write_ptr();

		// convert float to int with scale, clamp and round
		for (int n = 0; n < CHANNEL_BUFFER_SIZE; n++) {
			tmp = (int32_t)(output0[n] * volume_filtered * MAX_VAL);
			// enable LED on clip
			if (tmp <= -MAX_VAL) {
				tmp = -(MAX_VAL-1);
			} else if (tmp >= MAX_VAL) {
				tmp = MAX_VAL-1;
			}
			// make both audio channels the same
			outbuf[2*n] = outbuf[2*n+1] = tmp * (1<<8);
			if (usb) usb[2*n] = usb[2*n+1] = tmp * (1<<8);
		}
		if (usb) usb_audio_write_commit();

		rtcnt_t t_block = chSysGetRealtimeCounterX() - t_start;
		chSysLock();
		synth_load.sum += t_block;
		if (t_block > synth_load.peak) synth_load.peak = t_block;
		synth_load.blocks++;
		chSysUnlock();

		// Overload protection: the synth thread outranks buttons, USB console and LEDs,
		// so a too heavy synth would starve them. Yield a tick to keep the board responsive.
		if (t_block > SYNTH_BLOCK_CYCLES / 10 * 9) {
			chThdSleep(1);
		}

		//if (--n <= 0) {
		//	palToggleLine(LINE_LED1);       /* Orange.  */
		//	n = 100;
		//}

		// TODO: synth_tick better called here or in main?
		// synth_tick();

		if (chThdShouldTerminateX()) break;
	}

	//codec_muteCtl(1);
	//codec_pwrCtl(0);

	pThreadDSP=NULL;
	palToggleLine(LINE_LED1);
};

void start_synth_thread(void) {
	pThreadDSP = chThdCreateStatic(waSynthThread, sizeof(waSynthThread), NORMALPRIO+2, synthThread, NULL);
}

void computebufI(int32_t *inp, int32_t *outp) {
  int i;
  //for (i = 0; i < 32; i++) {
  //  inbuf[i] = inp[i];
  //}
  outbuf = outp;
  inbuf = inp;
  if (pThreadDSP) {
    chSysLockFromISR();
    chEvtSignalI(pThreadDSP, (eventmask_t)1);
    chSysUnlockFromISR();
  }
  else
    for (i = 0; i < PLAYBACK_BUFFER_SIZE; i++) {
      outp[i] = (i - PLAYBACK_BUFFER_SIZE / 2) * 1<<22;
      // square wave 34952 * (1<<8) * 2 * ((i>PLAYBACK_BUFFER_SIZE/2)-0.5);
      // saw ware (i - PLAYBACK_BUFFER_SIZE / 2) * 100000; // testing noise 0;
    }
}

void synth_profile_print(void *chp) {
	(void)chp;
}

int synth_num_voices(void) {
	return 1;
}

const char* synth_voice_name(int n, const char** preset) {
	if (preset) *preset = 0;
	return n == 0 ? "faust" : 0;
}

int synth_current_voice(void) {
	return 0;
}

int synth_select_voice(int n) {
	return n == 0;
}

void synth_params_print(void *chp, int target) {
	chprintf((BaseSequentialStream *)chp, "params %d faust 0\r\n", target);
}

int synth_param_set(int target, int index, float value) {
	(void)target; (void)index; (void)value;
	return 0;
}

void synth_heroes_print(void *chp) {
	chprintf((BaseSequentialStream *)chp, "H 0 |\r\n");
}

uint32_t synth_params_signature(void) {
	return 0;
}
