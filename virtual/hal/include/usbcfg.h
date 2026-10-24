/* The virtual board's USB configuration (in place of src/usb/usbcfg.h). */
#ifndef SV_HAL_USBCFG_H
#define SV_HAL_USBCFG_H

#include "bulk_usb.h"
#include "midi_usb.h"

#ifdef __cplusplus
extern "C" {
#endif
extern MidiUSBDriver MDU1;
extern BulkUSBDriver BDU1;
extern const USBConfig usbcfg;
extern const MidiUSBConfig midiusbcfg;
extern const BulkUSBConfig bulkusbcfg;
void init_usb_descriptor(void);
#ifdef __cplusplus
}
#endif

#endif
