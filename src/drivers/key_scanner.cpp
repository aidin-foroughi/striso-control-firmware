/*
 * The KeyScanner on the board: the scanner thread's calls (key_scanner.h), the message queue as
 * its sink, and the raw recordings (StcR, key_scanner_capture.hpp) with their low priority dump
 * over the bulk console.
 */
#include <cstring>

#include "ch.h"
#include "hal.h"

#include "chprintf.h"
#include "config.h"
#include "usbcfg.h"

extern "C" {
#include "messaging.h"
}

#include "key_scanner.h"
#include "key_scanner_capture.hpp"

scan_cal_t ks_cal[SCAN_BUTTONS];

namespace {

ks::KeyScanner scanner;
ScanFrame work; // the columns as the thread consumed them
volatile uint32_t cal_gen = 0;
uint32_t cal_seen = 0;

// --- raw recordings ------------------------------------------------------------------------
enum class Cap : uint8_t { idle,
	armed,
	running,
	done,
	dumping };
volatile Cap cap_state = Cap::idle;
volatile int cap_seconds = 0;
systime_t cap_t0 = 0;
ks::CaptureHeader cap_head;
uint8_t cap_header[ks::kHeaderBytes];
ks::RecordWriter rec;
uint32_t rec_overruns = 0;
int32_t cal_copy[SCAN_BUTTONS];

// the buffer: most of the free AXI SRAM and SRAM4 (D3), records don't straddle the two
constexpr size_t kBuf0 = 160 * 1024;
constexpr size_t kBuf1 = 63 * 1024;
uint8_t cap_buf0[kBuf0];
__attribute__((section(".ram4"))) uint8_t cap_buf1[kBuf1]; // not cleared at boot: no need
struct Region {
	uint8_t* base;
	size_t size;
	size_t used;
};
Region regions[2] = {{cap_buf0, kBuf0, 0}, {cap_buf1, kBuf1, 0}};
int region = 0;

bool commit(const uint8_t* p, int n) {
	while (region < 2) {
		Region& r = regions[region];
		if (r.used + static_cast<size_t>(n) <= r.size) {
			memcpy(r.base + r.used, p, static_cast<size_t>(n));
			r.used += static_cast<size_t>(n);
			cap_head.record_bytes += static_cast<uint32_t>(n);
			return true;
		}
		region++;
	}
	return false;
}

struct MsgSink {
	int send(const int (&msg)[6]) {
		int m[6] = {msg[0], msg[1], msg[2], msg[3], msg[4], msg[5]};
		const int rc = msgSend(6, m);
		if (rc) scan_stats.queue_full = scan_stats.queue_full + 1;
		if (cap_state == Cap::running) rec.send(msg, rc);
		return rc;
	}
	void wait() {
		scan_stats.off_waits = scan_stats.off_waits + 1;
		chThdSleep(1);
	}
};
MsgSink sink;

void copy_column(const ScanFrame& f, int col) {
	for (int n = 0; n < ks::kRows; n++) {
		const int b = col + n * ks::kColumns;
		work.on[b] = f.on[b];
		work.m[b] = f.m[b];
	}
}

// once per scan, before column 0: the configuration and calibration, and the recording
void frame_start(const ScanFrame& f) {
	const ks::Config cfg{config.zero_offset, config.message_interval};
	const bool cfg_changed = cfg.zero_offset != scanner.cfg.zero_offset || cfg.message_interval != scanner.cfg.message_interval;
	scanner.cfg = cfg;
	const uint32_t gen = cal_gen;
	const bool cal_changed = gen != cal_seen;
	if (cal_changed) {
		cal_seen = gen;
		for (int b = 0; b < ks::kButtons; b++) {
			scanner.keys[b].c_force = ks_cal[b].c_force;
			scanner.keys[b].c_breakpoint = ks_cal[b].c_breakpoint;
			scanner.keys[b].c_force2 = ks_cal[b].c_force2;
		}
	}
	work.seq = f.seq;
	work.t_start = f.t_start;

	if (cap_state == Cap::armed) {
		cap_head = ks::CaptureHeader{};
		cap_head.seconds = static_cast<uint32_t>(cap_seconds);
		cap_head.first_seq = work.seq;
		cap_head.first_t_start = work.t_start;
		ks::write_header(cap_header, cap_head, scanner); // the snapshot; counts at the dump
		rec.reset(work.seq, work.t_start);
		for (Region& r : regions) r.used = 0;
		region = 0;
		cap_t0 = chVTGetSystemTimeX();
		cap_state = Cap::running;
	} else if (cap_state == Cap::running && chTimeDiffX(cap_t0, chVTGetSystemTimeX()) >= TIME_S2I(cap_seconds)) {
		cap_state = Cap::done;
	}
	if (cap_state == Cap::running) {
		if (cal_changed) {
			for (int b = 0; b < ks::kButtons; b++) cal_copy[b] = scanner.keys[b].c_force;
		}
		rec.begin(work.seq, work.t_start, cfg_changed ? &scanner.cfg : nullptr, cal_changed ? cal_copy : nullptr);
		rec_overruns = scan_stats.overruns;
	}
}

void frame_end() {
	if (cap_state != Cap::running) return;
	rec.end(scan_stats.overruns != rec_overruns);
	if (commit(rec.buf.data(), rec.len)) {
		cap_head.frames++;
	} else {
		cap_head.flags |= 1; // full
		cap_state = Cap::done;
	}
}

// --- the dump: a low priority thread, writes with a timeout (the host may stop reading) ------
binary_semaphore_t dump_sem;
THD_WORKING_AREA(waScanDump, 512);

bool dump_write(const uint8_t* p, size_t n) {
	while (n) {
		const size_t k = n < 256 ? n : 256;
		if (obqWriteTimeout(&BDU1.obqueue, p, k, TIME_MS2I(500)) != k) return false;
		p += k;
		n -= k;
	}
	return true;
}

bool dump_line(const char* s) { return dump_write(reinterpret_cast<const uint8_t*>(s), strlen(s)); }

void ThreadScanDump(void*) {
	chRegSetThreadName("scan_dump");
	for (;;) {
		chBSemWait(&dump_sem);
		// the header's counts, then a hash over everything sent
		ks::CaptureHeader h = cap_head;
		ks::write_header_fixed(cap_header, h);
		uint32_t hash = ks::kFnvBasis;
		hash = ks::fnv1a_bytes(hash, cap_header, sizeof cap_header);
		for (const Region& r : regions) hash = ks::fnv1a_bytes(hash, r.base, r.used);
		const uint32_t total = static_cast<uint32_t>(sizeof cap_header) + h.record_bytes;
		char line[80];
		chsnprintf(line, sizeof line, "R dump %lu bytes %lu frames fnv %08lx\r\n", total, h.frames, hash);
		bool ok = dump_line(line) && dump_write(cap_header, sizeof cap_header);
		for (const Region& r : regions) ok = ok && dump_write(r.base, r.used);
		ok = ok && dump_line("\r\nR end\r\n");
		(void)ok; // a dump the host stopped reading is just cut off
		cap_state = Cap::done;
	}
}

const char* cap_state_name(Cap s) {
	switch (s) {
	case Cap::idle: return "idle";
	case Cap::armed: return "armed";
	case Cap::running: return "recording";
	case Cap::done: return "done";
	case Cap::dumping: return "dumping";
	}
	return "?";
}

} // namespace

extern "C" {

void ks_init(void) {
	scanner.init();
	for (int b = 0; b < ks::kButtons; b++) {
		ks_cal[b] = {scanner.keys[b].c_force, scanner.keys[b].c_breakpoint, scanner.keys[b].c_force2};
	}
	cal_seen = cal_gen;
}

void ks_calibration_changed(void) { cal_gen = cal_gen + 1; }

void ks_set_zero_offset(int b, int32_t zero_offset) { scanner.keys[b].zero_offset = zero_offset; }

void ks_boot_column(const ScanFrame* f, int col) {
	if (col == 0) frame_start(*f);
	copy_column(*f, col);
	scanner.boot_column(work, col);
}

void ks_column(const ScanFrame* f, int col) {
	if (col == 0) frame_start(*f);
	copy_column(*f, col);
	if (cap_state == Cap::running) rec.column(work, col);
	scanner.process_column(work, col, sink);
	if (col == ks::kColumns - 1) frame_end();
}

void ks_key_state(int b, int* status, int32_t* c_force, int32_t* zero_offset, int32_t* pres) {
	const ks::KeyState& k = scanner.keys[b];
	*status = static_cast<int>(k.status);
	*c_force = k.c_force;
	*zero_offset = k.zero_offset;
	*pres = k.pres;
}

void ks_capture(int seconds) {
	auto* out = reinterpret_cast<BaseSequentialStream*>(&BDU1);
	const Cap s = cap_state;
	if (seconds > 0) {
		if (s == Cap::armed || s == Cap::running || s == Cap::dumping) {
			chprintf(out, "R err busy (%s)\r\n", cap_state_name(s));
			return;
		}
		cap_seconds = seconds;
		cap_state = Cap::armed;
		chprintf(out, "R capture %d s, buffer %u bytes\r\n", seconds, static_cast<unsigned>(kBuf0 + kBuf1));
	} else if (s != Cap::done) {
		chprintf(out, "R err nothing to dump (%s)\r\n", cap_state_name(s));
	} else if (config.send_usb_bulk) {
		chprintf(out, "R err the stream is on: Stcs first\r\n");
	} else {
		cap_state = Cap::dumping;
		chBSemSignal(&dump_sem);
	}
}

void ks_capture_start_dump_thread(void) {
	chBSemObjectInit(&dump_sem, true);
	chThdCreateStatic(waScanDump, sizeof(waScanDump), LOWPRIO, ThreadScanDump, nullptr);
}

void ks_capture_print(void* stream) {
	auto* out = static_cast<BaseSequentialStream*>(stream);
	chprintf(out, "scan capture %s seconds %d frames %lu bytes %lu of %u full %lu\r\n", cap_state_name(cap_state),
	    cap_seconds, cap_head.frames, cap_head.record_bytes, static_cast<unsigned>(kBuf0 + kBuf1), cap_head.flags & 1);
}

} // extern "C"
