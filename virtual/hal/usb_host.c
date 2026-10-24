/* The virtual board's USB: the bus (usbConnectBus attaches the board), the bulk console BDU1 and
 * USB MIDI MDU1 as channels on byte queues.
 *
 * Console, host -> board: what the host wrote waits until the console thread reads it
 * (PExReceive, every 2 ms). Board -> host: writes (chprintf, the binary stream) go into the IN
 * buffers; at each USB frame (1 ms) they move to the host, as the bulk driver flushes its buffers
 * at SOF. obqWriteTimeout with TIME_IMMEDIATE (the binary stream) drops what does not fit in the
 * driver's two 256-byte buffers, as on the board; the blocking writes (chprintf) never drop.
 * obqResetI (StcV) empties the IN buffers: what was not yet sent is lost, as on the board. */
#include <stdlib.h>
#include <string.h>

#include "bulk_usb.h"
#include "midi_usb.h"
#include "sv_host.h"
#include "usbcfg.h"

typedef struct {
	uint8_t* buf;
	size_t len, cap, rd;
} bytes_t;

static void bytes_push(bytes_t* q, const uint8_t* p, size_t n, size_t limit) {
	if (q->len - q->rd + n > limit) return; /* the host does not read: drop (bounded memory) */
	if (q->rd > 0 && q->rd == q->len) q->rd = q->len = 0;
	if (q->len + n > q->cap) {
		if (q->rd > 0) {
			memmove(q->buf, q->buf + q->rd, q->len - q->rd);
			q->len -= q->rd;
			q->rd = 0;
		}
		if (q->len + n > q->cap) {
			size_t c = q->cap ? q->cap : 1024;
			while (c < q->len + n) c *= 2;
			q->buf = (uint8_t*)realloc(q->buf, c);
			q->cap = c;
		}
	}
	memcpy(q->buf + q->len, p, n);
	q->len += n;
}

static size_t bytes_pop(bytes_t* q, uint8_t* out, size_t max) {
	size_t n = q->len - q->rd;
	if (n > max) n = max;
	if (n) memcpy(out, q->buf + q->rd, n);
	q->rd += n;
	return n;
}

static size_t bytes_count(const bytes_t* q) { return q->len - q->rd; }
static void bytes_clear(bytes_t* q) { q->rd = q->len = 0; }

#define HOST_LIMIT (4u << 20) /* what the host may leave unread before the board's output drops */

static struct {
	int attached;
	bytes_t con_out;  /* host -> board */
	bytes_t con_dev;  /* board -> host, in the IN buffers */
	bytes_t con_host; /* board -> host, delivered */
	bytes_t midi_out, midi_dev, midi_host;
} U;

USBDriver USBD1;
const USBConfig usbcfg = {0};
const MidiUSBConfig midiusbcfg = {&USBD1, 1, 1};
const BulkUSBConfig bulkusbcfg = {&USBD1, 2, 2};
BulkUSBDriver BDU1;
MidiUSBDriver MDU1;

void svh_usb_init(void) { memset(&U, 0, sizeof U); }

void svh_usb_free(void) {
	bytes_t* all[] = {&U.con_out, &U.con_dev, &U.con_host, &U.midi_out, &U.midi_dev, &U.midi_host};
	for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) free(all[i]->buf);
	memset(&U, 0, sizeof U);
}
int svh_usb_attached(void) { return U.attached; }

void init_usb_descriptor(void) {}
void usbStart(USBDriver* usbp, const USBConfig* config) {
	(void)usbp;
	(void)config;
}
void usbStop(USBDriver* usbp) { (void)usbp; }
void usbConnectBus(USBDriver* usbp) {
	(void)usbp;
	U.attached = 1;
}
void usbDisconnectBus(USBDriver* usbp) {
	(void)usbp;
	U.attached = 0;
}

int32_t svh_console_write(const uint8_t* bytes, int32_t n) {
	if (!U.attached) return -1;
	bytes_push(&U.con_out, bytes, (size_t)n, HOST_LIMIT);
	return n;
}

int32_t svh_console_read(uint8_t* out, int32_t max) {
	if (!U.attached) return -1;
	return (int32_t)bytes_pop(&U.con_host, out, (size_t)max);
}

int32_t svh_midi_write(const uint8_t* bytes, int32_t n) {
	if (!U.attached) return -1;
	bytes_push(&U.midi_out, bytes, (size_t)n & ~(size_t)3, HOST_LIMIT);
	return n;
}

int32_t svh_midi_read(uint8_t* out, int32_t max) {
	if (!U.attached) return -1;
	return (int32_t)bytes_pop(&U.midi_host, out, (size_t)max & ~(size_t)3);
}

void svh_usb_sof(void) {
	uint8_t tmp[4096];
	size_t n;
	while ((n = bytes_pop(&U.con_dev, tmp, sizeof tmp)) > 0) bytes_push(&U.con_host, tmp, n, HOST_LIMIT);
	while ((n = bytes_pop(&U.midi_dev, tmp, sizeof tmp)) > 0) bytes_push(&U.midi_host, tmp, n, HOST_LIMIT);
}

/* --- the channels' methods --------------------------------------------------------------------------- */

static bytes_t* dev_of(void* ip) { return ip == (void*)&MDU1 ? &U.midi_dev : &U.con_dev; }
static bytes_t* out_of(void* ip) { return ip == (void*)&MDU1 ? &U.midi_out : &U.con_out; }

static size_t ch_write(void* ip, const uint8_t* bp, size_t n) {
	if (U.attached) bytes_push(dev_of(ip), bp, n, HOST_LIMIT);
	return n;
}
static size_t ch_writet(void* ip, const uint8_t* bp, size_t n, sysinterval_t t) {
	(void)t;
	return ch_write(ip, bp, n);
}
static msg_t ch_put(void* ip, uint8_t b) {
	ch_write(ip, &b, 1);
	return MSG_OK;
}
static msg_t ch_putt(void* ip, uint8_t b, sysinterval_t t) {
	(void)t;
	return ch_put(ip, b);
}
/* reads never wait: the firmware only reads with TIME_IMMEDIATE */
static size_t ch_readt(void* ip, uint8_t* bp, size_t n, sysinterval_t t) {
	(void)t;
	bytes_t* q = out_of(ip);
	if (bytes_count(q) < n) return 0; /* a whole MIDI packet or nothing */
	return bytes_pop(q, bp, n);
}
static size_t ch_read(void* ip, uint8_t* bp, size_t n) { return ch_readt(ip, bp, n, TIME_IMMEDIATE); }
static msg_t ch_gett(void* ip, sysinterval_t t) {
	uint8_t b;
	return ch_readt(ip, &b, 1, t) ? (msg_t)b : MSG_TIMEOUT;
}
static msg_t ch_get(void* ip) { return ch_gett(ip, TIME_IMMEDIATE); }
static msg_t ch_ctl(void* ip, unsigned int op, void* arg) {
	(void)ip;
	(void)op;
	(void)arg;
	return MSG_OK;
}

static const struct BulkUSBDriverVMT bdu_vmt = {ch_write, ch_read, ch_put, ch_get, ch_putt, ch_gett, ch_writet, ch_readt, ch_ctl};
static const struct MidiUSBDriverVMT mdu_vmt = {ch_write, ch_read, ch_put, ch_get, ch_putt, ch_gett, ch_writet, ch_readt, ch_ctl};

void bduObjectInit(BulkUSBDriver* bdup) {
	bdup->vmt = &bdu_vmt;
	bdup->state = BDU_STOP;
	bdup->obqueue.which = 0;
	bdup->ibqueue.which = 0;
}
void bduStart(BulkUSBDriver* bdup, const BulkUSBConfig* config) {
	bdup->config = config;
	bdup->state = BDU_READY;
}
void bduStop(BulkUSBDriver* bdup) { bdup->state = BDU_STOP; }

void mduObjectInit(MidiUSBDriver* mdup) {
	mdup->vmt = &mdu_vmt;
	mdup->state = MDU_STOP;
	mdup->obqueue.which = 1;
	mdup->ibqueue.which = 1;
}
void mduStart(MidiUSBDriver* mdup, const MidiUSBConfig* config) {
	mdup->config = config;
	mdup->state = MDU_READY;
}

/* the driver's two 256-byte IN buffers: an immediate write takes what fits */
size_t obqWriteTimeout(output_buffers_queue_t* obqp, const uint8_t* bp, size_t n, sysinterval_t timeout) {
	bytes_t* q = obqp->which ? &U.midi_dev : &U.con_dev;
	if (!U.attached) return 0;
	if (timeout == TIME_IMMEDIATE) {
		const size_t room = 2 * 256 > bytes_count(q) ? 2 * 256 - bytes_count(q) : 0;
		if (n > room) n = room;
	}
	bytes_push(q, bp, n, HOST_LIMIT);
	return n;
}

bool obqIsEmptyI(output_buffers_queue_t* obqp) { return bytes_count(obqp->which ? &U.midi_dev : &U.con_dev) == 0; }
void obqResetI(output_buffers_queue_t* obqp) { bytes_clear(obqp->which ? &U.midi_dev : &U.con_dev); }

/* --- USB MIDI out (midi_usb.c's packets: cable/CIN, then the message) --------------------------------- */
static void midi_packet(uint8_t port, uint8_t b0, uint8_t b1, uint8_t b2) {
	const uint8_t tx[4] = {(uint8_t)((((port - 1) & 0x0F) << 4) | ((b0 & 0xF0) >> 4)), b0, b1, b2};
	ch_write(&MDU1, tx, 4);
}
void midi_usb_MidiSend1(uint8_t port, uint8_t b0) { midi_packet(port, b0, 0, 0); }
void midi_usb_MidiSend2(uint8_t port, uint8_t b0, uint8_t b1) { midi_packet(port, b0, b1, 0); }
void midi_usb_MidiSend3(uint8_t port, uint8_t b0, uint8_t b1, uint8_t b2) { midi_packet(port, b0, b1, b2); }
