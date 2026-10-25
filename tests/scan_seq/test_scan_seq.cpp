// The key scan's sequence (src/drivers/scan_thread.cpp: the ScanSequencer and ScanConsumer of
// scan_sequencer.hpp, the output channels of scan_pins.c) against the old button_read.c
// (tests/ref/scan_seq_ref.c), on the same stand-in HAL (tests/scan_seq/stubs). After every ADC
// callback: every pin call, the hand-over's scan number and complete columns, the wakeups, both
// frames and the counters; after every column the thread takes: its KeyScanner calls, the frames
// counter, the aux buttons' messages. The new code must make the old code's calls, in the same
// order, from the same samples and cycle counts.
//
//   scans     scans with every number of measured columns (0..17), the keys at the threshold
//   random    random samples, cycle counts and pressed columns, 20000 scans
//   timing    callbacks per scan: 17 + 7 per measured column + the delay to 4 columns' time
//   thread    the zero level pass, the columns, lost columns and the aux buttons' debounce
#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "doctest/doctest.h"

#include "scan_thread.cpp" // NOLINT(bugprone-suspicious-include): the test reaches its sequencer and link

extern "C" {
#include "scan_ref.h"
const ScanFrame* ref_frame(int i);
uint32_t ref_link_seq(void);
uint32_t ref_link_ready(void);
}

namespace {

struct Op {
	char kind;
	uint32_t a = 0, b = 0, c = 0;
	bool operator==(const Op&) const = default;
};
std::vector<Op> trace;
void rec(char k, uint32_t a = 0, uint32_t b = 0, uint32_t c = 0) { trace.push_back({k, a, b, c}); }
std::string show(const Op& o) { return std::string(1, o.kind) + "(" + std::to_string(o.a) + "," + std::to_string(o.b) + "," + std::to_string(o.c) + ")"; }
std::string diff(const std::vector<Op>& a, const std::vector<Op>& b) {
	const std::size_t n = std::min(a.size(), b.size());
	for (std::size_t i = 0; i < n; i++) {
		if (!(a[i] == b[i])) return "op " + std::to_string(i) + ": old " + show(a[i]) + ", new " + show(b[i]);
	}
	if (a.size() != b.size()) return "lengths: old " + std::to_string(a.size()) + ", new " + std::to_string(b.size());
	return "";
}

uint32_t fnv(const void* p, std::size_t n, uint32_t h = 2166136261u) {
	const auto* b = static_cast<const uint8_t*>(p);
	for (std::size_t i = 0; i < n; i++) h = (h ^ b[i]) * 16777619u;
	return h;
}
uint32_t stats_hash(const volatile scan_stats_t& s) {
	scan_stats_t c;
	std::memcpy(&c, const_cast<const scan_stats_t*>(&s), sizeof c); // NOLINT(cppcoreguidelines-pro-type-const-cast)
	c.frames = 0;                                                   // the thread's, compared on its own
	return fnv(&c, sizeof c);
}

thread_t me{1};
int aux_level[4] = {0, 0, 0, 0};

} // namespace

extern "C" {
volatile scan_stats_t scan_stats; // button_read.c's
ADCDriver ADCD1;
DWT_Type dwt;
void palSetPort(ioportid_t port, ioportmask_t mask) { rec('S', static_cast<uint32_t>(port), mask); }
void palClearPort(ioportid_t port, ioportmask_t mask) { rec('C', static_cast<uint32_t>(port), mask); }
void palSetPadMode(ioportid_t port, iopadid_t pad, uint32_t mode) { rec('M', static_cast<uint32_t>(port), pad, mode); }
int palReadLine(ioline_t line) {
	const int v = line >= LINE_BUTTON_PORT && line <= LINE_BUTTON_ALT ? aux_level[line - LINE_BUTTON_PORT] : 0;
	rec('R', line, static_cast<uint32_t>(v));
	return v;
}
void chSysLock(void) {}
void chSysUnlock(void) {}
void chSysLockFromISR(void) { rec('K'); }
void chSysUnlockFromISR(void) { rec('k'); }
thread_t* chThdGetSelfX(void) { return &me; }
thread_t* chSchReadyI(thread_t* tp) {
	rec('W', static_cast<uint32_t>(tp->id));
	return tp;
}
void chSchGoSleepS(int /*state*/) {}
// the KeyScanner and the messages, both versions
void ks_boot_column(int col) { rec('b', static_cast<uint32_t>(col)); }
void ks_column(int col) { rec('c', static_cast<uint32_t>(col)); }
void ks_lost(void) { rec('l'); }
ScanFrame* ks_work(void) {
	static ScanFrame w;
	return &w;
}
bool event_send1(int src, int id, int a) {
	rec('E', static_cast<uint32_t>(src), static_cast<uint32_t>(id), static_cast<uint32_t>(a));
	return true;
}
void ref_ks_boot_column(int col) { ks_boot_column(col); }
void ref_ks_column(int col) { ks_column(col); }
void ref_ks_lost(void) { ks_lost(); }
bool ref_event_send1(int src, int id, int a) { return event_send1(src, id, a); }
}

namespace {

// a scan's samples: the four rows of each callback (4095 - level: a pressed key reads low)
using Samples = std::vector<std::array<adcsample_t, 4>>;

// both versions through the same callbacks, with the cycle counter at each; their traces
std::string compare_isr(const Samples& s, const std::vector<uint32_t>& cyc) {
	std::vector<Op> t[2];
	for (int old = 1; old >= 0; old--) {
		trace.clear();
		dwt.CYCCNT = 0;
		if (old) {
			std::memset(const_cast<scan_stats_t*>(&ref_scan_stats), 0, sizeof ref_scan_stats); // NOLINT
			ref_isr_first_scan();
		} else {
			std::memset(const_cast<scan_stats_t*>(&scan_stats), 0, sizeof scan_stats); // NOLINT
			sequencer = ks::ScanSequencer{};
			link = scan_link_t{};
			scan_isr_init();
		}
		for (std::size_t i = 0; i < s.size(); i++) {
			dwt.CYCCNT = cyc[i];
			if (old) {
				ref_set_waiting();
				ref_adc_callback(s[i].data());
			} else {
				tpReadButtons = &me;
				scan_isr_callback(s[i].data());
			}
			const ScanFrame* f0 = old ? ref_frame(0) : &link.frame[0];
			const ScanFrame* f1 = old ? ref_frame(1) : &link.frame[1];
			rec('L', old ? ref_link_seq() : link.seq, old ? ref_link_ready() : link.ready);
			rec('F', fnv(f0, sizeof *f0), fnv(f1, sizeof *f1));
			rec('T', stats_hash(old ? ref_scan_stats : scan_stats));
		}
		t[old] = trace;
	}
	return diff(t[1], t[0]);
}

constexpr adcsample_t kIdle = 4095 - 10;     // no key
constexpr adcsample_t kPressed = 4095 - 900; // a key well above the threshold

// the samples of one scan with the columns `pressed` (the rows of a pressed column read low),
// the measurement's samples, then the delay phase's (any value)
void scan(Samples& s, const std::vector<int>& pressed, std::mt19937& rng) {
	for (int c = 0; c < SCAN_COLUMNS; c++) {
		std::array<adcsample_t, 4> a = {kIdle, kIdle, kIdle, kIdle};
		for (int p : pressed) {
			if (p == c) a[rng() % 4] = kPressed;
		}
		s.push_back(a);
	}
	const int nm = static_cast<int>(pressed.size());
	const int measured = nm ? nm : 1;
	const int callbacks = 7 * measured + (nm < 4 ? 28 - 4 * nm : 0);
	for (int i = 0; i < callbacks; i++) {
		s.push_back({static_cast<adcsample_t>(rng() % 4096), static_cast<adcsample_t>(rng() % 4096),
		    static_cast<adcsample_t>(rng() % 4096), static_cast<adcsample_t>(rng() % 4096)});
	}
}

std::vector<uint32_t> cycles(std::size_t n, std::mt19937& rng) {
	std::vector<uint32_t> c(n);
	uint32_t t = rng();
	for (uint32_t& x : c) {
		t += 6000 + rng() % 30000; // one callback: ~30 us at 400 MHz, sometimes much longer
		if (rng() % 500 == 0) t += rng() % 4000000;
		x = t;
	}
	return c;
}

} // namespace

TEST_CASE("scans with every number of measured columns, keys at the threshold") {
	std::mt19937 rng(1);
	Samples s;
	for (int nm = 0; nm <= SCAN_COLUMNS; nm++) {
		std::vector<int> cols;
		cols.reserve(static_cast<std::size_t>(nm));
		for (int c = 0; c < nm; c++) cols.push_back((c * 7 + nm) % SCAN_COLUMNS);
		std::sort(cols.begin(), cols.end());
		cols.erase(std::unique(cols.begin(), cols.end()), cols.end());
		scan(s, cols, rng);
	}
	// the detection threshold: 4095 - 64 is no key, one below is
	for (adcsample_t v : {static_cast<adcsample_t>(4095 - 64), static_cast<adcsample_t>(4095 - 65), static_cast<adcsample_t>(4095 - 63)}) {
		for (int c = 0; c < SCAN_COLUMNS; c++) s.push_back({kIdle, kIdle, v, kIdle});
		for (int i = 0; i < 52; i++) s.push_back({1, 2, 3, 4});
	}
	const std::string d = compare_isr(s, cycles(s.size(), rng));
	CHECK_MESSAGE(d == "", d);
}

TEST_CASE("random samples, cycle counts and pressed columns, 20000 scans") {
	std::mt19937 rng(7);
	for (int run = 0; run < 20; run++) {
		Samples s;
		for (int k = 0; k < 1000; k++) {
			if (rng() % 10 == 0) { // any samples at all
				for (int i = 0; i < 60; i++) {
					s.push_back({static_cast<adcsample_t>(rng() % 4096), static_cast<adcsample_t>(rng() % 4096),
					    static_cast<adcsample_t>(rng() % 4096), static_cast<adcsample_t>(rng() % 4096)});
				}
				continue;
			}
			std::vector<int> cols;
			for (int c = 0; c < SCAN_COLUMNS; c++) {
				if (rng() % (1 + rng() % 8) == 0) cols.push_back(c);
			}
			scan(s, cols, rng);
		}
		CAPTURE(run);
		const std::string d = compare_isr(s, cycles(s.size(), rng));
		CHECK_MESSAGE(d == "", d);
	}
}

TEST_CASE("timing: callbacks per scan, 17 + 7 per measured column + the delay to 4 columns' time") {
	std::mt19937 rng(3);
	for (int nm = 0; nm <= SCAN_COLUMNS; nm++) {
		CAPTURE(nm);
		std::vector<int> cols;
		cols.reserve(static_cast<std::size_t>(nm));
		for (int c = 0; c < nm; c++) cols.push_back(c);
		Samples s;
		scan(s, cols, rng);
		scan(s, {}, rng);                                                          // the next scan's first callback starts it
		std::memset(const_cast<scan_stats_t*>(&scan_stats), 0, sizeof scan_stats); // NOLINT
		sequencer = ks::ScanSequencer{};
		link = scan_link_t{};
		scan_isr_init();
		trace.clear();
		int callbacks = 0;
		for (const auto& a : s) {
			scan_isr_callback(a.data());
			callbacks++;
			if (scan_stats.scans == 2) break;
		}
		const int measured = nm ? nm : 1;
		CHECK(callbacks - 1 == SCAN_COLUMNS + 7 * measured + (nm < 4 ? 28 - 4 * nm : 0));
		// the frame lists the measured columns, but column 16 is marked after the list is taken (as
		// before: a pressed column 16 is measured, and missing from the list and the recordings'
		// noise floor)
		const bool col16 = nm > 0 && cols.back() == 16;
		CHECK(link.frame[0].n == (col16 && nm > 1 ? nm - 1 : measured));
	}
}

TEST_CASE("thread: the zero level pass, the columns, lost columns, the aux buttons") {
	std::mt19937 rng(11);
	std::vector<std::pair<int, std::array<int, 4>>> steps;
	for (int i = 0; i < 17 * 400; i++) {
		int note = i % 17;
		if (rng() % 50 == 0) note = -2;
		std::array<int, 4> aux{};
		for (int& a : aux) a = (i / 17 / (5 + static_cast<int>(rng() % 3))) % 2 ^ static_cast<int>(rng() % 20 == 0);
		steps.emplace_back(note, aux);
	}
	std::vector<Op> t[2];
	uint32_t frames[2];
	for (int old = 1; old >= 0; old--) {
		trace.clear();
		std::memset(const_cast<scan_stats_t*>(old ? &ref_scan_stats : &scan_stats), 0, sizeof scan_stats); // NOLINT
		consumer = decltype(consumer){};
		for (const auto& [note, aux] : steps) {
			for (int n = 0; n < 4; n++) aux_level[n] = aux[n];
			if (old) {
				ref_thread_column(note);
			} else {
				consumer.column(thread_io, note);
			}
			rec('f', old ? ref_scan_stats.frames : scan_stats.frames);
		}
		t[old] = trace;
		frames[old] = old ? ref_scan_stats.frames : scan_stats.frames;
	}
	const std::string d = diff(t[1], t[0]);
	CHECK_MESSAGE(d == "", d);
	CHECK(frames[0] == frames[1]);
	int boots = 0, cols = 0, sends = 0;
	for (const Op& o : t[0]) {
		boots += o.kind == 'b';
		cols += o.kind == 'c';
		sends += o.kind == 'E';
	}
	CHECK(boots > 0);
	CHECK(cols > 0);
	CHECK(sends > 0);
}
