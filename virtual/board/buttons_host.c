/* The virtual board's keys, in place of src/drivers/button_read.c (ADC scanning: hardware only).
 *
 * The "read_buttons" thread sends the injected key and control messages with msgSend, where the
 * real scanner does (update_button, the aux buttons), so the firmware's own path runs from there:
 * the message queue, the send thread (synth_message, the binary stream), synth_control's key
 * handling. A release (pres 0) is retried while the queue is full, as update_button does.
 *
 * The calibration is button_read.c's: the boot calibration and StcC's reset are key_calibration.c's
 * (ks_boot_calibration, ks_reset_calibration) into the scanner's ks_cal, the per-key sensitivity
 * (Striso Live's parameter target 3) is key_factor from the config (fCfac<key>), set live with
 * buttonSetKeyFactor. There is no key scanner (key_scanner.cpp): injected messages are what it
 * would send. So StcE prints the board's counters of a scanner that never scanned, StcZ the
 * calibration with every key on at zero pressure, and StcR (raw scan recordings) answers an
 * error. */
#include "button_read.h"

#include "ccportab.h"

#include "ch.h"
#include "chprintf.h"
#include "config.h"
#include "sv_flash.h"
#include "hal.h"
#include "key_scanner.h"
#include "led.h"
#include "messaging.h"
#include "striso.h"
#include "sv_host.h"
#include "usbcfg.h"

#define N_BUTTONS 68
#define INBOX 1024

typedef struct {
	int size;
	int msg[6];
} inmsg_t;

static inmsg_t inbox[INBOX];
static unsigned in_head, in_tail;
static thread_t* tpReadButtons;
scan_cal_t ks_cal[SCAN_BUTTONS];
static int32_t c_force_factory[N_BUTTONS];
static int32_t zero_offset[N_BUTTONS];
static float key_factor[61];

static void wake(void) {
	if (tpReadButtons) {
		thread_t* tp = tpReadButtons;
		tpReadButtons = NULL;
		chSchReadyI(tp);
	}
}

static int push(int size, const int* msg) {
	if (in_head - in_tail >= INBOX) return -1;
	inmsg_t* m = &inbox[in_head % INBOX];
	m->size = size;
	for (int i = 0; i < size; i++) m->msg[i] = msg[i];
	in_head++;
	wake();
	return 0;
}

int32_t svh_inject_key(int32_t src, int32_t id, int32_t pres, int32_t velo, int32_t x, int32_t y) {
	const int msg[6] = {src, id, pres, velo, x, y};
	return push(6, msg);
}

int32_t svh_inject_control(int32_t id, int32_t value) {
	const int msg[3] = {ID_CONTROL, id, value};
	return push(3, msg);
}

static THD_WORKING_AREA(waThreadReadButtons, 256);
static void ThreadReadButtons(void* arg) {
	(void)arg;
	chRegSetThreadName("read_buttons");
	while (TRUE) {
		while (in_tail != in_head) {
			inmsg_t m = inbox[in_tail % INBOX];
			in_tail++;
			if (m.size == 6 && m.msg[2] == 0) {
				while (msgSend(6, m.msg)) { // note off messages are more important so keep trying
					chThdSleep(1);
				}
			} else {
				msgSend(m.size, m.msg);
			}
		}
		chSysLock();
		tpReadButtons = chThdGetSelfX();
		chSchGoSleepS(CH_STATE_SUSPENDED);
		chSysUnlock();
	}
}

int buttonKeyCount(void) {
	return 61;
}

float buttonGetKeyFactor(int n) {
	return (n >= 0 && n < 61) ? key_factor[n] : 0.0f;
}

int buttonSetKeyFactor(int n, float f) {
	if (n < 0 || n >= 61 || !(f >= 0.1f && f <= 10.0f)) return 0;
	int but = button_number_map[n];
	key_factor[n] = f;
	ks_cal[but].c_force = (int32_t)(c_force_factory[but] * f + 0.5f);
	return 1;
}

/* console 'C': button_read.c's, the calibration print and reset */
void buttonSetCalibration(void) {
	uint16_t base_calib_force = devspec_id->base_calib_force;
	if (base_calib_force == 0xffff) base_calib_force = CALIB_FORCE;
	if (base_calib_force == 0x0000) base_calib_force = ((1 << 18) / 64);
	for (int n = 0; n < N_BUTTONS; n++) {
		chprintf((BaseSequentialStream*)&BDU1, "but[%d] c_force: %d zero_offset: %d\r\n", n, ks_cal[n].c_force, zero_offset[n]);
	}
	ks_reset_calibration(ks_cal, base_calib_force);
	chprintf((BaseSequentialStream*)&BDU1, "c_force: %d\r\n", base_calib_force);
}

/* console 'E': button_read.c's lines, for a scanner that never scanned (and its idle recorder) */
void buttonPrintStats(BaseSequentialStream* out) {
	chprintf(out, "scan scans %lu frames %lu seq_gaps %lu overruns %lu lost_wakeups %lu\r\n", 0u, 0u, 0u, 0u, 0u);
	chprintf(out, "scan queue_full %lu off_retries %lu msg_underruns %d\r\n", 0u, 0u, underruns);
	chprintf(out, "scan cycles n %lu min %lu max %lu mean %lu (%lu cycles/us)\r\n", 0u, 0u, 0u, 0u, STM32_SYS_CK / 1000000U);
	chprintf(out, "scan cycles_hist base %d step %d:", SCAN_HIST_BASE, SCAN_HIST_STEP);
	chprintf(out, "\r\nscan callbacks_hist base %d:", SCAN_CB_BASE);
	chprintf(out, "\r\n");
	chprintf(out, "scan capture %s seconds %d frames %lu bytes %lu of %u full %lu\r\n", "idle", 0, 0u, 0u, 0u, 0u);
}

/* console 'Z': button_read.c's lines; every key on (status 2) at pressure 0 */
void buttonPrintKeys(BaseSequentialStream* out) {
	chprintf(out, "keys %d: but key status c_force factory factor_milli zero_offset pres\r\n", N_BUTTONS);
	for (int b = 0; b < N_BUTTONS; b++) {
		char name[4] = "---";
		int factor = 0;
		for (int n = 0; n < 61; n++) {
			if (button_number_map[n] == b) {
				put_button_name(n, name);
				name[3] = 0;
				factor = (int)(key_factor[n] * 1000.0f + 0.5f);
			}
		}
		chprintf(out, "%d %s %d %ld %ld %d %ld %ld\r\n", b, name, 2, ks_cal[b].c_force, c_force_factory[b], factor,
		    zero_offset[b], 0);
	}
}

/* console 'R': no scans to record */
void ks_capture(int seconds) {
	(void)seconds;
	chprintf((BaseSequentialStream*)&BDU1, "R err no key scanner (virtual board)\r\n");
}

/* ButtonReadStart without the ADC: the boot calibration as button_read.c's (key_calibration.c) */
void ButtonReadStart(void) {
	const uint32_t* UID = (const uint32_t*)UID_BASE;
	const bool calibrated =
	    calib_dis_force->UID[0] == UID[0] && calib_dis_force->UID[1] == UID[1] && calib_dis_force->UID[2] == UID[2];
	for (int n = 0; n < N_BUTTONS; n++) ks_cal[n].c_force = CALIB_FORCE; /* ks_init */
	ks_boot_calibration(calibrated, calib_dis_force->type, calib_dis_force->calib, ks_cal, c_force_factory, key_factor,
	    zero_offset);
	if (!calibrated) {
		led_rgb(0xff0000);
		chThdSleepMilliseconds(200);
	}
	chThdCreateStatic(waThreadReadButtons, sizeof(waThreadReadButtons), NORMALPRIO, ThreadReadButtons, NULL);
}
