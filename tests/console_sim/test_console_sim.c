/* The USB console and USB MIDI in, end to end on the host (from the app-core review's scenarios):
 * the real console.cpp, bulk_usb.c, midi_usb.c and ChibiOS hal_buffers.c/chprintf.c against a
 * fake USB controller (sim.c) and a host that parses what it receives like Striso Live
 * (host_parser.h, a C port of protocol.dart's StreamParser). Each case checks the fixed behaviour;
 * the review's finding it guards is in its name.
 *
 * The control thread's part (control_thread.cpp) is ts_send() below: the same frame encoding, written to the bulk
 * console with the call control_thread.cpp makes (TS_WRITE). */
#include <stdio.h>
#include <stdlib.h>

#include "host_parser.h"
#include "sim.h"
#include "striso.h"
#include "version.h"

/* the control thread: a frame to the bulk console */
#ifndef TS_WRITE
#define TS_WRITE(b, n) bduWriteFrame(&BDU1, (b), (n))
#endif
static void ts_send(int size, const int* msg) {
	uint8_t cmsg[16];
	cmsg[0] = 0x80 | ((uint8_t)msg[0]) << 3 | ((uint8_t)(size - 2));
	cmsg[1] = 0x7f & (uint8_t)msg[1];
	for (int c = 0; c < size - 2; c++) {
		cmsg[2 + c * 2] = 0x7f & (uint8_t)(msg[2 + c] >> 7);
		cmsg[3 + c * 2] = 0x7f & (uint8_t)(msg[2 + c]);
	}
	if (config.send_usb_bulk) TS_WRITE(cmsg, (size_t)(2 + (size - 2) * 2));
}

static jmp_buf halt_jmp;
static int failures;
static void check(const char* name, int ok) {
	failures += !ok;
	printf("%-4s %s\n", ok ? "ok" : "FAIL", name);
}

static void resync_parser(void) {
	sim_time += 2000; /* 1 s: a pending payload or prefix from the previous case times out */
	static const uint8_t z[7] = {0};
	for (int i = 0; i < 7; i++) PExReceiveByte(z[i]);
}
static void fresh(void) {
	sim_block_hook = sim_hook_reading_host;
	sim_init();
	resync_parser();
	sim_host_drain();
	memset(host_in_len, 0, sizeof host_in_len);
	memset(host_in_transfers, 0, sizeof host_in_transfers);
	config.send_usb_bulk = 0;
	config.send_midi_monitor = 0;
	sim_halted = sim_deadlocks = midi_in_calls = 0;
	sim_log_clear();
}
static void host_cmd(const void* bytes, size_t n) {
	size_t w = host_write(2, (const uint8_t*)bytes, n);
	if (w != n) printf("    (host write: %zu of %zu bytes accepted)\n", w, n);
}
static int contains(const uint8_t* hay, size_t n, const char* needle) {
	size_t m = strlen(needle);
	for (size_t i = 0; i + m <= n; i++)
		if (!memcmp(hay + i, needle, m)) return 1;
	return 0;
}
static int got_version(void) { return contains(host_in[2], host_in_len[2], FWVERSION " "); }
static void key_msg(int* m, int id, int pres) {
	m[0] = ID_DIS;
	m[1] = id;
	m[2] = pres;
	m[3] = 0;
	m[4] = 0;
	m[5] = 0;
}

/* R1 (D1): StcV while a bulk IN transfer is in flight */
static void version_inflight(int long_reply) {
	fresh();
	if (!long_reply) {
		host_cmd("StcS", 4);
		PExReceive();
		int m[6];
		for (int i = 0; i < 5; i++) {
			key_msg(m, i, 100 * i);
			ts_send(6, m);
		}
		sim_sof(); /* the SOF hook starts the transfer of the partial buffer; the host isn't reading */
		host_cmd("StcV", 4);
	} else {
		host_cmd("StckStcV", 8); /* 61 lines of key factors, then the version, in one write */
	}
	sim_halt_jmp = &halt_jmp;
	if (setjmp(halt_jmp) == 0) {
		PExReceive();
		sim_host_drain(); /* the in-flight transfer completes: bduDataTransmitted */
	}
	sim_halt_jmp = NULL;
	check(long_reply ? "R1/D1: StckStcV in one write: no halt, the version arrives"
	                 : "R1/D1: StcV with the stream on and a packet in flight: no halt, the version arrives",
	    !sim_halted && got_version());
}

/* R3: the console blocked on a full queue is woken; ThreadSend writes before it resumes */
static int s2_injected;
static int s2_hook(threads_queue_t* tqp) {
	(void)tqp;
	if (!s2_injected) {
		s2_injected = 1;
		host_read(2); /* the USB ISR frees a buffer and readies the console thread... */
		int m[6];
		key_msg(m, 42, 4000);
		ts_send(6, m); /* ...but ThreadSend (same priority, running) writes a frame first */
		return 1;
	}
	return sim_hook_reading_host(tqp);
}
static void two_writers(void) {
	fresh();
	host_cmd("StcS", 4);
	PExReceive();
	s2_injected = 0;
	sim_block_hook = s2_hook;
	host_cmd("Stcp", 4); /* 20 lines of parameters: more than the two 256-byte buffers */
	PExReceive();
	sim_block_hook = sim_hook_reading_host;
	sim_host_drain();
	host_parser_t* p = calloc(1, sizeof *p);
	hp_push(p, host_in[2], host_in_len[2]);
	int lines_ok = p->lines == 20, bogus = 0, key42 = 0;
	for (int i = 0; i < p->lines && i < 256; i++) {
		int idx;
		char nm[32];
		if (sscanf(p->line[i], "%d param%31s", &idx, nm) != 2) bogus++;
	}
	for (int i = 0; i < p->messages && i < 4096; i++)
		if (p->msg_id[i] == 42 && p->msg_v[i][0] == 4000)
			key42++;
		else
			bogus++;
	printf("    host: %d messages, %d lines, %d truncated\n", p->messages, p->lines, p->truncated);
	check("R3: a frame from ThreadSend while the console waits for space: delivered once, the text intact",
	    lines_ok && !bogus && key42 == 1 && !p->truncated && !sim_halted);
	free(p);
}

/* R4: a frame that doesn't fit is cut; the host reads the following text as its values */
static void truncated_frame(void) {
	fresh();
	host_cmd("StcS", 4);
	PExReceive();
	int m[6];
	for (int i = 0; i < 60; i++) {
		key_msg(m, i % 61, 1000 + i);
		ts_send(6, m);
	}
	host_cmd("Stcu", 4); /* any reply after the cut frame */
	PExReceive();
	sim_host_drain();
	host_parser_t* p = calloc(1, sizeof *p);
	hp_push(p, host_in[2], host_in_len[2]);
	int bogus = 0;
	for (int i = 0; i < p->messages; i++)
		if (p->msg_v[i][0] < 1000 || p->msg_v[i][0] > 1059) bogus++;
	int line_ok = p->lines >= 1 && strncmp(p->line[0], "usb audio source", 16) == 0;
	printf("    host: %d messages, %d lines, first \"%s\"\n", p->messages, p->lines, p->lines ? p->line[0] : "");
	check("R4: only whole frames are written: no phantom message, the reply line intact",
	    !bogus && !p->truncated && line_ok);
	free(p);
}

/* R5, R6: the parser after a stray prefix or a short payload */
static void desync_case(const char* name, const void* pre, size_t npre, int reconnect) {
	fresh();
	host_cmd(pre, npre);
	PExReceive();
	sim_host_drain();
	sim_time += 2000; /* 1 s later: e.g. the next striso.py invocation */
	if (reconnect) sim_usb_configure();
	host_in_len[2] = 0;
	sim_log_clear();
	host_cmd("StcV", 4);
	PExReceive();
	sim_host_drain();
	char buf[160];
	snprintf(buf, sizeof buf, "%s, then StcV: answered", name);
	check(buf, got_version());
}
static void desync_same_write(void) {
	fresh();
	host_cmd("SStcV", 5);
	PExReceive();
	sim_host_drain();
	check("R5: SStcV in one write: answered", got_version());
	fresh();
	host_cmd("StStcV", 6);
	PExReceive();
	sim_host_drain();
	check("R5: StStcV in one write: answered", got_version());
}

/* R9, R8: USB MIDI in */
static void midi_framing(void) {
	fresh();
	const uint8_t a[6] = {0x09, 0x90, 0x3c, 0x64, 0x08, 0x80}; /* note on, then 2 stray bytes */
	const uint8_t b[8] = {0x08, 0x80, 0x3c, 0x00, 0x0b, 0xb0, 0x07, 0x64}; /* note off, CC7 */
	host_write(1, a, 6);
	host_write(1, b, 8);
	PExReceive();
	const uint8_t c[4] = {0x0b, 0xb0, 0x40, 0x7f};
	host_write(1, c, 4);
	PExReceive();
	int ok = strstr(sim_log, "90 3c 64") && strstr(sim_log, "80 3c 00") && strstr(sim_log, "b0 07 64") &&
	    strstr(sim_log, "b0 40 7f") && midi_in_calls == 4;
	char* nl;
	while ((nl = strchr(sim_log, '\n'))) *nl = ';';
	printf("    handler calls: %s\n", sim_log);
	check("R9: a transfer that isn't a multiple of 4 bytes doesn't misframe the later events", ok);
}
static void midi_case(const char* name, uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3, int expect_call) {
	fresh();
	midi_in_calls = 0;
	const uint8_t d[4] = {b0, b1, b2, b3};
	host_write(1, d, 4);
	PExReceive();
	check(name, midi_in_calls == expect_call);
}

/* R10: a console write of exactly 64 bytes (no zero-length packet) */
static void out_64(void) {
	fresh();
	uint8_t cmd[64];
	memset(cmd, 0, sizeof cmd);
	memcpy(cmd + 60, "StcV", 4);
	host_cmd(cmd, 64);
	PExReceive();
	sim_host_drain();
	check("R10: a 64-byte console write is processed at once", got_version());
}

/* R7: a missed prefix turned a StcQ payload into commands: float 227.454 = 'S','t','c','C' */
static void payload_as_commands(void) {
	fresh();
	float f;
	uint32_t fb = 0x43637453u;
	memcpy(&f, &fb, 4);
	uint8_t cmd[11] = {'S', 'S', 't', 'c', 'Q', 0, 5};
	memcpy(cmd + 7, &f, 4);
	host_cmd(cmd, sizeof cmd);
	PExReceive();
	sim_host_drain();
	check("R7: S + StcQ(0, 5, 227.454): a parameter set, no calibration reset",
	    !strstr(sim_log, "CALIBRATION RESET") && strstr(sim_log, "param_set(0,5,43637453)"));
}

/* B14: the host stops reading during a long reply. A wait for room times out (500 ms) and the rest
 * of the reply is dropped: the main thread is back after one timeout, not blocked forever. */
static int host_gone(threads_queue_t* tqp) {
	(void)tqp;
	return 0;
}
static void host_stops_reading(void) {
	fresh();
	sim_block_hook = host_gone;
	sim_timeouts = 0;
	systime_t t0 = sim_time;
	host_cmd("Stcp", 4); /* 20 lines of parameters: more than the two 256-byte buffers */
	PExReceive();
	const int one_wait = !sim_deadlocks && sim_timeouts == 1 && sim_time - t0 == TIME_MS2I(500);
	printf("    host gone: %d timed-out waits, %u ms, %d blocked forever\n", sim_timeouts,
	    (unsigned)(sim_time - t0) / 2, sim_deadlocks);
	t0 = sim_time;
	host_cmd("Stcf", 4); /* the next reply, the host still gone: one wait again */
	PExReceive();
	const int next_one_wait = !sim_deadlocks && sim_timeouts == 2 && sim_time - t0 == TIME_MS2I(500);
	check("B14: the host stops reading during a reply: one 500 ms wait per reply, no thread blocked forever",
	    one_wait && next_one_wait && !sim_halted);

	/* the host reads again: what was queued arrives, then the next reply whole */
	sim_block_hook = sim_hook_reading_host;
	sim_host_drain();
	host_parser_t* p = calloc(1, sizeof *p);
	hp_push(p, host_in[2], host_in_len[2]);
	const int lines_before = p->lines;
	host_in_len[2] = 0;
	memset(p, 0, sizeof *p);
	host_cmd("Stcp", 4);
	PExReceive();
	sim_host_drain();
	hp_push(p, host_in[2], host_in_len[2]);
	int lines_ok = p->lines == 20;
	for (int i = 0; i < p->lines && i < 20; i++) {
		char want[16];
		snprintf(want, sizeof want, "%d param%02d ", i, i);
		if (strncmp(p->line[i], want, strlen(want)) != 0) lines_ok = 0;
	}
	printf("    host back: %d lines of the cut replies, then %d lines\n", lines_before, p->lines);
	check("B14: the host reads again: the next reply arrives whole", lines_ok && !sim_deadlocks && !p->truncated);
	free(p);
}

/* R23: Stcq, the control thread's inbox per lane (read only), from messaging_stub.cpp's numbers */
static void inbox_stats(void) {
	fresh();
	host_cmd("Stcq", 4);
	PExReceive();
	sim_host_drain();
	static const char* const want[] = {"q lanes 6 capacity 32", "q 0 drops 3 max 32 read_buttons",
	    "q 1 drops 0 max 5 send messages", "q 2 drops 0 max 0 -", "q 3 drops 0 max 0 -", "q 4 drops 0 max 0 -",
	    "q 5 drops 0 max 0 -", "q ump_out drops 0 held_back 0 note_offs_waited 0 note_offs_waiting 0"};
	host_parser_t* p = calloc(1, sizeof *p);
	hp_push(p, host_in[2], host_in_len[2]);
	int ok = p->lines == 8 && !p->messages;
	for (int i = 0; ok && i < 8; i++) ok = strcmp(p->line[i], want[i]) == 0;
	for (int i = 0; i < p->lines && i < 8; i++) printf("    \"%s\"\n", p->line[i]);
	check("R23: Stcq lists every lane of the inbox: drops, max depth, the sending thread; then MIDI 2.0 out's drops", ok);
	free(p);
}

/* StcH: one line, the current voice's hero names, a bar, the effects' (read only) */
static void heroes(void) {
	fresh();
	host_cmd("Stc\x83", 4); /* voice 3 */
	PExReceive();
	host_cmd("StcH", 4);
	PExReceive();
	sim_host_drain();
	check("StcH: \"H <voice> <names> | <fx names>\" for the current voice, one line",
	    !sim_halted && contains(host_in[2], host_in_len[2], "H 3 cutoff res | rvmix dlmix\r\n"));
}

/* R27: a host that writes console bytes without a pause. On the board the USB interrupt hands
 * over its next packet while the main thread reads: here each time the main thread leaves a
 * critical section (sim_irq_hook). Each pass must read a bounded share of them, and still take
 * the MIDI in that waits and return to synth_tick and led_tick. */
static int r27_packets_left;
static void r27_host_streams(void) {
	if (r27_packets_left > 0 && usbGetReceiveStatusI(&USBD1, 2)) {
		uint8_t junk[64];
		memset(junk, 'x', sizeof junk);
		r27_packets_left--;
		host_write(2, junk, sizeof junk);
	}
}
static void console_stream_bounded(void) {
	fresh();
	const int packets = 100; /* 6400 bytes */
	r27_packets_left = packets;
	sim_irq_hook = r27_host_streams;
	PExReceive(); /* the stream starts: its first packets arrive during this pass */
	/* a MIDI event waits while the stream goes on; then the passes, each one's share of it */
	const uint8_t note_on[4] = {0x09, 0x90, 0x3c, 0x64};
	host_write(1, note_on, 4);
	int passes = 0, most = 0, midi_first_pass = -1;
	while (r27_packets_left > 0 && passes < 1000) {
		const int before = r27_packets_left;
		PExReceive();
		passes++;
		if (before - r27_packets_left > most) most = before - r27_packets_left;
		if (midi_first_pass < 0 && midi_in_calls > 0) midi_first_pass = passes;
	}
	sim_irq_hook = NULL;
	PExReceive(); /* what is still queued */
	PExReceive();
	host_in_len[2] = 0;
	host_cmd("StcV", 4);
	PExReceive();
	sim_host_drain();
	printf("    %d packets in %d passes, at most %d a pass; the MIDI event in pass %d\n", packets, passes, most,
	    midi_first_pass);
	check("R27: a console stream without pause: each pass reads at most 256 bytes, MIDI in is taken at once",
	    most <= 256 / 64 + 1 && midi_first_pass == 1 && midi_in_calls == 1 && r27_packets_left == 0 && got_version());
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	fresh();
	host_cmd("StcV", 4);
	PExReceive();
	sim_host_drain();
	check("StcV with nothing in flight: answered", !sim_halted && got_version());
	version_inflight(0);
	version_inflight(1);
	two_writers();
	truncated_frame();
	desync_same_write();
	desync_case("R5: a stray \"S\"", "S", 1, 0);
	desync_case("R5: \"St\"", "St", 2, 0);
	desync_case("R6: \"Stct07\" (2 of 3 digits)", "Stct07", 6, 0);
	desync_case("R6: \"StcK\" without the id", "StcK", 4, 0);
	desync_case("R6: \"StcR\" without the digits", "StcR", 4, 0);
	desync_case("R6: \"StcQ\" + 5 of 6 bytes", "StcQ\x00\x01\x00\x00\x00", 9, 0);
	desync_case("R6: \"StcQ\" + 2 bytes and a USB re-enumeration", "StcQ\x00\x01", 6, 1);
	payload_as_commands();
	midi_framing();
	midi_case("R8: CIN 0xF with 0xE0 (not real-time): dropped", 0x0f, 0xe0, 0x00, 0x00, 0);
	midi_case("R8: CC7 with data 0xFF: dropped", 0x0b, 0xb0, 0x07, 0xff, 0);
	midi_case("R8: CIN 9 with status 0x80: dropped", 0x09, 0x80, 0x3c, 0x00, 0);
	midi_case("R8: SysEx (CIN 4): dropped", 0x04, 0xf0, 0x7e, 0x00, 0);
	midi_case("R8: CIN 0xF with 0xF8 (clock): passed", 0x0f, 0xf8, 0x00, 0x00, 1);
	midi_case("R8: CC7 = 100: passed", 0x0b, 0xb0, 0x07, 0x64, 1);
	midi_case("R8: program change (CIN 0xC): passed", 0x0c, 0xc0, 0x05, 0x00, 1);
	out_64();
	heroes();
	host_stops_reading();
	inbox_stats();
	console_stream_bounded();
	printf(failures ? "console sim: %d FAILED\n" : "console sim: all passed\n", failures);
	return failures ? 1 : 0;
}
