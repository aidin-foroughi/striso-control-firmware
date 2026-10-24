/* The virtual board's USB MIDI (in place of src/usb/midi_usb.h): MDU1 reads the host's 4-byte
 * event packets, midi_usb_MidiSend* queue packets for the host (virtual/hal/usb_host.c). */
#ifndef SV_HAL_MIDI_USB_H
#define SV_HAL_MIDI_USB_H

#include "hal.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { MDU_UNINIT = 0, MDU_STOP = 1, MDU_READY = 2 } mdustate_t;
struct MidiUSBDriverVMT {
	_base_asynchronous_channel_methods
};
typedef struct {
	USBDriver* usbp;
	usbep_t bulk_in, bulk_out;
} MidiUSBConfig;
typedef struct MidiUSBDriver {
	const struct MidiUSBDriverVMT* vmt;
	mdustate_t state;
	input_buffers_queue_t ibqueue;
	output_buffers_queue_t obqueue;
	const MidiUSBConfig* config;
} MidiUSBDriver;

void mduObjectInit(MidiUSBDriver* mdup);
void mduStart(MidiUSBDriver* mdup, const MidiUSBConfig* config);
void midi_usb_MidiSend1(uint8_t port, uint8_t b0);
void midi_usb_MidiSend2(uint8_t port, uint8_t b0, uint8_t b1);
void midi_usb_MidiSend3(uint8_t port, uint8_t b0, uint8_t b1, uint8_t b2);

#ifdef __cplusplus
}
#endif

#endif
