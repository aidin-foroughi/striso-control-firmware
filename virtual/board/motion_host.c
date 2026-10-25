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
 * The "motion" thread is ThreadAccel's LSM6DSL loop with the sensor read (lsm6dslReadMotion)
 * replaced by the injected reading (sv_inject_motion). The reading goes through the firmware's own
 * motion_filter.cpp: it writes the synth's motion controls and sends the stream's motion messages
 * (ID_ACCEL_STREAM, ~60 a second while the stream is on) as the board does. At power on the board
 * lies flat at rest. */
#include "ch.h"
#include "config.h"
#include "hal.h"
#include "messaging.h"
#include "motionsensor.h"
#include "striso.h"
#include "sv_host.h"
#include "synth.h"

static int16_t motion_raw[6] = {0, 0, 0, 0, 0, 16384}; /* gyro x y z, accel x y z: flat, at rest */

void svh_inject_motion(const int16_t raw[6]) {
	for (int i = 0; i < 6; i++) motion_raw[i] = raw[i];
}

static uint8_t lsm6dslReadMotion(int16_t* motion) {
	for (int i = 0; i < 6; i++) motion[i] = motion_raw[i];
	return MSG_OK; /* the injected reading: the bus never fails */
}

static THD_WORKING_AREA(waThreadAccel, 256);
static void ThreadAccel(void* arg) {
	(void)arg;
	chRegSetThreadName("motion");
	/* motionsensor.c's LSM6DSL reader loop: a reading, through the filter, every 3 ms */
	int16_t motion[6];
	while (TRUE) {
		if (lsm6dslReadMotion(motion) == MSG_OK) {
			motion_reading(motion);
		}
		chThdSleepMilliseconds(3);
	}
}

void MotionSensorStart(void) {
	chThdCreateStatic(waThreadAccel, sizeof(waThreadAccel), NORMALPRIO, ThreadAccel, NULL);
}
