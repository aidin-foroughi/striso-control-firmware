/* The virtual board's USB: the OTG low level driver (in place of ChibiOS' STM32 OTGv1 LLD; types
 * in hal_usb_lld.h) and the bus with the host on it. Above it the board's USB stack runs as it
 * is: ChibiOS' hal_usb.c, src/usb/usbcfg.c, bulk_usb.c and midi_usb.c on ChibiOS' hal_buffers.c.
 *
 * The bus. The board's D+ pull-up (usbConnectBus) puts the device on the bus. At the next USB frame
 * the host resets the bus (USBRST: hal_usb.c _usb_reset) and enumerates the device through EP0:
 * the device descriptor, SET_ADDRESS, the device and configuration descriptors, SET_CONFIGURATION
 * 1 (usbcfg.c's usb_event then sets up the endpoints and the drivers, as on the board). Then every
 * frame (1 ms, svh_usb_sof): the SOF interrupt (usbcfg.c sof_handler: the drivers send their
 * partly filled IN buffer), then the frame's bulk transactions, at most 19 packets of 64 bytes
 * (full speed): an IN transfer in flight completes into the host's receive buffer (its callback
 * starts the next one, which may go in the same frame), an OUT transfer a driver armed takes the
 * host's next packets (up to a short packet or the transfer's size). The host splits each write
 * into packets of 64 bytes (no zero-length packet after a write of 64*k bytes, as libusb and
 * WinUSB). The host's receive buffers hold HOST_LIMIT bytes: beyond that (the host does not read)
 * it NAKs the IN transfers, so the board's queue fills and its writes wait or drop, as on the
 * board. Removing the pull-up (usbDisconnectBus) takes the device off the bus: the host forgets
 * what it held, and the device sees the idle bus as a suspend (OTG USBSUSP: its transfers are
 * aborted, hal_usb.c _usb_suspend). The host never suspends a device on the bus: no wakeup. */
#include <stdlib.h>
#include <string.h>

#include "hal.h"
#include "sv_host.h"
#include "usbcfg.h"

/* --- the host's byte queues ------------------------------------------------------------------------ */
typedef struct {
	uint8_t* buf;
	size_t len, cap, rd;
} bytes_t;

static void bytes_push(bytes_t* q, const uint8_t* p, size_t n) {
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
	if (n) memcpy(q->buf + q->len, p, n);
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

#define HOST_LIMIT (4u << 20) /* what the host holds unread (and has still to send) */
#define FRAME_PACKETS 19      /* bulk packets of 64 bytes in a full-speed frame */
#define PACKET 64

static struct {
	int pullup;     /* the device is on the bus */
	int enumerated; /* the host reset and configured it since it came on the bus */
	uint16_t frame; /* the frame number (DSTS FNSOF) */
	/* host -> board: the host's packets not sent yet, each [length][bytes] */
	bytes_t con_out, midi_out;
	/* board -> host: received, not read by the host yet */
	bytes_t con_in, midi_in;
} U;

/* --- the OTG core (hal_usb_lld.c) ----------------------------------------------------------------- */
USBDriver USBD1;
static stm32_otg_t otg;
static uint16_t stalled_in, stalled_out; /* endpoints' STALL bits (DIEPCTL, DOEPCTL) */
static USBInEndpointState ep0_in;
static USBOutEndpointState ep0_out;
static uint8_t ep0setup_buffer[8];
static const USBEndpointConfig ep0config = {
    USB_EP_MODE_TYPE_CTRL, _usb_ep0setup, _usb_ep0in, _usb_ep0out, 0x40, 0x40, &ep0_in, &ep0_out, 1, ep0setup_buffer};

void usb_lld_init(void) {
	usbObjectInit(&USBD1);
	USBD1.otg = &otg;
}

/* the core is on; the pull-up puts it on the bus */
void usb_lld_start(USBDriver* usbp) { (void)usbp; }

/* the core is off: off the bus */
void usb_lld_stop(USBDriver* usbp) {
	(void)usbp;
	U.pullup = 0;
}

void usb_lld_reset(USBDriver* usbp) {
	stalled_in = stalled_out = 0;
	otg.DAINTMSK = DAINTMSK_OEPM(0) | DAINTMSK_IEPM(0);
	usbp->epc[0] = &ep0config; /* EP0 initialization, as the LLD's reset */
}

void usb_lld_set_address(USBDriver* usbp) { (void)usbp; }

void usb_lld_init_endpoint(USBDriver* usbp, usbep_t ep) {
	const USBEndpointConfig* epcp = usbp->epc[ep];
	stalled_in &= (uint16_t)~(1U << ep);
	stalled_out &= (uint16_t)~(1U << ep);
	if (epcp->out_state != NULL) otg.DAINTMSK |= DAINTMSK_OEPM(ep);
	if (epcp->in_state != NULL) otg.DAINTMSK |= DAINTMSK_IEPM(ep);
}

void usb_lld_disable_endpoints(USBDriver* usbp) {
	(void)usbp;
	otg.DAINTMSK = DAINTMSK_OEPM(0) | DAINTMSK_IEPM(0);
}

usbepstatus_t usb_lld_get_status_in(USBDriver* usbp, usbep_t ep) {
	if (ep > USB_MAX_ENDPOINTS || usbp->epc[ep] == NULL || usbp->epc[ep]->in_state == NULL) return EP_STATUS_DISABLED;
	return (stalled_in >> ep) & 1U ? EP_STATUS_STALLED : EP_STATUS_ACTIVE;
}

usbepstatus_t usb_lld_get_status_out(USBDriver* usbp, usbep_t ep) {
	if (ep > USB_MAX_ENDPOINTS || usbp->epc[ep] == NULL || usbp->epc[ep]->out_state == NULL) return EP_STATUS_DISABLED;
	return (stalled_out >> ep) & 1U ? EP_STATUS_STALLED : EP_STATUS_ACTIVE;
}

void usb_lld_read_setup(USBDriver* usbp, usbep_t ep, uint8_t* buf) { memcpy(buf, usbp->epc[ep]->setup_buf, 8); }

/* a transfer is armed (usbStartReceiveI, usbStartTransmitI): the bus moves it */
void usb_lld_start_out(USBDriver* usbp, usbep_t ep) {
	(void)usbp;
	(void)ep;
}
void usb_lld_start_in(USBDriver* usbp, usbep_t ep) {
	(void)usbp;
	(void)ep;
}

void usb_lld_stall_out(USBDriver* usbp, usbep_t ep) {
	(void)usbp;
	stalled_out |= (uint16_t)(1U << ep);
}
void usb_lld_stall_in(USBDriver* usbp, usbep_t ep) {
	(void)usbp;
	stalled_in |= (uint16_t)(1U << ep);
}
void usb_lld_clear_out(USBDriver* usbp, usbep_t ep) {
	(void)usbp;
	stalled_out &= (uint16_t)~(1U << ep);
}
void usb_lld_clear_in(USBDriver* usbp, usbep_t ep) {
	(void)usbp;
	stalled_in &= (uint16_t)~(1U << ep);
}

void svh_usb_lld_connect_bus(USBDriver* usbp) {
	(void)usbp;
	U.pullup = 1;
}
void svh_usb_lld_disconnect_bus(USBDriver* usbp) {
	(void)usbp;
	U.pullup = 0;
}

/* --- the host ---------------------------------------------------------------------------------------- */

/* the host's queues of an endpoint: EP1 USB MIDI, EP2 the console (usbcfg.h) */
static bytes_t* host_in(usbep_t ep) {
	return ep == USBD2_DATA_REQUEST_EP ? &U.con_in : ep == USBD1_DATA_REQUEST_EP ? &U.midi_in : NULL;
}
static bytes_t* host_out(usbep_t ep) {
	return ep == USBD2_DATA_AVAILABLE_EP ? &U.con_out : ep == USBD1_DATA_AVAILABLE_EP ? &U.midi_out : NULL;
}

/* a host write: its packets */
static void host_send(bytes_t* q, const uint8_t* p, size_t n) {
	while (n > 0) {
		const uint8_t k = (uint8_t)(n < PACKET ? n : PACKET);
		bytes_push(q, &k, 1);
		bytes_push(q, p, k);
		p += k;
		n -= k;
	}
}

/* one control transfer on EP0 (SETUP, data, status), as the host's stack does it. For a read,
 * data gets what the device sent (at most len bytes). Returns the data stage's length, or -1:
 * the device stalled or left the transfer unfinished. */
static int control(USBDriver* usbp, uint8_t type, uint8_t req, uint16_t value, uint16_t index, uint8_t* data, uint16_t len) {
	const uint8_t setup[8] = {type, req, (uint8_t)value, (uint8_t)(value >> 8), (uint8_t)index, (uint8_t)(index >> 8),
	    (uint8_t)len, (uint8_t)(len >> 8)};
	size_t got = 0;
	memcpy(ep0setup_buffer, setup, 8);
	stalled_in &= (uint16_t)~1U; /* a SETUP clears EP0's stall */
	stalled_out &= (uint16_t)~1U;
	_usb_isr_invoke_setup_cb(usbp, 0);
	for (int stage = 0; stage < 8; stage++) {
		if ((stalled_in | stalled_out) & 1U || usbp->ep0state == USB_EP0_ERROR) return -1;
		if (usbp->ep0state == USB_EP0_STP_WAITING) return (int)got;
		if (usbGetTransmitStatusI(usbp, 0)) {
			USBInEndpointState* isp = usbp->epc[0]->in_state;
			if (data != NULL && isp->txsize > 0) {
				const size_t n = isp->txsize < len - got ? isp->txsize : len - got;
				memcpy(data + got, isp->txbuf, n);
				got += n;
			}
			isp->txcnt = isp->txsize;
			_usb_isr_invoke_in_cb(usbp, 0);
		} else if (usbGetReceiveStatusI(usbp, 0)) {
			USBOutEndpointState* osp = usbp->epc[0]->out_state;
			size_t n = 0;
			if (!(type & 0x80) && data != NULL) { /* a write's data stage */
				n = len - got < osp->rxsize ? len - got : osp->rxsize;
				memcpy(osp->rxbuf, data + got, n);
				got += n;
			}
			osp->rxcnt = n;
			_usb_isr_invoke_out_cb(usbp, 0);
		} else {
			return -1;
		}
	}
	return -1;
}

/* the host resets the bus and configures the device */
static void enumerate(USBDriver* usbp) {
	static uint8_t d[1024];
	_usb_reset(usbp);
	if (control(usbp, 0x80, USB_REQ_GET_DESCRIPTOR, USB_DESCRIPTOR_DEVICE << 8, 0, d, 64) < 8) return;
	if (control(usbp, 0x00, USB_REQ_SET_ADDRESS, 1, 0, NULL, 0) < 0) return;
	if (control(usbp, 0x80, USB_REQ_GET_DESCRIPTOR, USB_DESCRIPTOR_DEVICE << 8, 0, d, 18) != 18) return;
	if (control(usbp, 0x80, USB_REQ_GET_DESCRIPTOR, USB_DESCRIPTOR_CONFIGURATION << 8, 0, d, 9) != 9) return;
	const uint16_t total = (uint16_t)(d[2] | d[3] << 8);
	if (total > sizeof d || control(usbp, 0x80, USB_REQ_GET_DESCRIPTOR, USB_DESCRIPTOR_CONFIGURATION << 8, 0, d, total) != total)
		return;
	if (control(usbp, 0x00, USB_REQ_SET_CONFIGURATION, d[5], 0, NULL, 0) < 0) return;
	U.enumerated = usbp->state == USB_ACTIVE;
}

/* the device left the bus: the host closes it; the device sees a suspend */
static void detach(USBDriver* usbp) {
	U.enumerated = 0;
	bytes_clear(&U.con_out);
	bytes_clear(&U.con_in);
	bytes_clear(&U.midi_out);
	bytes_clear(&U.midi_in);
	if (usbp->state == USB_READY || usbp->state == USB_SELECTED || usbp->state == USB_ACTIVE) {
		otg.DAINTMSK = DAINTMSK_OEPM(0) | DAINTMSK_IEPM(0); /* otg_disable_ep */
		_usb_suspend(usbp);
	}
}

/* an IN transfer in flight goes to the host: whole, if the frame and the host's buffer have room */
static bool frame_in(USBDriver* usbp, usbep_t ep, int* budget) {
	const USBEndpointConfig* epcp = usbp->epc[ep];
	bytes_t* q = host_in(ep);
	if (q == NULL || !usbGetTransmitStatusI(usbp, ep) || (stalled_in >> ep) & 1U) return false;
	USBInEndpointState* isp = epcp->in_state;
	const int packets = isp->txsize ? (int)((isp->txsize + epcp->in_maxsize - 1) / epcp->in_maxsize) : 1;
	if (packets > *budget || bytes_count(q) + isp->txsize > HOST_LIMIT) return false;
	bytes_push(q, isp->txbuf, isp->txsize);
	isp->txcnt = isp->txsize;
	*budget -= packets;
	_usb_isr_invoke_in_cb(usbp, ep);
	return true;
}

/* the host's next packets go into an armed OUT transfer, which ends with a short packet or full */
static bool frame_out(USBDriver* usbp, usbep_t ep, int* budget) {
	const USBEndpointConfig* epcp = usbp->epc[ep];
	bytes_t* q = host_out(ep);
	bool moved = false;
	if (q == NULL || !usbGetReceiveStatusI(usbp, ep) || (stalled_out >> ep) & 1U) return false;
	USBOutEndpointState* osp = epcp->out_state;
	while (*budget > 0 && bytes_count(q) > 0) {
		uint8_t pkt[PACKET];
		uint8_t k;
		bytes_pop(q, &k, 1);
		bytes_pop(q, pkt, k);
		const size_t n = k < osp->rxsize - osp->rxcnt ? k : osp->rxsize - osp->rxcnt;
		memcpy(osp->rxbuf + osp->rxcnt, pkt, n);
		osp->rxcnt += n;
		(*budget)--;
		moved = true;
		if (k < epcp->out_maxsize || osp->rxcnt >= osp->rxsize) {
			_usb_isr_invoke_out_cb(usbp, ep);
			break;
		}
	}
	return moved;
}

/* a USB frame: the SOF interrupt, then the frame's bulk transactions */
void svh_usb_sof(void) {
	USBDriver* usbp = &USBD1;
	if (!U.pullup || usbp->state == USB_UNINIT || usbp->state == USB_STOP) {
		if (U.enumerated || usbp->state == USB_ACTIVE) detach(usbp);
		return;
	}
	if (!U.enumerated) {
		enumerate(usbp);
		if (!U.enumerated) return;
	}
	U.frame = (uint16_t)((U.frame + 1U) & 0x7FFU);
	otg.DSTS = (otg.DSTS & ~DSTS_FNSOF_MASK) | ((uint32_t)U.frame << 8);
	_usb_isr_invoke_sof_cb(usbp);
	int budget = FRAME_PACKETS;
	for (bool more = true; more && budget > 0;) {
		more = false;
		for (usbep_t ep = 1; ep <= USB_MAX_ENDPOINTS; ep++) {
			if (usbp->epc[ep] == NULL) continue;
			if (frame_in(usbp, ep, &budget)) more = true;
			if (frame_out(usbp, ep, &budget)) more = true;
		}
	}
}

/* --- the instance's side ----------------------------------------------------------------------------- */

void svh_usb_init(void) {
	memset(&U, 0, sizeof U);
	memset(&otg, 0, sizeof otg);
	stalled_in = stalled_out = 0;
	usbInit(); /* halInit's part */
}

void svh_usb_free(void) {
	bytes_t* all[] = {&U.con_out, &U.con_in, &U.midi_out, &U.midi_in};
	for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) free(all[i]->buf);
	memset(&U, 0, sizeof U);
}

/* the host has the device: on the bus and configured */
int svh_usb_attached(void) { return U.pullup && U.enumerated; }

int32_t svh_console_write(const uint8_t* bytes, int32_t n) {
	if (!svh_usb_attached()) return -1;
	if (bytes_count(&U.con_out) + (size_t)n <= HOST_LIMIT) host_send(&U.con_out, bytes, (size_t)n);
	return n;
}

int32_t svh_console_read(uint8_t* out, int32_t max) {
	if (!svh_usb_attached()) return -1;
	return (int32_t)bytes_pop(&U.con_in, out, (size_t)max);
}

/* USB MIDI: whole 4-byte event packets */
int32_t svh_midi_write(const uint8_t* bytes, int32_t n) {
	if (!svh_usb_attached()) return -1;
	const size_t whole = (size_t)n & ~(size_t)3;
	if (bytes_count(&U.midi_out) + whole <= HOST_LIMIT) host_send(&U.midi_out, bytes, whole);
	return n;
}

int32_t svh_midi_read(uint8_t* out, int32_t max) {
	if (!svh_usb_attached()) return -1;
	return (int32_t)bytes_pop(&U.midi_in, out, (size_t)max & ~(size_t)3);
}
