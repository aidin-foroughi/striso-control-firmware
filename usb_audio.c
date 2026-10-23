/**
 * USB audio output: ChibiOS glue for the isochronous IN endpoint (see usb_audio.h and
 * usb_audio_core.h for the design).
 *
 * Transfer scheduling on the STM32 OTG core (ChibiOS OTGv1 LLD): one packet is armed at a time.
 * - The first packet is armed from the SOF hook once the host selects alt 1 or 2.
 * - Every following packet is armed from the endpoint callback, which the LLD invokes both after
 *   a completed transfer (XFRC, during the frame the host fetched it) and after an incomplete
 *   one (IISOIXFR: the host did not fetch it in its frame; the LLD disables the endpoint and
 *   flushes its FIFO). usb_lld_start_in() targets the frame after the current one (even/odd frame
 *   bit), so a packet armed from either callback is ready ~1 ms before the host asks for it.
 * - After an incomplete transfer the same packet is sent again instead of a new one, so no
 *   samples are lost to device side timing; the ring level absorbs the delay.
 * - The SOF hook re-arms if nothing is armed (self healing, e.g. after a suspend).
 * No LLD patch is needed: the in_maxsize of 296 (UA_EP_SIZE) works around the LLD sizing the TX
 * FIFO as in_maxsize / 4 words, rounded down (294 bytes would get a 292 byte FIFO).
 */
#include "ch.h"
#include "hal.h"
#include "chprintf.h"

#include "usb_audio.h"
#include "usb_audio_core.h"

static ua_core_t ua;
static volatile uint8_t ua_alt;      // alt setting of the AudioStreaming interface (0: idle)
static volatile bool ua_stream;      // the synth thread feeds the ring
static volatile bool ua_tone_on;
static uint8_t ua_pkt[UA_EP_SIZE] __attribute__((aligned(4)));
static size_t ua_pkt_len;
static uint8_t ua_freq_buf[4];       // SET_CUR sampling frequency data stage (ignored: 48 kHz only)
static const uint8_t ua_freq_48k[3] = {0x80, 0xBB, 0x00};
static uint8_t ua_alt_reply;

// statistics since the last 'StcU'
static uint32_t st_sent, st_missed, st_sofs, st_isr_max, st_isr_sum, st_isr_n;

static void ua_in_cb(USBDriver *usbp, usbep_t ep);

static USBInEndpointState ep3instate;
static const USBEndpointConfig ep3config = {
  USB_EP_MODE_TYPE_ISOC,
  NULL,
  ua_in_cb,
  NULL,
  UA_EP_SIZE,
  0x0000,
  &ep3instate,
  NULL,
  1,
  NULL
};

// --- synth thread side ------------------------------------------------------------------------

int32_t *usb_audio_write_ptr(void) {
  return ua_stream ? ua_write_ptr(&ua) : NULL;
}

void usb_audio_write_commit(void) {
  ua_write_commit(&ua, ua_tone_on);
}

void usb_audio_set_tone(bool on) {
  ua_tone_on = on;
}

bool usb_audio_tone(void) {
  return ua_tone_on;
}

// --- USB ISR side -----------------------------------------------------------------------------

// arm the next packet (system locked); fresh: build a new one, else resend the last one
static void ua_transmitI(USBDriver *usbp, bool fresh) {
  if (fresh || ua_pkt_len == 0) {
    const uint32_t t0 = DWT->CYCCNT;
    ua_pkt_len = ua_fill(&ua, ua_pkt, ua_alt == UA_ALT_16BIT ? 2 : 3);
    const uint32_t dt = DWT->CYCCNT - t0;
    st_isr_sum += dt;
    st_isr_n++;
    if (dt > st_isr_max) st_isr_max = dt;
  }
  usbStartTransmitI(usbp, UA_EP, ua_pkt, ua_pkt_len);
}

// endpoint callback: transfer completed, or failed (not fetched by the host in its frame)
static void ua_in_cb(USBDriver *usbp, usbep_t ep) {
  // a completed transfer has sent its packet (packet count 0); a failed one was disabled with
  // the packet still pending
  const bool missed = (usbp->otg->ie[ep].DIEPTSIZ & DIEPTSIZ_PKTCNT_MASK) != 0U;
  osalSysLockFromISR();
  if (missed) st_missed++; else st_sent++;
  if (ua_alt != 0U && usbp->state == USB_ACTIVE && !usbGetTransmitStatusI(usbp, ep)) {
    ua_transmitI(usbp, !missed);
  }
  osalSysUnlockFromISR();
}

void usb_audio_sof_hookI(USBDriver *usbp) {
  if (ua_alt == 0U || usbp->state != USB_ACTIVE) return;
  if (!ua.priming) st_sofs++;
  if (!usbGetTransmitStatusI(usbp, UA_EP)) {
    ua_transmitI(usbp, true);
  }
}

void usb_audio_configured_hookI(USBDriver *usbp) {
  ua_alt = 0;
  ua_stream = false;
  ua_pkt_len = 0;
  ua_stats_reset(&ua);
  usbInitEndpointI(usbp, UA_EP, &ep3config);
}

void usb_audio_reset_hookI(void) {
  ua_alt = 0;
  ua_stream = false;
}

void usb_audio_suspend_hookI(void) {
  // the LLD disables all endpoints on suspend; the alt setting stays (USB device state is kept
  // across suspend), the stream restarts at wakeup
  ua_stream = false;
}

void usb_audio_wakeup_hookI(USBDriver *usbp) {
  if (usbp->state != USB_ACTIVE || usbp->epc[UA_EP] == NULL) return;
  // the LLD's suspend handling masked all endpoint interrupts but EP0's
  usbp->otg->DAINTMSK |= DAINTMSK_IEPM(UA_EP);
  if (ua_alt != 0U) {
    ua_start(&ua);
    ua_stream = true;
  }
}

static void ua_set_alt(uint8_t alt) {
  osalSysLockFromISR();
  ua_alt = alt;
  if (alt != 0U) {
    ua_start(&ua);
    ua_pkt_len = 0;
    ua_stream = true;
  } else {
    ua_stream = false;   // an armed packet completes or is flushed, and is not re-armed
  }
  osalSysUnlockFromISR();
}

// Standard SET/GET_INTERFACE for the audio interfaces (3, 4) and the sampling frequency class
// requests to the endpoint. Everything else (including SET_INTERFACE to interfaces 0..2, which
// the firmware never handled) goes to the existing handling.
bool usb_audio_requests_hook(USBDriver *usbp) {
  const uint8_t rtype = usbp->setup[0] & (USB_RTYPE_TYPE_MASK | USB_RTYPE_RECIPIENT_MASK);
  const uint8_t req = usbp->setup[1];
  const uint16_t value = (uint16_t)(usbp->setup[2] | (usbp->setup[3] << 8));
  const uint8_t index = usbp->setup[4];
  const uint16_t length = (uint16_t)(usbp->setup[6] | (usbp->setup[7] << 8));

  if (rtype == (USB_RTYPE_TYPE_STD | USB_RTYPE_RECIPIENT_INTERFACE) &&
      (index == UA_AC_INTERFACE || index == UA_AS_INTERFACE)) {
    if (req == USB_REQ_SET_INTERFACE) {
      if (value > (index == UA_AS_INTERFACE ? UA_ALT_16BIT : 0)) return false;   // stall
      if (index == UA_AS_INTERFACE) ua_set_alt((uint8_t)value);
      usbSetupTransfer(usbp, NULL, 0, NULL);
      return true;
    }
    if (req == USB_REQ_GET_INTERFACE) {
      ua_alt_reply = index == UA_AS_INTERFACE ? ua_alt : 0;
      usbSetupTransfer(usbp, &ua_alt_reply, length < 1 ? length : 1, NULL);
      return true;
    }
    return false;
  }

  if (rtype == (USB_RTYPE_TYPE_CLASS | USB_RTYPE_RECIPIENT_ENDPOINT) &&
      index == (0x80 | UA_EP) && (value >> 8) == 0x01 /* SAMPLING_FREQ_CONTROL */) {
    const size_t n = length < 3 ? length : 3;
    if (req == 0x01) {                               // SET_CUR: accept (only 48 kHz exists)
      usbSetupTransfer(usbp, ua_freq_buf, n, NULL);
      return true;
    }
    if (req >= 0x81 && req <= 0x84) {                // GET_CUR, GET_MIN, GET_MAX, GET_RES
      usbSetupTransfer(usbp, (uint8_t *)ua_freq_48k, n, NULL);
      return true;
    }
  }
  return false;
}

// --- console ----------------------------------------------------------------------------------

// not inlined (LTO would merge its frame into main(), whose stack is 1 KB, for every path)
__attribute__((noinline)) void usb_audio_print_status(void *chp) {
  BaseSequentialStream *out = (BaseSequentialStream *)chp;
  struct { uint32_t packets, frames, underruns, overruns, primes, sizes[3]; int32_t lvl_min, lvl_max; } s;
  uint32_t sent, missed, sofs, isr_max, isr_sum, isr_n, level;
  int32_t lvl_f;
  uint8_t alt;
  bool tone, priming;

  chSysLock();
  s.packets = ua.packets; s.frames = ua.frames; s.underruns = ua.underruns;
  s.overruns = ua.overruns; s.primes = ua.primes;
  s.sizes[0] = ua.sizes[0]; s.sizes[1] = ua.sizes[1]; s.sizes[2] = ua.sizes[2];
  s.lvl_min = ua.lvl_min; s.lvl_max = ua.lvl_max;
  level = ua.w - ua.r;
  lvl_f = ua.lvl_q8 >> 8;
  priming = ua.priming;
  sent = st_sent; missed = st_missed; sofs = st_sofs;
  isr_max = st_isr_max; isr_sum = st_isr_sum; isr_n = st_isr_n;
  alt = ua_alt;
  tone = ua_tone_on;
  ua_stats_reset(&ua);
  st_sent = st_missed = st_sofs = st_isr_max = st_isr_sum = st_isr_n = 0;
  chSysUnlock();

  chprintf(out, "usb audio: %s, source %s\r\n",
           alt == UA_ALT_24BIT ? "streaming 48000 Hz 24 bit (alt 1)" :
           alt == UA_ALT_16BIT ? "streaming 48000 Hz 16 bit (alt 2)" : "idle (alt 0)",
           tone ? "test tone (L 1 kHz, R 500 Hz, -6 dBFS)" : "synth");
  chprintf(out, "since last U: packets sent %lu missed %lu built %lu (47:%lu 48:%lu 49:%lu)\r\n",
           sent, missed, s.packets, s.sizes[0], s.sizes[1], s.sizes[2]);
  if (sofs) {
    const uint64_t mhz = (uint64_t)s.frames * 1000000U / sofs;
    chprintf(out, "rate: %lu frames in %lu ms of host clock = %lu.%03lu Hz\r\n",
             s.frames, sofs, (uint32_t)(mhz / 1000U), (uint32_t)(mhz % 1000U));
  }
  chprintf(out, "ring: level %lu now, %ld filtered, %ld..%ld before packets (target %d of %d)%s\r\n",
           level, lvl_f, s.lvl_max < 0 ? 0 : s.lvl_min, s.lvl_max < 0 ? 0 : s.lvl_max,
           UA_TARGET_FRAMES, UA_RING_FRAMES, priming ? ", priming" : "");
  chprintf(out, "underruns %lu overruns %lu primes %lu; packet build avg %lu max %lu cycles\r\n",
           s.underruns, s.overruns, s.primes, isr_n ? isr_sum / isr_n : 0, isr_max);
}
