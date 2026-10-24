// The KeyScanner (src/drivers/key_scanner.hpp) against the key processing it replaced
// (tests/ref/button_read_ref.c: button_read.c's update_button and cross talk correction,
// unchanged), bit for bit: the same messages and the same state after every scan, on synthetic
// scans (tests/scan_sim.hpp): ramps, taps, holds, pairs in a column, corners, chords, changing
// configuration and calibration, a full message queue. And the raw recording format
// (key_scanner_capture.hpp): what the board records replays to the same messages.
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <limits>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "doctest/doctest.h"
#include "key_scanner.hpp"
#include "key_scanner_capture.hpp"
#include "scan_replay.hpp"
#include "scan_sim.hpp"

extern "C" {
#include "button_read_ref.h"
}

namespace {

using Msg6 = std::array<int, 6>;

// which send attempts the message queue refuses (by attempt number)
struct Refusals {
	int every = 0; // 0: never; else every n-th attempt, and the 2 after it (note-offs retry)
	[[nodiscard]] bool refuse(uint32_t attempt) const { return every && (attempt % static_cast<uint32_t>(every)) < 3; }
};

struct RefLog {
	std::vector<Msg6> msgs; // every attempt
	uint32_t attempts = 0;
	int waits = 0;
	Refusals refusals;
} g_ref;

} // namespace

extern "C" int ref_send(int size, int* msg) {
	REQUIRE(size == 6);
	Msg6 m{};
	std::copy(msg, msg + 6, m.begin());
	g_ref.msgs.push_back(m);
	return g_ref.refusals.refuse(g_ref.attempts++) ? 1 : 0;
}

extern "C" void ref_wait(int ticks) {
	REQUIRE(ticks == 1);
	g_ref.waits++;
}

namespace {

struct TestSink {
	std::vector<Msg6> msgs;
	uint32_t attempts = 0;
	int waits = 0;
	Refusals refusals;
	ks::RecordWriter* rec = nullptr;
	int send(const int (&msg)[6]) {
		Msg6 m{};
		std::copy(msg, msg + 6, m.begin());
		msgs.push_back(m);
		const int rc = refusals.refuse(attempts++) ? 1 : 0;
		if (rec) rec->send(msg, rc);
		return rc;
	}
	void wait() { waits++; }
};

std::string diff_state(const ks::KeyScanner& s) {
	std::ostringstream o;
	for (int b = 0; b < ks::kButtons; b++) {
		ref_key_state r{};
		ref_get_state(b, &r);
		const ks::KeyState& k = s.keys[b];
		if (r.pres != k.pres || r.velo != k.velo || r.zero_offset != k.zero_offset || r.zero_time != k.zero_time ||
		    r.zero_max != k.zero_max || r.key_detect3 != k.key_detect3 || std::bit_cast<uint32_t>(r.fact) != std::bit_cast<uint32_t>(k.fact) ||
		    r.status != static_cast<int>(k.status) || r.timer != k.timer) {
			o << "button " << b << ": ref pres " << r.pres << " velo " << r.velo << " zero " << r.zero_offset << " kd3 " << r.key_detect3
			  << " fact " << r.fact << " status " << r.status << " timer " << r.timer << " / scanner pres " << k.pres << " velo " << k.velo
			  << " zero " << k.zero_offset << " kd3 " << k.key_detect3 << " fact " << k.fact << " status " << static_cast<int>(k.status)
			  << " timer " << k.timer;
			return o.str();
		}
	}
	if (ref_buttons_pressed() != s.buttons_pressed) return "buttons_pressed differs";
	for (int c = 0; c < ks::kColumns; c++) {
		if (ref_col_pressed(c) != s.col_pressed[c]) return "col_pressed differs";
	}
	return "";
}

struct Scenario {
	uint32_t seed = 1;
	int frames = 2000;
	int boot_frames = 0; // the zero level pass first
	std::vector<scansim::Press> presses;
	// per frame changes of the configuration and calibration (applied to both before the scan)
	std::function<void(int frame, ks::Config& cfg)> config;
	std::function<void(int frame, std::array<int32_t, ks::kButtons>& c_force)> calibration;
	Refusals refusals;
	float noise = 3.0f;
	bool compare_ref = true; // false: the KeyScanner alone (where it differs from the original on purpose)
};

struct Outcome {
	std::vector<Msg6> msgs;
	std::vector<bool> queued; // per attempt: the queue took it
	int waits = 0;
	int notes = 0; // note starts (pres > 0 after a 0 or the first message of a key)
	std::string error;
};

// runs a scenario through the reference and the KeyScanner, scan by scan
Outcome run(const Scenario& sc, ks::KeyScanner& s, ks::RecordWriter* rec = nullptr, int rec_from = -1, int rec_to = -1,
    std::vector<uint8_t>* blob = nullptr) {
	Outcome out;
	scansim::Board board(sc.seed);
	board.presses = sc.presses;
	board.noise = sc.noise;
	ScanFrame f{};
	ref_init();
	s.init();
	g_ref = RefLog{};
	g_ref.refusals = sc.refusals;
	TestSink sink;
	sink.refusals = sc.refusals;
	std::array<int32_t, ks::kButtons> c_force{};
	for (int b = 0; b < ks::kButtons; b++) c_force[b] = s.keys[b].c_force;
	ks::CaptureHeader head;
	std::vector<uint8_t> records;
	for (int frame = 0; frame < sc.frames; frame++) {
		ks::Config cfg = s.cfg;
		if (sc.config) sc.config(frame, cfg);
		const bool cfg_changed = cfg.zero_offset != s.cfg.zero_offset || cfg.message_interval != s.cfg.message_interval;
		s.cfg = cfg;
		ref_config.zero_offset = cfg.zero_offset;
		ref_config.message_interval = cfg.message_interval;
		bool cal_changed = false;
		if (sc.calibration) {
			std::array<int32_t, ks::kButtons> c = c_force;
			sc.calibration(frame, c);
			cal_changed = c != c_force;
			c_force = c;
			for (int b = 0; b < ks::kButtons; b++) {
				s.keys[b].c_force = c[b];
				ref_set_c_force(b, c[b]);
			}
		}
		board.scan(frame, f);
		// the interrupt's writes, as the original thread saw them
		for (int b = 0; b < ks::kButtons; b++) {
			if (board.measured(f, b % ks::kColumns)) {
				ref_set_inputs(b, f.on[b], f.m[b].p, f.m[b].s0, f.m[b].s1, f.m[b].s2);
			} else {
				ref_set_on(b, f.on[b]);
			}
		}
		const bool recording = rec && frame >= rec_from && frame < rec_to;
		if (recording) {
			if (frame == rec_from) {
				head.first_seq = f.seq;
				head.first_t_start = f.t_start;
				blob->assign(ks::kHeaderBytes, 0);
				ks::write_header(blob->data(), head, s);
				rec->reset(f.seq, f.t_start);
			}
			rec->begin(f.seq, f.t_start, cfg_changed ? &s.cfg : nullptr, cal_changed ? c_force.data() : nullptr);
			sink.rec = rec;
		}
		for (int col = 0; col < ks::kColumns; col++) {
			if (frame < sc.boot_frames) {
				ref_boot_column(col);
				s.boot_column(f, col);
			} else {
				ref_column(col);
				if (recording) rec->column(f, col);
				s.process_column(f, col, sink);
			}
		}
		if (recording) {
			rec->end(false);
			records.insert(records.end(), rec->buf.begin(), rec->buf.begin() + rec->len);
			head.frames++;
			sink.rec = nullptr;
		}
		const std::string d = sc.compare_ref ? diff_state(s) : std::string();
		if (!d.empty()) {
			out.error = "scan " + std::to_string(frame) + ": " + d;
			break;
		}
		if (sc.compare_ref && sink.msgs != g_ref.msgs) {
			out.error = "scan " + std::to_string(frame) + ": messages differ";
			break;
		}
	}
	if (rec && blob && !blob->empty()) {
		head.record_bytes = static_cast<uint32_t>(records.size());
		ks::write_header_fixed(blob->data(), head);
		blob->insert(blob->end(), records.begin(), records.end());
	}
	out.msgs = sink.msgs;
	out.waits = sink.waits;
	for (uint32_t a = 0; a < sink.attempts; a++) out.queued.push_back(!sc.refusals.refuse(a));
	if (sc.compare_ref && out.error.empty() && g_ref.waits != sink.waits) out.error = "waits differ";
	std::array<bool, ks::kButtons> down{};
	for (const Msg6& m : out.msgs) {
		if (m[2] > 0 && !down[m[1]]) out.notes++;
		down[m[1]] = m[2] > 0;
	}
	return out;
}

ks::KeyScanner scanner; // 4 KB: not on the stack

int col_row(int col, int row) { return col + row * ks::kColumns; }

} // namespace

TEST_CASE("key scanner: the board's int32 wrap and float to int conversion") {
	CHECK(ks::wmul(4097, 1048320) == static_cast<int32_t>(4097U * 1048320U));
	CHECK(ks::wadd(INT32_MAX, 1) == INT32_MIN);
	CHECK(ks::f2i(3.0e9f) == INT32_MAX);
	CHECK(ks::f2i(-3.0e9f) == INT32_MIN);
	CHECK(ks::f2i(-2147483648.0f) == INT32_MIN);
	CHECK(ks::f2i(std::numeric_limits<float>::quiet_NaN()) == 0);
	CHECK(ks::f2i(-1.9f) == -1);
	CHECK(ks::f2i(1234567.9f) == 1234567);
	for (int32_t v : {0, 1, -1, 63, -64, 64, 8191, -8192, INT32_MAX, INT32_MIN}) CHECK(ks::unzigzag(ks::zigzag(v)) == v);
}

TEST_CASE("key scanner: ramps and taps, as the original") {
	Scenario sc;
	sc.seed = 11;
	sc.presses = {
	    {col_row(3, 0), scansim::ramp(50, 300, 200, 300, 1.2f)}, // slow press and release
	    {col_row(7, 1), scansim::tap(100, 1.5f), 0.2f, -0.1f},   // fast strikes
	    {col_row(7, 1), scansim::tap(400, 0.6f), -0.2f, 0.1f},
	    {col_row(12, 2), scansim::ramp(200, 10, 900, 5, 1.9f), 0.3f}, // hard press, near saturation
	    {col_row(0, 3), scansim::tap(700, 0.3f)},
	    {col_row(16, 1), scansim::ramp(900, 2, 400, 2, 0.8f), 0.0f, 0.4f},
	};
	const Outcome o = run(sc, scanner);
	CHECK_MESSAGE(o.error.empty(), o.error);
	CHECK(o.notes >= 6);
	CHECK(o.msgs.size() > 500);
}

TEST_CASE("key scanner: light holds, the zero level and the boot pass, as the original") {
	Scenario sc;
	sc.seed = 12;
	sc.boot_frames = ks::kBootFrames;
	sc.frames = 3000;
	sc.presses = {
	    {col_row(5, 0), scansim::hold(0, 300, 0.02f)},    // touched at boot
	    {col_row(9, 2), scansim::hold(200, 2400, 0.03f)}, // a steady pp note (the zero level learns it)
	    {col_row(2, 1), scansim::hold(400, 1500, 0.15f)},
	    {col_row(14, 3), scansim::hold(1000, 1600, 0.01f)},
	};
	const Outcome o = run(sc, scanner);
	CHECK_MESSAGE(o.error.empty(), o.error);
	CHECK(scanner.keys[col_row(5, 0)].zero_offset > 0); // the boot pass took the touched key's level
	CHECK(o.notes >= 2);
}

TEST_CASE("key scanner: pairs in one column, as the original") {
	Scenario sc;
	sc.seed = 13;
	sc.frames = 2500;
	sc.presses = {
	    {col_row(4, 0), scansim::ramp(50, 40, 800, 40, 1.0f)},
	    {col_row(4, 1), scansim::ramp(300, 5, 300, 5, 0.4f)},  // a second key in the column
	    {col_row(4, 2), scansim::ramp(500, 5, 600, 5, 0.08f)}, // a light third one
	    {col_row(10, 3), scansim::ramp(1200, 30, 600, 30, 0.9f)},
	    {col_row(10, 0), scansim::ramp(1300, 5, 200, 5, 0.05f)}, // light key next to a hard one
	    {col_row(10, 1), scansim::tap(1600, 1.2f)},
	};
	const Outcome o = run(sc, scanner);
	CHECK_MESSAGE(o.error.empty(), o.error);
	CHECK(o.notes >= 5);
}

TEST_CASE("key scanner: three and four corners, as the original") {
	Scenario sc;
	sc.seed = 14;
	sc.frames = 2500;
	// a rectangle: columns 2 and 9, rows 0 and 2; the corners come one by one
	sc.presses = {
	    {col_row(2, 0), scansim::ramp(50, 20, 1500, 20, 0.9f)},
	    {col_row(2, 2), scansim::ramp(150, 20, 1200, 20, 0.7f)},
	    {col_row(9, 0), scansim::ramp(300, 20, 900, 20, 0.8f)},
	    {col_row(9, 2), scansim::ramp(600, 20, 300, 20, 0.5f)}, // the fourth corner
	    {col_row(13, 1), scansim::ramp(1700, 10, 300, 10, 0.6f)},
	    {col_row(13, 3), scansim::ramp(1750, 10, 300, 10, 0.6f)},
	    {col_row(6, 1), scansim::ramp(1800, 10, 300, 10, 0.3f)},
	};
	const Outcome o = run(sc, scanner);
	CHECK_MESSAGE(o.error.empty(), o.error);
	CHECK(o.notes >= 6);
}

TEST_CASE("key scanner: chords, configuration and calibration changes, as the original") {
	for (uint32_t seed = 21; seed < 33; seed++) {
		CAPTURE(seed);
		std::mt19937 rng(seed);
		Scenario sc;
		sc.seed = seed;
		sc.frames = 1800;
		sc.noise = 1.0f + static_cast<float>(rng() % 8);
		for (int i = 0; i < 14; i++) {
			const int b = static_cast<int>(rng() % ks::kButtons);
			const int start = static_cast<int>(rng() % 1500);
			const float peak = 0.02f + static_cast<float>(rng() % 1800) / 1000.0f;
			const int rise = 1 + static_cast<int>(rng() % 60);
			const float tx = static_cast<float>(static_cast<int>(rng() % 61) - 30) / 100.0f;
			const float ty = static_cast<float>(static_cast<int>(rng() % 61) - 30) / 100.0f;
			sc.presses.push_back({b, scansim::ramp(start, rise, static_cast<int>(rng() % 400), 1 + static_cast<int>(rng() % 60), peak), tx, ty});
		}
		sc.config = [seed](int frame, ks::Config& cfg) {
			if (frame == 0) cfg = ks::Config{0, 1};
			if (frame == 600) cfg.message_interval = static_cast<int32_t>(seed % 4); // 0: every scan (B1)
			if (frame == 900) cfg.zero_offset = static_cast<int32_t>((seed % 3) * ((1 << 24) / 128) * 4);
			if (frame == 1400) cfg.message_interval = 15;
		};
		sc.calibration = [seed](int frame, std::array<int32_t, ks::kButtons>& c) {
			if (frame == 0 || frame == 1000) {
				std::mt19937 r(seed * 7 + static_cast<uint32_t>(frame));
				for (int b = 0; b < ks::kButtons; b++) {
					if (c[b]) c[b] = 1500 + static_cast<int32_t>(r() % 8000); // factory x key factor
				}
			}
		};
		// (with a full queue the KeyScanner differs from the original on purpose: S4)
		const Outcome o = run(sc, scanner);
		CHECK_MESSAGE(o.error.empty(), o.error);
	}
}

TEST_CASE("key scanner: a raw recording replays to the board's messages") {
	Scenario sc;
	sc.seed = 41;
	sc.frames = 2500;
	std::mt19937 rng(41);
	for (int i = 0; i < 20; i++) {
		const int b = static_cast<int>(rng() % ks::kButtons);
		sc.presses.push_back({b, scansim::ramp(static_cast<int>(rng() % 2200), 1 + static_cast<int>(rng() % 40), static_cast<int>(rng() % 500), 1 + static_cast<int>(rng() % 40), 0.05f + static_cast<float>(rng() % 1500) / 1000.0f), 0.1f, -0.1f});
	}
	sc.config = [](int frame, ks::Config& cfg) {
		if (frame == 1200) cfg.message_interval = 2;
	};
	sc.calibration = [](int frame, std::array<int32_t, ks::kButtons>& c) {
		if (frame == 1500) c[20] = c[20] * 2; // a live key factor
	};
	sc.refusals.every = 31;
	sc.compare_ref = false; // a full queue: S4
	ks::RecordWriter rec;
	std::vector<uint8_t> blob;
	const int from = 700;
	const Outcome o = run(sc, scanner, &rec, from, 2300, &blob);
	REQUIRE_MESSAGE(o.error.empty(), o.error);

	const scanreplay::Result r = scanreplay::replay(blob);
	REQUIRE_MESSAGE(r.error.empty(), r.error);
	CHECK(r.frames == 1600);
	CHECK(r.mismatched == 0);
	CHECK(r.refused > 0);
	// the live run's messages of scans 700..2299: count them by replaying the frame indices
	std::vector<Msg6> replayed;
	for (const scanreplay::Msg& m : r.msgs) replayed.push_back(m.v);
	REQUIRE(!replayed.empty());
	// they are a contiguous run of the live messages
	bool found = false;
	for (size_t off = 0; off + replayed.size() <= o.msgs.size() && !found; off++) {
		found = std::equal(replayed.begin(), replayed.end(), o.msgs.begin() + static_cast<long>(off));
	}
	CHECK(found);
	CHECK(blob.size() < 1600u * 120u); // a few bytes per scan

	// the stream check: the queued messages, on the wire, are found in the live stream
	std::vector<Msg6> stream;
	{
		uint32_t a = 0;
		for (const Msg6& m : o.msgs) {
			if (!sc.refusals.refuse(a++)) stream.push_back(scanreplay::on_wire(m));
		}
	}
	int matches = 0;
	CHECK(scanreplay::find_in_stream(r.msgs, stream, &matches) >= 0);
	CHECK(matches == 1);

	// a corrupted recording is noticed (a flipped byte may hit a value no message depends on: many)
	std::vector<uint8_t> bad = blob;
	for (size_t i = ks::kHeaderBytes + 50; i < bad.size(); i += 997) bad[i] ^= 0x55;
	const scanreplay::Result rb = scanreplay::replay(bad);
	CHECK((!rb.error.empty() || rb.mismatched > 0));
}

TEST_CASE("key scanner S4: a full message queue never blocks the scanner, and no note-off is lost") {
	Scenario sc;
	sc.seed = 51;
	sc.frames = 3000;
	std::mt19937 rng(51);
	for (int i = 0; i < 40; i++) {
		const int b = static_cast<int>(rng() % ks::kButtons);
		sc.presses.push_back({b, scansim::ramp(static_cast<int>(rng() % 2500), 1 + static_cast<int>(rng() % 20), static_cast<int>(rng() % 200), 1 + static_cast<int>(rng() % 10), 0.1f + static_cast<float>(rng() % 1200) / 1000.0f)});
	}
	sc.config = [](int frame, ks::Config& cfg) {
		if (frame == 0) cfg.message_interval = 0; // every key every scan: the queue overflows (B1)
	};
	sc.refusals.every = 7; // 3 of every 7 sends refused
	sc.compare_ref = false;
	const Outcome o = run(sc, scanner);
	REQUIRE_MESSAGE(o.error.empty(), o.error);
	CHECK(o.waits == 0); // the scanner never sleeps waiting for the queue
	// every key was released by the end: the last message the queue took for it is a note-off,
	// and between a key's note-off and its next press the queue took no update of it
	std::array<int, ks::kButtons> last{};
	last.fill(-1);
	int offs = 0;
	for (size_t i = 0; i < o.msgs.size(); i++) {
		if (!o.queued[i]) continue;
		const Msg6& m = o.msgs[i];
		last[m[1]] = m[2];
		offs += m[2] == 0;
	}
	for (int b = 0; b < ks::kButtons; b++) {
		CAPTURE(b);
		CHECK((last[b] == -1 || last[b] == 0));
		CHECK(scanner.keys[b].status == ks::Status::off);
	}
	CHECK(offs >= 20);
}
