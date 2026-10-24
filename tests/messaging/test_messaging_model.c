/* R2: model check of src/app/messaging.c, the message ring between the producers (keys, motion,
 * aux jack, MIDI in) and ThreadSend, compiled as is (tests/messaging/stubs: the mutex is a no-op
 * because each operation is atomic in the model, and a reader that would go to sleep jumps back
 * here: "the queue looks empty").
 *
 * Property: every message msgSend accepted (returned 0) comes out of msgGet intact and in order,
 * and msgGet never finds the queue empty while accepted messages are pending.
 *
 * Part 1 (test_messaging_model_small, BUFFERSIZE 7): every sequence of up to DEPTH operations
 * (msgSend of 0..3 values, or msgGet). Part 2 (test_messaging_model, the board's 240 slots):
 * key-message traffic (size 6, some size 3) in bursts with a slower reader.
 *
 * Before the fix a message that filled the ring exactly left msg_write == msg_read: the ring
 * looked empty and its whole backlog (up to ~34 key messages, note-offs included) was lost and
 * overwritten, uncounted (shortest: S2 S3 G with 7 slots). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ch.h"
jmp_buf model_sleep_jmp;
int model_readied;
#include "messaging.c"

#ifndef DEPTH
#define DEPTH 10
#endif

/* reference FIFO of the accepted messages */
typedef struct {
	int size;
	int v[10];
} rmsg_t;
static rmsg_t refq[4096];
static int rq_head, rq_tail, next_id;

typedef struct {
	int buf[BUFFERSIZE], r, w, under;
	int rh, rt, id;
} snap_t;
static void save(snap_t* s) {
	memcpy(s->buf, msg_buffer, sizeof msg_buffer);
	s->r = msg_read;
	s->w = msg_write;
	s->under = underruns;
	s->rh = rq_head;
	s->rt = rq_tail;
	s->id = next_id;
}
static void restore(const snap_t* s) {
	memcpy(msg_buffer, s->buf, sizeof msg_buffer);
	msg_read = s->r;
	msg_write = s->w;
	underruns = s->under;
	rq_head = s->rh;
	rq_tail = s->rt;
	next_id = s->id;
}

enum { OK = 0, LOST_INVISIBLE = 1, CORRUPT = 2, WRONG_SIZE = 3 };
static const char* vname[] = {"ok", "accepted messages invisible (the queue looks empty)",
                              "message content corrupted", "wrong size"};

static int do_send(int size) {
	int m[10];
	int id = ++next_id;
	for (int n = 0; n < size; n++) m[n] = id * 16 + n;
	int r = msgSend(size, m);
	if (r == 0) {
		refq[rq_tail % 4096].size = size;
		memcpy(refq[rq_tail % 4096].v, m, sizeof m);
		rq_tail++;
	}
	return r;
}
/* a violation code */
static int do_get(void) {
	int m[10];
	int size;
	if (setjmp(model_sleep_jmp)) {
		tpMsg = NULL; /* the reader would sleep here: the queue looks empty */
		return rq_head != rq_tail ? LOST_INVISIBLE : OK;
	}
	size = msgGet(9, m);
	if (rq_head == rq_tail) return CORRUPT; /* data out of an empty reference queue */
	rmsg_t* e = &refq[rq_head % 4096];
	rq_head++;
	if (size != e->size) return WRONG_SIZE;
	for (int n = 0; n < size; n++)
		if (m[n] != e->v[n]) return CORRUPT;
	return OK;
}

static int depth_max, max_size;
static long explored, violations[4];
static char trace[64][8], first_trace[4][512];
static void dfs(int d) {
	explored++;
	if (d == depth_max) return;
	snap_t s;
	save(&s);
	for (int op = -1; op <= max_size; op++) {
		int v = OK;
		if (op < 0) {
			v = do_get();
			snprintf(trace[d], 8, "G");
		} else {
			int r = do_send(op);
			snprintf(trace[d], 8, "S%d%s", op, r ? "x" : "");
		}
		if (v != OK) {
			if (!violations[v]++) {
				char* t = first_trace[v];
				t[0] = 0;
				for (int i = 0; i <= d; i++) {
					strcat(t, trace[i]);
					strcat(t, " ");
				}
			}
		} else {
			dfs(d + 1);
		}
		restore(&s);
	}
}

int main(void) {
	MessagingInit();
	long bad = 0;
	if (BUFFERSIZE < 16) {
		depth_max = DEPTH;
		max_size = 3;
		printf("exhaustive: BUFFERSIZE %d, message sizes 0..%d, all sequences of up to %d sends/gets\n",
		    BUFFERSIZE, max_size, depth_max);
		dfs(0);
		printf("  %ld states explored\n", explored);
		for (int v = 1; v < 4; v++) {
			bad += violations[v];
			printf("  %-52s %ld%s%s\n", vname[v], violations[v], violations[v] ? "   first: " : "",
			    violations[v] ? first_trace[v] : "");
		}
		printf("  (S<n>: msgSend of n values, x = rejected as overflow; G: msgGet)\n");
	} else {
		/* the scanner sends size-6 messages in bursts; the reader takes one every 2 sends */
		srand(1);
		long sent = 0, accepted = 0, rejected = 0, invisible_events = 0, lost = 0, corrupt = 0;
		for (long step = 0; step < 300000; step++) {
			int burst = rand() % 3 == 0;
			if (burst || rand() % 2) {
				sent++;
				if (do_send(rand() % 8 == 0 ? 3 : 6) == 0)
					accepted++;
				else
					rejected++;
			} else {
				int pend = rq_tail - rq_head;
				int v = do_get();
				if (v == LOST_INVISIBLE) {
					invisible_events++;
					lost += pend;
					rq_head = rq_tail; /* those are gone: the next send overwrites them */
				} else if (v != OK) {
					corrupt++;
					rq_head = rq_tail;
					msg_read = msg_write; /* resync the model */
				}
			}
		}
		printf("the board's ring (BUFFERSIZE %d), 300k ops, size-6 key messages, a slower reader:\n", BUFFERSIZE);
		printf("  sent %ld, accepted %ld, rejected %ld (underruns %d)\n", sent, accepted, rejected, underruns);
		printf("  the queue looked empty with messages pending: %ld times, %ld accepted messages lost\n",
		    invisible_events, lost);
		printf("  corrupted or misordered deliveries: %ld\n", corrupt);
		bad = invisible_events + corrupt + (rejected != underruns);
	}
	printf(bad ? "messaging model: FAILED\n" : "messaging model: all passed\n");
	return bad ? 1 : 0;
}
