/* The virtual board's USB bulk console (interface 2: EP 0x02 OUT, 0x82 IN), in place of
 * src/usb/bulk_usb.h: a BaseAsynchronousChannel on two byte queues (virtual/hal/usb_host.c).
 * BDU1 reads what the host wrote, and writes go to the host through the IN buffers, which the
 * USB frame (1 ms) hands to the host. */
#ifndef SV_HAL_BULK_USB_H
#define SV_HAL_BULK_USB_H

#include "hal.h"

#define BULK_USB_BUFFERS_SIZE 256
#define BULK_USB_BUFFERS_NUMBER 2

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { BDU_UNINIT = 0, BDU_STOP = 1, BDU_READY = 2 } bdustate_t;
struct BulkUSBDriverVMT {
	_base_asynchronous_channel_methods
};
typedef struct {
	USBDriver* usbp;
	usbep_t bulk_in, bulk_out;
} BulkUSBConfig;
typedef struct BulkUSBDriver {
	const struct BulkUSBDriverVMT* vmt;
	bdustate_t state;
	input_buffers_queue_t ibqueue;
	output_buffers_queue_t obqueue;
	const BulkUSBConfig* config;
} BulkUSBDriver;

void bduObjectInit(BulkUSBDriver* bdup);
void bduStart(BulkUSBDriver* bdup, const BulkUSBConfig* config);
void bduStop(BulkUSBDriver* bdup);

#ifdef __cplusplus
}
#endif

#endif
