/**
 * USB audio output: ChibiOS glue for the isochronous IN endpoint (see usb_audio.h and
 * usb_audio_core.h for the design).
 *
 * Transfer scheduling on the STM32 OTG core (ChibiOS OTGv1 LLD): one packet is armed at a time,
 * always for the frame after the one it is armed in (usb_lld_start_in sets the even/odd frame
 * bit from the current frame), so a packet armed during frame F is fetched by the host in F+1.
 * - The first packet is armed from the SOF hook once the host selects a streaming alt setting.
 * - Every following one is armed from the endpoint callback, which the LLD invokes both after a
 *   completed transfer (XFRC: the host fetched the packet; this is ~1 ms before the next fetch)
 *   and after an incomplete one (IISOIXFR at the end of the periodic frame interval, EOPF: the
 *   endpoint was still enabled, the LLD disables it and flushes its FIFO).
 * - Incomplete transfers: if the flushed packet was due in this frame (the host did not fetch it,
 *   or it was not ready in time), it is dropped and a fresh one is built for the next frame: the
 *   stream stays in real time and the host's lost frame does not add latency. If it was due in the
 *   next frame (flushed early by the LLD), the same packet is armed again: nothing is lost.
 * - EOPF is moved from 80% to 95% of the frame (DCFG.PFIVL): a full speed host may schedule the
 *   IN transaction anywhere in the first 90% of the frame and a 294 byte packet takes ~0.23 ms on
 *   the bus, so at 80% a transfer the host starts after ~57% of the frame (likely behind a hub,
 *   with other periodic traffic) would be flushed in flight.
 * - The SOF hook re-arms if nothing is armed (self healing, e.g. after a suspend).
 * No LLD patch is needed: the in_maxsize of 296 (UA_EP_SIZE) works around the LLD sizing the TX
 * FIFO as in_maxsize / 4 words, rounded down (294 bytes would get a 292 byte FIFO).
 *
 * Diagnostics ('StcU', usb_audio_print_status): every SOF is timestamped (DWT cycle counter), so
 * the console shows the SOF interrupt's jitter (how late the USB ISR ran), frames whose packet
 * was not in the FIFO at their SOF (device late), misses the host caused (the packet was ready),
 * the phase in the frame at which packets are built (the host's fetch time), the synth's commit
 * delay after its codec DMA interrupt and blocks it lost, and with CH_DBG_STATISTICS the longest
 * kernel lock sections (they hold off every interrupt at priority >= 2, the USB one included).
 */
#include "ch.h"
#include "hal.h"
#include "chprintf.h"

#include "usb_audio.h"
#include "usb_audio_core.h"

#define UA_CYC_US (STM32_SYS_CK / 1000000U) // DWT cycles per microsecond
#define UA_CYC_MS (STM32_SYS_CK / 1000U)
#define UA_SOF_LATE (UA_CYC_MS + 100U * UA_CYC_US) // an SOF interval > 1.1 ms: the ISR ran late
#define UA_BLOCK_CYC ((uint32_t)((uint64_t)STM32_SYS_CK * UA_BLOCK / UA_RATE))

static ua_core_t ua;
static volatile uint8_t ua_alt; // alt setting of the AudioStreaming interface (0: idle)
static volatile bool ua_tone_on;
static bool ua_flowing; // the host fetched a packet since the stream started (for the counters)
static uint8_t ua_pkt[UA_EP_SIZE] __attribute__((aligned(4)));
static size_t ua_pkt_len;
static uint16_t ua_arm_fn;          // frame number the armed packet was armed in (due in the next)
static bool ua_ready_at_sof;        // the packet due in this frame was in the FIFO at its SOF
static uint32_t ua_sof_t;           // DWT at the last SOF hook
static uint16_t ua_sof_fn = 0xFFFF; // its frame number (0xFFFF: none yet)
static uint8_t ua_ctl[16];          // class request data stages
static uint8_t ua_alt_reply;

// codec side: DMA interrupts (one per block) and the last one's time (DWT); synth side: blocks
// committed, the codec block count at the last commit
static volatile uint32_t ua_codec_blocks, ua_codec_t;
static uint32_t ua_commits, ua_last_blocks;
static bool ua_commits_valid;

enum {
  UA_BUILD_XFRC, // after the host fetched the previous packet
  UA_BUILD_MISS, // after an incomplete transfer (the host did not fetch it in its frame)
  UA_BUILD_SOF,  // nothing armed at an SOF (stream start, after a suspend)
};

// statistics since the last 'StcU'
static struct {
  uint32_t sent, missed, missed_host, missed_early, sofs, sof_late, sof_lost, sof_dt_max;
  uint32_t not_ready, builds[3], build_n, build_sum, build_max;
  uint32_t phase_min, phase_max, phase_sum, phase_n; // fresh builds after XFRC: cycles after SOF
  uint32_t isr_sum;                                  // cycles in this file's ISR hooks
  // synth side: commits one block after a DMA interrupt (delay after it), others (the render ran
  // past the next interrupt, or interrupts merged: a block lost)
  uint32_t commit_n, commit_sum, commit_max, off_cadence;
} st;

// The latency marker (console Stcl1, usb_audio_marker_request): asked by the console thread,
// written into the next packet the ISR builds (usb_audio_core.h, ua_mark_packet), reported by the
// console thread (usb_audio_marker_poll) once the host fetched that packet: its frame, the ring
// level before it, and the time from the command's OUT packet to the marker's IN transfer.
enum { UA_MK_IDLE, UA_MK_WANTED, UA_MK_ARMED, UA_MK_SENT, UA_MK_LOST };
static volatile uint8_t ua_mk_state;
static uint32_t ua_mk_seq, ua_mk_rx_cyc, ua_mk_done_cyc;
static uint16_t ua_mk_rx_fn, ua_mk_fn;
static int32_t ua_mk_level, ua_mk_target;
static uint8_t ua_mk_bps;
static systime_t ua_mk_asked;
extern volatile uint32_t bdu_rx_cycles; // bulk_usb.c: when the last console OUT packet arrived
extern volatile uint16_t bdu_rx_frame;

static void ua_stats_clear(void) {
  st = (__typeof__(st)){0};
  st.phase_min = 0xFFFFFFFFU;
}

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
    NULL};

// the current (full speed, 11 bit) frame number: DSTS.FNSOF is bits 21:8 (ChibiOS' mask is 10 bits)
static uint16_t ua_frame(USBDriver *usbp) {
  return (uint16_t)((usbp->otg->DSTS >> 8) & 0x7FFU);
}

// --- codec DMA ISR and synth thread side ------------------------------------------------------

void usb_audio_codec_tickI(void) {
  ua_codec_t = DWT->CYCCNT;
  ua_codec_blocks = ua_codec_blocks + 1;
}

int32_t *usb_audio_write_ptr(void) {
  return ua_write_ptr(&ua); // always: the ring holds the latest output for a fast stream start
}

// The synth renders one block per codec DMA interrupt (its wakeup event, event mask 1 in
// synth_strisy.cpp and the Faust synth). Its lag, codec blocks minus commits, stays constant while
// it keeps up. A render that runs past the next interrupt is one commit off the cadence and is
// caught up at once (the event is already pending); two interrupts before the synth waited again
// merge their events, a block is never rendered and the lag grows for good: ua_write_sync()
// repeats a block for it, as the jack repeats its stale buffer.
void usb_audio_write_commit(void) {
  chSysLock();
  const uint32_t now = DWT->CYCCNT;
  const uint32_t blocks = ua_codec_blocks;
  const uint32_t t_irq = ua_codec_t;
  const bool pending = (chThdGetSelfX()->epending & 1U) != 0U;
  chSysUnlock();
  ua_commits++;
  if (ua_commits_valid) {
    if (blocks - ua_last_blocks == 1U) {
      const uint32_t d = now - t_irq;
      st.commit_n++;
      st.commit_sum += d;
      if (d > st.commit_max) st.commit_max = d;
    } else {
      st.off_cadence++;
    }
  }
  ua_commits_valid = true;
  ua_last_blocks = blocks;
  ua_write_commit(&ua, ua_tone_on);
  ua_write_sync(&ua, (int32_t)(blocks - ua_commits) - (pending ? 1 : 0));
}

void usb_audio_set_tone(bool on) {
  ua_tone_on = on;
}

bool usb_audio_tone(void) {
  return ua_tone_on;
}

int usb_audio_set_target(int frames) {
  chSysLock();
  const int t = (int)ua_set_target(&ua, frames);
  chSysUnlock();
  return t;
}

int usb_audio_target(void) {
  return (int)ua.target;
}

// --- USB ISR side -----------------------------------------------------------------------------

// arm the next packet (system locked): a new one, or (UA_BUILD_MISS with resend) the last again
static void ua_transmitI(USBDriver *usbp, int why, bool resend) {
  if (!resend || ua_pkt_len == 0) {
    const uint32_t t0 = DWT->CYCCNT;
    const int bps = ua_alt == UA_ALT_16BIT ? 2 : 3;
    const uint32_t level = ua.w - ua.r;
    ua_pkt_len = ua_fill(&ua, ua_pkt, bps);
    if (ua_mk_state == UA_MK_WANTED) {
      ua_mark_packet(ua_pkt, (int)(ua_pkt_len / (size_t)(UA_CHANNELS * bps)), bps);
      ua_mk_level = (int32_t)level;
      ua_mk_target = ua.target;
      ua_mk_bps = (uint8_t)bps;
      ua_mk_state = UA_MK_ARMED;
    }
    const uint32_t dt = DWT->CYCCNT - t0;
    st.build_sum += dt;
    st.build_n++;
    if (dt > st.build_max) st.build_max = dt;
    st.builds[why]++;
    if (why == UA_BUILD_XFRC && ua_sof_fn != 0xFFFF) {
      const uint32_t ph = t0 - ua_sof_t;
      st.phase_sum += ph;
      st.phase_n++;
      if (ph < st.phase_min) st.phase_min = ph;
      if (ph > st.phase_max) st.phase_max = ph;
    }
  }
  ua_arm_fn = ua_frame(usbp);
  usbStartTransmitI(usbp, UA_EP, ua_pkt, ua_pkt_len);
}

// endpoint callback: transfer completed, or failed (not fetched by the host in its frame)
static void ua_in_cb(USBDriver *usbp, usbep_t ep) {
  const uint32_t t0 = DWT->CYCCNT;
  // a completed transfer has sent its packet (packet count 0); a failed one was disabled with
  // the packet still pending
  const bool missed = (usbp->otg->ie[ep].DIEPTSIZ & DIEPTSIZ_PKTCNT_MASK) != 0U;
  // the frame the flushed packet was due in (even/odd) against the current one
  const bool due_odd = (usbp->otg->ie[ep].DIEPCTL & DIEPCTL_EONUM) != 0U;
  const bool now_odd = (usbp->otg->DSTS & DSTS_FNSOF_ODD) != 0U;
  const bool early = missed && due_odd != now_odd;
  osalSysLockFromISR();
  if (missed) {
    st.missed++;
    if (early) st.missed_early++;
    else if (ua_ready_at_sof) st.missed_host++; // it was in the FIFO: the host did not ask
  } else {
    st.sent++;
    ua_flowing = true;
  }
  if (ua_mk_state == UA_MK_ARMED) { // the packet that just completed (or failed) carried it
    if (!missed) {
      ua_mk_state = UA_MK_SENT;
      ua_mk_done_cyc = t0;
      ua_mk_fn = ua_frame(usbp);
    } else if (!early) {
      ua_mk_state = UA_MK_LOST; // dropped (an early flush is sent again, marker and all)
    }
  }
  if (ua_alt != 0U && usbp->state == USB_ACTIVE && !usbGetTransmitStatusI(usbp, ep)) {
    ua_transmitI(usbp, missed ? UA_BUILD_MISS : UA_BUILD_XFRC, early);
  }
  osalSysUnlockFromISR();
  st.isr_sum += DWT->CYCCNT - t0;
}

void usb_audio_sof_hookI(USBDriver *usbp) {
  const uint32_t t0 = DWT->CYCCNT;
  const uint16_t fn = ua_frame(usbp);
  if (ua_sof_fn != 0xFFFF) {
    const uint32_t dt = t0 - ua_sof_t;
    if (dt > st.sof_dt_max) st.sof_dt_max = dt;
    if (dt > UA_SOF_LATE) st.sof_late++;
    const uint16_t dfn = (uint16_t)((fn - ua_sof_fn) & 0x7FF);
    if (dfn != 1U) st.sof_lost += dfn == 0U ? 1U : dfn - 1U;
  }
  ua_sof_t = t0;
  ua_sof_fn = fn;
  if (ua_alt == 0U || usbp->state != USB_ACTIVE) return;
  st.sofs++;
  const bool armed = usbGetTransmitStatusI(usbp, UA_EP);
  // the packet due in this frame: armed in the previous frame and completely in the TX FIFO
  const USBInEndpointState *isp = usbp->epc[UA_EP]->in_state;
  ua_ready_at_sof = armed && ua_arm_fn == ((fn - 1U) & 0x7FFU) && isp->txcnt >= isp->txsize;
  if (ua_flowing && !ua_ready_at_sof) st.not_ready++;
  if (!armed) ua_transmitI(usbp, UA_BUILD_SOF, false);
  st.isr_sum += DWT->CYCCNT - t0;
}

void usb_audio_configured_hookI(USBDriver *usbp) {
  ua_alt = 0;
  ua_pkt_len = 0;
  ua_flowing = false;
  if (ua.target == 0) ua_init(&ua);
  ua_stats_reset(&ua);
  // end of periodic frame interrupt (incomplete isochronous IN) at 95% of the frame, not 80%
  usbp->otg->DCFG = (usbp->otg->DCFG & ~DCFG_PFIVL_MASK) | DCFG_PFIVL(3);
  usbInitEndpointI(usbp, UA_EP, &ep3config);
}

void usb_audio_reset_hookI(void) {
  ua_alt = 0;
  ua_flowing = false;
}

void usb_audio_suspend_hookI(void) {
  // the LLD disables all endpoints on suspend; the alt setting stays (USB device state is kept
  // across suspend), the stream restarts at wakeup
  ua_flowing = false;
}

void usb_audio_wakeup_hookI(USBDriver *usbp) {
  if (usbp->state != USB_ACTIVE || usbp->epc[UA_EP] == NULL) return;
  // the LLD's suspend handling masked all endpoint interrupts but EP0's
  usbp->otg->DAINTMSK |= DAINTMSK_IEPM(UA_EP);
  if (ua_alt != 0U) {
    ua_start(&ua);
    ua_pkt_len = 0;
  }
}

static void ua_set_alt(uint8_t alt) {
  osalSysLockFromISR();
  ua_alt = alt;
  ua_flowing = false;
  if (alt != 0U) {
    ua_start(&ua);
    ua_pkt_len = 0;
  } // alt 0: an armed packet completes or is flushed, and is not re-armed
  osalSysUnlockFromISR();
}

#if USB_AUDIO_UAC2
// UAC2 clock source (UA2_CLOCK_ID), one fixed 48 kHz frequency (ADC-2 5.2.5.1)
static const uint8_t ua2_freq_cur[4] = {0x80, 0xBB, 0x00, 0x00};
static const uint8_t ua2_freq_range[14] = {
    0x01, 0x00,             // wNumSubRanges 1
    0x80, 0xBB, 0x00, 0x00, // dMIN 48000
    0x80, 0xBB, 0x00, 0x00, // dMAX 48000
    0x00, 0x00, 0x00, 0x00, // dRES 0 (a discrete frequency)
};
static const uint8_t ua2_valid[1] = {1};
#else
static const uint8_t ua_freq_48k[3] = {0x80, 0xBB, 0x00};
#endif

static bool ua_reply(USBDriver *usbp, const uint8_t *data, size_t size, uint16_t length) {
  usbSetupTransfer(usbp, (uint8_t *)data, length < size ? length : size, NULL);
  return true;
}

// Standard SET/GET_INTERFACE for the audio interfaces (3, 4) and the class requests: UAC2, the
// clock source's sampling frequency (GET CUR/RANGE, SET CUR accepted) and validity (GET CUR) on
// interface 3; UAC1, the sampling frequency on the endpoint. Everything else (including
// SET_INTERFACE to interfaces 0..2, which the firmware never handled) goes to the existing
// handling; unknown class requests stall.
bool usb_audio_requests_hook(USBDriver *usbp) {
  const uint8_t rtype = usbp->setup[0] & (USB_RTYPE_TYPE_MASK | USB_RTYPE_RECIPIENT_MASK);
  const bool in = (usbp->setup[0] & USB_RTYPE_DIR_DEV2HOST) != 0U;
  const uint8_t req = usbp->setup[1];
  const uint16_t value = (uint16_t)(usbp->setup[2] | (usbp->setup[3] << 8));
  const uint8_t index = usbp->setup[4];
  const uint16_t length = (uint16_t)(usbp->setup[6] | (usbp->setup[7] << 8));

  if (rtype == (USB_RTYPE_TYPE_STD | USB_RTYPE_RECIPIENT_INTERFACE) &&
      (index == UA_AC_INTERFACE || index == UA_AS_INTERFACE)) {
    if (req == USB_REQ_SET_INTERFACE) {
      if (value > (index == UA_AS_INTERFACE ? UA_ALT_16BIT : 0)) return false; // stall
      if (index == UA_AS_INTERFACE) ua_set_alt((uint8_t)value);
      usbSetupTransfer(usbp, NULL, 0, NULL);
      return true;
    }
    if (req == USB_REQ_GET_INTERFACE) {
      ua_alt_reply = index == UA_AS_INTERFACE ? ua_alt : 0;
      return ua_reply(usbp, &ua_alt_reply, 1, length);
    }
    return false;
  }

#if USB_AUDIO_UAC2
  if (rtype == (USB_RTYPE_TYPE_CLASS | USB_RTYPE_RECIPIENT_INTERFACE) && index == UA_AC_INTERFACE &&
      usbp->setup[5] == UA2_CLOCK_ID && (value & 0xFF) == 0) {
    const uint8_t cs = (uint8_t)(value >> 8);
    if (cs == 0x01) {                                                                              // CS_SAM_FREQ_CONTROL
      if (in && req == 0x01) return ua_reply(usbp, ua2_freq_cur, sizeof ua2_freq_cur, length);     // CUR
      if (in && req == 0x02) return ua_reply(usbp, ua2_freq_range, sizeof ua2_freq_range, length); // RANGE
      if (!in && req == 0x01 && length == 4) {                                                     // SET CUR: only 48 kHz exists; accept and ignore
        usbSetupTransfer(usbp, ua_ctl, 4, NULL);
        return true;
      }
    }
    if (cs == 0x02 && in && req == 0x01) return ua_reply(usbp, ua2_valid, 1, length); // CS_CLOCK_VALID
    return false;
  }
#else
  if (rtype == (USB_RTYPE_TYPE_CLASS | USB_RTYPE_RECIPIENT_ENDPOINT) && index == (0x80 | UA_EP) &&
      (value >> 8) == 0x01 /* SAMPLING_FREQ_CONTROL */) {
    const size_t n = length < 3 ? length : 3;
    if (req == 0x01) { // SET_CUR: accept (only 48 kHz exists)
      usbSetupTransfer(usbp, ua_ctl, n, NULL);
      return true;
    }
    if (req >= 0x81 && req <= 0x84) // GET_CUR, GET_MIN, GET_MAX, GET_RES
      return ua_reply(usbp, ua_freq_48k, sizeof ua_freq_48k, length);
  }
#endif
  return false;
}

// --- console ----------------------------------------------------------------------------------

static uint32_t ua_us(uint32_t cycles) {
  return (cycles + UA_CYC_US / 2) / UA_CYC_US;
}

bool usb_audio_marker_request(void) {
  chSysLock();
  const bool idle = ua_mk_state == UA_MK_IDLE;
  if (idle) {
    ua_mk_seq++;
    ua_mk_rx_cyc = bdu_rx_cycles;
    ua_mk_rx_fn = bdu_rx_frame;
    ua_mk_asked = chVTGetSystemTimeX();
    ua_mk_state = UA_MK_WANTED;
  }
  chSysUnlock();
  return idle;
}

void usb_audio_marker_poll(void *chp) {
  BaseSequentialStream *out = (BaseSequentialStream *)chp;
  if (ua_mk_state == UA_MK_IDLE) return;
  chSysLock();
  const uint8_t s = ua_mk_state;
  const uint32_t seq = ua_mk_seq, rx = ua_mk_rx_cyc, done = ua_mk_done_cyc;
  const uint16_t rx_fn = ua_mk_rx_fn, fn = ua_mk_fn;
  const int32_t level = ua_mk_level, target = ua_mk_target;
  const int bits = ua_mk_bps * 8;
  const bool stale = chVTTimeElapsedSinceX(ua_mk_asked) > TIME_MS2I(100);
  if (s == UA_MK_SENT || s == UA_MK_LOST || stale) ua_mk_state = UA_MK_IDLE;
  chSysUnlock();
  if (s == UA_MK_SENT) {
    chprintf(out, "usb audio marker %lu: frame %u index 0 level %ld target %ld, bus %lu us after rx (rx frame %u), right %d frames -20 dBFS %d bit\r\n",
        seq, fn, level, target, ua_us(done - rx), rx_fn, UA_MARK_FRAMES, bits);
  } else if (s == UA_MK_LOST) {
    chprintf(out, "usb audio marker %lu: lost (the host did not fetch its packet)\r\n", seq);
  } else if (stale) {
    chprintf(out, "usb audio marker %lu: not streaming\r\n", seq);
  }
}

void usb_audio_echo(void *chp) {
  chSysLock();
  const uint32_t now = DWT->CYCCNT, rx = bdu_rx_cycles, sof = ua_sof_t;
  chSysUnlock();
  // the reply is queued now and goes out when the bulk driver flushes at the next SOF
  const uint32_t in_frame = now - sof;
  const uint32_t to_sof = in_frame < UA_CYC_MS ? UA_CYC_MS - in_frame : 0;
  chprintf((BaseSequentialStream *)chp, "usb audio echo: held %lu us\r\n", ua_us(now - rx + to_sof));
}

// not inlined (LTO would merge its frame into main(), whose stack is 1 KB, for every path); the
// snapshots are static for the same reason (only the console thread calls this)
__attribute__((noinline)) void usb_audio_print_status(void *chp) {
  BaseSequentialStream *out = (BaseSequentialStream *)chp;
  static struct {
    uint32_t packets, frames, underruns, overruns, primes, fast_starts, silent, sizes[3];
    int32_t lvl_min, lvl_max;
  } s;
  static __typeof__(st) t;
  uint32_t level, crit_thd = 0, crit_isr = 0, lost, repeats;
  int32_t lvl_f, target;
  uint8_t alt;
  bool tone, priming;

  chSysLock();
  lost = ua.lost;
  repeats = ua.repeats;
  s.packets = ua.packets;
  s.frames = ua.frames;
  s.underruns = ua.underruns;
  s.overruns = ua.overruns;
  s.primes = ua.primes;
  s.fast_starts = ua.fast_starts;
  s.silent = ua.silent;
  s.sizes[0] = ua.sizes[0];
  s.sizes[1] = ua.sizes[1];
  s.sizes[2] = ua.sizes[2];
  s.lvl_min = ua.lvl_min;
  s.lvl_max = ua.lvl_max;
  level = ua.w - ua.r;
  lvl_f = ua.lvl_q8 >> 8;
  target = ua.target;
  priming = ua.priming;
  t = st;
  alt = ua_alt;
  tone = ua_tone_on;
  ua_stats_reset(&ua);
  ua_stats_clear();
#if CH_DBG_STATISTICS
  crit_thd = ch.kernel_stats.m_crit_thd.worst;
  crit_isr = ch.kernel_stats.m_crit_isr.worst;
  ch.kernel_stats.m_crit_thd.worst = 0;
  ch.kernel_stats.m_crit_isr.worst = 0;
#endif
  chSysUnlock();

  chprintf(out, "usb audio (UAC%d): %s, source %s\r\n", USB_AUDIO_UAC2 ? 2 : 1,
      alt == UA_ALT_24BIT     ? "streaming 48000 Hz 24 bit (alt 1)" :
          alt == UA_ALT_16BIT ? "streaming 48000 Hz 16 bit (alt 2)" :
                                "idle (alt 0)",
      tone ? "test tone (L 1 kHz, R 500 Hz, -6 dBFS)" : "synth");
  chprintf(out, "since last U: packets sent %lu missed %lu (host skipped %lu, flushed early %lu); "
                "built %lu (47:%lu 48:%lu 49:%lu)\r\n",
      t.sent, t.missed, t.missed_host, t.missed_early, s.packets, s.sizes[0], s.sizes[1], s.sizes[2]);
  if (t.sofs) {
    const uint64_t mhz = (uint64_t)s.frames * 1000000U / t.sofs;
    chprintf(out, "rate: %lu frames in %lu ms of host clock = %lu.%03lu Hz\r\n", s.frames, t.sofs,
        (uint32_t)(mhz / 1000U), (uint32_t)(mhz % 1000U));
  }
  chprintf(out, "ring: level %lu now, %ld filtered, %ld..%ld before packets (target %ld = %ld us, of %d)%s\r\n",
      level, lvl_f, s.lvl_max < 0 ? 0 : s.lvl_min, s.lvl_max < 0 ? 0 : s.lvl_max, target,
      target * 1000 / 48, UA_RING_FRAMES, priming ? ", priming" : "");
  chprintf(out, "faults: underruns %lu overruns %lu primes %lu (silent packets %lu), fast starts %lu\r\n",
      s.underruns, s.overruns, s.primes, s.silent, s.fast_starts);
  chprintf(out, "frames: not ready at SOF %lu; SOF late (>1.1 ms) %lu lost %lu, max interval %lu us\r\n",
      t.not_ready, t.sof_late, t.sof_lost, ua_us(t.sof_dt_max));
  chprintf(out, "builds: after fetch %lu, after miss %lu, at SOF %lu; fetch at %lu..%lu us (avg %lu) after SOF; "
                "build avg %lu max %lu cycles\r\n",
      t.builds[UA_BUILD_XFRC], t.builds[UA_BUILD_MISS], t.builds[UA_BUILD_SOF],
      t.phase_n ? ua_us(t.phase_min) : 0, ua_us(t.phase_max), t.phase_n ? ua_us(t.phase_sum / t.phase_n) : 0,
      t.build_n ? t.build_sum / t.build_n : 0, t.build_max);
  chprintf(out, "synth: commit %lu us avg, %lu max after its DMA interrupt (block %lu us); off cadence %lu; "
                "since boot: blocks lost %lu, repeated %lu\r\n",
      t.commit_n ? ua_us(t.commit_sum / t.commit_n) : 0, ua_us(t.commit_max), ua_us(UA_BLOCK_CYC), t.off_cadence,
      lost, repeats);
  chprintf(out, "cpu: usb audio ISR hooks %lu cycles", t.isr_sum);
  if (t.sofs) { // streaming: one SOF per ms
    const uint32_t ppm = (uint32_t)((uint64_t)t.isr_sum * 1000000U / ((uint64_t)t.sofs * UA_CYC_MS));
    chprintf(out, " = %lu.%02lu%% of %lu ms", ppm / 10000U, (ppm / 100U) % 100U, t.sofs);
  }
#if CH_DBG_STATISTICS
  chprintf(out, "; longest lock: thread %lu us, ISR %lu us\r\n", ua_us(crit_thd), ua_us(crit_isr));
#else
  (void)crit_thd;
  (void)crit_isr;
  chprintf(out, "\r\n");
#endif
}
