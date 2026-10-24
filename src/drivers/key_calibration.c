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
/*
 * The buttons' calibration at boot and for StcC (from ButtonReadStart and buttonSetCalibration in
 * button_read.c), without the hardware, so the PC tests run it (tests/test_key_calibration.cpp).
 */
#include <stdbool.h>
#include <stdint.h>

#include "config.h"
#include "config_store.h"
#include "key_scanner.h"

// the positions without a key: their channels read noise, they never get a c_force
static const int no_key[] = {52, 54, 57, 59, 61, 64, 66};

void ks_boot_calibration(bool calibrated, unsigned type, const uint16_t calib[SCAN_BUTTONS], scan_cal_t cal[SCAN_BUTTONS],
                         int32_t c_force_factory[SCAN_BUTTONS], float key_factor[SCAN_KEYS], int32_t zero_offset[SCAN_BUTTONS]) {
  for (int n = 0; n < SCAN_KEYS; n++) {
    key_factor[n] = 1.0f;
  }
  for (int n = 0; n < SCAN_BUTTONS; n++) {
    zero_offset[n] = 0;
  }

  // the factory calibration, when the board's calibration record is this board's
  if (calibrated) {
    for (int n=0; n<SCAN_BUTTONS; n++) {
#ifdef BREAKPOINT_CALIBRATION
      cal[n].c_force = calib[n];
#else
      if (type == 0x02) { /* devspec_id->hardware_revision == 1 */
        cal[n].c_force = 2 * calib[n]; // * 2 to make up for non-linearity
      } else if (type == 0x00) { /* devspec_id->hardware_revision == 0 */
        // calibrated per sensor, so should be / 3 to get the same sensitivity.
        // However with 10k pullup the higher force measurement is not stable, so make it 1.5 times more sensitive
        cal[n].c_force = 2 * calib[n] / 2;
      }
#endif
    }
  }
  for (unsigned i = 0; i < sizeof no_key / sizeof no_key[0]; i++) {
    cal[no_key[i]].c_force = 0;
  }
  for (int n = 0; n < SCAN_BUTTONS; n++) {
    c_force_factory[n] = cal[n].c_force;
  }

  // the user's key factors and zero offsets, on every board (S11: they used to need the
  // calibration record), with the range buttonSetKeyFactor accepts (it was 0..10 here: 0 made
  // the key dead)
  char key[8] = "fCoffC_";
  float f;
  for (int n = 0; n < SCAN_KEYS; n++) {
    key[2] = 'f';
    key[3] = 'a';
    key[4] = 'c';
    put_button_name(n, &key[5]);
    int but = button_number_map[n];
    f = getConfigFloat(key);
    if (f != 1.0f && f >= 0.1f && f <= 10.0f) {
      cal[but].c_force = (int32_t)(cal[but].c_force * f + 0.5f);
      key_factor[n] = f;
    }
    key[2] = 'o';
    key[3] = 'f';
    key[4] = 'f';
    f = getConfigFloat(key);
    if (f > 0.0f && f <= 1.0f) {
      zero_offset[but] = f * (1 << 24);
    }
  }
}

void ks_reset_calibration(scan_cal_t cal[SCAN_BUTTONS], int32_t base_calib_force) {
  for (int n=0; n<SCAN_BUTTONS; n++) {
    cal[n].c_force = base_calib_force;
    cal[n].c_breakpoint = INT32_MAX;
  }
  // S11: not for the positions without a key
  for (unsigned i = 0; i < sizeof no_key / sizeof no_key[0]; i++) {
    cal[no_key[i]].c_force = 0;
  }
}
