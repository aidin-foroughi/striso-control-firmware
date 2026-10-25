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
 * The virtual board's main: src/app/main.c without the hardware. main.c itself can't be built for
 * the PC (its fwinfo block casts pointers to 32 bits, vsqrtf is Cortex-M7 assembly), so its two
 * loops are here, copied verbatim: the blinker (Thread1) and the main loop (synth_tick, led_tick,
 * PExReceive), with the boot sequence in the same order. The control thread (synth_message and the
 * binary stream) is the firmware's own src/app/control_thread.cpp. tests/test_mirror.py checks the
 * copies against main.c, so a change there fails the virtual board's tests until it is made here
 * too.
 * Not here: ws2812 (USE_WS2812 is off), the UART (USE_UART is off), midi_config (it sends the MPE
 * configuration to USB MIDI: kept, it is MIDI out).
 */
#include "ch.h"
#include "hal.h"
#include "chprintf.h"

#define CONFIG_HERE
#include "config.h"
#undef CONFIG_HERE
#include "striso.h"
#include "usbcfg.h"
#include "pconnection.h"
#include "synth.h"
#include "button_read.h"
#include "messaging.h"
#include "motionsensor.h"
#include "version.h"
#include "led.h"
#include "aux_jack.h"
#include "codec.h"

#include "sv_host.h"

/* main.c: --- copied from here (Thread1) --- */
/*
 * LED flash thread.
 */
static THD_WORKING_AREA(waThread1, 128);
static void Thread1(void *arg) {

  (void)arg;
  chRegSetThreadName("blinker");
  // int msg[8];
  // msg[0] = ID_SYS;
  // msg[1] = ID_SYS_MSGQUE_OVERFLOW_BB;
  while (TRUE) {
    chThdSleepMilliseconds(300);
    palSetLine(LINE_LED1);
    chThdSleepMilliseconds(300);
    // msg[2] = underruns;
    //if (!msgSend(3,msg))
      palClearLine(LINE_LED1);
  }
}

/* main.c: --- copied up to here --- */

/* main.c's main(), without the hardware (halInit, chSysInit, the UART, ws2812) */
void svh_firmware_main(void* arg) {
  (void)arg;
  // Enable the headphone amp to stop noise at boot with some amps
  palSetLine(LINE_HP_EN);

  led_init();
  led_rgb(0x000800);

#ifdef USE_USB
  InitPConnection();
#endif

  /*
   * Creates the control thread (control_thread.cpp: the keys to the synth and the binary stream).
   */
  control_thread_start();

  /*
   * Creates the LED flash thread.
   */
  chThdCreateStatic(waThread1, sizeof(waThread1), NORMALPRIO, Thread1, NULL);

#ifdef USE_INTERNAL_SYNTH
  codec_init(SAMPLERATE);
  start_synth_thread();
#endif

  for (int i=8; i<168; i++) {
    led_rgb3(0,i,0);
    chThdSleepMilliseconds(1);
  }

  aux_jack_init();
  synth_control_init();

#ifdef USE_MIDI_OUT
  // Send initial configuration MIDI
  midi_config();
#endif

  ButtonReadStart();

#if defined(USE_MPU6050) || defined(USE_LSM6DSL)
  MotionSensorStart();
#endif

  /* main.c: --- the main loop, copied --- */
  while (1) {
    chThdSleepMilliseconds(2);
    synth_tick();
    led_tick();

#ifdef USE_USB
    PExReceive();
#endif
  }
}
