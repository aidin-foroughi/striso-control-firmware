// The key scan's interrupt on the board's CPU under QEMU (ctest target.scanseq): the old
// button_read.c (tests/ref/scan_seq_ref.c, compiled as the firmware compiles C) and the new
// ScanSequencer (scan_thread.cpp, compiled as the firmware compiles C++) through the same scans:
// their pin writes, frames, hand-over and counters must be the same, and the instructions per
// callback (QEMU -icount shift=0: SysTick on the CPU clock counts instructions / 40) are printed
// for both, so a change in the interrupt's time, which sets the scan period, shows.
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "scan_thread.cpp"

extern "C" {
#include "scan_ref.h"
const ScanFrame* ref_frame(int i);
uint32_t ref_link_seq(void);
uint32_t ref_link_ready(void);

volatile scan_stats_t scan_stats;
ADCDriver ADCD1;
DWT_Type dwt;
GPIO_TypeDef target_gpio[9];
thread_t target_thread{1};
int target_hashing = 0;
uint32_t target_hash = 0;
void ks_boot_column(int) {}
void ks_column(int) {}
void ks_lost(void) {}
ScanFrame* ks_work(void) {
	static ScanFrame w;
	return &w;
}
bool event_send1(int, int, int) { return true; }
void ref_ks_boot_column(int) {}
void ref_ks_column(int) {}
void ref_ks_lost(void) {}
bool ref_event_send1(int, int, int) { return true; }
}

#define SYST_CSR (*reinterpret_cast<volatile uint32_t*>(0xE000E010u))
#define SYST_RVR (*reinterpret_cast<volatile uint32_t*>(0xE000E014u))
#define SYST_CVR (*reinterpret_cast<volatile uint32_t*>(0xE000E018u))

namespace {

uint32_t fnv(const void* p, std::size_t n, uint32_t h) {
	const auto* b = static_cast<const uint8_t*>(p);
	for (std::size_t i = 0; i < n; i++) h = (h ^ b[i]) * 16777619u;
	return h;
}

// the samples of the scans, the same for both: random pressed columns, random measurements
struct Gen {
	uint32_t s = 2463534242u;
	int cb = 0, ncb = 0;
	uint16_t pressed_mask[17] = {};
	uint32_t next() {
		s ^= s << 13;
		s ^= s >> 17;
		s ^= s << 5;
		return s;
	}
	void samples(adcsample_t a[4]) {
		if (cb == ncb) { // a new scan: its pressed columns
			int nm = 0;
			const uint32_t density = 1 + next() % 8;
			for (uint16_t& p : pressed_mask) {
				p = next() % density == 0 ? static_cast<uint16_t>(1u << (next() % 4)) : 0;
				nm += p != 0;
			}
			const int measured = nm ? nm : 1;
			ncb = 17 + 7 * measured + (nm < 4 ? 28 - 4 * nm : 0);
			cb = 0;
		}
		for (int r = 0; r < 4; r++) {
			if (cb < 17) {
				a[r] = static_cast<adcsample_t>((pressed_mask[cb] >> r & 1) ? 4095 - 900 : 4095 - 10);
			} else {
				a[r] = static_cast<adcsample_t>(next() % 4096);
			}
		}
		cb++;
	}
};

constexpr int kCallbacks = 200000;

// one run of the old (old) or the new interrupt; the outputs' hash, the SysTick ticks taken
uint32_t run(bool old, bool hashing, uint64_t* ticks) {
	Gen g;
	target_hashing = hashing;
	target_hash = 2166136261u;
	dwt.CYCCNT = 0;
	if (old) {
		std::memset(const_cast<scan_stats_t*>(&ref_scan_stats), 0, sizeof ref_scan_stats);
		ref_isr_first_scan();
	} else {
		std::memset(const_cast<scan_stats_t*>(&scan_stats), 0, sizeof scan_stats);
		sequencer = ks::ScanSequencer{};
		link = scan_link_t{};
		scan_isr_init();
	}
	static adcsample_t s[4];
	uint32_t h = 2166136261u;
	uint64_t t = 0;
	for (int i = 0; i < kCallbacks; i++) {
		g.samples(s);
		dwt.CYCCNT = dwt.CYCCNT + 12000 + (g.next() % 20000);
		const uint32_t t0 = SYST_CVR;
		if (old) {
			ref_set_waiting();
			ref_adc_callback(s);
		} else {
			tpReadButtons = &target_thread;
			adcsample_t c[4];
			for (int k = 0; k < 4; k++) c[k] = s[k]; // as ref_adc_callback copies them
			scan_isr_callback(c);
		}
		t += (t0 - SYST_CVR) & 0xffffffu;
		if (hashing) {
			const uint32_t seq = old ? ref_link_seq() : link.seq;
			const uint32_t ready = old ? ref_link_ready() : link.ready;
			h = fnv(&seq, 4, h);
			h = fnv(&ready, 4, h);
			if (i % 64 == 0) {
				h = fnv(old ? ref_frame(0) : &link.frame[0], sizeof(ScanFrame), h);
				h = fnv(old ? ref_frame(1) : &link.frame[1], sizeof(ScanFrame), h);
			}
		}
	}
	scan_stats_t st;
	std::memcpy(&st, const_cast<const scan_stats_t*>(old ? &ref_scan_stats : &scan_stats), sizeof st);
	h = fnv(&st, sizeof st, h);
	*ticks = t;
	return h ^ target_hash;
}

} // namespace

int main() {
	SYST_RVR = 0xffffffu;
	SYST_CVR = 0;
	SYST_CSR = 5; // enabled, CPU clock, no interrupt
	uint64_t ticks[2];
	const uint32_t h_old = run(true, true, &ticks[0]);
	const uint32_t h_new = run(false, true, &ticks[1]);
	(void)run(true, false, &ticks[0]); // timed without the hashing
	(void)run(false, false, &ticks[1]);
	const double per_old = static_cast<double>(ticks[0]) * 40.0 / kCallbacks;
	const double per_new = static_cast<double>(ticks[1]) * 40.0 / kCallbacks;
	std::printf("scanseq: %d callbacks, outputs %s (old %08lx new %08lx), instructions per callback old %.1f new %.1f\n", kCallbacks,
	    h_old == h_new ? "equal" : "DIFFER", static_cast<unsigned long>(h_old), static_cast<unsigned long>(h_new), per_old, per_new);
	return h_old == h_new ? 0 : 1;
}
