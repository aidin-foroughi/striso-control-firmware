/* D1: the console's 'V' discards the bulk console's pending output (src/app/pconnection.c ->
 * bduDiscardOutputI -> obq_discard_pendingI, src/usb/obq_discard.h) while an IN transfer may be
 * reading the head buffer of that queue. Runs ChibiOS' own hal_buffers.c (third_party/ChibiOS) with
 * a model of bulk_usb.c's endpoint handling: one IN transfer at a time from the head of the queue,
 * started by the queue's notify callback, the SOF flush or the completion of the previous one, and
 * released at its completion (bduDataTransmitted). Fails on any ChibiOS assertion (chSysHalt on
 * the board), on queue counters out of range, or when a buffer is overwritten while in flight. */
#include <stdlib.h>

#include "hal.h"
#include "obq_discard.h"

int halts;
#define BSIZE 256 /* BULK_USB_BUFFERS_SIZE */
#define BNUM 2    /* BULK_USB_BUFFERS_NUMBER */
#define EPSIZE 64 /* the endpoint's max packet size */
static uint8_t ob[BQ_BUFFER_SIZE(BNUM, BSIZE)];
static output_buffers_queue_t q;
static int failures;

/* the IN endpoint */
static struct {
  bool busy;
  uint8_t *buf; /* NULL: a zero length packet */
  size_t n;
  uint8_t copy[BSIZE]; /* the data when the transfer started */
} ep;
/* what the host received */
static uint8_t host[1 << 16];
static size_t host_n;

static void start_transmit(void) { /* usbStartTransmitI of the head buffer, if there is one */
  size_t n;
  uint8_t *b = obqGetFullBufferI(&q, &n);
  if (b != NULL) {
    ep.busy = true;
    ep.buf = b;
    ep.n = n;
    memcpy(ep.copy, b, n);
  }
}
static void obnotify(io_buffers_queue_t *bqp) { /* bulk_usb.c obnotify */
  (void)bqp;
  if (!ep.busy) start_transmit();
}
static void sof(void) { /* bduSOFHookI */
  if (!ep.busy && obqTryFlushI(&q)) start_transmit();
}
static void transmitted(void) { /* the host fetched the transfer: bduDataTransmitted */
  if (!ep.busy) return;
  if (ep.buf != NULL && memcmp(ep.buf, ep.copy, ep.n) != 0) {
    printf("  buffer overwritten while its IN transfer was in flight\n");
    failures++;
  }
  if (ep.buf != NULL && host_n + ep.n <= sizeof host) {
    memcpy(host + host_n, ep.copy, ep.n);
    host_n += ep.n;
  }
  size_t done = ep.buf != NULL ? ep.n : 0;
  ep.busy = false;
  if (done > 0) obqReleaseEmptyBufferI(&q);
  start_transmit();
  if (!ep.busy && done > 0 && done % EPSIZE == 0) { /* zero length packet */
    ep.busy = true;
    ep.buf = NULL;
    ep.n = 0;
  }
}
static void discard(void) { obq_discard_pendingI(&q, ep.busy); } /* the console's 'V' */
static void write_bytes(int c, size_t n) {
  uint8_t b[BSIZE * BNUM + 64];
  memset(b, c, n);
  (void)obqWriteTimeout(&q, b, n, TIME_IMMEDIATE);
}
static void write_str(const char *s) { (void)obqWriteTimeout(&q, (const uint8_t *)s, strlen(s), TIME_IMMEDIATE); }

static void reset_all(void) {
  obqObjectInit(&q, true, ob, BSIZE, BNUM, obnotify, NULL);
  memset(&ep, 0, sizeof ep);
  host_n = 0;
  halts = 0;
}
static void drain(void) {
  for (int i = 0; i < 16; i++) {
    sof();
    transmitted();
  }
}
static void check(const char *name, bool ok) {
  if (!ok) failures++;
  printf("%-4s %s\n", ok ? "ok" : "FAIL", name);
}
static bool host_is(const uint8_t *want, size_t n) { return host_n == n && memcmp(host, want, n) == 0; }
static bool counters_ok(void) { return q.bcounter <= q.bn; }

static const char reply[] = "2026-10-24_v2.2.4 0123456789ABCDEF01234567\r\n";

int main(void) {
  uint8_t want[1024];

  /* the review's case: the stream left on, 'V' arrives while a packet is armed and not fetched */
  reset_all();
  write_bytes(0x80, 200);
  sof(); /* the partial buffer is flushed, its IN transfer starts */
  discard();
  write_str(reply);
  drain();
  memset(want, 0x80, 200);
  memcpy(want + 200, reply, sizeof reply - 1);
  check("V during an IN transfer: no assertion, the armed packet then the reply",
        halts == 0 && counters_ok() && host_is(want, 200 + sizeof reply - 1));
  check("  the queue is empty again afterwards", q.bcounter == q.bn && !ep.busy);

  /* both buffers full (one in flight, one queued) and a partial third: only the armed one stays */
  reset_all();
  write_bytes('a', BSIZE);
  write_bytes('b', BSIZE);
  write_bytes('c', 10);
  discard();
  write_str(reply);
  drain();
  memset(want, 'a', BSIZE);
  memcpy(want + BSIZE, reply, sizeof reply - 1);
  check("V with a full queue: queued and partial data dropped, the reply follows the armed packet",
        halts == 0 && counters_ok() && host_is(want, BSIZE + sizeof reply - 1));

  /* nothing in flight: everything pending is dropped */
  reset_all();
  write_bytes('x', 10);
  discard();
  write_str(reply);
  drain();
  check("V with no transfer in flight: only the reply",
        halts == 0 && counters_ok() && host_is((const uint8_t *)reply, sizeof reply - 1));

  /* a zero length packet in flight (the queue is empty) */
  reset_all();
  write_bytes('z', 2 * EPSIZE);
  sof();
  transmitted(); /* 128 bytes done: a ZLP follows */
  bool zlp = ep.busy && ep.buf == NULL;
  write_bytes('y', 5);
  discard();
  write_str(reply);
  drain();
  memset(want, 'z', 2 * EPSIZE);
  memcpy(want + 2 * EPSIZE, reply, sizeof reply - 1);
  check("V while a zero length packet is in flight",
        zlp && halts == 0 && counters_ok() && host_is(want, 2 * EPSIZE + sizeof reply - 1));

  /* random interleavings of the stream, the console, the SOF flush, completions and 'V' */
  reset_all();
  srand(1);
  int bad_counters = 0;
  for (int i = 0; i < 200000; i++) {
    switch (rand() % 6) {
    case 0:
    case 1: write_bytes(i & 0xFF, 1 + (size_t)(rand() % 300)); break;
    case 2: sof(); break;
    case 3:
    case 4: transmitted(); break;
    case 5:
      if (rand() % 8 == 0) discard();
      break;
    }
    if (!counters_ok()) bad_counters++;
    if (host_n > sizeof host / 2) host_n = 0;
  }
  drain();
  check("random interleavings: no assertion, counters in range, nothing overwritten in flight",
        halts == 0 && bad_counters == 0 && failures == 0 && q.bcounter == q.bn);

  printf(failures ? "obq discard: FAILED\n" : "obq discard: all passed\n");
  return failures ? 1 : 0;
}
