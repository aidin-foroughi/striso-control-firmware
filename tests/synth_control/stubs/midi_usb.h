/* Host stand-in for src/usb/midi_usb.h (tests/synth_control): the test logs what is sent */
#include <stdint.h>
extern "C" {
void midi_usb_MidiSend1(uint8_t port, uint8_t b0);
void midi_usb_MidiSend2(uint8_t port, uint8_t b0, uint8_t b1);
void midi_usb_MidiSend3(uint8_t port, uint8_t b0, uint8_t b1, uint8_t b2);
}
