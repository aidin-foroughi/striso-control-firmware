/* The virtual board's hardware without a model: the codec (its block interrupt drives the synth),
 * the aux jack (nothing plugged in), serial MIDI out (the jack is not in MIDI mode), ws2812 (not
 * on the board). */
#include "aux_jack.h"
#include "codec.h"
#include "config.h"
#include "hal.h"
#include "midi_serial.h"
#include "sv_host.h"
#include "usb_audio.h"
#include "ws2812.h"

/* --- codec: the SAI DMA double buffers; each block interrupt hands the synth the next half ---------- */
int32_t buf[PLAYBACK_BUFFER_SIZE];
int32_t buf2[PLAYBACK_BUFFER_SIZE];
int32_t rbuf[PLAYBACK_BUFFER_SIZE];
int32_t rbuf2[PLAYBACK_BUFFER_SIZE];
static int codec_running, codec_half;

void codec_init(uint16_t samplerate) {
	(void)samplerate;
	codec_running = 1;
}
void codec_stop(void) { codec_running = 0; }
void codec_clearbuffer(void) {}
void codec_linein_enable(void) {}
void codec_linein_disable(void) {}
int svh_codec_running(void) { return codec_running; }

void svh_codec_interrupt(void) {
	usb_audio_codec_tickI();
	codec_half ^= 1;
	computebufI(codec_half ? rbuf2 : rbuf, codec_half ? buf2 : buf);
}

/* --- aux jack: nothing plugged in; its thread waits (as on the board without a jack) ------------------ */
static THD_WORKING_AREA(waAuxJack, 256);
static void AuxJack(void* arg) {
	(void)arg;
	chRegSetThreadName("auxjack");
	for (;;) chSchGoSleepS(CH_STATE_SUSPENDED);
}
void aux_jack_init(void) { chThdCreateStatic(waAuxJack, sizeof(waAuxJack), NORMALPRIO, AuxJack, NULL); }
void aux_jack_switch_mode_setting(jack2_mode_t mode) { config.jack2_mode_setting = mode; }
void aux_power_enable(void) {}
void aux_power_disable(void) {}

/* --- serial MIDI: the jack is not a MIDI port here --------------------------------------------------- */
void serial_midi_enable(void) {}
void serial_midi_disable(void) {}
void serial_MidiSend1(uint8_t b0) { (void)b0; }
void serial_MidiSend2(uint8_t b0, uint8_t b1) {
	(void)b0;
	(void)b1;
}
void serial_MidiSend3(uint8_t b0, uint8_t b1, uint8_t b2) {
	(void)b0;
	(void)b1;
	(void)b2;
}
int serial_MidiGetOutputBufferPending(void) { return 0; }

/* --- ws2812: not on this board (USE_WS2812 is off) ------------------------------------------------------- */
void ws2812_init(void) {}
ws2812_err_t ws2812_write_led(uint32_t led_number, uint8_t r, uint8_t g, uint8_t b) {
	(void)led_number;
	(void)r;
	(void)g;
	(void)b;
	return WS2812_SUCCESS;
}
