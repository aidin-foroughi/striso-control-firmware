/*
 * OutStream: the console's replies, a BaseSequentialStream (what chprintf and the firmware's print
 * functions write to) over a ChibiOS channel, on the board the bulk console BDU1. Its writes wait
 * at most `timeout` for room in the channel: if the host stops reading, the main thread, which
 * writes the replies, is held up for one timeout instead of forever (B14). After a wait that timed
 * out the rest of the reply is dropped at once; begin() starts the next reply with a fresh wait.
 *
 * The channel's writes are its timed ones (putt, writet); on the bulk console they keep the
 * driver's rule for ThreadSend's binary frames (bulk_usb.c bduWriteFrame: held back during a
 * console write, so text never splits a frame).
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "hal.h"

// Chn: a channel type (a struct with a vmt that has putt and writet), e.g. BulkUSBDriver
template <class Chn>
class OutStream {
public:
	constexpr OutStream(Chn* chn, sysinterval_t timeout) :
	    base_{&kVmt}, chn_(chn), timeout_(timeout) {}

	// for chprintf and the print functions (their void* or BaseSequentialStream* argument)
	BaseSequentialStream* stream() { return &base_; }

	// a new reply: wait for room again
	void begin() { stalled_ = false; }
	// the current reply was cut short (a write timed out, or the USB was reset or suspended)
	[[nodiscard]] bool stalled() const { return stalled_; }
	// replies cut short since boot
	[[nodiscard]] uint32_t timeouts() const { return timeouts_; }

	// not inlined: the channel's method is called through its vmt here and only here (the build's
	// stack check is told where these indirect calls go, cmake/firmware_checks.cmake)
	__attribute__((noinline)) size_t write(const uint8_t* bp, size_t n) {
		if (stalled_ || n == 0) return 0;
		const size_t w = chnWriteTimeout(chn_, bp, n, timeout_);
		if (w < n) stall();
		return w;
	}

	__attribute__((noinline)) msg_t put(uint8_t b) {
		if (stalled_) return MSG_TIMEOUT;
		const msg_t m = chnPutTimeout(chn_, b, timeout_);
		if (m != MSG_OK) stall();
		return m;
	}

private:
	void stall() {
		stalled_ = true;
		timeouts_++;
	}

	// base_ is the first member of this standard-layout class: the stream's methods get its
	// address, which is the OutStream's
	static OutStream* self(void* ip) { return reinterpret_cast<OutStream*>(ip); }
	static size_t vmt_write(void* ip, const uint8_t* bp, size_t n) { return self(ip)->write(bp, n); }
	static size_t vmt_read(void* /*ip*/, uint8_t* /*bp*/, size_t /*n*/) { return 0; }
	static msg_t vmt_put(void* ip, uint8_t b) { return self(ip)->put(b); }
	static msg_t vmt_get(void* /*ip*/) { return MSG_RESET; }
	static constexpr BaseSequentialStreamVMT kVmt = {0, &vmt_write, &vmt_read, &vmt_put, &vmt_get};

	BaseSequentialStream base_;
	Chn* chn_;
	sysinterval_t timeout_;
	bool stalled_ = false;
	uint32_t timeouts_ = 0;
};
