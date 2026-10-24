/*
 * Discarding the pending data of a ChibiOS output buffers queue (hal_buffers.h) that a USB IN
 * endpoint transmits from. Header only: the host test (tests/usb_queue) runs it against ChibiOS'
 * own hal_buffers.c.
 */
#ifndef OBQ_DISCARD_H
#define OBQ_DISCARD_H

#include "hal.h"

/*
 * Drops the data queued in obqp that no transfer is using. in_flight: an IN transfer may be
 * reading the head buffer of the queue (usbGetTransmitStatusI of the endpoint).
 *
 * obqResetI() alone is wrong while a transfer is in flight: the transfer's completion
 * (bduDataTransmitted) releases the head buffer with obqReleaseEmptyBufferI(), which then asserts
 * "buffers queue empty" (chSysHalt), or without asserts counts one free buffer more than exist,
 * so a later write overwrites a buffer that is being transmitted. So the head buffer stays queued
 * when a transfer is in flight (the host receives it anyway: it may already be in the endpoint's
 * FIFO); every buffer after it, and the partly written one, is dropped. Threads waiting for space
 * are woken with MSG_RESET, as obqResetI() does.
 *
 * @iclass
 */
static inline void obq_discard_pendingI(output_buffers_queue_t *obqp, bool in_flight) {
  if (!in_flight || obqIsEmptyI(obqp)) {
    /* nothing to keep (an in-flight zero length packet uses no buffer of the queue) */
    obqResetI(obqp);
    return;
  }
  /* the head stays the only full buffer: the next one to write follows it */
  uint8_t *next = obqp->brdptr + obqp->bsize;
  if (next >= obqp->btop) {
    next = obqp->buffers;
  }
  obqp->bwrptr   = next;
  obqp->bcounter = bqSizeX(obqp) - 1U;
  obqp->ptr      = NULL;
  obqp->top      = NULL;
  osalThreadDequeueAllI(&obqp->waiting, MSG_RESET);
}

#endif
