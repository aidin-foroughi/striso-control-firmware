/* Fuzz of the USB console on the host: the real console.cpp, bulk_usb.c, midi_usb.c and ChibiOS
 * hal_buffers.c/chprintf.c (sim.c's fake USB controller) under random hosts, under ASan + UBSan in
 * the host-asan preset (a memory error or undefined behaviour aborts the run).
 *
 * Each session: random console writes (bytes biased to the command alphabet: "Stc" prefixes,
 * command codes, payloads, voice bytes, junk), USB MIDI transfers of any length, a host that reads,
 * reads slowly or stops reading (the console's waits for room then time out), SOFs, USB
 * re-enumerations, time steps (some past the 100 ms stale interval), and ThreadSend frames while
 * the stream is on, also from inside the console's waits (the R3 interleaving).
 *
 * Checked after every step: no chSysHalt (R1), no thread blocked forever (B14), and every message
 * the host parses (host_parser.h, as Striso Live) is a frame that was sent, whole: text never cuts
 * or fakes a frame (R3, R4). StcV is left out of the console bytes for that check: with the stream
 * on, its discard of the unsent output can cut a frame that straddles the buffer in flight (see the
 * report of the console lane). A re-enumeration drops everything queued, so the host starts parsing
 * afresh, as Striso Live does when it reconnects.
 *
 * Usage: test_console_fuzz [sessions (2000)] [steps (400)] [seed] */
#include <stdio.h>
#include <stdlib.h>

#include "host_parser.h"
#include "sim.h"
#include "striso.h"

static uint64_t rng_state;
static uint32_t rnd(void) {
	rng_state = rng_state * 6364136223846793005ull + 1442695040888963407ull;
	return (uint32_t)(rng_state >> 33);
}
static uint32_t below(uint32_t n) { return rnd() % n; }

static host_parser_t parser;
static int failures, frames_sent, frames_seen;
static int host_mode; /* 0 reads, 1 reads now and then, 2 gone */

static void fail(const char* what, int session, int step) {
	if (failures < 10) printf("FAIL session %d step %d: %s\n", session, step, what);
	failures++;
}

/* main.c ThreadSend's frame of a key message: id, pres 1000..1999, velo/x/y 0 */
static void send_frame(void) {
	if (!config.send_usb_bulk) return;
	const int id = (int)below(61), pres = 1000 + (int)below(1000);
	uint8_t f[10] = {(uint8_t)(0x80 | ID_DIS << 3 | 4), (uint8_t)id, (uint8_t)(pres >> 7), (uint8_t)(pres & 0x7f), 0, 0, 0, 0, 0, 0};
	(void)bduWriteFrame(&BDU1, f, sizeof f);
	frames_sent++;
}

/* the console's waits: the host reads (or not), ThreadSend writes a frame in between */
static int fuzz_hook(threads_queue_t* tqp) {
	(void)tqp;
	if (below(4) == 0) send_frame();
	if (host_mode == 2 || (host_mode == 1 && below(3) != 0)) return 0;
	sim_sof();
	return host_read(2) >= 0 || host_read(1) >= 0;
}

/* what the host received so far, parsed; every message must be one of send_frame's */
static void check_host(int session, int step) {
	hp_push(&parser, host_in[2], host_in_len[2]);
	host_in_len[2] = 0;
	host_in_len[1] = 0;
	for (int i = 0; i < parser.messages && i < 4096; i++) {
		const int ok = parser.msg_src[i] == ID_DIS && parser.msg_count[i] == 4 && parser.msg_id[i] < 61 &&
		    parser.msg_v[i][0] >= 1000 && parser.msg_v[i][0] < 2000 && parser.msg_v[i][1] == 0 &&
		    parser.msg_v[i][2] == 0 && parser.msg_v[i][3] == 0;
		if (!ok) {
			char b[96];
			snprintf(b, sizeof b, "a message that wasn't sent: src %d id %d values %d %d %d %d", parser.msg_src[i],
			    parser.msg_id[i], parser.msg_v[i][0], parser.msg_v[i][1], parser.msg_v[i][2], parser.msg_v[i][3]);
			fail(b, session, step);
		}
	}
	frames_seen += parser.messages;
	parser.messages = 0;
	parser.lines = 0;
	if (parser.truncated) {
		fail("a frame cut by a header byte", session, step);
		parser.truncated = 0;
	}
	if (sim_halted) fail(sim_halt_reason ? sim_halt_reason : "halt", session, step);
	if (sim_deadlocks) fail("a thread blocked forever (B14)", session, step);
	sim_halted = sim_deadlocks = 0;
}

static const char kCodes[] = "DBSsMmIKNXYyPpfgkQvLUtluCEZR";
static uint8_t console_byte(void) {
	switch (below(8)) {
	case 0: return 'S';
	case 1: return 't';
	case 2: return 'c';
	case 3: return (uint8_t)kCodes[below(sizeof kCodes - 1)];
	case 4: return (uint8_t)(0x80 + below(16)); /* a voice (or junk in a payload) */
	case 5: return (uint8_t)('0' + below(10));
	default: {
		uint8_t b = (uint8_t)rnd();
		return b == 'V' ? 'v' : b;
	}
	}
}

static void console_write(void) {
	uint8_t buf[300];
	size_t n = 1 + below(below(8) == 0 ? sizeof buf : 24);
	for (size_t i = 0; i < n;) {
		if (below(3) == 0 && i + 4 <= n) { /* a whole command */
			buf[i++] = 'S';
			buf[i++] = 't';
			buf[i++] = 'c';
			buf[i++] = (uint8_t)kCodes[below(sizeof kCodes - 1)];
		} else {
			buf[i++] = console_byte();
		}
	}
	(void)host_write(2, buf, n);
}

static void midi_write(void) {
	uint8_t buf[64];
	size_t n = below(sizeof buf + 1);
	for (size_t i = 0; i < n; i++) buf[i] = (uint8_t)rnd();
	if (n) (void)host_write(1, buf, n);
}

static void session(int s, int steps) {
	sim_block_hook = fuzz_hook;
	host_mode = 0;
	sim_init();
	sim_time += 2000;
	sim_host_drain();
	memset(host_in_len, 0, sizeof host_in_len);
	memset(&parser, 0, sizeof parser);
	config.send_usb_bulk = (int)below(2);
	config.send_midi_monitor = 0;
	sim_halted = sim_deadlocks = 0;
	jmp_buf halt;
	sim_halt_jmp = &halt;
	if (setjmp(halt) != 0) {
		fail(sim_halt_reason ? sim_halt_reason : "halt", s, -1);
		sim_halt_jmp = NULL;
		return;
	}
	for (int k = 0; k < steps; k++) {
		switch (below(16)) {
		case 0: case 1: case 2: case 3: console_write(); PExReceive(); break;
		case 4: midi_write(); PExReceive(); break;
		case 5: case 6: case 7: send_frame(); break;
		case 8: case 9: if (host_mode != 2) sim_host_drain(); break;
		case 10: sim_sof(); if (host_mode == 0) host_read(2); break;
		case 11: host_mode = (int)below(3); break;
		case 12: sim_time += below(5) == 0 ? 150 + below(4000) : below(40); break;
		case 13: config.send_usb_bulk = (int)below(2); break;
		case 14:
			if (below(8) == 0) { /* a USB re-enumeration: the device drops what it queued, the host too */
				sim_usb_configure();
				check_host(s, k);
				memset(&parser, 0, sizeof parser);
			}
			break;
		default: PExReceive(); break;
		}
		check_host(s, k);
	}
	/* the host reads again: everything still queued arrives */
	host_mode = 0;
	sim_host_drain();
	check_host(s, steps);
	sim_halt_jmp = NULL;
}

int main(int argc, char** argv) {
	setvbuf(stdout, NULL, _IONBF, 0);
	const int sessions = argc > 1 ? atoi(argv[1]) : 2000;
	const int steps = argc > 2 ? atoi(argv[2]) : 400;
	rng_state = argc > 3 ? strtoull(argv[3], NULL, 0) : 0x5751534f;
	sim_map_devspec(0);
	for (int s = 0; s < sessions; s++) session(s, steps);
	printf("console fuzz: %d sessions of %d steps, %d frames sent, %d received whole, %d timed-out waits, %d failures\n",
	    sessions, steps, frames_sent, frames_seen, sim_timeouts, failures);
	return failures ? 1 : 0;
}
