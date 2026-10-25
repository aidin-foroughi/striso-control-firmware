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
 * The board's start (was main.c's, the application is app.cpp): main(), the .fwinfo record the
 * UF2 bootloader reads at a fixed flash address, and config.h's definitions (CONFIG_HERE). C, as
 * main.c was: the record's initializer casts addresses to 32 bits, which C++ can't do in a
 * constant initializer.
 */
#include "ch.h"
#include "hal.h"

#define CONFIG_HERE
#include "config.h"
#undef CONFIG_HERE
#include "version.h"
#include "config_editor/config_editor.h"
#include "app.h"

/**
 *  Firmware version description on fixed flash address for bootloader
 */
#define FWVERSION_PREFIX "Firmware version: striso_control_"
// the line and its NUL fit (R24: C accepts an initializer that fills the array, without the NUL)
_Static_assert(sizeof(FWVERSION_PREFIX FWVERSION "\r\n") <= 512, "fwinfo: the version line is too long");
__attribute__ ((section(".fwinfo"))) __attribute__((used))
const struct {
  char fwversion[512];
  uint32_t confightm[16];
} fwinfo = {
  .fwversion = FWVERSION_PREFIX FWVERSION "\r\n",
  .confightm = {(uint32_t)confightm_p1, sizeof(confightm_p1),
                (uint32_t)devspec_id->id, sizeof(devspec_id->id),
                (uint32_t)fwinfo.fwversion, sizeof(FWVERSION_PREFIX FWVERSION) - 1,
                (uint32_t)confightm_p2, sizeof(confightm_p2),
                (uint32_t)default_config.records, sizeof(default_config),
                (uint32_t)flash_config, sizeof(default_config),
                (uint32_t)confightm_p3, sizeof(confightm_p3)}
};

/*
 * Application entry point.
 */
int main(void) {
  /*
   * System initializations.
   * - HAL initialization, this also initializes the configured device drivers
   *   and performs the board-specific initializations.
   * - Kernel initialization, the main() function becomes a thread and the
   *   RTOS is active.
   */
  halInit();
  chSysInit();

  app_run();
}
