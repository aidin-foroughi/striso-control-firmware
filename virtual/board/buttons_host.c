/* The virtual board's keys, in place of src/drivers/button_read.c (ADC scanning: hardware only).
 *
 * The "read_buttons" thread sends the injected key and control messages with msgSend, where the
 * real scanner does (update_button, the aux buttons), so the firmware's own path runs from there:
 * the message queue, the send thread (synth_message, the binary stream), synth_control's key
 * handling. A release (pres 0) is retried while the queue is full, as update_button does.
 *
 * The per-key sensitivity (Striso Live's parameter target 3) is button_read.c's: key_factor from
 * the config (fCfac<key>) at boot as ButtonReadStart reads it, set live with buttonSetKeyFactor.
 * The factor scales the scanner's force calibration, which the virtual board has no use for:
 * injected messages are what the scanner would send after it. */
#include "button_read.h"

#include "ccportab.h"

#include "ch.h"
#include "chprintf.h"
#include "config.h"
#include "sv_flash.h"
#include "hal.h"
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
static int32_t c_force[N_BUTTONS];
static int32_t c_force_factory[N_BUTTONS];
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
	c_force[but] = (int32_t)(c_force_factory[but] * f + 0.5f);
	return 1;
}

/* console 'C': as button_read.c, the calibration print (the virtual board's factory values) */
void buttonSetCalibration(void) {
	uint16_t base_calib_force = devspec_id->base_calib_force;
	if (base_calib_force == 0xffff) base_calib_force = CALIB_FORCE;
	if (base_calib_force == 0x0000) base_calib_force = ((1 << 18) / 64);
	for (int n = 0; n < N_BUTTONS; n++) {
		chprintf((BaseSequentialStream*)&BDU1, "but[%d] c_force: %d zero_offset: %d\r\n", n, c_force[n], 0);
		c_force[n] = base_calib_force;
	}
	chprintf((BaseSequentialStream*)&BDU1, "c_force: %d\r\n", base_calib_force);
}

/* ButtonReadStart without the ADC: the factory calibration (calib_dis_force, type 0x02) and the
 * key factors from the config, as button_read.c reads them */
void ButtonReadStart(void) {
	for (int n = 0; n < N_BUTTONS; n++) c_force[n] = CALIB_FORCE;
	for (int n = 0; n < 61; n++) key_factor[n] = 1.0f;
	const uint32_t* UID = (const uint32_t*)UID_BASE;
	if (calib_dis_force->UID[0] == UID[0] && calib_dis_force->UID[1] == UID[1] && calib_dis_force->UID[2] == UID[2]) {
		for (int n = 0; n < N_BUTTONS; n++)
			if (calib_dis_force->type == 0x02) c_force[n] = 2 * calib_dis_force->calib[n];
		for (int n = 0; n < N_BUTTONS; n++) c_force_factory[n] = c_force[n];
		CC_ALIGN(8) char key[8] = "fCoffC_";
		for (int n = 0; n < 61; n++) {
			key[2] = 'f';
			key[3] = 'a';
			key[4] = 'c';
			put_button_name(n, &key[5]);
			int but = button_number_map[n];
			float f = getConfigFloat(key);
			if (f != 1.0f && f >= 0.0f && f <= 10.0f) {
				c_force[but] = (int32_t)(c_force[but] * f + 0.5f);
				key_factor[n] = f;
			}
		}
	} else {
		for (int n = 0; n < N_BUTTONS; n++) c_force_factory[n] = c_force[n];
		led_rgb(0xff0000);
		chThdSleepMilliseconds(200);
	}
	chThdCreateStatic(waThreadReadButtons, sizeof(waThreadReadButtons), NORMALPRIO, ThreadReadButtons, NULL);
}
