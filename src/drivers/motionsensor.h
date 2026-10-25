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
#ifndef _MOTIONSENSOR_H_
#define _MOTIONSENSOR_H_

#define I2CD_MOTION I2CD3
#define LINE_MOTION_I2C_SDA LINE_I2C3_SDA
#define LINE_MOTION_I2C_SCL LINE_I2C3_SCL

#include <stdint.h>

/* the accelerometer's full scale in g (2, 4, 8 or 16): CTRL1_XL, and the scaling of its readings */
#define MOTION_ACC_FS 2

void MotionSensorStart(void);
/* one LSM6DSL reading (gyro x y z, accel x y z, as read), on the motion thread: the motion
 * message and the synth's motion controls (motion_filter.cpp) */
void motion_reading(const int16_t raw[6]);

#endif
