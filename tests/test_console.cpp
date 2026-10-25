// The USB console's grammar (src/app/console.hpp) and USB MIDI in (src/app/midi_in_parser.hpp) on
// the PC, with recording handlers in place of the board's. The board's handlers, its output stream
// and the USB driver underneath are tested end to end in tests/console_sim.
//
// The fuzz cases compare the Console with a reference written from the protocol's description
// (find "Stc", take the code and its payload; a gap longer than the stale interval drops what is
// pending), on random byte streams with random timing; under host-asan they also run under ASan
// and UBSan.
#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "console.hpp"
#include "doctest/doctest.h"
#include "midi_in_parser.hpp"

namespace {

struct Call {
	std::string handler;
	uint8_t code;
	std::vector<uint8_t> payload;
	bool operator==(const Call&) const = default;
};

struct Recorder {
	std::vector<Call> calls;
	int begins = 0;
	void begin() { begins++; }
	void add(const char* h, const console::Request& r) {
		calls.push_back(Call{h, r.code, std::vector<uint8_t>(r.payload.begin(), r.payload.end())});
	}
};

struct TestCommands {
	using Out = Recorder;
	using Req = console::Request;
	static inline int stops = 0;
	static void stop_stream() { stops++; }
	static void dfu(Out& o, const Req& r) { o.add("dfu", r); }
	static void bootloader(Out& o, const Req& r) { o.add("bootloader", r); }
	static void stream_on(Out& o, const Req& r) { o.add("stream_on", r); }
	static void stream_off(Out& o, const Req& r) { o.add("stream_off", r); }
	static void monitor_on(Out& o, const Req& r) { o.add("monitor_on", r); }
	static void monitor_off(Out& o, const Req& r) { o.add("monitor_off", r); }
	static void version(Out& o, const Req& r) { o.add("version", r); }
	static void threads(Out& o, const Req& r) { o.add("threads", r); }
	static void tuning(Out& o, const Req& r) { o.add("tuning", r); }
	static void settings_key(Out& o, const Req& r) { o.add("settings_key", r); }
	static void settings_layout(Out& o, const Req& r) { o.add("settings_layout", r); }
	static void state(Out& o, const Req& r) { o.add("state", r); }
	static void state_counter(Out& o, const Req& r) { o.add("state_counter", r); }
	static void heroes(Out& o, const Req& r) { o.add("heroes", r); }
	static void profile(Out& o, const Req& r) { o.add("profile", r); }
	static void voice_params(Out& o, const Req& r) { o.add("voice_params", r); }
	static void fx_params(Out& o, const Req& r) { o.add("fx_params", r); }
	static void touch_params(Out& o, const Req& r) { o.add("touch_params", r); }
	static void key_params(Out& o, const Req& r) { o.add("key_params", r); }
	static void set_param(Out& o, const Req& r) { o.add("set_param", r); }
	static void voices(Out& o, const Req& r) { o.add("voices", r); }
	static void select_voice(Out& o, const Req& r) { o.add("select_voice", r); }
	static void load(Out& o, const Req& r) { o.add("load", r); }
	static void audio_status(Out& o, const Req& r) { o.add("audio_status", r); }
	static void audio_target(Out& o, const Req& r) { o.add("audio_target", r); }
	static void audio_latency(Out& o, const Req& r) { o.add("audio_latency", r); }
	static void audio_tone(Out& o, const Req& r) { o.add("audio_tone", r); }
	static void calibration_reset(Out& o, const Req& r) { o.add("calibration_reset", r); }
	static void scanner_stats(Out& o, const Req& r) { o.add("scanner_stats", r); }
	static void scanner_keys(Out& o, const Req& r) { o.add("scanner_keys", r); }
	static void scanner_record(Out& o, const Req& r) { o.add("scanner_record", r); }
};

using TestConsole = console::Console<TestCommands>;
constexpr uint32_t kStale = 200; // 100 ms in 2 kHz ticks, as on the board

struct Fixture {
	Recorder out;
	TestConsole con{out, kStale};
	uint32_t now = 1000;
	// bytes arriving together (one host write), then `gap` ticks
	void send(const std::string& bytes, uint32_t gap = 1) {
		for (const char ch : bytes) con.feed(static_cast<uint8_t>(ch), now);
		now += gap;
	}
	[[nodiscard]] std::vector<std::string> handlers() const {
		std::vector<std::string> h;
		h.reserve(out.calls.size());
		for (const Call& c : out.calls) h.push_back(c.handler);
		return h;
	}
};

std::string bytes(std::initializer_list<int> b) {
	std::string s;
	for (const int x : b) s += static_cast<char>(x);
	return s;
}

const console::Command<Recorder>* entry(uint8_t c) {
	return console::find(TestConsole::table, c);
}

} // namespace

// --- the table: the protocol's commands -------------------------------------------------------

TEST_CASE("console table: every command of the protocol, its payload length") {
	// the protocol as Striso Live and tools/striso.py use it (README, protocol.dart)
	const std::string no_payload = "DBSsMmVINXYyPpfHgkvLUuCEZ";
	for (const char c : no_payload) {
		CAPTURE(c);
		REQUIRE(entry(static_cast<uint8_t>(c)) != nullptr);
		CHECK(entry(static_cast<uint8_t>(c))->payload_len == 0);
	}
	const std::pair<char, int> with_payload[] = {{'K', 1}, {'Q', 6}, {'t', 3}, {'l', 1}, {'R', 2}};
	for (const auto& cn : with_payload) {
		const char c = cn.first;
		const int n = cn.second;
		CAPTURE(c);
		REQUIRE(entry(static_cast<uint8_t>(c)) != nullptr);
		CHECK(entry(static_cast<uint8_t>(c))->payload_len == n);
	}
	CHECK(std::size(console::kCommands<TestCommands>) == no_payload.size() + std::size(with_payload) + 1);
	for (int c = 0x80; c <= 0xFF; c++) CHECK(entry(static_cast<uint8_t>(c)) == entry(0x80));
	for (int c = 0; c < 0x80; c++) {
		if (no_payload.find(static_cast<char>(c)) == std::string::npos && std::string("KQtlR").find(static_cast<char>(c)) == std::string::npos) {
			CAPTURE(c);
			CHECK(entry(static_cast<uint8_t>(c)) == nullptr);
		}
	}
}

TEST_CASE("console table: the flags") {
	for (const auto& e : console::kCommands<TestCommands>) {
		CAPTURE(e.code);
		const bool dangerous = e.code == 'C' || e.code == 'B' || e.code == 'D';
		CHECK(((e.flags & console::kDangerous) != 0) == dangerous);
		CHECK(((e.flags & console::kStopsStream) != 0) == (e.code == 'V'));
		CHECK(((e.flags & console::kHighBytes) != 0) == (e.code == 0x80));
	}
	CHECK(console::valid(TestConsole::table));
}

TEST_CASE("console table: valid() rejects a duplicate code, a long payload, a stray high code") {
	using C = console::Command<Recorder>;
	const C dup[] = {{'V', 0, &TestCommands::version, 0}, {'V', 0, &TestCommands::threads, 0}};
	const C longp[] = {{'Q', 7, &TestCommands::set_param, 0}};
	const C high[] = {{0x90, 0, &TestCommands::select_voice, 0}};
	const C two_high[] = {{0x80, 0, &TestCommands::select_voice, console::kHighBytes},
	    {0x80, 0, &TestCommands::voices, console::kHighBytes}};
	CHECK_FALSE(console::valid(std::span<const C>(dup)));
	CHECK_FALSE(console::valid(std::span<const C>(longp)));
	CHECK_FALSE(console::valid(std::span<const C>(high)));
	CHECK_FALSE(console::valid(std::span<const C>(two_high)));
}

// --- the grammar ----------------------------------------------------------------------------------

TEST_CASE("console: a command per prefix, payloads taken whole, the request's code and bytes") {
	Fixture f;
	f.send("StcV");
	f.send("StcK" + bytes({42}));
	f.send("StcQ" + bytes({0, 5, 0x53, 0x74, 0x63, 0x43}));
	f.send("Stct072StcR05Stcl1");
	f.send("Stc" + bytes({0x83}));
	REQUIRE(f.out.calls.size() == 7);
	CHECK(f.out.calls[0] == Call{"version", 'V', {}});
	CHECK(f.out.calls[1] == Call{"settings_key", 'K', {42}});
	CHECK(f.out.calls[2] == Call{"set_param", 'Q', {0, 5, 0x53, 0x74, 0x63, 0x43}});
	CHECK(f.out.calls[3] == Call{"audio_target", 't', {'0', '7', '2'}});
	CHECK(f.out.calls[4] == Call{"scanner_record", 'R', {'0', '5'}});
	CHECK(f.out.calls[5] == Call{"audio_latency", 'l', {'1'}});
	CHECK(f.out.calls[6] == Call{"select_voice", 0x83, {}});
	CHECK(f.out.begins == 7);
	CHECK(f.con.idle());
}

TEST_CASE("console: V stops the stream before its reply, nothing else does") {
	Fixture f;
	TestCommands::stops = 0;
	f.send("StcpStcfStckStcSStcs");
	CHECK(TestCommands::stops == 0);
	f.send("StcV");
	CHECK(TestCommands::stops == 1);
}

TEST_CASE("console: unknown codes and bytes outside a prefix do nothing") {
	Fixture f;
	f.send("hello Stc! St c S t c StcW Stc\r\n V K Q");
	CHECK(f.out.calls.empty());
	CHECK(f.con.idle());
}

TEST_CASE("console: an S in a partial prefix starts it again (R5)") {
	Fixture f;
	f.send("SStcV");
	f.send("StStcp");
	f.send("StSStcf");
	CHECK(f.handlers() == std::vector<std::string>{"version", "voice_params", "fx_params"});
}

TEST_CASE("console: 'S' after the prefix is the stream command, not a restart") {
	Fixture f;
	f.send("StcStcV");
	// "Stc" + 'S' is StcS; then "tcV" is no command
	CHECK(f.handlers() == std::vector<std::string>{"stream_on"});
}

TEST_CASE("console: payload bytes are never commands (R7)") {
	Fixture f;
	// float 227.454 is the bytes "StcC": the StcQ payload, not a calibration reset
	f.send("S" + std::string("StcQ") + bytes({0, 5}) + "StcC");
	REQUIRE(f.out.calls.size() == 1);
	CHECK(f.out.calls[0] == Call{"set_param", 'Q', {0, 5, 'S', 't', 'c', 'C'}});
	// a key id that is a prefix byte
	f.send("StcK" + std::string("S") + "tcV");
	REQUIRE(f.out.calls.size() == 2);
	CHECK(f.out.calls[1] == Call{"settings_key", 'K', {'S'}});
}

TEST_CASE("console: a payload or prefix that stops arriving is dropped after the stale interval (R6)") {
	const std::pair<std::string, const char*> partial[] = {
	    {"S", "S"}, {"St", "St"}, {"Stc", "Stc"}, {"Stct07", "Stct07"}, {"StcK", "StcK"}, {"StcR", "StcR"},
	    {"StcQ" + bytes({0, 1, 0, 0, 0}), "StcQ + 5 of 6"}};
	for (const auto& pn : partial) {
		const char* name = pn.second;
		CAPTURE(name);
		Fixture f;
		f.send(pn.first, kStale + 1);
		f.send("StcV");
		CHECK(f.handlers() == std::vector<std::string>{"version"});
	}
	// exactly the stale interval: still the same command
	Fixture f;
	f.send("StcK", kStale);
	f.send("V");
	CHECK(f.out.calls == std::vector<Call>{Call{"settings_key", 'K', {'V'}}});
}

TEST_CASE("console: a gap while idle drops nothing") {
	Fixture f;
	f.send("StcV", 100000);
	f.send("StcV");
	CHECK(f.out.calls.size() == 2);
	f.send("StcQ" + bytes({1, 2}));
	CHECK_FALSE(f.con.idle());
}

TEST_CASE("console: the clock wraps (free-running ticks)") {
	Fixture f;
	f.now = 0xFFFFFFF0u;
	f.send("StcQ" + bytes({0, 1}), 0x20); // crosses 0: 32 ticks, not stale
	f.send(bytes({0, 0, 0x80, 0x3F}));
	REQUIRE(f.out.calls.size() == 1);
	CHECK(f.out.calls[0].handler == "set_param");
	f.now = 0xFFFFFFF0u;
	f.send("StcK", kStale + 1); // crosses 0 and is stale
	f.send("StcV");
	CHECK(f.out.calls.back().handler == "version");
	CHECK(f.out.calls.size() == 2);
}

TEST_CASE("console: feed() returns the command it ran") {
	Fixture f;
	CHECK(f.con.feed('S', 0) == nullptr);
	CHECK(f.con.feed('t', 0) == nullptr);
	CHECK(f.con.feed('c', 0) == nullptr);
	const auto* v = f.con.feed('V', 0);
	REQUIRE(v != nullptr);
	CHECK(v->code == 'V');
}

// --- fuzz: the Console against a reference ---------------------------------------------------------

namespace {

// The protocol from its description: within a run of bytes with no gap above the stale interval,
// find "Stc", the next byte is the code; a known code takes its payload's bytes (any value); then
// the search goes on after it. What a gap cuts short is dropped.
std::vector<Call> reference(const std::vector<uint8_t>& b, const std::vector<uint32_t>& t) {
	std::vector<Call> calls;
	size_t start = 0;
	while (start < b.size()) {
		size_t end = start + 1;
		while (end < b.size() && t[end] - t[end - 1] <= kStale) end++;
		const std::string seg(b.begin() + static_cast<std::ptrdiff_t>(start), b.begin() + static_cast<std::ptrdiff_t>(end));
		size_t p = 0;
		for (;;) {
			const size_t k = seg.find("Stc", p);
			if (k == std::string::npos || k + 3 >= seg.size()) break;
			const auto code = static_cast<uint8_t>(seg[k + 3]);
			const auto* e = entry(code);
			if (e == nullptr) {
				p = k + 4;
				continue;
			}
			if (k + 4 + e->payload_len > seg.size()) break;
			Call c{"", code, {}};
			for (size_t i = 0; i < e->payload_len; i++) c.payload.push_back(static_cast<uint8_t>(seg[k + 4 + i]));
			// the handler's name, from the recorder's own dispatch of this code
			Recorder r;
			e->run(r, console::Request{code, c.payload});
			c.handler = r.calls.at(0).handler;
			calls.push_back(c);
			p = k + 4 + e->payload_len;
		}
		start = end;
	}
	return calls;
}

void fuzz_round(std::mt19937& rng, size_t len) {
	static const std::string alphabet = "StcStcStcDBSsMmVINKXYyPpfgkQvLUtluCEZR0123456789";
	std::vector<uint8_t> b(len);
	std::vector<uint32_t> t(len);
	uint32_t now = rng(); // anywhere, wraps included
	for (size_t i = 0; i < len; i++) {
		const uint32_t r = rng() % 16;
		b[i] = r < 10 ? static_cast<uint8_t>(alphabet[rng() % alphabet.size()]) : r < 12 ? static_cast<uint8_t>(0x80 + rng() % 128) :
		                                                                                   static_cast<uint8_t>(rng());
		const uint32_t g = rng() % 32;
		now += g < 26 ? 0 : g < 30 ? rng() % kStale :
		    g == 30                ? kStale + (rng() % 2) :
		                             kStale + 1 + rng() % 100000;
		t[i] = now;
	}
	Recorder out;
	TestConsole con{out, kStale};
	size_t ran = 0;
	for (size_t i = 0; i < len; i++) {
		if (con.feed(b[i], t[i]) != nullptr) ran++;
	}
	const std::vector<Call> want = reference(b, t);
	CHECK(ran == out.calls.size());
	if (out.calls != want) {
		// the first difference, for the report
		size_t i = 0;
		while (i < want.size() && i < out.calls.size() && want[i] == out.calls[i]) i++;
		FAIL("console differs from the reference at call " << i << " of " << want.size() << " (got "
		                                                   << out.calls.size() << ")");
	}
}

} // namespace

TEST_CASE("console fuzz: random byte streams and timing, the same commands as the reference") {
	std::mt19937 rng(20261024);
	for (int round = 0; round < 3000; round++) fuzz_round(rng, 1 + rng() % 400);
	for (int round = 0; round < 20; round++) fuzz_round(rng, 20000);
}

// --- USB MIDI in ---------------------------------------------------------------------------------

namespace {

std::vector<midi_in::Event> parse(const std::vector<uint8_t>& t) {
	std::vector<midi_in::Event> ev;
	midi_in::MidiInParser::transfer(t, [&](const midi_in::Event& e) { ev.push_back(e); });
	return ev;
}

bool same(const midi_in::Event& e, int port, int s, int d1, int d2) {
	return e.port == port && e.status == s && e.data1 == d1 && e.data2 == d2;
}

} // namespace

TEST_CASE("midi in: whole events of a transfer, the tail dropped (R9)") {
	const auto a = parse({0x09, 0x90, 0x3c, 0x64, 0x08, 0x80}); // note on, then 2 stray bytes
	REQUIRE(a.size() == 1);
	CHECK(same(a[0], 1, 0x90, 0x3c, 0x64));
	const auto b = parse({0x08, 0x80, 0x3c, 0x00, 0x0b, 0xb0, 0x07, 0x64}); // note off, CC7
	REQUIRE(b.size() == 2);
	CHECK(same(b[0], 1, 0x80, 0x3c, 0x00));
	CHECK(same(b[1], 1, 0xb0, 0x07, 0x64));
	CHECK(parse({}).empty());
	CHECK(parse({0x09, 0x90, 0x3c}).empty());
}

TEST_CASE("midi in: dispatch by CIN, 7-bit data only (R8)") {
	struct Case {
		const char* name;
		std::vector<uint8_t> e;
		bool pass;
	};
	const Case cases[] = {
	    {"CIN 0xF with 0xE0 (not real-time)", {0x0f, 0xe0, 0x00, 0x00}, false},
	    {"CC7 with data 0xFF", {0x0b, 0xb0, 0x07, 0xff}, false},
	    {"CIN 9 with status 0x80", {0x09, 0x80, 0x3c, 0x00}, false},
	    {"SysEx (CIN 4)", {0x04, 0xf0, 0x7e, 0x00}, false},
	    {"reserved CIN 0", {0x00, 0x90, 0x3c, 0x64}, false},
	    {"reserved CIN 1", {0x01, 0x90, 0x3c, 0x64}, false},
	    {"a status without bit 7", {0x09, 0x10, 0x3c, 0x64}, false},
	    {"note on, velocity 0x80", {0x09, 0x90, 0x3c, 0x80}, false},
	    {"CIN 0xF with 0xF8 (clock)", {0x0f, 0xf8, 0x00, 0x00}, true},
	    {"CC7 = 100", {0x0b, 0xb0, 0x07, 0x64}, true},
	    {"program change (CIN 0xC), data2 not read", {0x0c, 0xc0, 0x05, 0xff}, true},
	    {"pitch bend, cable 2", {0x2e, 0xe3, 0x00, 0x40}, true},
	};
	for (const Case& c : cases) {
		CAPTURE(c.name);
		CHECK(parse(c.e).size() == (c.pass ? 1u : 0u));
	}
	const auto pc = parse({0x0c, 0xc0, 0x05, 0xff});
	REQUIRE(pc.size() == 1);
	CHECK(same(pc[0], 1, 0xc0, 0x05, 0x00));
	const auto pb = parse({0x2e, 0xe3, 0x00, 0x40});
	REQUIRE(pb.size() == 1);
	CHECK(same(pb[0], 3, 0xe3, 0x00, 0x40));
}

TEST_CASE("midi in fuzz: every event passed is a valid one, every valid one is passed") {
	std::mt19937 rng(4242);
	for (int round = 0; round < 20000; round++) {
		std::vector<uint8_t> t(rng() % 65);
		for (uint8_t& x : t) x = static_cast<uint8_t>(rng() % 3 == 0 ? rng() & 0x7f : rng());
		size_t valid = 0;
		for (size_t i = 0; i + 4 <= t.size(); i += 4) {
			// the rules, stated per CIN (USB MIDI 1.0 table 4-1)
			const int cin = t[i] & 0x0F, st = t[i + 1];
			int n = 0;
			bool ok = (st & 0x80) != 0;
			if (cin >= 8 && cin <= 0xE) {
				ok = ok && (st >> 4) == cin;
				n = cin == 0xC || cin == 0xD ? 1 : 2;
			} else if (cin == 2 || cin == 3) {
				n = cin - 1; // 2- and 3-byte system common
			} else if (cin == 0xF) {
				ok = ok && st >= 0xF8; // real-time only
			} else {
				ok = false; // 0, 1 reserved, 4-7 SysEx
			}
			for (int k = 0; k < n; k++) ok = ok && (t[i + 2 + k] & 0x80) == 0;
			valid += ok ? 1 : 0;
		}
		const auto ev = parse(t);
		REQUIRE(ev.size() == valid);
		for (const auto& e : ev) {
			CHECK((e.status & 0x80) != 0);
			CHECK((e.data1 & 0x80) == 0);
			CHECK((e.data2 & 0x80) == 0);
			CHECK(e.port >= 1);
			CHECK(e.port <= 16);
		}
	}
}
