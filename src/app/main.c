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

#include "ch.h"
#include "hal.h"
#include "chprintf.h"

#define CONFIG_HERE
#include "config.h"
#undef CONFIG_HERE
#include "striso.h"
#include "usbcfg.h"
// #include "exceptions.h"
#include "pconnection.h"
#include "synth.h"
#include "button_read.h"
#include "messaging.h"
#include "motionsensor.h"
#include "ws2812.h"
#include "version.h"
#include "midi_serial.h"
#include "led.h"
#include "config_editor/config_editor.h"
#include "aux_jack.h"

/**
 *  Firmware version description on fixed flash address for bootloader
 */
#define FWVERSION_PREFIX "Firmware version: striso_control_"
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
                (uint32_t)default_config, sizeof(default_config),
                (uint32_t)flash_config, sizeof(default_config),
                (uint32_t)confightm_p3, sizeof(confightm_p3)}
};

// force sqrtf to use FPU, the standard one apparently doesn't
float vsqrtf(float op1) {
  float result;
  __ASM volatile ("vsqrt.f32 %0, %1" : "=w" (result) : "w" (op1) );
  return (result);
}

#ifdef USE_UART
static SerialConfig ser_cfg = {
    500000,
    0,
    0,
    0,
};
#endif

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

  // Enable the headphone amp to stop noise at boot with some amps
  palSetLine(LINE_HP_EN);

  /*
   * Activates the serial driver using the driver default configuration.
   */
#ifdef USE_UART
  sdStart(&SD1, &ser_cfg);
#endif

#ifdef USE_WS2812
  ws2812_init();
  ws2812_write_led(0, 15, 31,  0);
  ws2812_write_led(1, 15, 31,  0);
  ws2812_write_led(2, 15, 31,  0);
  ws2812_write_led(3,  0, 15, 15);
#endif
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

#ifdef USE_WS2812
  ws2812_write_led(0,  0,  1,  0);
  ws2812_write_led(1,  0,  0,  0);
  ws2812_write_led(2,  0,  0,  0);
  ws2812_write_led(3,  0, 15, 15);
#endif

  while (1) {
    chThdSleepMilliseconds(2);
    synth_tick();
    led_tick();

#ifdef USE_USB
    PExReceive();
#endif
  }
}
