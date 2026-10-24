/*
 * Raw scan recordings (StcR): what the KeyScanner consumed, scan by scan, so the PC can replay it
 * through the same KeyScanner and must get the board's messages exactly (tests/scan_replay.cpp).
 *
 * A recording is a header, then one record per scan. All numbers little endian.
 *
 * Header (kHeaderBytes):
 *   u32 magic "KSR1", u16 version, u16 header bytes, u32 frames, u32 record bytes,
 *   u32 seconds asked, u32 flags (bit 0: stopped because the buffer was full),
 *   u32 seq and u32 t_start of the first scan, u32 noise statistics bytes, u32 0,
 *   the KeyScanner's state before the first scan: Config (2 x i32), i32 buttons_pressed,
 *   17 x i32 col_pressed, 68 x key: i32 pres velo c_force c_breakpoint c_force2 zero_offset
 *   zero_time zero_max key_detect3, u32 fact (float bits), i32 timer, u8 status, 3 x u8 0.
 *
 * Record of a scan (varint: 7 bits per byte, low first; zz: zigzag signed varint):
 *   u8 flags: bit 0 the scan number is not the previous + 1: varint seq follows,
 *             bit 1 the configuration changed: zz zero_offset, zz message_interval follow,
 *             bit 2 the calibration changed: 68 x zz c_force follow
 *   varint DWT cycles since the previous scan's start
 *   columns as the scanner thread consumed them, those with a key above kKeyDetect only:
 *     varint column + 1, then per row: zz on, zz p, and when on > 0: zz s0, zz s1, zz s2, each
 *     the difference from the last value recorded for that key and field
 *   varint 0
 *   varint messages the scanner tried to send in this scan
 *   u8 bit 0: the interrupt overwrote the frame while the thread read it (torn),
 *      bit 1: more refused sends than recorded
 *   when messages > 0: u16 hash (fnv1a of the messages, low 16 bits), varint refused n,
 *     n x varint index of a refused attempt (the queue was full)
 * Only the inputs update_button reads are kept (KeyScanner::load_column): a column without a key
 * above kKeyDetect is left out, and so are the corner samples of a key at or below it.
 *
 * After the records, the noise statistics (kStatsBytes, NoiseStats): per button of every scan the
 * detection sample on (all of them, also those the records leave out) and the pressure p of the
 * scans that measured it: u32 count, u32 sum, u64 sum of squares, u32 max, each for on, then p.
 * They make a recording of nobody playing useful: the noise floor of every key.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "key_scanner.hpp"

namespace ks {

inline constexpr uint32_t kCaptureMagic = 0x3152534bU; // "KSR1"
inline constexpr uint16_t kCaptureVersion = 1;
inline constexpr int kKeyBytes = 48;
inline constexpr int kHeaderBytes = 40 + 8 + 4 + 4 * kColumns + kKeyBytes * kButtons;
inline constexpr int kMaxFails = 64; // refused sends recorded per scan
// the longest record: flags, seq, config, calibration, cycles, 17 columns, sends
inline constexpr int kMaxRecordBytes = 1 + 5 + 10 + 5 * kButtons + 5 + kColumns * (1 + kRows * 5 * 3) + 1 + 5 + 1 + 2 + 5 + 5 * kMaxFails;

enum : uint8_t { kRecSeq = 1,
	kRecConfig = 2,
	kRecCal = 4 };
enum : uint8_t { kEndTorn = 1,
	kEndFailsTruncated = 2 };

[[nodiscard]] constexpr uint32_t zigzag(int32_t v) { return (static_cast<uint32_t>(v) << 1) ^ static_cast<uint32_t>(v >> 31); }
[[nodiscard]] constexpr int32_t unzigzag(uint32_t u) { return static_cast<int32_t>(u >> 1) ^ -static_cast<int32_t>(u & 1); }

[[nodiscard]] inline uint32_t fnv1a(uint32_t h, const int (&msg)[6]) {
	for (int v : msg) {
		for (int i = 0; i < 4; i++) {
			h ^= (static_cast<uint32_t>(v) >> (8 * i)) & 0xffU;
			h *= 16777619U;
		}
	}
	return h;
}
inline constexpr uint32_t kFnvBasis = 2166136261U;

[[nodiscard]] inline uint32_t fnv1a_bytes(uint32_t h, const uint8_t* p, size_t n) {
	for (size_t i = 0; i < n; i++) {
		h ^= p[i];
		h *= 16777619U;
	}
	return h;
}

struct ByteWriter {
	uint8_t* p;
	void u8(uint32_t v) { *p++ = static_cast<uint8_t>(v); }
	void u16(uint32_t v) {
		u8(v);
		u8(v >> 8);
	}
	void u32(uint32_t v) {
		u16(v);
		u16(v >> 16);
	}
	void var(uint32_t v) {
		while (v >= 0x80) {
			u8((v & 0x7f) | 0x80);
			v >>= 7;
		}
		u8(v);
	}
	void zz(int32_t v) { var(zigzag(v)); }
};

struct ByteReader {
	const uint8_t* p;
	const uint8_t* end;
	bool ok = true;
	uint32_t u8() {
		if (p >= end) {
			ok = false;
			return 0;
		}
		return *p++;
	}
	uint32_t u16() {
		const uint32_t lo = u8();
		return lo | (u8() << 8);
	}
	uint32_t u32() {
		const uint32_t lo = u16();
		return lo | (u16() << 16);
	}
	uint32_t var() {
		uint32_t v = 0;
		for (int shift = 0; shift < 35; shift += 7) {
			const uint32_t b = u8();
			v |= (b & 0x7f) << shift;
			if (!(b & 0x80)) return v;
		}
		ok = false;
		return v;
	}
	int32_t zz() { return unzigzag(var()); }
};

// the KeyScanner's state at the start of a recording
inline void write_snapshot(ByteWriter& w, const KeyScanner& s) {
	w.u32(static_cast<uint32_t>(s.cfg.zero_offset));
	w.u32(static_cast<uint32_t>(s.cfg.message_interval));
	w.u32(static_cast<uint32_t>(s.buttons_pressed));
	for (int32_t c : s.col_pressed) w.u32(static_cast<uint32_t>(c));
	for (const KeyState& k : s.keys) {
		for (int32_t v : {k.pres, k.velo, k.c_force, k.c_breakpoint, k.c_force2, k.zero_offset, k.zero_time, k.zero_max, k.key_detect3}) {
			w.u32(static_cast<uint32_t>(v));
		}
		uint32_t fb = 0;
		std::memcpy(&fb, &k.fact, 4);
		w.u32(fb);
		w.u32(static_cast<uint32_t>(k.timer));
		w.u32(static_cast<uint32_t>(k.status));
	}
}

inline void read_snapshot(ByteReader& r, KeyScanner& s) {
	s.init();
	s.cfg.zero_offset = static_cast<int32_t>(r.u32());
	s.cfg.message_interval = static_cast<int32_t>(r.u32());
	s.buttons_pressed = static_cast<int32_t>(r.u32());
	for (int32_t& c : s.col_pressed) c = static_cast<int32_t>(r.u32());
	for (KeyState& k : s.keys) {
		for (int32_t* v : {&k.pres, &k.velo, &k.c_force, &k.c_breakpoint, &k.c_force2, &k.zero_offset, &k.zero_time, &k.zero_max, &k.key_detect3}) {
			*v = static_cast<int32_t>(r.u32());
		}
		const uint32_t fb = r.u32();
		std::memcpy(&k.fact, &fb, 4);
		k.timer = static_cast<int32_t>(r.u32());
		k.status = static_cast<Status>(r.u32() & 0xff);
	}
}

struct CaptureHeader {
	uint32_t frames = 0;
	uint32_t record_bytes = 0;
	uint32_t seconds = 0;
	uint32_t flags = 0;
	uint32_t first_seq = 0;
	uint32_t first_t_start = 0;
	uint32_t stats_bytes = 0; // after the records: kStatsBytes, or 0
};

// the noise floor: sum, sum of squares and max of a sample
struct Acc {
	uint32_t n = 0;
	uint32_t sum = 0;
	uint64_t sumsq = 0;
	uint32_t max = 0;
	void add(uint32_t v) {
		n++;
		sum += v;
		sumsq += static_cast<uint64_t>(v) * v;
		if (v > max) max = v;
	}
};

struct NoiseStats {
	std::array<Acc, kButtons> on{};
	std::array<Acc, kButtons> p{};
};

inline constexpr int kStatsBytes = kButtons * 2 * 20;

inline void write_stats(uint8_t* out, const NoiseStats& s) {
	ByteWriter w{out};
	for (const auto* a : {&s.on, &s.p}) {
		for (const Acc& x : *a) {
			w.u32(x.n);
			w.u32(x.sum);
			w.u32(static_cast<uint32_t>(x.sumsq));
			w.u32(static_cast<uint32_t>(x.sumsq >> 32));
			w.u32(x.max);
		}
	}
}

inline void read_stats(ByteReader& r, NoiseStats& s) {
	for (auto* a : {&s.on, &s.p}) {
		for (Acc& x : *a) {
			x.n = r.u32();
			x.sum = r.u32();
			const uint64_t lo = r.u32();
			x.sumsq = lo | (static_cast<uint64_t>(r.u32()) << 32);
			x.max = r.u32();
		}
	}
}

inline constexpr int kHeaderFixedBytes = 40;

// the header's first kHeaderFixedBytes: the counts
inline void write_header_fixed(uint8_t* out, const CaptureHeader& h) {
	ByteWriter w{out};
	w.u32(kCaptureMagic);
	w.u16(kCaptureVersion);
	w.u16(kHeaderBytes);
	w.u32(h.frames);
	w.u32(h.record_bytes);
	w.u32(h.seconds);
	w.u32(h.flags);
	w.u32(h.first_seq);
	w.u32(h.first_t_start);
	w.u32(h.stats_bytes);
	w.u32(0); // reserved
}

inline void write_header(uint8_t* out, const CaptureHeader& h, const KeyScanner& snapshot) {
	write_header_fixed(out, h);
	ByteWriter w{out + kHeaderFixedBytes};
	write_snapshot(w, snapshot);
}

// the header (at least kHeaderBytes); false if it isn't one
inline bool read_header(const uint8_t* p, size_t n, CaptureHeader& h, KeyScanner& snapshot) {
	if (n < static_cast<size_t>(kHeaderBytes)) return false;
	ByteReader r{p, p + n};
	if (r.u32() != kCaptureMagic || r.u16() != kCaptureVersion || r.u16() != kHeaderBytes) return false;
	h.frames = r.u32();
	h.record_bytes = r.u32();
	h.seconds = r.u32();
	h.flags = r.u32();
	h.first_seq = r.u32();
	h.first_t_start = r.u32();
	h.stats_bytes = r.u32();
	(void)r.u32();
	read_snapshot(r, snapshot);
	return r.ok;
}

// Builds one scan's record while the scanner thread processes it (the board: key_scanner.cpp).
class RecordWriter {
public:
	std::array<uint8_t, kMaxRecordBytes> buf{};
	int len = 0;

	// before the first record: the last values start at 0, the previous scan is the first one
	void reset(uint32_t first_seq, uint32_t first_t_start) {
		last_ = {};
		seq_ = first_seq - 1;
		t_start_ = first_t_start;
	}

	void begin(uint32_t seq, uint32_t t_start, const Config* cfg, const int32_t* c_force) {
		ByteWriter w{buf.data()};
		const uint8_t flags = (seq != seq_ + 1 ? kRecSeq : 0) | (cfg ? kRecConfig : 0) | (c_force ? kRecCal : 0);
		w.u8(flags);
		if (flags & kRecSeq) w.var(seq);
		if (cfg) {
			w.zz(cfg->zero_offset);
			w.zz(cfg->message_interval);
		}
		if (c_force) {
			for (int b = 0; b < kButtons; b++) w.zz(c_force[b]);
		}
		w.var(t_start - t_start_);
		seq_ = seq;
		t_start_ = t_start;
		len = static_cast<int>(w.p - buf.data());
		attempts_ = 0;
		fails_ = 0;
		end_flags_ = 0;
		hash_ = kFnvBasis;
	}

	// column col of frame f as KeyScanner::load_column reads it
	void column(const ScanFrame& f, int col) {
		bool any = false;
		for (int n = 0; n < kRows; n++) any = any || f.on[col + n * kColumns] > kKeyDetect;
		if (!any) return;
		ByteWriter w{buf.data() + len};
		w.var(static_cast<uint32_t>(col + 1));
		for (int n = 0; n < kRows; n++) {
			const int b = col + n * kColumns;
			const bool up = f.on[b] > kKeyDetect;
			put(w, b, 0, up ? f.on[b] : 0);
			put(w, b, 1, f.m[b].p);
			if (up) {
				put(w, b, 2, f.m[b].s0);
				put(w, b, 3, f.m[b].s1);
				put(w, b, 4, f.m[b].s2);
			}
		}
		len = static_cast<int>(w.p - buf.data());
	}

	void send(const int (&msg)[6], int rc) {
		hash_ = fnv1a(hash_, msg);
		if (rc != 0) {
			if (fails_ < kMaxFails) {
				fail_idx_[fails_++] = attempts_;
			} else {
				end_flags_ |= kEndFailsTruncated;
			}
		}
		attempts_++;
	}

	void end(bool torn) {
		ByteWriter w{buf.data() + len};
		w.var(0);
		w.var(attempts_);
		w.u8(end_flags_ | (torn ? kEndTorn : 0));
		if (attempts_) {
			w.u16(hash_ & 0xffffU);
			w.var(static_cast<uint32_t>(fails_));
			for (int i = 0; i < fails_; i++) w.var(fail_idx_[i]);
		}
		len = static_cast<int>(w.p - buf.data());
	}

private:
	void put(ByteWriter& w, int b, int field, int32_t v) {
		int32_t& last = last_[b][field];
		w.zz(v - last);
		last = v;
	}

	std::array<std::array<int32_t, 5>, kButtons> last_{};
	uint32_t seq_ = 0;
	uint32_t t_start_ = 0;
	uint32_t attempts_ = 0;
	int fails_ = 0;
	uint8_t end_flags_ = 0;
	uint32_t hash_ = kFnvBasis;
	std::array<uint32_t, kMaxFails> fail_idx_{};
};

// One decoded scan record (the PC).
struct FrameRecord {
	ScanFrame frame{};
	bool has_config = false;
	Config cfg{};
	bool has_cal = false;
	std::array<int32_t, kButtons> c_force{};
	uint32_t cycles = 0;
	uint32_t attempts = 0;
	uint8_t end_flags = 0;
	uint16_t hash = 0;
	std::array<uint32_t, kMaxFails> fails{};
	int nfails = 0;
};

class RecordReader {
public:
	explicit RecordReader(ByteReader r) :
	    r_(r) {}

	void reset(uint32_t first_seq, uint32_t first_t_start) {
		last_ = {};
		seq_ = first_seq - 1;
		t_start_ = first_t_start;
	}

	// false at the end or on a malformed record (then ok() is false)
	bool next(FrameRecord& rec) {
		if (r_.p >= r_.end) return false;
		rec = FrameRecord{};
		const uint32_t flags = r_.u8();
		seq_ = (flags & kRecSeq) ? r_.var() : seq_ + 1;
		rec.frame.seq = seq_;
		if (flags & kRecConfig) {
			rec.has_config = true;
			rec.cfg.zero_offset = r_.zz();
			rec.cfg.message_interval = r_.zz();
		}
		if (flags & kRecCal) {
			rec.has_cal = true;
			for (int32_t& c : rec.c_force) c = r_.zz();
		}
		rec.cycles = r_.var();
		t_start_ += rec.cycles;
		rec.frame.t_start = t_start_;
		for (;;) {
			const uint32_t c = r_.var();
			if (c == 0 || !r_.ok) break;
			if (c > static_cast<uint32_t>(kColumns)) {
				r_.ok = false;
				break;
			}
			const int col = static_cast<int>(c) - 1;
			rec.frame.cols[rec.frame.n++] = static_cast<uint8_t>(col);
			for (int n = 0; n < kRows; n++) {
				const int b = col + n * kColumns;
				const int32_t on = get(b, 0);
				rec.frame.on[b] = static_cast<uint16_t>(on);
				rec.frame.m[b].p = static_cast<uint16_t>(get(b, 1));
				if (on > 0) {
					rec.frame.m[b].s0 = static_cast<uint16_t>(get(b, 2));
					rec.frame.m[b].s1 = static_cast<uint16_t>(get(b, 3));
					rec.frame.m[b].s2 = static_cast<uint16_t>(get(b, 4));
				}
			}
		}
		rec.attempts = r_.var();
		rec.end_flags = static_cast<uint8_t>(r_.u8());
		if (rec.attempts) {
			rec.hash = static_cast<uint16_t>(r_.u16());
			rec.nfails = static_cast<int>(r_.var());
			if (rec.nfails > kMaxFails) r_.ok = false;
			for (int i = 0; i < rec.nfails && r_.ok; i++) rec.fails[i] = r_.var();
		}
		return r_.ok;
	}

	[[nodiscard]] bool ok() const { return r_.ok; }

private:
	int32_t get(int b, int field) {
		int32_t& last = last_[b][field];
		last += r_.zz();
		if (last < 0 || last > kSampleMax) r_.ok = false; // the ADC can't give it
		return last;
	}

	ByteReader r_;
	std::array<std::array<int32_t, 5>, kButtons> last_{};
	uint32_t seq_ = 0;
	uint32_t t_start_ = 0;
};

} // namespace ks
