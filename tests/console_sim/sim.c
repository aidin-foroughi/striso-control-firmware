/* Host simulation runtime for tests/console_sim (from the app-core review's harness): a fake
 * OSAL, a fake USB device controller (the OTG driver's transfer contract), a scripted host, and
 * stand-ins for the synth, button and USB audio functions pconnection.c calls, with the real
 * ones' range checks and return values. The firmware code under test (pconnection.c,
 * bulk_usb.c, midi_usb.c, ChibiOS hal_buffers.c and chprintf.c) is compiled as is. */
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

#include "ch.h"
#include "hal.h"
#include "usbcfg.h"
#include "config.h"
#include "sim.h"

int sim_lock_depth;
systime_t sim_time;
sim_dwt_t sim_dwt;
sim_pwr_t sim_pwr;
sim_rtc_t sim_rtc;
uint32_t sim_uid[3] = {0x11111111, 0x22222222, 0x33333333};
USBDriver USBD1;
MidiUSBDriver MDU1;
BulkUSBDriver BDU1;
static sim_otg_t otg;

/* ---- event log ---- */
char sim_log[1 << 16];
size_t sim_log_len;
void sim_logf(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(sim_log + sim_log_len, sizeof sim_log - sim_log_len, fmt, ap);
  va_end(ap);
  if (n > 0) sim_log_len += (size_t)n;
  if (sim_log_len >= sizeof sim_log - 256) sim_log_len = 0; /* fuzzing: keep it bounded */
}
void sim_log_clear(void) { sim_log_len = 0; sim_log[0] = 0; }

/* ---- halt (chSysHalt) ---- */
int sim_halted;
const char *sim_halt_reason;
jmp_buf *sim_halt_jmp;
void sim_halt(const char *reason, const char *func) {
  sim_halted++;
  sim_halt_reason = reason;
  sim_logf("HALT(%s in %s)\n", reason, func);
  if (sim_halt_jmp) longjmp(*sim_halt_jmp, 1);
  fprintf(stderr, "chSysHalt: %s in %s (no handler)\n", reason, func);
  abort();
}

/* ---- threads: blocking waits run the scripted other side ---- */
int (*sim_block_hook)(threads_queue_t *tqp);
int sim_deadlocks;
int sim_sleeps;
msg_t osalThreadEnqueueTimeoutS(threads_queue_t *tqp, sysinterval_t timeout) {
  if (timeout == TIME_IMMEDIATE) return MSG_TIMEOUT;
  tqp->waiters++;
  tqp->woken = 0;
  for (int k = 0; k < 64 && !tqp->woken; k++) {
    if (!sim_block_hook || !sim_block_hook(tqp)) break;
  }
  if (tqp->woken) return tqp->wake_msg;
  tqp->waiters--;
  sim_deadlocks++;
  sim_logf("BLOCKED-FOREVER(timeout=%s)\n", timeout == TIME_INFINITE ? "TIME_INFINITE" : "finite");
  return MSG_TIMEOUT;
}
void osalThreadDequeueNextI(threads_queue_t *tqp, msg_t msg) {
  if (tqp->waiters > 0) { tqp->waiters--; tqp->woken = 1; tqp->wake_msg = msg; }
}
void osalThreadDequeueAllI(threads_queue_t *tqp, msg_t msg) {
  while (tqp->waiters > 0) { tqp->waiters--; tqp->woken = 1; tqp->wake_msg = msg; }
}
void sim_sleep_ticks(sysinterval_t t) { sim_sleeps += (int)t; }
void chRegSetThreadName(const char *name) { (void)name; }

static uint8_t stk_a[256], stk_b[256];
static thread_t th_b = {stk_b, {stk_b + 200}, 1, {{128}}, 8, "send messages", NULL};
static thread_t th_a = {stk_a, {stk_a + 100}, 1, {{128}}, 1, "main", &th_b};
thread_t *chRegFirstThread(void) {
  memset(stk_a, 0x55, 64); memset(stk_b, 0x55, 128);
  return &th_a;
}
thread_t *chRegNextThread(thread_t *tp) { return tp->next; }

int sim_resets;
jmp_buf *sim_reset_jmp;
void NVIC_SystemReset(void) {
  sim_resets++;
  sim_logf("RESET(bkp0r=%08x)\n", (unsigned)sim_rtc.BKP0R);
  if (sim_reset_jmp) longjmp(*sim_reset_jmp, 1);
}

/* ---- fake USB device controller ---- */
static USBInEndpointState ep1in, ep2in;
static USBOutEndpointState ep1out, ep2out;
static const USBEndpointConfig ep1cfg = {USB_EP_MODE_TYPE_BULK, NULL, mduDataTransmitted, mduDataReceived, 64, 64, &ep1in, &ep1out, 1, NULL};
static const USBEndpointConfig ep2cfg = {USB_EP_MODE_TYPE_BULK, NULL, bduDataTransmitted, bduDataReceived, 64, 64, &ep2in, &ep2out, 1, NULL};
const USBConfig usbcfg;
const MidiUSBConfig midiusbcfg = {&USBD1, USBD1_DATA_REQUEST_EP, USBD1_DATA_AVAILABLE_EP};
const BulkUSBConfig bulkusbcfg = {&USBD1, USBD2_DATA_REQUEST_EP, USBD2_DATA_AVAILABLE_EP};
void init_usb_descriptor(void) {}
void usbDisconnectBus(USBDriver *usbp) { (void)usbp; }
void usbConnectBus(USBDriver *usbp) { (void)usbp; }
void usbStart(USBDriver *usbp, const USBConfig *cfg) { (void)cfg; usbp->state = USB_READY; usbp->otg = &otg; }

void usbStartTransmitI(USBDriver *usbp, usbep_t ep, const uint8_t *buf, size_t n) {
  osalDbgAssert(!usbGetTransmitStatusI(usbp, ep), "sim: transmit while busy");
  usbp->transmitting |= (uint16_t)(1U << ep);
  usbp->txbuf[ep] = buf;
  usbp->txn[ep] = n;
  usbp->epc[ep]->in_state->txsize = n;
}
void usbStartReceiveI(USBDriver *usbp, usbep_t ep, uint8_t *buf, size_t n) {
  osalDbgAssert(!usbGetReceiveStatusI(usbp, ep), "sim: receive while busy");
  usbp->receiving |= (uint16_t)(1U << ep);
  usbp->rxbuf[ep] = buf;
  usbp->rxmax[ep] = n;
  usbp->epc[ep]->out_state->rxsize = n;
  usbp->epc[ep]->out_state->rxcnt = 0;
}

/* the board enumerates: USB_EVENT_CONFIGURED as usbcfg.c handles it */
void sim_usb_configure(void) {
  osalSysLockFromISR();
  USBD1.state = USB_ACTIVE;
  USBD1.epc[1] = &ep1cfg;
  USBD1.epc[2] = &ep2cfg;
  USBD1.transmitting = USBD1.receiving = 0;
  bduConfigureHookI(&BDU1);
  mduConfigureHookI(&MDU1);
  osalSysUnlockFromISR();
}

/* host: what came in on each IN endpoint */
uint8_t host_in[3][1 << 16];
size_t host_in_len[3];
int host_in_transfers[3];
/* the host completes the IN transfer that is pending on ep (one URB); returns its size or -1 */
long host_read(usbep_t ep) {
  if (!usbGetTransmitStatusI(&USBD1, ep)) return -1;
  size_t n = USBD1.txn[ep];
  if (host_in_len[ep] + n < sizeof host_in[ep]) {
    memcpy(host_in[ep] + host_in_len[ep], USBD1.txbuf[ep], n);
    host_in_len[ep] += n;
  } else {
    host_in_len[ep] = 0; /* fuzzing: wrap */
  }
  host_in_transfers[ep]++;
  USBD1.transmitting &= (uint16_t)~(1U << ep);
  USBD1.epc[ep]->in_state->txsize = n;
  USBD1.epc[ep]->in_cb(&USBD1, ep); /* the ISR: bduDataTransmitted / mduDataTransmitted */
  return (long)n;
}
/* the host sends one OUT transfer as 64-byte packets; returns bytes accepted (NAK: stops) */
size_t host_write(usbep_t ep, const uint8_t *data, size_t len) {
  size_t done = 0;
  do {
    if (!usbGetReceiveStatusI(&USBD1, ep)) return done; /* NAK: no buffer armed */
    USBOutEndpointState *os = USBD1.epc[ep]->out_state;
    size_t pkt = len - done > 64 ? 64 : len - done;
    size_t room = USBD1.rxmax[ep] - os->rxcnt;
    if (pkt > room) pkt = room;
    memcpy(USBD1.rxbuf[ep] + os->rxcnt, data + done, pkt);
    os->rxcnt += pkt;
    done += pkt;
    /* OTG: the transfer completes on a short packet or when rxsize bytes arrived */
    if (pkt < 64 || os->rxcnt == USBD1.rxmax[ep]) {
      USBD1.receiving &= (uint16_t)~(1U << ep);
      USBD1.epc[ep]->out_cb(&USBD1, ep); /* the ISR: bduDataReceived / mduDataReceived */
    }
  } while (done < len);
  return done;
}
void sim_sof(void) {
  osalSysLockFromISR();
  bduSOFHookI(&BDU1);
  mduSOFHookI(&MDU1);
  osalSysUnlockFromISR();
}
/* a host that keeps reading: flush partial buffers (SOF) and fetch everything pending */
int sim_host_drain(void) {
  int any = 0;
  for (int k = 0; k < 4096; k++) {
    sim_sof();
    long a = host_read(2), b = host_read(1);
    if (a < 0 && b < 0) break;
    any = 1;
  }
  return any;
}
int sim_hook_reading_host(threads_queue_t *tqp) {
  (void)tqp;
  sim_sof();
  return host_read(2) >= 0 || host_read(1) >= 0;
}

/* ---- the fake device-specific flash (V reads devspec_id / calib_dis_force) ---- */
void sim_map_devspec(int erased) {
  static int mapped;
  if (!mapped) {
    void *want = (void *)(uintptr_t)DEVSPEC_FLASH_START;
    void *p = mmap(want, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p != want) { fprintf(stderr, "cannot map devspec flash\n"); exit(2); }
    mapped = 1;
  }
  uint8_t *f = (uint8_t *)(uintptr_t)DEVSPEC_FLASH_START;
  memset(f, 0xff, 4096);
  if (!erased) {
    memcpy(f + 12, "SN0001", 6);
    f[12 + 6] = 0;
    memcpy(f + 256 + 256 - 16, "2026-01-01", 11);
  }
}

void sim_init(void) {
  sim_map_devspec(0);
  InitPConnection();
  sim_usb_configure();
}

/* ---- stand-ins with the real functions' contracts ---- */
volatile synth_load_t synth_load;
int synth_settings_key(int id) { sim_logf("settings_key(%d)\n", id); return id >= 0 && id < 68; }
int synth_touch_count(void) { return 5; }
static float touch_v[5];
const char *synth_touch_info(int i, float *lo, float *hi) {
  static const char *n[5] = {"presS", "veloS", "tiltS", "bendS", "zero"};
  if (i < 0 || i >= 5) return 0;
  *lo = 0; *hi = 4;
  return n[i];
}
float synth_touch_get(int i) { return (i >= 0 && i < 5) ? touch_v[i] : 0.0f; }
int synth_touch_set(int i, float v) {
  sim_logf("touch_set(%d,%08x)\n", i, (unsigned)(*(uint32_t *)&v));
  if (i < 0 || i >= 5 || !(v == v)) return 0;
  touch_v[i] = v;
  return 1;
}
int buttonKeyCount(void) { return 61; }
float buttonGetKeyFactor(int n) { (void)n; return 1.0f; }
int buttonSetKeyFactor(int n, float f) {
  sim_logf("key_factor(%d)\n", n);
  return n >= 0 && n < 61 && f >= 0.1f && f <= 10.0f;
}
void buttonSetCalibration(void) { sim_logf("CALIBRATION RESET\n"); }
void buttonPrintStats(BaseSequentialStream *out) { chprintf(out, "scanner stats\r\n"); }
void buttonPrintKeys(BaseSequentialStream *out) { chprintf(out, "keys\r\n"); }
void ks_capture(int seconds) { sim_logf("ks_capture(%d)\n", seconds); }
int synth_param_set(int target, int index, float value) {
  uint32_t b; memcpy(&b, &value, 4);
  sim_logf("param_set(%d,%d,%08x)\n", target, index, (unsigned)b);
  return target >= 0 && target <= 1 && index >= 0 && index < 20;
}
void synth_tuning_print(void *chp) { chprintf((BaseSequentialStream *)chp, "tuning 12tet\r\n"); }
void synth_profile_print(void *chp) { (void)chp; }
void synth_params_print(void *chp, int target) {
  for (int i = 0; i < 20; i++)
    chprintf((BaseSequentialStream *)chp, "%d param%02d group %08lx %08lx %08lx %d\r\n", i, i, 0ul, 0x3f800000ul, 0ul, target);
}
static int cur_voice;
int synth_num_voices(void) { return 12; }
const char *synth_voice_name(int n, const char **preset) {
  if (n < 0 || n >= 12) return NULL;
  if (preset) *preset = n & 1 ? "preset" : NULL;
  return "engine";
}
int synth_current_voice(void) { return cur_voice; }
int synth_select_voice(int n) { sim_logf("select_voice(%d)\n", n); if (n < 0 || n >= 12) return 0; cur_voice = n; return 1; }
static int ua_target = 72, ua_tone;
int usb_audio_set_target(int f) { sim_logf("ua_target(%d)\n", f); ua_target = f < 49 ? 49 : f > 256 ? 256 : f; return ua_target; }
int usb_audio_target(void) { return ua_target; }
void usb_audio_set_tone(bool on) { ua_tone = on; }
bool usb_audio_tone(void) { return ua_tone; }
void usb_audio_print_status(void *chp) { chprintf((BaseSequentialStream *)chp, "usb audio status\r\n"); }
/* the latency marker (Stcl): no stream here */
bool usb_audio_marker_request(void) { sim_logf("ua_marker\n"); return true; }
void usb_audio_marker_poll(void *chp) { (void)chp; }
void usb_audio_echo(void *chp) { chprintf((BaseSequentialStream *)chp, "usb audio echo\r\n"); }
int midi_in_calls;
void MidiInMsgHandler(midi_device_t dev, uint8_t port, uint8_t b0, uint8_t b1, uint8_t b2) {
  (void)dev;
  midi_in_calls++;
  sim_logf("midi(port=%u %02x %02x %02x)\n", port, b0, b1, b2);
}
