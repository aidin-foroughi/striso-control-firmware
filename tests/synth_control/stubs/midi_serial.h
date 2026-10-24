/* Host stand-in for src/drivers/midi_serial.h (tests/synth_control) */
#include <stdint.h>
extern "C" {
void serial_MidiSend1(uint8_t b0);
void serial_MidiSend2(uint8_t b0, uint8_t b1);
void serial_MidiSend3(uint8_t b0, uint8_t b1, uint8_t b2);
}
