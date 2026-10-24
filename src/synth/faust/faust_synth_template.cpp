
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
<<includeIntrinsic>>

// Class
<<includeclass>>

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

uint32_t synth_params_signature(void) {
	return 0;
}
