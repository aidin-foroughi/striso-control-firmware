/**
 * USB audio output, asynchronous isochronous IN: the internal synth as a class compliant USB
 * sound card input, 48 kHz stereo, 24 bit (alt setting 1) or 16 bit (alt 2). The codec jack keeps
 * playing; USB gets the same samples (see usb_audio_core.h).
 *
 * USB Audio Class 2.0: on Windows 10 1703+ it loads usbaudio2.sys (WaveRT, the low latency path);
 * macOS (AppleUSBAudio) and Linux (snd-usb-audio) have class drivers. doc/usb_audio.md has the
 * descriptors and the reasoning.
 *
 * USB function layout (usbcfg.c): interfaces 0 (AudioControl) + 1 (MIDIStreaming) are the MIDI
 * function, 2 is the vendor bulk interface (WinUSB), 3 (AudioControl) + 4 (AudioStreaming) are
 * this audio function, with the isochronous IN endpoint 0x83.
 */
#ifndef USB_AUDIO_H
#define USB_AUDIO_H

#include <stdint.h>
#include <stdbool.h>

#define UA_AC_INTERFACE 3
#define UA_AS_INTERFACE 4
#define UA_EP 3
#define UA_ALT_24BIT 1
#define UA_ALT_16BIT 2
// UAC2 entity IDs: input terminal (the synth), output terminal (USB streaming), clock source
#define UA2_IT_ID 1
#define UA2_OT_ID 2
#define UA2_CLOCK_ID 3
// endpoint config packet size: the descriptor's 294 rounded up to whole FIFO words (the OTG LLD
// sizes the TX FIFO as in_maxsize / 4 words, so 294 would give 292 bytes and never fit a packet)
#define UA_EP_SIZE 296

#ifdef __cplusplus
extern "C" {
#endif

// synth thread: where to write this block's 32 stereo frames (int32, 24 bit << 8, interleaved
// L R); never NULL (the ring always holds the latest output); after writing call
// usb_audio_write_commit()
int32_t *usb_audio_write_ptr(void);
void usb_audio_write_commit(void);

// codec DMA interrupt, once per block (diagnostics: the synth's commit delay and lost blocks)
void usb_audio_codec_tickI(void);

// test tone instead of the synth on USB (the jack keeps the synth): console 'Stcu' toggles
void usb_audio_set_tone(bool on);
bool usb_audio_tone(void);

// ring target in frames (level kept before each packet; clamped to 49..256, default 96 = 2 ms):
// console 'Stct<3 digits>'. Takes effect at the next packet (a skip ahead, or a short priming
// silence when raised). Not saved: a reset goes back to the default.
int usb_audio_set_target(int frames);
int usb_audio_target(void);

// console 'StcU': stream state and statistics since the last call
void usb_audio_print_status(void *chp);

#ifdef HAL_USE_USB
// hooks for usbcfg.c (ISR context, called with the system locked except the requests hook)
void usb_audio_configured_hookI(USBDriver *usbp);
void usb_audio_reset_hookI(void);
void usb_audio_suspend_hookI(void);
void usb_audio_wakeup_hookI(USBDriver *usbp);
void usb_audio_sof_hookI(USBDriver *usbp);
bool usb_audio_requests_hook(USBDriver *usbp);
#endif

#ifdef __cplusplus
}
#endif

#endif /* USB_AUDIO_H */
