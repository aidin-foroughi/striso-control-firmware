// Model checks of the control thread's inbox (src/app/inbox.hpp, as messaging.cpp uses it), on a
// stand-in kernel (stubs/ch.h). The properties (R2, B7 and the lanes):
//   - every event send() accepted comes out of receive() once, intact, in the order of the sends
//     (one order over all lanes);
//   - receive() never sleeps while accepted events are pending, and a sleeping consumer is woken
//     by the send that follows (no lost wakeup);
//   - send() refuses an event exactly when the sending thread's lane is full, and counts it
//     (drops() == the refusals): one producer's flood never takes another's room;
//   - the kernel calls are made as ChibiOS requires (suspend and resume under the lock, the lock
//     not taken twice).
//
//   small    every sequence of up to DEPTH operations on Inbox<3, 2> (two private lanes and a
//            shared one): sends from producers A, B, C and the consumer itself (K), receives (G),
//            the values 0..7 per event; the lanes are taken in every order
//   board    the board's Inbox<6, 32>, 300k operations: the scanner in bursts, motion, the aux
//            jack, main, the consumer's own sends, a slower reader
//   threads  real threads on Inbox<6, 8>: four producers retry refused sends, the consumer
//            sleeps in receive(); per-producer order, nothing lost, no hang (a lost wakeup would)
//
// The int ring this replaced lost its whole backlog when a message filled it exactly (shortest
// with 7 slots: S2 S3 G) and could miss a wakeup between its empty check and its sleep.
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "ch.h"
#include "inbox.hpp"

// --- the stand-in kernel ----------------------------------------------------------------------
namespace kernel {
bool threaded = false;
// the model: the running thread, the lock depth, the consumer's suspension
constexpr int kConsumer = 4;
thread_t threads[5] = {{0}, {1}, {2}, {3}, {kConsumer}}; // producers 0..3, the consumer
thread_t* current = nullptr;
int lock_depth = 0;
bool misuse = false;
bool suspended = false;
std::jmp_buf sleep_jmp;
// real threads
std::mutex lock;
std::condition_variable resumed;
thread_local thread_t self{-1};
} // namespace kernel

void chSysLock() {
	if (kernel::threaded) {
		kernel::lock.lock();
		return;
	}
	kernel::misuse |= kernel::lock_depth != 0;
	kernel::lock_depth++;
}
void chSysUnlock() {
	if (kernel::threaded) {
		kernel::lock.unlock();
		return;
	}
	kernel::misuse |= kernel::lock_depth != 1;
	kernel::lock_depth--;
}
thread_t* chThdGetSelfX() { return kernel::threaded ? &kernel::self : kernel::current; }
msg_t chThdSuspendS(thread_reference_t* trp) {
	if (kernel::threaded) {
		*trp = &kernel::self;
		std::unique_lock<std::mutex> lk(kernel::lock, std::adopt_lock);
		kernel::resumed.wait(lk, [trp] { return *trp == nullptr; });
		lk.release();
		return MSG_OK;
	}
	kernel::misuse |= kernel::lock_depth != 1 || *trp != nullptr;
	*trp = kernel::current;
	kernel::suspended = true;
	kernel::lock_depth = 0; // the kernel lets go of the lock while the thread sleeps
	std::longjmp(kernel::sleep_jmp, 1);
}
void chThdResumeS(thread_reference_t* trp, msg_t msg) {
	(void)msg;
	if (kernel::threaded) {
		if (*trp != nullptr) {
			*trp = nullptr;
			kernel::resumed.notify_all();
		}
		return;
	}
	kernel::misuse |= kernel::lock_depth != 1;
	if (*trp != nullptr) {
		*trp = nullptr;
		kernel::suspended = false;
	}
}

namespace {

enum Violation { OK,
	LOST_INVISIBLE,
	LOST_WAKEUP,
	WRONG_EVENT,
	WRONG_REFUSAL,
	WRONG_COUNT,
	KERNEL_MISUSE,
	NV };
const char* const vname[NV] = {"ok", "receive slept with accepted events pending",
    "a sleeping consumer was not woken by a send", "an event came out wrong (content or order)",
    "a send refused with room in its lane, or accepted into a full one",
    "drops() is not the number of refusals", "a kernel call outside the lock, or the lock taken twice"};

// The reference: the accepted events in order, each lane's count, the lanes' owners (the rule:
// the first Lanes - 1 threads to send get their own, the others share the last).
template <std::size_t Lanes, std::size_t N>
struct Reference {
	static constexpr int kMax = static_cast<int>(std::bit_ceil(Lanes * N)); // >= the events an inbox holds
	struct Ev {
		KeyEvent e;
		int lane;
	};
	Ev fifo[kMax];
	int head = 0, tail = 0;
	int count[Lanes] = {};
	int owner[Lanes] = {}; // thread id + 1, 0: free
	long refused = 0;
	int next_id = 0;

	int lane_of(int thread) {
		for (std::size_t i = 0; i + 1 < Lanes; i++) {
			if (owner[i] == 0) owner[i] = thread + 1;
			if (owner[i] == thread + 1) return static_cast<int>(i);
		}
		return static_cast<int>(Lanes - 1);
	}
};

bool same(const KeyEvent& a, const KeyEvent& b) {
	return a.src == b.src && a.id == b.id && a.n == b.n && std::equal(a.v.begin(), a.v.begin() + a.n, b.v.begin());
}

KeyEvent event_of(int id) {
	const int n = id % 8;
	int v[7];
	for (int i = 0; i < n; i++) v[i] = id * 8 + i - 3000;
	return KeyEvent::make(static_cast<Source>(id % 7), id & 0xff, std::span<const int>(v, static_cast<std::size_t>(n)));
}

template <std::size_t Lanes, std::size_t N>
struct Model {
	Inbox<Lanes, N> inbox;
	Reference<Lanes, N> ref;

	// thread t sends the next event
	Violation send(int t) {
		kernel::current = &kernel::threads[t];
		const KeyEvent e = event_of(++ref.next_id);
		const int lane = ref.lane_of(t);
		const bool room = ref.count[lane] < static_cast<int>(N);
		const bool ok = inbox.send(e);
		if (kernel::misuse || kernel::lock_depth != 0) return KERNEL_MISUSE;
		if (ok != room) return WRONG_REFUSAL;
		if (ok) {
			ref.fifo[ref.tail++ % ref.kMax] = {e, lane};
			ref.count[lane]++;
		} else {
			ref.refused++;
		}
		if (inbox.drops() != static_cast<uint32_t>(ref.refused)) return WRONG_COUNT;
		if (kernel::suspended && ref.head != ref.tail) return LOST_WAKEUP;
		return OK;
	}

	// the consumer receives (only while it is awake)
	Violation receive() {
		kernel::current = &kernel::threads[kernel::kConsumer];
		if (setjmp(kernel::sleep_jmp) != 0) { // it went to sleep
			return ref.head != ref.tail ? LOST_INVISIBLE : OK;
		}
		const KeyEvent got = inbox.receive();
		if (kernel::misuse || kernel::lock_depth != 0) return KERNEL_MISUSE;
		if (ref.head == ref.tail) return WRONG_EVENT; // an event out of an empty inbox
		const auto& want = ref.fifo[ref.head++ % ref.kMax];
		ref.count[want.lane]--;
		return same(got, want.e) ? OK : WRONG_EVENT;
	}
};

// --- small: exhaustive --------------------------------------------------------------------------
#ifndef DEPTH
#define DEPTH 10
#endif
using Small = Model<3, 2>;
struct Snapshot {
	unsigned char bytes[sizeof(Small)];
	bool suspended;
};
Small small;
long explored, violations[NV];
std::string trace[64], first_trace[NV];
const char* const op_name[] = {"SA", "SB", "SC", "SK", "G"};
constexpr int op_thread[] = {0, 1, 2, kernel::kConsumer}; // SK: the consumer's own send

void save(Snapshot& s) {
	std::memcpy(s.bytes, static_cast<const void*>(&small), sizeof small);
	s.suspended = kernel::suspended;
}
void restore(const Snapshot& s) {
	std::memcpy(static_cast<void*>(&small), s.bytes, sizeof small);
	kernel::suspended = s.suspended;
	kernel::misuse = false;
	kernel::lock_depth = 0;
}

void dfs(int d) {
	explored++;
	if (d == DEPTH) return;
	auto* s = new Snapshot; // (deep recursion: off the stack)
	save(*s);
	for (int op = 0; op < 5; op++) {
		if (op == 4 && kernel::suspended) continue; // the consumer sleeps: only sends
		const long refused = small.ref.refused;
		const Violation v = op < 4 ? small.send(op_thread[op]) : small.receive();
		trace[d] = std::string(op_name[op]) + (small.ref.refused != refused ? "x" : "");
		if (v != OK) {
			if (violations[v]++ == 0) {
				for (int i = 0; i <= d; i++) first_trace[v] += trace[i] + " ";
			}
		} else {
			dfs(d + 1);
		}
		restore(*s);
	}
	delete s;
}

int run_small() {
	std::printf("exhaustive: Inbox<3 lanes, 2 events>, producers A B C and the consumer K, every sequence of up to %d sends/receives\n", DEPTH);
	dfs(0);
	std::printf("  %ld states explored\n", explored);
	long bad = 0;
	for (int v = 1; v < NV; v++) {
		bad += violations[v];
		std::printf("  %-75s %ld%s%s\n", vname[v], violations[v], violations[v] ? "   first: " : "", first_trace[v].c_str());
	}
	std::printf("  (S<t>: thread t sends, x = refused (its lane full); G: the consumer receives)\n");
	return bad ? 1 : 0;
}

// --- board: the board's inbox under traffic ---------------------------------------------------
int run_board() {
	static Model<6, 32> m;
	std::mt19937 rng(1);
	long sent = 0, accepted = 0, received = 0, bad = 0, counts[NV] = {};
	// threads: 0 the scanner (bursts), 1 motion, 2 the aux jack, 3 main, and the consumer's own
	// sends (the MIDI monitor)
	for (long step = 0; step < 300000; step++) {
		const unsigned r = rng() % 16;
		Violation v = OK;
		if (r < 3) { // the scanner: a chord's burst
			for (int i = 0; i < 4 && v == OK; i++) {
				const long before = m.ref.refused;
				v = m.send(0);
				sent++;
				accepted += m.ref.refused == before;
			}
		} else if (r < 6) {
			const int t = 1 + static_cast<int>(rng() % 4); // 1..3, or 4: the consumer
			const long before = m.ref.refused;
			v = m.send(t);
			sent++;
			accepted += m.ref.refused == before;
		} else if (!kernel::suspended) {
			const bool had = m.ref.head != m.ref.tail;
			v = m.receive();
			received += had;
		}
		if (v != OK) {
			counts[v]++;
			bad++;
			break;
		}
	}
	// drain
	while (m.ref.head != m.ref.tail && !kernel::suspended) {
		const Violation v = m.receive();
		received++;
		if (v != OK) {
			counts[v]++;
			bad++;
			break;
		}
	}
	std::printf("the board's inbox (Inbox<6 lanes, 32 events>), 300k steps, key bursts and four other producers, a slower reader:\n");
	std::printf("  sent %ld, accepted %ld, refused %ld (drops %u), received %ld\n", sent, accepted, m.ref.refused, m.inbox.drops(), received);
	for (std::size_t l = 0; l < decltype(m.inbox)::lanes(); l++) {
		const auto st = m.inbox.stats(l);
		std::printf("  lane %zu: thread %d, drops %u, max depth %u\n", l, st.owner ? st.owner->id : -1, st.drops, st.max_depth);
	}
	for (int v = 1; v < NV; v++) {
		if (counts[v]) std::printf("  %s\n", vname[v]);
	}
	bad += accepted != received;
	return bad ? 1 : 0;
}

// --- threads: real concurrency ----------------------------------------------------------------
int run_threads() {
	kernel::threaded = true;
	static Inbox<6, 8> inbox;
	constexpr int kProducers = 4, kEach = 50000;
	std::atomic<long> refused{0};
	std::atomic<bool> done{false};
	std::thread watchdog([&done] {
		for (int i = 0; i < 1200 && !done; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
		if (!done) {
			std::printf("threads: no progress for 120 s (a lost wakeup?)\n");
			std::fflush(stdout);
			std::_Exit(1);
		}
	});
	std::vector<std::thread> producers;
	producers.reserve(kProducers);
	for (int p = 0; p < kProducers; p++) {
		producers.emplace_back([p, &refused] {
			kernel::self = {p};
			for (int i = 0; i < kEach; i++) {
				const KeyEvent e = KeyEvent::make(Source::dis, p, {i & 0x7fff, i >> 15});
				while (!inbox.send(e)) {
					refused++;
					std::this_thread::yield();
				}
				if (i % 64 == 0) std::this_thread::sleep_for(std::chrono::microseconds(50)); // let the consumer sleep
			}
		});
	}
	kernel::self = {kProducers};
	int next[kProducers] = {};
	long wrong = 0;
	for (long n = 0; n < static_cast<long>(kProducers) * kEach; n++) {
		const KeyEvent e = inbox.receive();
		const int p = e.id;
		const int i = e.v[0] | e.v[1] << 15;
		if (p < 0 || p >= kProducers || e.n != 2 || i != next[p]) wrong++;
		if (p >= 0 && p < kProducers) next[p] = i + 1;
	}
	for (auto& t : producers) t.join();
	done = true;
	watchdog.join();
	std::printf("threads: %d producers x %d events through Inbox<6, 8>: %ld out of order or wrong, %ld refusals retried (drops %u), inbox %s\n",
	    kProducers, kEach, wrong, refused.load(), inbox.drops(), inbox.empty() ? "empty" : "NOT EMPTY");
	return wrong == 0 && inbox.empty() && inbox.drops() == static_cast<uint32_t>(refused.load()) ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
	const std::string part = argc > 1 ? argv[1] : "small";
	const int rc = part == "small" ? run_small() : part == "board" ? run_board() :
	    part == "threads"                                          ? run_threads() :
	                                                                 2;
	std::printf(rc ? "messaging model (%s): FAILED\n" : "messaging model (%s): all passed\n", part.c_str());
	return rc;
}
