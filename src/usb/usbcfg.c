/*
 ChibiOS/RT - Copyright (C) 2006-2013 Giovanni Di Sirio

 modified by Johannes Taelman

 Licensed under the Apache License, Version 2.0 (the "License");
 you may not use this file except in compliance with the License.
 You may obtain a copy of the License at

 http://www.apache.org/licenses/LICENSE-2.0

 Unless required by applicable law or agreed to in writing, software
 distributed under the License is distributed on an "AS IS" BASIS,
 WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 See the License for the specific language governing permissions and
 limitations under the License.
 */

#include "ch.h"
#include "hal.h"
#include "usbcfg.h"
#include "config.h"
#include "usb_audio.h"

/*
 * Serial over USB Driver structure.
 */
MidiUSBDriver MDU1;
BulkUSBDriver BDU1;


/*
 * USB Device Descriptor.
 */
static const uint8_t vcom_device_descriptor_data[18] = {
  USB_DESC_DEVICE       (0x0200,        /* bcdUSB (2.0).                    */
                         0xEF,          /* bDeviceClass (Misc).             */
                         0x02,          /* bDeviceSubClass.                 */
                         0x01,          /* bDeviceProtocol. (IAD)           */
                         0x40,          /* bMaxPacketSize.                  */
                         0xF055,        /* idVendor (FOSS).                 */
                         0x57C0,        /* idProduct.                       */
                         0x0202,        /* bcdDevice (2.02: USB audio 2.0. A
                                           new value is a new hardware ID
                                           REV_xxxx on Windows, so it binds the
                                           function's driver afresh instead of
                                           from its cache). */
                         1,             /* iManufacturer.                   */
                         2,             /* iProduct.                        */
                         3,             /* iSerialNumber.                   */
                         1)             /* bNumConfigurations.              */
};

/*
 * Device Descriptor wrapper.
 */
static const USBDescriptor vcom_device_descriptor = {
  sizeof vcom_device_descriptor_data,
  vcom_device_descriptor_data
};

/*
 * Configuration: 5 interfaces in 3 functions.
 *   IAD 0..1 MIDI:   0 AudioControl (no audio streaming), 1 MIDIStreaming (EP 0x01/0x81 bulk)
 *   IAD 2    vendor: 2 bulk console/stream (EP 0x02/0x82, WinUSB via the MS OS descriptors)
 *   IAD 3..4 audio:  3 AudioControl, 4 AudioStreaming (EP 0x83 isochronous IN, asynchronous;
 *                    alt 0 zero bandwidth, alt 1 48 kHz 24 bit, alt 2 48 kHz 16 bit).
 *                    USB Audio Class 2.0.
 * The audio function is added after the existing ones so interfaces 0..2, their endpoints and
 * the MS OS descriptors (which name interface 2) are unchanged. The MIDI function stays a USB
 * Audio 1.0 MIDIStreaming function (Windows: usbaudio.sys, macOS/Linux: their USB
 * MIDI class drivers), as on other UAC2 devices with MIDI.
 * Checked by tools/usbaudio/check_descriptors.py; documented in doc/usb_audio.md.
 */
#define UA_CONFIG_TOTAL_LENGTH  304
/* AC class-specific descriptors: header 9 + clock source 8 + input terminal 17 + output 12 */
#define UA2_AC_TOTAL            46
#define UA_AS_ALT(alt, subslot, bits, maxpacket)                                \
  /* Interface 4, alternate setting alt: one isochronous endpoint, IP 2.0. */ \
  USB_DESC_INTERFACE(UA_AS_INTERFACE, alt, 0x01,                              \
                     0x01,      /* bInterfaceClass (Audio).                */ \
                     0x02,      /* bInterfaceSubClass (AudioStreaming).    */ \
                     0x20, 0),  /* bInterfaceProtocol (IP_VERSION_02_00).  */ \
  /* Class-specific AS General (ADC-2 4.9.2): terminal link: the USB       \
     streaming output terminal, no controls, Type I, PCM, 2 channels       \
     (front left, front right). */                                          \
  0x10, 0x24, 0x01, UA2_OT_ID, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x02,    \
  0x03, 0x00, 0x00, 0x00, 0x00,                                               \
  /* Type I format (FMT-2 2.3.1.6): subslot bytes, bit resolution. */        \
  0x06, 0x24, 0x02, 0x01, subslot, bits,                                      \
  /* Standard AS isochronous audio data endpoint (ADC-2 4.10.1.1, USB 2.0   \
     9.6.6): 0x83 IN, isochronous asynchronous data, 49 frames max, every   \
     frame (full speed bInterval 1 = 1 ms). No feedback endpoint: an IN    \
     stream's rate is in its packet sizes. */                               \
  0x07, 0x05, 0x80 | UA_EP, 0x05, USB_DESC_WORD(maxpacket), 0x01,           \
  /* Class-specific AS isochronous endpoint (ADC-2 4.10.1.2): no           \
     MaxPacketsOnly, no controls, no lock delay. */                         \
  0x08, 0x25, 0x01, 0x00, 0x00, 0x00, USB_DESC_WORD(0x0000)

static const uint8_t vcom_configuration_descriptor_data[]=
{
 /* Configuration Descriptor.*/
 USB_DESC_CONFIGURATION(UA_CONFIG_TOTAL_LENGTH, /* wTotalLength.            */
                        0x05,          /* bNumInterfaces.                  */
                        0x01,          /* bConfigurationValue.             */
                        0,             /* iConfiguration.                  */
                        0xC0,          /* bmAttributes (self powered).     */
                        50),           /* bMaxPower (100mA).               */
 /* Interface Association Descriptor.*/
 USB_DESC_INTERFACE_ASSOCIATION(0x00, /* bFirstInterface.                  */
                                0x02, /* bInterfaceCount.                  */
                                0x01, /* bFunctionClass (Audio).           */
                                0x03, /* bFunctionSubClass.                */
                                0x00, /* bFunctionProcotol                 */
                                0),   /* iInterface.                       */
  0x09, 0x04, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00, // Interface 0              INTERFACE DESC (bLength bDescType bInterfaceNumber bAltSetting bNumEndpoints bInterfaceClass bInterfaceSubClass bInterfaceProtocol iInterface)
  0x09, 0x24, 0x01, 0x00, 0x01, 0x09, 0x00, 0x01, 0x01, // CS Interface (audio)     CLASS SPECIFIC AC INTERFACE DESC
  0x09, 0x04, 0x01, 0x00, 0x02, 0x01, 0x03, 0x00, 0x00, // Interface 1              INTERFACE DESC (bLength bDescType bInterfaceNumber bAltSetting bNumEndpoints bInterfaceClass bInterfaceSubClass bInterfaceProtocol iInterface)
  0x07, 0x24, 0x01, 0x00, 0x01, 0x41, 0x00,             // CS Interface (midi)      CLASS SPECIFIC MS INTERFACE DESC
  0x06, 0x24, 0x02, 0x01, 0x01, 0x00,                   //   IN  Jack 1 (emb)       MIDI IN JACK DESC (bLength bDescType bDescSubType bJackType bJackID iJack)
  0x06, 0x24, 0x02, 0x02, 0x02, 0x00,                   //   IN  Jack 2 (ext)       MIDI IN JACK DESC (bLength bDescType bDescSubType bJackType bJackID iJack)
  0x09, 0x24, 0x03, 0x01, 0x03, 0x01, 0x02, 0x01, 0x00, //   OUT Jack 3 (emb)       MIDI OUT JACK DESC (bLength bDescType bDescSubType bJackType bJackID bNrInputPins baSourceID(1) baSourceID(1) iJack)
  0x09, 0x24, 0x03, 0x02, 0x04, 0x01, 0x01, 0x01, 0x00, //   OUT Jack 4 (ext)       MIDI OUT JACK DESC (bLength bDescType bDescSubType bJackType bJackID bNrInputPins baSourceID(1) baSourceID(1) iJack)
  0x09, 0x05, 0x01, 0x02, 0x40, 0x00, 0x00, 0x00, 0x00, // Endpoint OUT             ENDPOINT DESC  (bLength bDescType bEndpointAddr bmAttr wMaxPacketSize(2 bytes)  bInterval bRefresh bSyncAddress)
  0x05, 0x25, 0x01, 0x01, 0x01,                         //   CS EP IN  Jack         CLASS SPECIFIC MS BULK DATA EP DESC
  0x09, 0x05, 0x81, 0x02, 0x40, 0x00, 0x00, 0x00, 0x00, // Endpoint IN              ENDPOINT DESC  (bLength bDescType bEndpointAddr bmAttr wMaxPacketSize(2 bytes)  bInterval bRefresh bSyncAddress)
  0x05, 0x25, 0x01, 0x01, 0x03,                         //   CS EP OUT Jack          CLASS SPECIFIC MS BULK DATA EP DESC

  /* Interface Association Descriptor.*/
  USB_DESC_INTERFACE_ASSOCIATION(0x02, /* bFirstInterface.                  */
                              0x01, /* bInterfaceCount.                  */
                              0xFF, /* bFunctionClass (Vendor Specific).  */
                              0x00, /* bFunctionSubClass.                */
                              0x00, /* bFunctionProcotol                 */
                              4),   /* iInterface.                       */
  /* Interface Descriptor.*/
  USB_DESC_INTERFACE    (0x02,          /* bInterfaceNumber.                */
                         0x00,          /* bAlternateSetting.               */
                         0x02,          /* bNumEndpoints.                   */
                         0xFF,          /* bInterfaceClass (Vendor Specific). */
                         0x00,
                         0x00,
                         4),         /* iInterface.                      */
  /* Endpoint 2 Descriptor.*/
  USB_DESC_ENDPOINT     (USBD2_DATA_AVAILABLE_EP,       /* bEndpointAddress.*/
                         0x02,          /* bmAttributes (Bulk).             */
                         0x0040,        /* wMaxPacketSize.                  */
                         0x00),         /* bInterval.                       */
  /* Endpoint 2 Descriptor.*/
  USB_DESC_ENDPOINT     (USBD2_DATA_REQUEST_EP|0x80,    /* bEndpointAddress.*/
                         0x02,          /* bmAttributes (Bulk).             */
                         0x0040,        /* wMaxPacketSize.                  */
                         0x00),         /* bInterval.                       */

  /* Interface Association Descriptor: USB audio 2.0 (synth to host).*/
  USB_DESC_INTERFACE_ASSOCIATION(UA_AC_INTERFACE, /* bFirstInterface.       */
                                 0x02, /* bInterfaceCount.                  */
                                 0x01, /* bFunctionClass (Audio).           */
                                 0x00, /* bFunctionSubClass (undefined, as
                                          ADC-2 4.6 requires).              */
                                 0x20, /* bFunctionProtocol (AF_VERSION_02_00):
                                          Windows binds usbaudio2.sys on
                                          Class_01&SubClass_00&Prot_20.     */
                                 5),   /* iFunction ("Striso Synth").       */
  /* Interface 3: AudioControl, IP 2.0, no endpoints (no interrupt endpoint:
     nothing to notify).*/
  USB_DESC_INTERFACE    (UA_AC_INTERFACE, /* bInterfaceNumber.              */
                         0x00,          /* bAlternateSetting.               */
                         0x00,          /* bNumEndpoints.                   */
                         0x01,          /* bInterfaceClass (Audio).         */
                         0x01,          /* bInterfaceSubClass (AudioControl). */
                         0x20,          /* bInterfaceProtocol (IP 2.0).     */
                         5),            /* iInterface ("Striso Synth").     */
  /* Class-specific AC Interface Header (ADC-2 4.7.2): bcdADC 2.00, category
     musical instrument (0x09), wTotalLength 46, no latency control.*/
  0x09, 0x24, 0x01, USB_DESC_WORD(0x0200), 0x09, USB_DESC_WORD(UA2_AC_TOTAL), 0x00,
  /* Clock Source (ADC-2 4.7.2.1): ID 3, internal fixed clock (the codec's
     48 kHz PLL), sampling frequency control read only, validity control
     read only, no associated terminal, no name.*/
  0x08, 0x24, 0x0A, UA2_CLOCK_ID, 0x01, 0x05, 0x00, 0x00,
  /* Input Terminal (ADC-2 4.7.2.4): ID 1, Line connector (0x0603), clocked by the clock source, 2 channels (front left, front
     right), no controls, iTerminal 5.*/
  0x11, 0x24, 0x02, UA2_IT_ID, USB_DESC_WORD(0x0603), 0x00, UA2_CLOCK_ID, 0x02,
  0x03, 0x00, 0x00, 0x00, 0x00, USB_DESC_WORD(0x0000), 5,
  /* Output Terminal (ADC-2 4.7.2.5): ID 2, USB streaming (0x0101), source:
     the input terminal, clocked by the clock source, no controls.*/
  0x0C, 0x24, 0x03, UA2_OT_ID, USB_DESC_WORD(0x0101), 0x00, UA2_IT_ID, UA2_CLOCK_ID,
  USB_DESC_WORD(0x0000), 0x00,
  /* Interface 4, alternate setting 0: zero bandwidth.*/
  USB_DESC_INTERFACE    (UA_AS_INTERFACE, /* bInterfaceNumber.              */
                         0x00,          /* bAlternateSetting.               */
                         0x00,          /* bNumEndpoints.                   */
                         0x01,          /* bInterfaceClass (Audio).         */
                         0x02,          /* bInterfaceSubClass (AudioStreaming). */
                         0x20,          /* bInterfaceProtocol (IP 2.0).     */
                         0),            /* iInterface.                      */
  /* alt 1: 24 bit in 3 byte subframes (subslots), 49 * 2 * 3 = 294 bytes max.*/
  UA_AS_ALT(UA_ALT_24BIT, 3, 24, 294),
  /* alt 2: 16 bit, 49 * 2 * 2 = 196 bytes max.*/
  UA_AS_ALT(UA_ALT_16BIT, 2, 16, 196)
};

_Static_assert(sizeof(vcom_configuration_descriptor_data) == UA_CONFIG_TOTAL_LENGTH,
               "wTotalLength must match the configuration descriptor");

/*
 * Configuration Descriptor wrapper.
 */
static const USBDescriptor vcom_configuration_descriptor = {
  sizeof vcom_configuration_descriptor_data,
  vcom_configuration_descriptor_data
};

/*
 * U.S. English language identifier.
 */
static const uint8_t vcom_string0[] = {
  USB_DESC_BYTE(4),                     /* bLength.                         */
  USB_DESC_BYTE(USB_DESCRIPTOR_STRING), /* bDescriptorType.                 */
  USB_DESC_WORD(0x0409)                 /* wLANGID (U.S. English).          */
};

/*
 * Manufacturer string.
 */
static const uint8_t descriptor_manufacturer_string[] = {
  USB_DESC_BYTE(14),                    /* bLength.                         */
  USB_DESC_BYTE(USB_DESCRIPTOR_STRING), /* bDescriptorType.                 */
  'S', 0, 't', 0, 'r', 0, 'i', 0, 's', 0, 'o', 0,
};

/*
 * Product string.
 * modified on initialization: init_usb_descriptor() adds the board number (up to 4 characters)
 * and sets bLength and the size in vcom_strings[] together.
 */
static uint8_t descriptor_product_string[] = {
  USB_DESC_BYTE(26),                    /* bLength.                         */
  USB_DESC_BYTE(USB_DESCRIPTOR_STRING), /* bDescriptorType.                 */
  'S', 0, 't', 0, 'r', 0, 'i', 0, 's', 0, 'o', 0, ' ', 0,
  'b', 0, 'o', 0, 'a', 0, 'r', 0, 'd', 0, ' ', 0,
  ' ', 0, ' ', 0, ' ', 0, ' ', 0,
};
_Static_assert(sizeof descriptor_product_string == 28 + 2 * 4,
               "the product string has room for a board number of 4 characters, no more");

static uint8_t descriptor_serial_string[] = {
  USB_DESC_BYTE(50),                    /* bLength.                         */
  USB_DESC_BYTE(USB_DESCRIPTOR_STRING), /* bDescriptorType.                 */
  '0', 0, '0', 0, '0', 0, '0', 0,
  '0', 0, '0', 0, '0', 0, '0', 0,
  '0', 0, '0', 0, '0', 0, '0', 0,
  '0', 0, '0', 0, '0', 0, '0', 0,
  '0', 0, '0', 0, '0', 0, '0', 0,
  '0', 0, '0', 0, '0', 0, '0', 0
};

/*
 * Device Description string.
 */
static const uint8_t vcom_string4[] = {
  USB_DESC_BYTE(44),                    /* bLength.                         */
  USB_DESC_BYTE(USB_DESCRIPTOR_STRING), /* bDescriptorType.                 */
  'S', 0, 't', 0, 'r', 0, 'i', 0, 's', 0, 'o', 0, ' ', 0,
  'B', 0, 'u', 0, 'l', 0, 'k', 0, ' ', 0,
  'I', 0, 'n', 0, 't', 0, 'e', 0, 'r', 0, 'f', 0, 'a', 0, 'c', 0, 'e', 0,
};

/*
 * Audio function string (IAD iFunction, AudioControl iInterface, input terminal iTerminal).
 */
static const uint8_t vcom_string5[] = {
  USB_DESC_BYTE(26),                    /* bLength.                         */
  USB_DESC_BYTE(USB_DESCRIPTOR_STRING), /* bDescriptorType.                 */
  'S', 0, 't', 0, 'r', 0, 'i', 0, 's', 0, 'o', 0, ' ', 0,
  'S', 0, 'y', 0, 'n', 0, 't', 0, 'h', 0,
};

/* WCID implementation reference:
 *  https://github.com/pbatard/libwdi/wiki/WCID-Devices
 */
static const uint8_t wcid_string[] = {
  USB_DESC_BYTE(18),                    /* bLength.                         */
  USB_DESC_BYTE(USB_DESCRIPTOR_STRING), /* bDescriptorType.                 */
  'M', 0, 'S', 0, 'F', 0, 'T', 0, '1', 0, '0', 0, '0', 0, /* MSFT100        */
  20, 0x00 /* vendor code, padding */
};

/*
 * Strings wrappers array. The size is what get_descriptor() hands out: the descriptor's bLength
 * (the product string's changes at boot, init_usb_descriptor).
 */
static USBDescriptor vcom_strings[] = {
  {sizeof vcom_string0, vcom_string0},
  {sizeof descriptor_manufacturer_string, descriptor_manufacturer_string},
  {26, descriptor_product_string},
  {sizeof descriptor_serial_string, descriptor_serial_string},
  {sizeof vcom_string4, vcom_string4},
  {sizeof vcom_string5, vcom_string5}
};

static const USBDescriptor wcid_descriptor = {sizeof wcid_string, wcid_string};

void inttohex(uint32_t v, unsigned char *p){
  int nibble;
  for (nibble = 0;nibble<8;nibble++){
    unsigned char c = (v>>(28-nibble*4))&0xF;
    if (c<10) c=c+'0';
    else c=c+'A'-10;
    *p = c;
    p += 2;
  }
}

/*
 * Initialize the device specific parts of the descriptor strings.
 */
void init_usb_descriptor(void) {
  uint32_t* const UID = (uint32_t*)UID_BASE;

  // use microcontroller unique ID as serial number (little-endian)
  inttohex(UID[2],&descriptor_serial_string[2]);
  inttohex(UID[1],&descriptor_serial_string[2+16]);
  inttohex(UID[0],&descriptor_serial_string[2+32]);

  // add Striso board number to product string (if available)
  if (devspec_id->UID[0] == UID[0] &&
      devspec_id->UID[1] == UID[1] &&
      devspec_id->UID[2] == UID[2]) {
    int n;
    for (n = 0; n < 4 && devspec_id->id[n] >= 33 && devspec_id->id[n] < 128; n++) {
      descriptor_product_string[28 + 2*n] = devspec_id->id[n];
    }
    descriptor_product_string[0] = USB_DESC_BYTE(28 + 2*n);
    vcom_strings[2].ud_size = 28 + 2*n;
  }
}

/*
 * Handles the GET_DESCRIPTOR callback. All required descriptors must be
 * handled here.
 */
static const USBDescriptor *get_descriptor(USBDriver *usbp,
                                           uint8_t dtype,
                                           uint8_t dindex,
                                           uint16_t lang) {
  (void)usbp;
  (void)lang;
  switch (dtype) {
  case USB_DESCRIPTOR_DEVICE:
    return &vcom_device_descriptor;
  case USB_DESCRIPTOR_CONFIGURATION:
    return &vcom_configuration_descriptor;
  case USB_DESCRIPTOR_STRING:
    if (dindex < sizeof vcom_strings / sizeof vcom_strings[0])
      return &vcom_strings[dindex];
    if (dindex == 0xEE)
      return &wcid_descriptor;
    break;
  }
  return NULL;
}

/**
 * @brief   IN EP1 state.
 */
static USBInEndpointState ep1instate;

/**
 * @brief   OUT EP1 state.
 */
static USBOutEndpointState ep1outstate;

/**
 * @brief   EP1 initialization structure (both IN and OUT).
 */
static const USBEndpointConfig ep1config = {
  USB_EP_MODE_TYPE_BULK,
  NULL,
  mduDataTransmitted,
  mduDataReceived,
  0x0040,
  0x0040,
  &ep1instate,
  &ep1outstate,
  1,
  NULL
};

/**
 * @brief   IN EP2 state.
 */
static USBInEndpointState ep2instate;

/**
 * @brief   OUT EP2 state.
 */
static USBOutEndpointState ep2outstate;

/**
 * @brief   EP2 initialization structure (both IN and OUT).
 */
static const USBEndpointConfig ep2config = {
  USB_EP_MODE_TYPE_BULK,
  NULL,
  bduDataTransmitted,
  bduDataReceived,
  0x0040,
  0x0040,
  &ep2instate,
  &ep2outstate,
  1,
  NULL
};

/*
 * After a USB suspend and a resume without a bus reset (host sleep, selective suspend): the OTG
 * LLD's suspend handling (otg_disable_ep) disabled every endpoint's transfer and masked all
 * endpoint interrupts but EP0's, and its wakeup handling restores neither. Unmask the interrupts
 * of every configured endpoint and flush the TX FIFOs of the IN transfers it aborted (the data
 * left in them would be sent before the next transfer's); the drivers' wakeup hooks then re-arm
 * their transfers. Only when the masks show that a suspend disabled the endpoints.
 */
static void usb_resume_endpointsI(USBDriver *usbp) {
  stm32_otg_t *otgp = usbp->otg;
  uint32_t mask = 0;

  if (usbp->state != USB_ACTIVE)
    return;
  for (usbep_t ep = 1; ep <= USB_MAX_ENDPOINTS; ep++) {
    if (usbp->epc[ep] == NULL)
      continue;
    if (usbp->epc[ep]->in_state != NULL)
      mask |= DAINTMSK_IEPM(ep);
    if (usbp->epc[ep]->out_state != NULL)
      mask |= DAINTMSK_OEPM(ep);
  }
  if ((otgp->DAINTMSK & mask) == mask)
    return;  // not disabled by a suspend
  for (usbep_t ep = 1; ep <= USB_MAX_ENDPOINTS; ep++) {
    if (usbp->epc[ep] == NULL || usbp->epc[ep]->in_state == NULL)
      continue;
    otgp->GRSTCTL = GRSTCTL_TXFNUM(ep) | GRSTCTL_TXFFLSH;
    while ((otgp->GRSTCTL & GRSTCTL_TXFFLSH) != 0)
      ;
    osalSysPolledDelayX(18);  // 3 PHY clocks, as the LLD's otg_txfifo_flush
  }
  otgp->DAINTMSK |= mask;
}

/*
 * Handles the USB driver global events.
 */
static void usb_event(USBDriver *usbp, usbevent_t event) {

  switch (event) {
  case USB_EVENT_ADDRESS:
    return;
  case USB_EVENT_CONFIGURED:
    chSysLockFromISR();
    if (usbp->state == USB_ACTIVE) {

      /* Enables the endpoints specified into the configuration.
        Note, this callback is invoked from an ISR so I-Class functions
        must be used.*/
      usbInitEndpointI(usbp, USBD1_DATA_REQUEST_EP, &ep1config);
      usbInitEndpointI(usbp, USBD2_DATA_REQUEST_EP, &ep2config);

      bduConfigureHookI(&BDU1);
      mduConfigureHookI(&MDU1);
      usb_audio_configured_hookI(usbp);
    } else if (usbp->state == USB_SELECTED) {
      usbDisableEndpointsI(usbp);
    }
    chSysUnlockFromISR();
    return;
  case USB_EVENT_RESET:
    /* GET_STATUS(device) answers usbp->status, which the reset cleared: report self powered
       (D0), as the configuration descriptor's bmAttributes (0xC0) says.*/
    usbp->status |= 1U;
    /* falls through */
  case USB_EVENT_UNCONFIGURED:
    /* Falls into.*/
  case USB_EVENT_SUSPEND:
    chSysLockFromISR();
    /* Disconnection event on suspend.*/
    bduSuspendHookI(&BDU1);
    mduSuspendHookI(&MDU1);
    if (event == USB_EVENT_SUSPEND)
      usb_audio_suspend_hookI();
    else
      usb_audio_reset_hookI();

    chSysUnlockFromISR();
    return;
  case USB_EVENT_WAKEUP:
    chSysLockFromISR();
    /* Connection event on wakeup.*/
    usb_resume_endpointsI(usbp);
    bduWakeupHookI(&BDU1);
    mduWakeupHookI(&MDU1);
    usb_audio_wakeup_hookI(usbp);

    chSysUnlockFromISR();
    return;
  case USB_EVENT_STALLED:
    return;
  }
  return;
}

static const uint8_t mscompatid[] = {
0x40, 0x00, 0x00, 0x00,  /* DWORD (LE)  Descriptor length (64 bytes) */
0x00, 0x01,  /* BCD WORD (LE)   Version ('1.0') */
0x04, 0x00,  /* WORD (LE)   Compatibility ID Descriptor index (0x0004) */
0x02,    /* BYTE    Number of sections (2) */
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,    /* 7 BYTES     Reserved */
0x00,    /* BYTE    Interface Number (Interface #0) */
0x01,    /* BYTE    Reserved */
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* 8 BYTES (NUL-terminated?) ASCII String    Compatible ID (null) */
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* 8 BYTES (NUL-terminated?) ASCII String    Sub-Compatible ID (unused) */
0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* 6 BYTES      Reserved */
0x02,    /* BYTE    Interface Number (Interface #2) */
0x01,    /* BYTE    Reserved */
0x57, 0x49, 0x4E, 0x55, 0x53, 0x42, 0x00, 0x00,  /* 8 BYTES (NUL-terminated?) ASCII String    Compatible ID ("WINUSB\0\0") */
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* 8 BYTES (NUL-terminated?) ASCII String    Sub-Compatible ID (unused) */
0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* 6 BYTES      Reserved */
};


static const uint8_t msdescriptor0[] = {
0x0A, 0x00, 0x00, 0x00,  /* Descriptor length (10 bytes) */
0x00, 0x01,  /*Version ('1.0') */
0x05, 0x00, /*  WORD (LE)   Descriptor index (5) */
0x00, 0x00  /*  WORD (LE)   Number of sections (0) */
};

static const uint8_t msdescriptor1[] = {
0x94, 0x00, 0x00, 0x00,  /* Descriptor length (146 bytes) */
0x00, 0x01,  /*Version ('1.0') */
0x05, 0x00, /*  WORD (LE)   Descriptor index (5) */
0x01, 0x00, /*  WORD (LE)   Number of sections (1) */
0x8A, 0x00, 0x00, 0x00,  /* DWORD (LE)  Size of the property section (136 bytes) */
0x07, 0x00, 0x00, 0x00,  /* DWORD (LE)  Property data type (7 = Unicode REG_MULTI_SZ, */
0x2a, 0x00, /*  WORD (LE)   Property name length (42 bytes) */
0x44, 0x00, 0x65, 0x00, 0x76, 0x00, 0x69, 0x00,
0x63, 0x00, 0x65, 0x00, 0x49, 0x00, 0x6E, 0x00,
0x74, 0x00, 0x65, 0x00, 0x72, 0x00, 0x66, 0x00,
0x61, 0x00, 0x63, 0x00, 0x65, 0x00, 0x47, 0x00,
0x55, 0x00, 0x49, 0x00, 0x44, 0x00, 0x73, 0x00, 0x00, 0x00, /* NUL-terminated Unicode String (LE) Property name "DeviceInterfaceGUIDs" */
0x50, 0x00, 0x00, 0x00,  /* DWORD (LE)  Property data length (80 bytes) */
0x7B, 0x00, 0x38, 0x00, 0x38, 0x00, 0x42, 0x00,
0x41, 0x00, 0x45, 0x00, 0x30, 0x00, 0x33, 0x00,
0x32, 0x00, 0x2D, 0x00, 0x35, 0x00, 0x41, 0x00,
0x38, 0x00, 0x31, 0x00, 0x2D, 0x00, 0x34, 0x00,
0x39, 0x00, 0x66, 0x00, 0x30, 0x00, 0x2D, 0x00,
0x42, 0x00, 0x43, 0x00, 0x33, 0x00, 0x44, 0x00,
0x2D, 0x00, 0x41, 0x00, 0x34, 0x00, 0x46, 0x00,
0x46, 0x00, 0x31, 0x00, 0x33, 0x00, 0x38, 0x00,
0x32, 0x00, 0x31, 0x00, 0x36, 0x00, 0x44, 0x00,
0x36, 0x00, 0x7D, 0x00, 0x00, 0x00, 0x00, 0x00
/* double NUL-terminated Unicode String (LE) Property name "{88BAE032-5A81-49f0-BC3D-A4FF138216D6}" */
};

static bool specialRequestsHook(USBDriver *usbp) {
  if (
      (usbp->setup[0] == 0xC0) &&
      (usbp->setup[1] == 0x14) &&
      (usbp->setup[2] == 0x00) &&
      (usbp->setup[3] == 0x00) &&
      (usbp->setup[4] == 0x04)
    ) {
    usbSetupTransfer(usbp, (uint8_t *)&mscompatid, usbp->setup[6], NULL);
    return TRUE;
  } else   if (
      (usbp->setup[0] == 0xC1) &&
      (usbp->setup[1] == 0x14) &&
      (usbp->setup[2] == 0x00) &&
      (usbp->setup[3] == 0x00) &&
      (usbp->setup[4] == 0x05)
    ) {
    usbSetupTransfer(usbp, (uint8_t *)&msdescriptor0, usbp->setup[6], NULL);
    return TRUE;
  } else   if (
      (usbp->setup[0] == 0xC1) &&
      (usbp->setup[1] == 0x14) &&
      (usbp->setup[2] == 0x02) &&
      (usbp->setup[3] == 0x00) &&
      (usbp->setup[4] == 0x05)
    ) {
    usbSetupTransfer(usbp, (uint8_t *)&msdescriptor1, usbp->setup[6], NULL);
    return TRUE;
  }

  /* USB audio: SET/GET_INTERFACE of interfaces 3 and 4, sampling frequency.*/
  return usb_audio_requests_hook(usbp);
}

/*
 * Handles the USB driver global events.
 */
static void sof_handler(USBDriver *usbp) {

  osalSysLockFromISR();
  bduSOFHookI(&BDU1);
  mduSOFHookI(&MDU1);
  usb_audio_sof_hookI(usbp);
  osalSysUnlockFromISR();
}

/*
 * USB driver configuration.
 */
const USBConfig usbcfg = {
  usb_event,
  get_descriptor,
  specialRequestsHook,
  sof_handler
};

/*
 * Midi USB driver configuration.
 */
const MidiUSBConfig midiusbcfg = {
  &USBD1,
  USBD1_DATA_REQUEST_EP,
  USBD1_DATA_AVAILABLE_EP
};

/*
 * Bulk USB driver configuration.
 */
const BulkUSBConfig bulkusbcfg = {
  &USBD1,
  USBD2_DATA_REQUEST_EP,
  USBD2_DATA_AVAILABLE_EP
};
