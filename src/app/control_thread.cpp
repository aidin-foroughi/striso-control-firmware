/*
 * The control thread ("send messages"): it owns the key handling. Each event from the inbox
 * (messaging.cpp) goes to synth_message (the voices, the settings keys, MIDI in and out over USB),
 * then, unless it was a request to this thread (Source::internal), to the binary stream as a frame
 * (wire.hpp).
 */
#include <array>
#include <cstddef>
#include <cstdint>

#include "ch.h"
#include "hal.h"

// C headers: their variables (config, BDU1) have C linkage
extern "C" {
#include "config.h"
#include "synth.h"
#include "usbcfg.h"
}
#include "messaging.h"
#include "messaging.hpp"
#include "wire.hpp"

namespace {

// synth_message's layout: src, id, then the values
void to_synth(const KeyEvent& e) {
	std::array<int, 2 + KeyEvent::kMaxValues> msg{};
	msg[0] = static_cast<int>(e.src);
	msg[1] = e.id;
	for (std::size_t c = 0; c < e.n; c++) msg[2 + c] = e.v[c];
	(void)synth_message(2 + e.n, msg.data());
}

void to_stream(const KeyEvent& e) {
	std::array<uint8_t, wire::kMaxFrame> frame{};
	const std::size_t len = wire::encode(e, frame);
#ifdef USE_UART
	chSequentialStreamWrite((BaseSequentialStream*)&SD1, frame.data(), len);
#endif
#ifdef USE_USB
	if (config.send_usb_bulk) {
		// whole frames only, never mixed into a console write (bulk_usb.c bduWriteFrame)
		(void)bduWriteFrame(&BDU1, frame.data(), len);
	}
#endif
	(void)len;
}

// the key thread: synth_message, the settings keys (load_preset), MIDI in, MIDI out over USB.
// Its deepest direct path (tools/stackcheck.py, the USB driver's indirect calls left out) is
// 564 bytes (500 at 3d6f498: 12 bytes to spare), so 1024 leaves a margin.
THD_WORKING_AREA(waControlThread, 1024);
void ControlThread(void* arg) {
	(void)arg;
	chRegSetThreadName("send messages");
	while (true) {
		const KeyEvent e = messaging::receive();
		to_synth(e);
		if (!wire::on_wire(e.src)) continue; // a request to this thread: done, not paced
		to_stream(e);
		// limit the stream to 2 messages per millisecond (with CH_FREQUENCY = 2000), for
		// compatibility with MIDI usb on Axoloti
		chThdSleep(1);
	}
}

} // namespace

extern "C" void control_thread_start(void) {
	chThdCreateStatic(waControlThread, sizeof(waControlThread), NORMALPRIO, ControlThread, nullptr);
}
