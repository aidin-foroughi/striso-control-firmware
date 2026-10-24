/*
 * The virtual board's USB low level driver: in place of ChibiOS' STM32 OTGv1 LLD
 * (os/hal/ports/STM32/LLD/OTGv1/hal_usb_lld.h), included by ChibiOS' own hal_usb.h. The types
 * are the OTG LLD's (the driver structure, the endpoint states and configurations), the
 * functions run on the virtual bus of virtual/hal/usb_host.c instead of the OTG core. So the
 * board's USB stack above it runs as it is: ChibiOS' hal_usb.c (enumeration, EP0, the events),
 * src/usb/usbcfg.c (endpoints, events, SOF), bulk_usb.c and midi_usb.c on hal_buffers.c.
 *
 * The OTG core's registers: only those the board's code reads or writes (stm32_otg_t below).
 */
#ifndef HAL_USB_LLD_H
#define HAL_USB_LLD_H

/* as the OTG LLD: the core takes the address before the status stage, EP0's status stage is
 * done by hal_usb.c, the SET_ADDRESS status by software */
#define USB_EP0_STATUS_STAGE USB_EP0_STATUS_STAGE_SW
#define USB_SET_ADDRESS_MODE USB_EARLY_SET_ADDRESS
#define USB_SET_ADDRESS_ACK_HANDLING USB_SET_ADDRESS_ACK_SW
/* STM32H743 OTG1 (stm32_registry.h STM32_OTG1_ENDPOINTS), full speed */
#define USB_MAX_ENDPOINTS 8
#define STM32_USB_USE_OTG1 TRUE

/* the OTG core's registers the board's code touches: the frame number (DSTS, bulk_usb.c), the
 * endpoint interrupt mask and the FIFO flush (DAINTMSK, GRSTCTL: usbcfg.c usb_resume_endpointsI) */
typedef struct {
	volatile uint32_t DSTS;
	volatile uint32_t DAINTMSK;
	volatile uint32_t GRSTCTL;
} stm32_otg_t;
#define DSTS_FNSOF_MASK (0x3FFFU << 8)
#define DAINTMSK_IEPM(n) (1U << (n))
#define DAINTMSK_OEPM(n) (1U << (16 + (n)))
#define GRSTCTL_TXFNUM(n) ((uint32_t)(n) << 6)
/* the virtual core flushes a TX FIFO at once (an IN transfer's data is the queue's buffer until it
 * completes): the flush flag never reads set, so it is 0 here */
#define GRSTCTL_TXFFLSH 0U
#define osalSysPolledDelayX(cycles) ((void)(cycles))

typedef struct {
	size_t txsize;
	size_t txcnt;
	const uint8_t* txbuf;
	size_t totsize;
} USBInEndpointState;

typedef struct {
	size_t rxsize;
	size_t rxcnt;
	uint8_t* rxbuf;
	size_t totsize;
} USBOutEndpointState;

typedef struct {
	uint32_t ep_mode;
	usbepcallback_t setup_cb;
	usbepcallback_t in_cb;
	usbepcallback_t out_cb;
	uint16_t in_maxsize;
	uint16_t out_maxsize;
	USBInEndpointState* in_state;
	USBOutEndpointState* out_state;
	uint16_t in_multiplier;
	uint8_t* setup_buf;
} USBEndpointConfig;

typedef struct {
	usbeventcb_t event_cb;
	usbgetdescriptor_t get_descriptor_cb;
	usbreqhandler_t requests_hook_cb;
	usbcallback_t sof_cb;
} USBConfig;

struct USBDriver {
	usbstate_t state;
	const USBConfig* config;
	uint16_t transmitting;
	uint16_t receiving;
	const USBEndpointConfig* epc[USB_MAX_ENDPOINTS + 1];
	void* in_params[USB_MAX_ENDPOINTS];
	void* out_params[USB_MAX_ENDPOINTS];
	usbep0state_t ep0state;
	uint8_t* ep0next;
	size_t ep0n;
	usbcallback_t ep0endcb;
	uint8_t setup[8];
	uint16_t status;
	uint8_t address;
	uint8_t configuration;
	usbstate_t saved_state;
	stm32_otg_t* otg;
};

#define usb_lld_get_transaction_size(usbp, ep) ((usbp)->epc[ep]->out_state->rxcnt)
#define usb_lld_connect_bus(usbp) svh_usb_lld_connect_bus(usbp)
#define usb_lld_disconnect_bus(usbp) svh_usb_lld_disconnect_bus(usbp)
#define usb_lld_wakeup_host(usbp) ((void)(usbp))

extern USBDriver USBD1;

#ifdef __cplusplus
extern "C" {
#endif
void usb_lld_init(void);
void usb_lld_start(USBDriver* usbp);
void usb_lld_stop(USBDriver* usbp);
void usb_lld_reset(USBDriver* usbp);
void usb_lld_set_address(USBDriver* usbp);
void usb_lld_init_endpoint(USBDriver* usbp, usbep_t ep);
void usb_lld_disable_endpoints(USBDriver* usbp);
usbepstatus_t usb_lld_get_status_in(USBDriver* usbp, usbep_t ep);
usbepstatus_t usb_lld_get_status_out(USBDriver* usbp, usbep_t ep);
void usb_lld_read_setup(USBDriver* usbp, usbep_t ep, uint8_t* buf);
void usb_lld_start_out(USBDriver* usbp, usbep_t ep);
void usb_lld_start_in(USBDriver* usbp, usbep_t ep);
void usb_lld_stall_out(USBDriver* usbp, usbep_t ep);
void usb_lld_stall_in(USBDriver* usbp, usbep_t ep);
void usb_lld_clear_out(USBDriver* usbp, usbep_t ep);
void usb_lld_clear_in(USBDriver* usbp, usbep_t ep);
void svh_usb_lld_connect_bus(USBDriver* usbp);
void svh_usb_lld_disconnect_bus(USBDriver* usbp);
#ifdef __cplusplus
}
#endif

#endif
