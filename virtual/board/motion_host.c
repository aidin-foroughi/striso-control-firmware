/**
 * Copyright (C) 2019 Piers Titus van der Torren
 *
 * This file is part of Striso Control.
 *
 * Striso Control is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * Striso Control is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
 * A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * Striso Control. If not, see <http://www.gnu.org/licenses/>.
 */
/* The virtual board's motion sensor, in place of src/drivers/motionsensor.c (I2C: hardware only).
 * The "motion" thread is ThreadAccel's LSM6DSL loop, its body copied verbatim, with the sensor
 * read (lsm6dslReadMotion) replaced by the injected reading (sv_inject_motion): it writes the
 * synth's motion controls and sends the stream's motion messages (ID_ACCEL_STREAM, ~60 a second
 * while the stream is on) as the board does. At power on the board lies flat at rest. */
#include <math.h>

#include "ch.h"
#include "config.h"
#include "hal.h"
#include "messaging.h"
#include "motionsensor.h"
#include "striso.h"
#include "sv_host.h"
#include "synth.h"

#define pow2(x) ((x) * (x))

static int16_t motion_raw[6] = {0, 0, 0, 0, 0, 16384}; /* gyro x y z, accel x y z: flat, at rest */

void svh_inject_motion(const int16_t raw[6]) {
	for (int i = 0; i < 6; i++) motion_raw[i] = raw[i];
}

static void lsm6dslReadMotion(int16_t* motion) {
	for (int i = 0; i < 6; i++) motion[i] = motion_raw[i];
}

static THD_WORKING_AREA(waThreadAccel, 256);
static void ThreadAccel(void* arg) {
	(void)arg;
	chRegSetThreadName("motion");

	int16_t ax, ay, az, gx, gy, gz;
	int msg[9];
	msg[0] = ID_ACCEL;
	msg[1] = 8; // range?
#define ACC_FS 2

	/* motionsensor.c: --- copied from here (the LSM6DSL reader loop) --- */
  /* Reader thread loop.*/
  int16_t motion[6];
  int stream_count = 0;
  while (TRUE) {
    lsm6dslReadMotion(motion);

    ax = -motion[3]; gx = -motion[0];
    ay = -motion[4]; gy = -motion[1];
    az = -motion[5]; gz = -motion[2];

    float acc_x = ((float)ax)*(ACC_FS/32768.0f);
    float acc_y = ((float)ay)*(ACC_FS/32768.0f);
    float acc_z = ((float)az)*(ACC_FS/32768.0f);
    float acc_abs = sqrtf(pow2(acc_x) + pow2(acc_y) + pow2(acc_z));
    if (acc_abs>0.001) {
      acc_x /= acc_abs;
      acc_y /= acc_abs;
      acc_z /= acc_abs;
    }

    if (config.send_motion_interval) {
      if (config.send_motion_interval != 127) {
        // 127 means only internal synth
        // acceleration in message has maximum range 8g (16g for acc_abs)
        // TODO: make /4 dependent on ACC_FS
        msg[2] = (ax/4)>>2;
        msg[3] = (ay/4)>>2;
        msg[4] = (az/4)>>2;
        msg[5] = ((int16_t)(acc_abs * 32768.0f/8.0f))>>2;
        msg[6] = gx>>2;
        msg[7] = gy>>2;
        msg[8] = gz>>2;
        msgSend(9,msg);
      }
      else if (config.send_usb_bulk && ++stream_count >= 4) {
        // internal synth only, but the USB stream is on (e.g. the Striso Live app): send every
        // 4th reading (~60 per second) as stream-only motion data, no MIDI side effects
        stream_count = 0;
        msg[0] = ID_ACCEL_STREAM;
        msg[2] = (ax/4)>>2;
        msg[3] = (ay/4)>>2;
        msg[4] = (az/4)>>2;
        msg[5] = ((int16_t)(acc_abs * 32768.0f/8.0f))>>2;
        msg[6] = gx>>2;
        msg[7] = gy>>2;
        msg[8] = gz>>2;
        msgSend(9,msg);
        msg[0] = ID_ACCEL;
      }

#ifdef USE_INTERNAL_SYNTH
      float rot_x = ((float)gx)*(1.0f/32768.0f);
      float rot_y = ((float)gy)*(1.0f/32768.0f);
      float rot_z = ((float)gz)*(1.0f/32768.0f);

      *(synth_interface.acc_abs) = acc_abs;
      *(synth_interface.acc_x) = acc_x;
      *(synth_interface.acc_y) = acc_y;
      *(synth_interface.acc_z) = acc_z;
      *(synth_interface.rot_x) = rot_x;
      *(synth_interface.rot_y) = rot_y;
      *(synth_interface.rot_z) = rot_z;
#endif
    }

    /* Waiting until the next 10 milliseconds time interval.*/
    // TODO: wait for interrupt at MPU_READY pin
    chThdSleepMilliseconds(3);
  }
	/* motionsensor.c: --- copied up to here --- */
}

void MotionSensorStart(void) {
	chThdCreateStatic(waThreadAccel, sizeof(waThreadAccel), NORMALPRIO, ThreadAccel, NULL);
}
