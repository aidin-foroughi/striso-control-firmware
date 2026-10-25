/* The aux jack's detection state machine (src/drivers/aux_jack.c, compiled as is) against a small
 * electrical model of the jack: the detect contact, the pull-ups and pull-downs, a floating tip that
 * holds its charge, a TS (mono) plug that shorts the ring to the sleeve, a TRS plug with the ring
 * on aux power. The model answers the ADC conversions and GPIO reads; adcConvert() ends a run after
 * a fixed number of conversions (longjmp out of the thread's endless loop).
 *
 * D3: in autodetect, a TS sustain pedal (the most common one) must be detected as a switch pedal
 * (the ring-short branch); before the fix short_circuit_count was reset after every pass, so it
 * never was, and the jack stayed in autodetect. */
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "hal.h"
config_t config = {JACK2_MODE_AUTODETECT, JACK2_MODE_DISABLED};
ADCDriver ADCD3;
#include "aux_jack.c"

enum plug { PLUG_NONE,
	PLUG_TS_PEDAL,
	PLUG_TRS_PEDAL };
static enum plug plug;
static int pedal_closed; /* the tip switch to the sleeve */
static int tx_mode = PAL_MODE_INPUT_PULLDOWN, detect = 1, vdd_low = 0;
static int tip_charge = 0; /* a floating tip holds its last level */
static int conversions, max_conversions, detected_at, unplug_at;
static adcsample_t last_s[2];
static int cpu_writes; /* D9: the CPU wrote the ADC's DMA buffer between two conversions */
static jmp_buf done;
static int (*script)(int conv); /* returns pedal_closed for this conversion */

static int tip_level(void) {
	if (plug == PLUG_NONE) return detect ? 16383 : 0; /* the tip touches the detect contact */
	if (pedal_closed) return 0;
	if (tx_mode == PAL_MODE_INPUT_PULLUP) tip_charge = 16383;
	else if (tx_mode == PAL_MODE_INPUT_PULLDOWN) tip_charge = 30;
	return tip_charge; /* analog / input: floating, holds the charge */
}
static int ring_level(void) {
	if (plug == PLUG_TS_PEDAL) return 0;                      /* a mono plug shorts the ring */
	if (plug == PLUG_TRS_PEDAL) return vdd_low ? 16000 : 100; /* aux power on the ring, or pulled down */
	return 16383;
}
void adcConvert(ADCDriver* d, const ADCConversionGroup* g, adcsample_t* s, int depth) {
	(void)d;
	(void)g;
	(void)depth;
	if (detected_at == 0 && config.jack2_mode == JACK2_MODE_PEDAL_SWITCH) detected_at = conversions;
	if (++conversions > max_conversions) longjmp(done, 1);
	if (conversions > 1 && (s[0] != last_s[0] || s[1] != last_s[1])) cpu_writes++;
	if (unplug_at && conversions == unplug_at) plug = PLUG_NONE;
	pedal_closed = script(conversions);
	s[0] = (adcsample_t)tip_level();
	s[1] = (adcsample_t)ring_level();
	last_s[0] = s[0];
	last_s[1] = s[1];
}
void palSetLineMode(ioline_t l, int mode) {
	if (l == LINE_AUX_UART2_TX) tx_mode = mode;
}
void palSetLine(ioline_t l) {
	if (l == LINE_AUX_JACK_DETECT) detect = 1;
	if (l == LINE_AUX_VDD) vdd_low = 0;
}
void palClearLine(ioline_t l) {
	if (l == LINE_AUX_JACK_DETECT) detect = 0;
	if (l == LINE_AUX_VDD) vdd_low = 1;
}
int palReadLine(ioline_t l) { return l == LINE_AUX_UART2_TX ? tip_level() > 8192 : 0; }
void chRegSetThreadName(const char* n) { (void)n; }
void chThdSleepMilliseconds(int ms) { (void)ms; }
void chThdSleepMicroseconds(int us) { (void)us; }
void* chThdCreateStatic(void* wa, size_t s, int p, void (*f)(void*), void* a) {
	(void)wa;
	(void)s;
	(void)p;
	(void)f;
	(void)a;
	return 0;
}
void adcStart(ADCDriver* d, const ADCConfig* c) {
	(void)d;
	(void)c;
}
void codec_linein_enable(void) {}
void codec_linein_disable(void) {}
void serial_midi_enable(void) {}
void serial_midi_disable(void) {}
// the settings: sGjack2 (deprecated) is "auto"
bool config_text_is(const char* key, const char* text) { return memcmp(key, "sGjack2 ", 8) == 0 && strcmp(text, "auto") == 0; }
static int pedal_msgs, last_pedal1_msg = -1;
int msgSend(int size, int* msg) {
	(void)size;
	if (msg[1] == IDC_PEDAL_1) {
		pedal_msgs++;
		last_pedal1_msg = msg[2];
	}
	return 0;
}
void led_rgb3_blink(int r, int g, int b, sysinterval_t t) {
	(void)r;
	(void)g;
	(void)b;
	(void)t;
}

static const char* mode_name(jack2_mode_t m) {
	static const char* n[] = {"DISABLED", "MIDI", "PEDAL_EXPRESSION", "PEDAL_SWITCH", "LINEIN", "AUTODETECT", "ERROR"};
	return n[m];
}
static int open_always(int c) {
	(void)c;
	return 0;
}
static int tap_then_hold(int c) { return c < 200 ? (c / 40) % 2 : 1; }
static int tap_every_40(int c) { return (c / 40) % 2; } /* pressed ~40 conversions (~0.5 s), released ~40 */

static int failures;
/* one run from an empty jack with the setting auto; expect the mode at the end, the detection
   within max_detect conversions (0: no detection expected) and at least min_msgs pedal messages */
static void run(const char* name, enum plug p, int (*sc)(int), int n, jack2_mode_t expect, int max_detect,
    int min_msgs) {
	config.jack2_mode = JACK2_MODE_DISABLED;
	config.jack2_mode_setting = JACK2_MODE_AUTODETECT;
	short_circuit_count = 0;
	conversions = 0;
	max_conversions = n;
	detected_at = 0;
	pedal_msgs = 0;
	last_pedal1_msg = -1;
	cpu_writes = 0;
	plug = p;
	script = sc;
	tx_mode = PAL_MODE_INPUT_PULLDOWN;
	detect = 1;
	aux_power_disable();
	if (!setjmp(done)) AuxJack(NULL);
	bool ok = config.jack2_mode == expect && pedal_msgs >= min_msgs && cpu_writes == 0 &&
	    (!unplug_at || last_pedal1_msg == 0) &&
	    (max_detect == 0 || (detected_at > 0 && detected_at <= max_detect));
	failures += !ok;
	printf("%-4s %-40s mode %-16s (expected %s), detected after %d conversions, pedal-1 messages %d%s\n",
	    ok ? "ok" : "FAIL", name, mode_name(config.jack2_mode), mode_name(expect), detected_at, pedal_msgs,
	    cpu_writes ? ", the CPU wrote the DMA buffer" : "");
}

int main(void) {
	/* ~80 conversions per second; a switch pedal should show (pink) within about half a second */
	run("TS (mono) switch pedal, untouched", PLUG_TS_PEDAL, open_always, 2000, JACK2_MODE_PEDAL_SWITCH, 60, 0);
	run("TS (mono) switch pedal, pressed/released", PLUG_TS_PEDAL, tap_every_40, 2000, JACK2_MODE_PEDAL_SWITCH, 60, 20);
	run("TRS switch pedal, pressed/released", PLUG_TRS_PEDAL, tap_every_40, 2000, JACK2_MODE_PEDAL_SWITCH, 200, 20);
	run("nothing plugged in", PLUG_NONE, open_always, 2000, JACK2_MODE_DISABLED, 0, 0);
	/* D9: unplugged while pressed: a pedal-off message, without writing the ADC's DMA buffer */
	unplug_at = 300;
	run("TRS switch pedal held, then unplugged", PLUG_TRS_PEDAL, tap_then_hold, 400, JACK2_MODE_DISABLED, 200, 1);
	unplug_at = 0;
	printf(failures ? "aux jack: FAILED\n" : "aux jack: all passed\n");
	return failures ? 1 : 0;
}
