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
 * The virtual board's main: src/app/start.c without the hardware. start.c itself can't be built for
 * the PC (its .fwinfo record casts addresses to 32 bits), so its two other parts are here: config.h's
 * definitions (CONFIG_HERE), and main() without halInit and chSysInit (the scheduler, hal/sched.cpp,
 * is up already): the firmware's main thread runs the board's own app_run (src/app/app.cpp, compiled
 * as it is: fw_app.cpp), the boot from the thread table and the main loop.
 */
#include "ch.h"
#include "hal.h"

#define CONFIG_HERE
#include "config.h"
#undef CONFIG_HERE
#include "app.h"

#include "sv_host.h"

void svh_firmware_main(void* arg) {
  (void)arg;
  app_run();
}
