// The hand-over of the scans from the ADC interrupt to the scanner thread
// (src/drivers/key_scanner_link.h), against a simulated interrupt and scheduler:
//   S3: every column the thread takes is from the scan it thinks it is, never a mix of two, also
//       when the thread falls behind by a scan or more (it then notices, and skips ahead);
//   S2: the thread never sleeps while a column is waiting (a wakeup lost between its check and
//       its sleep), wherever the interrupt runs.
#include <cstdint>
#include <random>
#include <vector>

#include "doctest/doctest.h"

namespace {
void os_lock();
void os_unlock();
void os_sleep();
void os_window();
} // namespace

#define SCAN_OS_LOCK() os_lock()
#define SCAN_OS_UNLOCK() os_unlock()
#define SCAN_OS_SLEEP_S() os_sleep()
#define SCAN_OS_WINDOW() os_window()
#include "key_scanner_link.h"

namespace {

scan_link_t link;

// the interrupt: writes scan j column by column, every value tagged with the scan number
struct Isr {
	uint32_t j = 0;
	int c = 0; // next column
	ScanFrame* f = nullptr;
	void start() {
		j = 0;
		c = 0;
		f = scan_link_begin(&link, j);
		header();
	}
	static uint16_t tag(uint32_t j) { return static_cast<uint16_t>(j % 4000 + 1); }
	void header() {
		f->seq = j;
		f->t_start = j * 1000;
		f->n = 1;
		f->cols[0] = static_cast<uint8_t>(j % 17);
	}
	// one conversion step; true: it published (and woke the thread)
	bool step() {
		if (c == SCAN_COLUMNS) {
			f = scan_link_begin(&link, ++j);
			c = 0;
			header();
			return false;
		}
		for (int n = 0; n < SCAN_ROWS; n++) {
			const int b = c + n * SCAN_COLUMNS;
			f->on[b] = tag(j);
			f->m[b] = {tag(j), tag(j), tag(j), tag(j)};
		}
		scan_link_publish(&link, static_cast<uint32_t>(++c));
		return true;
	}
} isr;

std::mt19937 rng(7);
bool locked = false;
int window_steps = 0; // ISR steps at a window (0..window_steps)
uint32_t slept_with_work = 0;
scan_pos_t* g_pos = nullptr;

void os_lock() {
	REQUIRE(!locked);
	locked = true;
}
void os_unlock() {
	REQUIRE(locked);
	locked = false;
}
// sleeping: a column already there means its wakeup was lost; then the interrupt runs on until
// it publishes (its wakeup)
bool (*isr_until_wake)() = nullptr; // the realistic interrupt below, else the simple one
int (*wake_lag)() = nullptr;        // callbacks between the wakeup and the thread running
void os_sleep() {
	REQUIRE(locked);
	const int avail = scan_link_avail(&link, g_pos->k);
	if (avail < 0 || avail > g_pos->col) slept_with_work++;
	if (isr_until_wake) {
		while (!isr_until_wake()) {
		}
		for (int i = wake_lag ? wake_lag() : 0; i > 0; i--) isr_until_wake();
		return;
	}
	while (!isr.step()) {
	}
}
// an interrupt may run here
void os_window() {
	REQUIRE(!locked);
	const int n = window_steps ? static_cast<int>(rng() % static_cast<uint32_t>(window_steps + 1)) : 0;
	for (int i = 0; i < n; i++) isr.step();
}

// the columns the thread took: all from the scan it is in?
uint32_t check_column(const ScanFrame& w, int col, uint32_t k) {
	uint32_t bad = 0;
	const uint16_t t = Isr::tag(k);
	if (col == 0 && (w.seq != k || w.t_start != k * 1000)) bad++;
	for (int n = 0; n < SCAN_ROWS; n++) {
		const int b = col + n * SCAN_COLUMNS;
		if (w.on[b] != t || w.m[b].p != t || w.m[b].s0 != t || w.m[b].s1 != t || w.m[b].s2 != t) bad++;
	}
	return bad;
}

} // namespace

TEST_CASE("scan link S3: the thread never takes a column of another scan, also when it falls behind") {
	link = scan_link_t{};
	isr.start();
	scan_pos_t pos{};
	g_pos = &pos;
	ScanFrame work{};
	uint32_t taken = 0;
	uint32_t mixed = 0;
	window_steps = 0;
	for (int it = 0; it < 200000; it++) {
		const uint32_t k = pos.k;
		const int c = scan_link_take(&link, &pos, &work);
		if (c >= 0) {
			taken++;
			mixed += check_column(work, c, k);
		}
		// the interrupt runs on: mostly a little, sometimes for more than a scan (the thread is
		// preempted, or blocked)
		const uint32_t r = rng() % 100;
		const int steps = r < 70 ? 1 : r < 97 ? static_cast<int>(rng() % 10) :
		                                        18 + static_cast<int>(rng() % 30);
		for (int i = 0; i < steps; i++) isr.step();
	}
	CHECK(taken > 50000);
	CHECK(pos.lost > 0); // it fell behind
	CHECK(mixed == 0);
}

TEST_CASE("scan link S2: the thread never sleeps with a column waiting") {
	link = scan_link_t{};
	isr.start();
	scan_pos_t pos{};
	g_pos = &pos;
	ScanFrame work{};
	uint32_t caught = 0;
	slept_with_work = 0;
	window_steps = 2; // the interrupt may publish between the thread's check and its sleep
	uint32_t taken = 0;
	for (int it = 0; it < 20000; it++) {
		const int c = scan_link_wait(&link, &pos, &work, &caught);
		if (c >= 0) taken++;
	}
	CHECK(taken > 15000);
	CHECK(slept_with_work == 0);
	CHECK(caught > 0); // the second check under the lock found the columns the window published
}

namespace {

// The interrupt as on the board (button_read.c), callback by callback: a scan starts with 17
// detection callbacks (scan_link_begin at the first), then publishes the columns before the first
// measured one (no wakeup), takes 7 callbacks per measured column and publishes after each (with
// a wakeup): up to the next measured column, or the end. With fewer than 4 measured it publishes
// 16 after the last and ends with a delay phase (20 + 7 * 4 callbacks in all before it, then the
// scan complete + wakeup); with 4 or more the last one completes the scan. The next callback
// starts the next scan. Idle, the one measured column is the stale one: 52 callbacks per scan,
// as the board's StcE shows.
struct BoardIsr {
	uint32_t seq = 0;
	int cb = 0; // callback within the scan
	ScanFrame* f = nullptr;
	std::vector<int> measured{7};
	int ncb = 52;
	std::mt19937* vary = nullptr; // random measured columns per scan
	void start() {
		seq = 0;
		cb = 0;
	}
	void new_scan() {
		if (vary) {
			std::vector<int> m;
			for (int c = 0; c < SCAN_COLUMNS; c++) {
				if ((*vary)() % 6 == 0) m.push_back(c);
			}
			if (m.empty()) m.push_back(static_cast<int>((*vary)() % SCAN_COLUMNS)); // the stale one
			measured = m;
		}
		const int nm = static_cast<int>(measured.size());
		ncb = SCAN_COLUMNS + 7 * nm + (nm < 4 ? 28 - 4 * nm : 0);
		if (nm == 1 && !vary) ncb = 52;
		f = scan_link_begin(&link, seq);
		f->seq = seq;
		f->t_start = seq * 1000;
	}
	// one ADC callback; true: it woke the thread
	bool callback() {
		bool wake = false;
		if (cb == 0) new_scan();
		const int nm = static_cast<int>(measured.size());
		const uint16_t t = Isr::tag(seq);
		if (cb < SCAN_COLUMNS) {
			for (int n = 0; n < SCAN_ROWS; n++) f->on[cb + n * SCAN_COLUMNS] = t;
			if (cb == SCAN_COLUMNS - 1) scan_link_publish(&link, static_cast<uint32_t>(measured[0]));
		} else if (cb < SCAN_COLUMNS + 7 * nm) {
			const int i = (cb - SCAN_COLUMNS) / 7;
			const int col = measured[static_cast<size_t>(i)];
			for (int n = 0; n < SCAN_ROWS; n++) f->m[col + n * SCAN_COLUMNS] = {t, t, t, t};
			if ((cb - SCAN_COLUMNS) % 7 == 6) {
				uint32_t ready = 0;
				if (i + 1 < nm) ready = static_cast<uint32_t>(measured[static_cast<size_t>(i + 1)]);
				else ready = nm < 4 ? 16U : static_cast<uint32_t>(SCAN_COLUMNS);
				scan_link_publish(&link, ready);
				wake = true;
			}
		}
		if (++cb == ncb) {
			if (nm < 4) { // the end of the delay phase: the scan is complete
				scan_link_publish(&link, SCAN_COLUMNS);
				wake = true;
			}
			cb = 0;
			seq++;
		}
		return wake;
	}
} board_isr;

bool board_isr_until_wake() { return board_isr.callback(); }

struct TakeLog {
	uint32_t taken_scans = 0;  // scans of which every column was taken
	uint32_t out_of_order = 0; // a column taken twice, or out of order
	uint32_t mixed = 0;
	uint32_t jumps = 0; // moves to a later scan (after a lost one)
};

// The scanner thread on the board's timing: it sleeps until a wakeup, then runs `lag()`
// callbacks later (preempted by the synth, or busy), and takes the columns there are.
TakeLog run_board(int takes, int (*lag)(), scan_pos_t& pos) {
	wake_lag = lag;
	link = scan_link_t{};
	board_isr.start();
	isr_until_wake = board_isr_until_wake;
	window_steps = 0;
	pos = scan_pos_t{};
	g_pos = &pos;
	ScanFrame work{};
	uint32_t caught = 0;
	slept_with_work = 0;
	TakeLog log;
	uint32_t next_k = 0;
	int next_col = 0;
	for (int it = 0; it < takes; it++) {
		const uint32_t k = pos.k;
		const int c = scan_link_wait(&link, &pos, &work, &caught);
		if (c < 0) continue;
		if (k != next_k || c != next_col) {
			if (c == 0 && static_cast<int32_t>(k - next_k) > 0) {
				log.jumps++; // the thread moved on to a later scan
			} else {
				log.out_of_order++;
			}
		}
		for (int n = 0; n < SCAN_ROWS; n++) log.mixed += work.on[c + n * SCAN_COLUMNS] != Isr::tag(k);
		next_col = c + 1;
		next_k = k;
		if (next_col == SCAN_COLUMNS) {
			next_col = 0;
			next_k = k + 1;
			log.taken_scans++;
		}
	}
	isr_until_wake = nullptr;
	wake_lag = nullptr;
	board_isr.vary = nullptr;
	board_isr.measured = {7};
	return log;
}

} // namespace

TEST_CASE("scan link: the board's timing: every scan is taken exactly once") {
	scan_pos_t pos{};
	// the thread finishes column 16 before the next scan starts (idle: always), and lagging by
	// one or two columns (7 and 14 callbacks), and by most of a scan (45 of 52 callbacks)
	int (*const lags[])() = {[] { return 0; }, [] { return 7; }, [] { return 14; }, [] { return 45; }};
	for (int (*lag)() : lags) {
		CAPTURE(lag());
		const TakeLog log = run_board(17 * 3000, lag, pos);
		CHECK(pos.lost == 0);
		CHECK(pos.skipped == 0);
		CHECK(log.out_of_order == 0);
		CHECK(log.jumps == 0);
		CHECK(log.mixed == 0);
		CHECK(log.taken_scans >= 2990);
		CHECK(slept_with_work == 0);
	}
}

TEST_CASE("scan link: the board's timing, random measured columns and lags: never a scan twice, losses counted") {
	static std::mt19937 vary(5);
	static std::mt19937 r(9);
	board_isr.vary = &vary;
	scan_pos_t pos{};
	// mostly short holdups, sometimes more than a scan (then scans are lost, and counted)
	const TakeLog log = run_board(
	    17 * 20000, [] {
		    const uint32_t x = r() % 1000;
		    return x < 900 ? static_cast<int>(r() % 8) : x < 990 ? static_cast<int>(r() % 50) :
		                                                           60 + static_cast<int>(r() % 100);
	    },
	    pos);
	CHECK(log.out_of_order == 0); // never a column twice, never backwards
	CHECK(log.mixed == 0);
	CHECK(pos.lost >= log.jumps); // a long holdup right after a jump can lose again before a take
	CHECK(log.jumps > 0);
	CHECK(pos.lost > 0);
	CHECK(static_cast<int32_t>(pos.skipped) > 0); // counted forward, never wrapped
	CHECK(pos.skipped < 20000);
	// every scan is either taken whole or counted as skipped
	CHECK(log.taken_scans + pos.skipped >= pos.k - 1);
	CHECK(log.taken_scans + pos.skipped <= pos.k + 1);
}
