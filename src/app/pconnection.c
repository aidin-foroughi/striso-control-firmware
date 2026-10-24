/**
 * Copyright (C) 2013, 2014, 2015 Johannes Taelman
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
#include <string.h>

#include "ch.h"
#include "hal.h"
#include "chprintf.h"
#include "usb_audio.h"
#include "pconnection.h"
#include "usbcfg.h"
#include "midi.h"
//#include "string.h"
// #include "exceptions.h"
#include "bulk_usb.h"
#include "midi.h"
#include "synth.h"
#include "button_read.h"
#include "config_store.h"
#include "midi_usb.h"
#include "config.h"
#include "version.h"
#include "ws2812.h"
#include "button_read.h"
#include "key_scanner.h"

//#define DEBUG_SERIAL 1

void BootLoaderInit(void);

#define BOOTLOADER_RTC_SIGNATURE   0x71a21877

/**
 * Set boot signature and reset
 */
void reset_to_uf2_bootloader(void) {
  // Enable writing to backup domain
  // STM32F4:
  // PWR->CR |= PWR_CR_DBP;
  // STM32H7:
  PWR->CR1 |= PWR_CR1_DBP;
  // Set boot signature in RTC backup register
  RTC->BKP0R = BOOTLOADER_RTC_SIGNATURE;

  NVIC_SystemReset();
}

static void cmd_threads(BaseSequentialStream *chp) {
  static const char *states[] = {CH_STATE_NAMES};
  thread_t *tp;

  chprintf(chp, "stklimit    stack     addr   unused refs prio     state         name\r\n");
  tp = chRegFirstThread();
  do {
#if (CH_DBG_ENABLE_STACK_CHECK == TRUE) || (CH_CFG_USE_DYNAMIC == TRUE)
    uint32_t stklimit = (uint32_t)tp->wabase;
#else
    uint32_t stklimit = 0U;
#endif
#if CH_DBG_FILL_THREADS
    char *stk = (char *)(tp->wabase);
    int nfree = 0;
    while(*stk == 0x55){
      nfree++;
      stk++;
    }
#else
    int nfree -1;
#endif
    chprintf(chp, "%08lx %08lx %08lx %8lu %4lu %4lu %9s %12s\r\n",
             stklimit, (uint32_t)tp->ctx.sp, (uint32_t)tp, nfree,
             (uint32_t)tp->refs - 1, (uint32_t)tp->hdr.pqueue.prio, states[tp->state],
             tp->name == NULL ? "" : tp->name);
    tp = chRegNextThread(tp);
  } while (tp != NULL);
}

void InitPConnection(void) {

  // initializes descriptor strings
  init_usb_descriptor();

  /*
   * Initializes a serial-over-USB CDC driver.
   */
  mduObjectInit(&MDU1);
  mduStart(&MDU1, &midiusbcfg);
  bduObjectInit(&BDU1);
  bduStart(&BDU1, &bulkusbcfg);

  /*
   * Activates the USB driver and then the USB bus pull-up on D+.
   * Note, a delay is inserted in order to not have to disconnect the cable
   * after a reset.
   */
  usbDisconnectBus(midiusbcfg.usbp);
  chThdSleepMilliseconds(500);
  usbStart(midiusbcfg.usbp, &usbcfg);
  usbConnectBus(midiusbcfg.usbp);
}

/*
 * String length up to ascii char above 128 or 0, max length len
 */
int strlenmax(const unsigned char* s, int len) {
    const unsigned char *s0 = s;
    while (*s++ && *s < 0xf8 && s <= s0 + len)
        ;
    return (s - s0) - 1;
}

static uint32_t float_bits(float f) {
  union { float f; uint32_t u; } v;
  v.f = f;
  return v.u;
}

// same line format as synth_params_print: "index name group min max value kind", floats as hex bits
static void print_touch_params(BaseSequentialStream *out) {
  chprintf(out, "params 2 touch %d\r\n", synth_touch_count());
  for (int i = 0; i < synth_touch_count(); i++) {
    float lo, hi;
    const char* name = synth_touch_info(i, &lo, &hi);
    chprintf(out, "%d %s touch %08lx %08lx %08lx 0\r\n", i, name, float_bits(lo), float_bits(hi),
             float_bits(synth_touch_get(i)));
  }
}

static void print_key_params(BaseSequentialStream *out) {
  chprintf(out, "params 3 keys %d\r\n", buttonKeyCount());
  for (int n = 0; n < buttonKeyCount(); n++) {
    char name[4];
    put_button_name(n, name);
    name[3] = 0;
    chprintf(out, "%d %s keys %08lx %08lx %08lx 0\r\n", n, name, float_bits(0.1f), float_bits(10.0f),
             float_bits(buttonGetKeyFactor(n)));
  }
}

void PExReceiveByte(unsigned char c) {
  // ws2812_write_led(0, 0,67,0);
  static char header = 0;
  static int state = 0;
  // binary payload of the 'Q' command (set parameter): target, index, float32 little endian;
  // of the 'K' command (press a settings key): the key id; of the 't' command (USB audio ring
  // target): 3 ASCII digits
  static unsigned char payload[6];
  static int payload_len = 0, payload_need = 0;
  static char payload_cmd = 0;
  static systime_t last_byte;

  // A command arrives in one host write: a payload or a "Stc" prefix that stopped arriving (a
  // short command, a killed tool, a USB re-enumeration in between) is dropped after 100 ms, so it
  // can't swallow the next command's bytes.
  if ((payload_need > 0 || state > 0) && chVTTimeElapsedSinceX(last_byte) > TIME_MS2I(100)) {
    payload_need = 0;
    state = 0;
  }
  last_byte = chVTGetSystemTimeX();

  if (payload_need > 0) {
    payload[payload_len++] = c;
    if (payload_len == payload_need && payload_cmd == 'K') {
      payload_need = 0;
      const int ok = synth_settings_key(payload[0]);
      chprintf((BaseSequentialStream *)&BDU1, "K %s %d\r\n", ok ? "ok" : "err", payload[0]);
    }
    else if (payload_len == payload_need && payload_cmd == 'l') {
      payload_need = 0;
      if (payload[0] == '1') {
        if (!usb_audio_marker_request()) chprintf((BaseSequentialStream *)&BDU1, "usb audio marker: busy\r\n");
      } else {
        usb_audio_echo(&BDU1);
      }
    }
    else if (payload_len == payload_need && payload_cmd == 't') {
      payload_need = 0;
      int n = 0, ok = 1;
      for (int i = 0; i < 3; i++) {
        if (payload[i] < '0' || payload[i] > '9') ok = 0;
        n = n * 10 + (payload[i] - '0');
      }
      if (ok && n > 0) usb_audio_set_target(n);
      chprintf((BaseSequentialStream *)&BDU1, "usb audio target %d frames (%d us)%s\r\n",
               usb_audio_target(), usb_audio_target() * 1000 / 48, ok ? "" : ": not 3 digits, unchanged");
    }
    else if (payload_len == payload_need && payload_cmd == 'R') {
      payload_need = 0;
      if (payload[0] >= '0' && payload[0] <= '9' && payload[1] >= '0' && payload[1] <= '9') {
        ks_capture((payload[0] - '0') * 10 + (payload[1] - '0'));
      } else {
        chprintf((BaseSequentialStream *)&BDU1, "R err not 2 digits\r\n");
      }
    }
    else if (payload_len == payload_need) {
      payload_need = 0;
      union { uint32_t u; float f; } v;
      v.u = (uint32_t)payload[2] | ((uint32_t)payload[3] << 8) | ((uint32_t)payload[4] << 16) | ((uint32_t)payload[5] << 24);
      int ok = 0;
      if (payload[0] == 2) {
        ok = synth_touch_set(payload[1], v.f);
      } else if (payload[0] == 3) {
        ok = buttonSetKeyFactor(payload[1], v.f);
      }
#ifdef USE_INTERNAL_SYNTH
      else {
        ok = synth_param_set(payload[0], payload[1], v.f);
      }
#endif
      if (!ok) {
        chprintf((BaseSequentialStream *)&BDU1, "Q err %d %d\r\n", payload[0], payload[1]);
      }
    }
    return;
  }

  if (!header) {
    switch (state) {
    case 0:
      if (c == 'S')
        state++;
      break;
    case 1:
      if (c == 't')
        state++;
      else
        state = c == 'S'; // "SStc": an 'S' starts the prefix again
      break;
    case 2:
      if (c == 'c')
        state++;
      else
        state = c == 'S';
      break;
    case 3:
      state = 0;
      if (c == 'D') { // go to DFU mode
        // ws2812_write_led(0, 64, 8, 0);
        // ws2812_write_led(1, 64, 8, 0);
        // ws2812_write_led(2, 64, 8, 0);
        chprintf((BaseSequentialStream * )&BDU1, "Resetting to DFU mode...\r\n");
        chThdSleepMilliseconds(2);
        // exception_initiate_dfu();
      }
      else if (c == 'B') { // go to bootloader
        // ws2812_write_led(0, 64, 8, 0);
        // ws2812_write_led(1, 64, 8, 0);
        // ws2812_write_led(2, 64, 8, 0);
        chprintf((BaseSequentialStream * )&BDU1, "Resetting to UF2 bootloader mode...\r\n");
        chThdSleepMilliseconds(2);
        reset_to_uf2_bootloader();
      }
      else if (c == 'S') { // enable binary protocol over USB Bulk
        config.send_usb_bulk = 1;
      }
      else if (c == 's') { // disable binary protocol over USB Bulk
        config.send_usb_bulk = 0;
        config.send_midi_monitor = 0;
        chprintf((BaseSequentialStream * )&BDU1, "Stcs\r\n");
      }
      else if (c == 'M') { // enable MIDI monitor over binary protocol
        config.send_usb_bulk = 1;
        config.send_midi_monitor = 1;
      }
      else if (c == 'm') { // disable MIDI monitor over binary protocol
        config.send_midi_monitor = 0;
      }
      else if (c == 'V') { // firmware version
        config.send_usb_bulk = 0;
        // drop the stream data not sent yet; a packet the host hasn't fetched stays (a reset under
        // it halted the board, D1)
        chSysLock();
        bduDiscardOutputI(&BDU1);
        chSysUnlock();
        chprintf((BaseSequentialStream *)&BDU1, FWVERSION " %08X%08X%08X ",
                 ((uint32_t*)UID_BASE)[2], ((uint32_t*)UID_BASE)[1], ((uint32_t*)UID_BASE)[0]);
        int n = strlenmax(devspec_id->id, sizeof(devspec_id->id) + sizeof(devspec_id->model));
        if (n) streamWrite((BaseSequentialStream *)&BDU1, devspec_id->id, n);
        chprintf((BaseSequentialStream *)&BDU1, "\r\n");
        n = strlenmax(calib_dis_force->date, sizeof(calib_dis_force->date));
        if (n) {
          chprintf((BaseSequentialStream *)&BDU1, "Calibration date: ");
          streamWrite((BaseSequentialStream *)&BDU1, calib_dis_force->date, n);
          chprintf((BaseSequentialStream *)&BDU1, "\r\n");
        }
        if (config.debug) {
          chprintf((BaseSequentialStream *)&BDU1, "Debug: enabled\r\n");
        }
      }
      else if (c == 'I') { // thread info
        cmd_threads((BaseSequentialStream *)&BDU1);
      }
      else if (c == 'N') { // the active tuning and every key's pitch (read only, src/synth/tuning.h)
        synth_tuning_print(&BDU1);
      }
      else if (c == 'K') { // press a settings key: 1 payload byte follows, the key id (whitelist:
        // synth_settings_key in synth_control.cpp); replies "K ok <id>" or "K err <id>"
        payload_cmd = 'K';
        payload_len = 0;
        payload_need = 1;
      }
#ifdef USE_INTERNAL_SYNTH
      else if (c == 'P') { // synth engine profile per section since last read
        synth_profile_print(&BDU1);
      }
      else if (c == 'p') { // list the current voice's live parameters
        synth_params_print(&BDU1, 0);
      }
      else if (c == 'f') { // list the master effects' live parameters
        synth_params_print(&BDU1, 1);
      }
      else if (c == 'g') { // list the global touch parameters (target 2)
        print_touch_params((BaseSequentialStream *)&BDU1);
      }
      else if (c == 'k') { // list the per-key sensitivity factors (target 3)
        print_key_params((BaseSequentialStream *)&BDU1);
      }
      else if (c == 'Q') { // set parameter: 6 payload bytes follow (target, index, float32 LE)
        payload_cmd = 'Q';
        payload_len = 0;
        payload_need = 6;
      }
      else if (c == 'v') { // list voices, '*' marks the current one
        for (int n = 0; n < synth_num_voices(); n++) {
          const char* preset;
          const char* name = synth_voice_name(n, &preset);
          chprintf((BaseSequentialStream *)&BDU1, "%c %d %s%s%s\r\n", n == synth_current_voice() ? '*' : ' ',
                   n, name, preset ? ":" : "", preset ? preset : "");
        }
      }
      else if (c >= 0x80) { // select voice c - 0x80
        if (synth_select_voice(c - 0x80)) {
          const char* preset;
          const char* name = synth_voice_name(c - 0x80, &preset);
          chprintf((BaseSequentialStream *)&BDU1, "voice %d %s%s%s\r\n", c - 0x80, name, preset ? ":" : "", preset ? preset : "");
        } else {
          chprintf((BaseSequentialStream *)&BDU1, "no voice %d\r\n", c - 0x80);
        }
      }
      else if (c == 'L') { // synth DSP load since last read, in CPU cycles per block
        chSysLock();
        uint64_t sum = synth_load.sum;
        uint32_t peak = synth_load.peak;
        uint32_t blocks = synth_load.blocks;
        synth_load.sum = 0;
        synth_load.peak = 0;
        synth_load.blocks = 0;
        chSysUnlock();
        chprintf((BaseSequentialStream *)&BDU1, "synth blocks %lu avg %lu peak %lu budget %lu cycles\r\n",
                 blocks, blocks ? (uint32_t)(sum / blocks) : 0, peak, SYNTH_BLOCK_CYCLES);
      }
#endif
      else if (c == 'U') { // USB audio status and statistics since the last 'U'
        usb_audio_print_status(&BDU1);
      }
      else if (c == 't') { // USB audio ring target: 3 ASCII digits follow, frames (49..256; 000:
        // just report it), e.g. Stct072; replies "usb audio target <n> frames (<us> us)"
        payload_cmd = 't';
        payload_len = 0;
        payload_need = 3;
      }
      else if (c == 'l') { // USB audio latency: 1 payload byte follows. '1': a marker in the next
        // packet (usb_audio.h, usb_audio_marker_request; the reply comes once it was sent), '0':
        // an echo ("usb audio echo: held <us> us") for the console's round trip
        payload_cmd = 'l';
        payload_len = 0;
        payload_need = 1;
      }
      else if (c == 'u') { // USB audio source: toggle the test tone (the jack keeps the synth)
        usb_audio_set_tone(!usb_audio_tone());
        chprintf((BaseSequentialStream *)&BDU1, "usb audio source: %s\r\n",
                 usb_audio_tone() ? "test tone (L 1 kHz, R 500 Hz, -6 dBFS)" : "synth");
      }
      else if (c == 'C') { // Calibration mode
        buttonSetCalibration();
      }
      else if (c == 'E') { // key scanner counters since boot (read only): scan sequence gaps,
        // overruns, lost wakeups, queue full, cycles and ADC callbacks per scan
        buttonPrintStats((BaseSequentialStream *)&BDU1);
      }
      else if (c == 'Z') { // every button's zero_offset, c_force and factor (read only)
        buttonPrintKeys((BaseSequentialStream *)&BDU1);
      }
      else if (c == 'R') { // raw key scan recording: 2 ASCII digits follow, seconds to record
        // (StcR05), or 00: send the last recording (tools/record_raw.py; key_scanner.h)
        payload_cmd = 'R';
        payload_len = 0;
        payload_need = 2;
      }
      break;
    }
  }
}

// One USB-MIDI event packet (USB MIDI 1.0, 4): dispatch by its code index number (CIN), 7-bit
// data only. Dropped: reserved CINs (0, 1), SysEx (4-7, not handled here), a channel message
// whose status doesn't match its CIN, a single byte (CIN 0xF) that isn't real-time, a status
// without bit 7, a data byte with bit 7 (e.g. CC7 = 255 would set the volume to 4x).
static void usb_midi_event(const uint8_t *e) {
  static const uint8_t len[16] = {0, 0, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1};
  const uint8_t cin = e[0] & 0x0F;
  if (cin < 2 || (cin >= 4 && cin <= 7)) return;
  if (!(e[1] & 0x80)) return;
  if (cin >= 8 && cin <= 0x0E && (e[1] >> 4) != cin) return;
  if (cin == 0x0F && e[1] < 0xF8) return;
  for (int i = 2; i <= len[cin]; i++) {
    if (e[i] & 0x80) return;
  }
  MidiInMsgHandler(MIDI_DEVICE_USB_DEVICE, ((e[0] & 0xF0) >> 4) + 1, e[1],
                   len[cin] >= 2 ? e[2] : 0, len[cin] >= 3 ? e[3] : 0);
}

void PExReceive(void) {
  // get USB bulk bytes from host
  unsigned char received;
  while (chnReadTimeout(&BDU1, &received, 1, TIME_IMMEDIATE)) {
    PExReceiveByte(received);
  }
  usb_audio_marker_poll(&BDU1); // the reply to a latency marker, once it went out

  // get USB MIDI events from host, a whole transfer at a time: events are 4 bytes and never span
  // transfers, so the tail of a transfer that isn't a multiple of 4 is dropped instead of
  // shifting every later event
  for (;;) {
    uint8_t ev[MIDI_USB_BUFFERS_SIZE];
    size_t n;
    chSysLock();
    if (ibqGetFullBufferTimeoutS(&MDU1.ibqueue, TIME_IMMEDIATE) != MSG_OK) {
      chSysUnlock();
      break;
    }
    n = (size_t)(MDU1.ibqueue.top - MDU1.ibqueue.ptr);
    memcpy(ev, MDU1.ibqueue.ptr, n);
    ibqReleaseEmptyBufferS(&MDU1.ibqueue);
    chSysUnlock();
    for (size_t i = 0; i + 4 <= n; i += 4) {
      usb_midi_event(&ev[i]);
    }
  }
}

/*
 void USBDMidiPoll(void) {
 uint8_t r[4];
 while (chnReadTimeout(&MDU1, &r, 4, TIME_IMMEDIATE)) {
 MidiInMsgHandler(MIDI_DEVICE_USB_DEVICE, (( r[0] & 0xF0) >> 4)+ 1, r[1], r[2], r[3]);
 }
 }
 */
