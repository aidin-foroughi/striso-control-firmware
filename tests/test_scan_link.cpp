// The hand-over of the scans from the ADC interrupt to the scanner thread
// (src/drivers/key_scanner_link.h), against a simulated interrupt and scheduler:
//   S3: every column the thread takes is from the scan it thinks it is, never a mix of two, also
//       when the thread falls behind by a scan or more (it then notices, and skips ahead);
//   S2: the thread never sleeps while a column is waiting (a wakeup lost between its check and
//       its sleep), wherever the interrupt runs.
#include <cstdint>
#include <random>

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
void os_sleep() {
	REQUIRE(locked);
	const int avail = scan_link_avail(&link, g_pos->k);
	if (avail < 0 || avail > g_pos->col) slept_with_work++;
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
