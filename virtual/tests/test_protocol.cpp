// The console protocol, as a host sees it on the bulk interface, against transcripts.
//
// virtual/tests/transcripts/<session>.txt: the virtual board's console sessions, sent and
// received bytes, as the board's code answers them (the lists, Q writes and their errors, voice
// selection, StcN in several tunings, StcK's whitelist and refusals, the settings presets, the
// binary stream, the settings-key layout StcX, the hero parameters StcH). The board is deterministic, so a transcript is
// exact; only the version string (it follows git) and the thread list's addresses are masked.
// With SV_UPDATE_TRANSCRIPTS=1 the test writes them instead (then read the diff before committing).
//
// virtual/tests/transcripts/board/<cmd>.bin: the real board's replies to the read-only commands
// (V L p f g k N U I), recorded with tools/striso.py from a Striso on USB. The virtual board's
// replies must match them field for field, except what is the hardware's own: the UID, serial and
// calibration date, CPU and USB audio numbers, thread addresses and stack use, and the live values
// (touch, key factors, parameters) that were changed or saved on that board.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "board.h"
#include "doctest/doctest.h"

namespace {

const std::string dir = STRISO_TRANSCRIPTS;

std::string show(const std::string& bytes, bool reply) {
	std::string out;
	for (size_t i = 0; i < bytes.size(); i++) {
		const unsigned char c = static_cast<unsigned char>(bytes[i]);
		if (reply && c == '\r' && i + 1 < bytes.size() && bytes[i + 1] == '\n') {
			out += "\n";
			i++;
		} else if (c >= 32 && c < 127 && c != '\\') {
			out += static_cast<char>(c);
		} else {
			char h[8];
			std::snprintf(h, sizeof h, "\\x%02X", c);
			out += h;
		}
	}
	return out;
}

std::string masked(std::string s) {
	const std::string v = sv_firmware_version();
	for (size_t p; (p = s.find(v)) != std::string::npos;) s.replace(p, v.size(), "<FWVERSION>");
	// cmd_threads: stack limit, stack pointer, thread address (host addresses)
	static const std::regex threads(R"(^[0-9A-F]{8} [0-9A-F]{8} [0-9A-F]{8} )");
	std::string out;
	size_t p = 0;
	while (p < s.size()) {
		size_t e = s.find('\n', p);
		e = e == std::string::npos ? s.size() : e + 1;
		out += std::regex_replace(s.substr(p, e - p), threads, "XXXXXXXX XXXXXXXX XXXXXXXX ");
		p = e;
	}
	return out;
}

struct Session {
	vt::Board bd;
	std::string text;
	void send(const std::string& bytes, int ms = 30) {
		const std::string r = bd.send(bytes, ms);
		text += "> " + show(bytes, false) + "\n" + masked(show(r, true));
		if (!r.empty() && text.back() != '\n') text += "\n";
	}
	void cmd(const std::string& c, int ms = 30) { send("Stc" + c, ms); }
	void key(const std::string& note, int id, int pres, int velo, int x, int y, int ms = 30) {
		sv_inject_key(bd.b, 0, id, pres, velo, x, y);
		sv_advance_ms(bd.b, ms);
		const std::string r = bd.read();
		text += "~ key " + note + "\n" + masked(show(r, true));
		if (!r.empty() && text.back() != '\n') text += "\n";
	}
};

std::string q(int target, int index, float value) {
	uint32_t u;
	std::memcpy(&u, &value, 4);
	std::string s = "StcQ";
	s += static_cast<char>(target);
	s += static_cast<char>(index);
	for (int k = 0; k < 4; k++) s += static_cast<char>((u >> (8 * k)) & 0xff);
	return s;
}

std::string k(int id) { return std::string("StcK") + static_cast<char>(id); }
std::string voice(int n) { return std::string("Stc") + static_cast<char>(0x80 + n); }

void check_transcript(const std::string& name, const std::string& text) {
	const std::string path = dir + "/" + name + ".txt";
	const char* upd = std::getenv("SV_UPDATE_TRANSCRIPTS");
	if (upd && *upd == '1') {
		std::ofstream(path, std::ios::binary) << text;
		MESSAGE("wrote " << path);
		return;
	}
	std::ifstream f(path, std::ios::binary);
	REQUIRE_MESSAGE(f.good(), "no transcript " << path << " (SV_UPDATE_TRANSCRIPTS=1 writes it)");
	std::stringstream ss;
	ss << f.rdbuf();
	std::string want = ss.str();
	want.erase(std::remove(want.begin(), want.end(), '\r'), want.end());
	const std::vector<std::string> a = vt::lines(want), b = vt::lines(text);
	size_t i = 0;
	while (i < a.size() && i < b.size() && a[i] == b[i]) i++;
	if (i < a.size() || i < b.size()) {
		CAPTURE(name);
		CAPTURE(i + 1);
		CHECK_MESSAGE(false, "transcript line " << (i + 1) << ": expected [" << (i < a.size() ? a[i] : "<end>") << "], got ["
		                                        << (i < b.size() ? b[i] : "<end>") << "]");
	}
}

} // namespace

TEST_CASE("protocol: version, load, the voice list and every parameter list") {
	Session s;
	for (const char* c : {"V", "L", "v", "p", "f", "g", "k", "N", "U", "I"}) s.cmd(c, 40);
	check_transcript("lists", s.text);
}

TEST_CASE("protocol: Q writes, their errors and the lists after them") {
	Session s;
	s.send(q(0, 0, 0.5f));          // the voice's first parameter
	s.send(q(0, 200, 1.0f));        // no such parameter: Q err
	s.send(q(1, 2, 0.25f));         // an effects parameter
	s.send(q(1, 99, 0.0f));         // Q err
	s.send(q(2, 0, 2.0f));          // touch: pressure
	s.send(q(2, 1, 100.0f));        // touch: velocity, clamped to 4
	s.send(q(2, 5, 1.0f));          // Q err: 5 touch parameters
	s.send(q(3, 30, 1.5f));         // key factor of D_3
	s.send(q(3, 31, 0.05f));        // Q err: below 0.1
	s.send(q(3, 61, 1.0f));         // Q err: 61 keys
	s.send(q(4, 0, 1.0f));          // Q err: no target 4
	s.send(q(0, 1, std::nanf(""))); // NaN: queued, ignored by the synth
	s.cmd("p");
	s.cmd("f");
	s.cmd("g");
	s.cmd("k", 40);
	check_transcript("qwrites", s.text);
}

TEST_CASE("protocol: voice selection, its errors, the list and parameters of the selected voice") {
	Session s;
	for (int n : {0, 15, 47, 55, 56, 127}) s.send(voice(n));
	s.cmd("v", 40);
	s.cmd("p");
	check_transcript("voices", s.text);
}

TEST_CASE("protocol: StcH, the hero parameters of the selected voice (its preset's or its engine's) and the effects'") {
	Session s;
	// strisy (none), va:classic and va:pad (its own), bowed:violin and bowed:section (its own),
	// modal, wind:flute, ep:wurli, brass:harmon, trumpet:bb jazz (voices.txt has the list)
	for (int n : {0, 1, 2, 15, 19, 9, 32, 42, 53, 57}) {
		s.send(voice(n));
		s.cmd("H");
	}
	check_transcript("heroes", s.text);
}

TEST_CASE("protocol: StcN in the tunings of the settings card (StcK)") {
	Session s;
	s.cmd("N", 40);
	for (int id : {36, 38, 23, 25, 27, 29, 31, 33, 34}) {
		s.send(k(id));
		s.cmd("N", 40);
	}
	check_transcript("tunings", s.text);
}

TEST_CASE("protocol: StcK's whitelist: every settings key, ok or refused") {
	Session s;
	for (int id = 0; id < 70; id++) s.send(k(id), 20);
	s.cmd("v", 40);
	s.cmd("N", 40);
	check_transcript("settings_keys", s.text);
}

TEST_CASE("protocol: StcX, the settings-key layout") {
	Session s;
	s.cmd("X", 40);
	check_transcript("settings_layout", s.text);
}

TEST_CASE("protocol: the settings presets (StcK 35 37 39 41 43 45 30 32): touch settings after each") {
	Session s;
	for (int id : {35, 37, 39, 41, 43, 45, 30, 32}) {
		s.send(k(id));
		s.cmd("g");
	}
	check_transcript("presets", s.text);
}

TEST_CASE("protocol: the binary stream (S) with keys and motion, then s") {
	Session s;
	s.cmd("S", 20);
	s.key("D_3 press", 30, 2000, 3000, 100, -200, 5);
	s.key("D_3 harder", 30, 6000, 1000, 0, 0, 5);
	s.key("D_3 release", 30, 0, -500, 0, 0, 5);
	s.cmd("s", 20);
	s.key("after s: nothing", 30, 3000, 0, 0, 0, 5);
	check_transcript("stream", s.text);
}

// --- the real board -------------------------------------------------------------------------------

namespace {

std::string board_reply(const std::string& c) {
	std::ifstream f(dir + "/board/" + c + ".bin", std::ios::binary);
	std::stringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

struct Fields {
	std::vector<std::string> t;
	explicit Fields(const std::string& line) {
		std::istringstream is(line);
		for (std::string w; is >> w;) t.push_back(w);
	}
};

// parameter lists: header and every line field for field; values only counted
void compare_params(const std::string& name, const std::string& board, const std::string& virt, int* value_diffs) {
	const auto a = vt::lines(board), b = vt::lines(virt);
	CAPTURE(name);
	REQUIRE(a.size() == b.size());
	CHECK(a[0] == b[0]);
	for (size_t i = 1; i < a.size(); i++) {
		if (a[i].empty() && b[i].empty()) continue;
		const Fields fa(a[i]), fb(b[i]);
		CAPTURE(a[i]);
		CAPTURE(b[i]);
		REQUIRE(fa.t.size() == 7);
		REQUIRE(fb.t.size() == 7);
		for (int k : {0, 1, 2, 3, 4, 6}) CHECK(fa.t[k] == fb.t[k]); // index name group min max kind
		if (fa.t[5] != fb.t[5]) {
			++*value_diffs;
			MESSAGE(name << " value differs: board [" << a[i] << "] virtual [" << b[i] << "]");
		}
	}
}

std::string digits_masked(const std::string& s) { return std::regex_replace(s, std::regex("[0-9]+"), "#"); }

} // namespace

TEST_CASE("protocol: the read-only replies are the real board's, field for field") {
	const std::string board_v = board_reply("V");
	REQUIRE_MESSAGE(!board_v.empty(), "no recorded board replies in " << dir << "/board");
	vt::Board bd;
	int value_diffs = 0;

	SUBCASE("V: version, UID, device id and model; calibration date") {
		const auto a = vt::lines(board_v), b = vt::lines(bd.cmd("V"));
		// the version: <date>_<git describe>_all (in Docker a worktree has no git: unknown_all)
		const std::regex v1(R"(^\S+_all [0-9A-F]{24} .*Striso board$)"), v2(R"(^Calibration date: \S+$)");
		REQUIRE(a.size() >= 2);
		REQUIRE(b.size() >= 2);
		CHECK(std::regex_match(a[0], v1));
		CHECK(std::regex_match(b[0], v1));
		CHECK(std::regex_match(a[1], v2));
		CHECK(std::regex_match(b[1], v2));
		MESSAGE("board: " << a[0] << " / virtual: " << b[0]);
	}
	SUBCASE("L: the synth load line, the same budget") {
		const std::string a = board_reply("L"), b = bd.cmd("L");
		CHECK(digits_masked(a) == digits_masked(b));
		CHECK(a.find("budget 266666 cycles") != std::string::npos);
		CHECK(b.find("budget 266666 cycles") != std::string::npos);
	}
	SUBCASE("p: the board's voice's parameters (the virtual board selects the same voice first)") {
		const std::string a = board_reply("p");
		const std::string head = vt::lines(a)[0]; // "params 0 <engine:preset> <count>"
		const std::string want = Fields(head).t[2];
		int entry = -1;
		for (const std::string& l : vt::lines(bd.cmd("v", 40)))
			if (l.size() > 4 && l.substr(l.find(' ', 2) + 1) == want) entry = std::atoi(l.c_str() + 2);
		REQUIRE_MESSAGE(entry >= 0, "the virtual board has no voice " << want);
		bd.send(voice(entry));
		compare_params("p", a, bd.cmd("p", 40), &value_diffs);
	}
	SUBCASE("f g k: effects, touch and key lists") {
		compare_params("f", board_reply("f"), bd.cmd("f", 40), &value_diffs);
		compare_params("g", board_reply("g"), bd.cmd("g", 40), &value_diffs);
		compare_params("k", board_reply("k"), bd.cmd("k", 40), &value_diffs);
	}
	SUBCASE("N: the tuning and every key's pitch") {
		CHECK(board_reply("N") == bd.cmd("N", 40));
	}
	SUBCASE("U: the USB audio lines (the virtual board has no stream: its counters are 0)") {
		const auto a = vt::lines(board_reply("U")), b = vt::lines(bd.cmd("U", 40));
		std::vector<std::string> la, lb; // the lines' labels, numbers masked, the rate line only streaming
		// streaming, the board adds the rate line and the ISR's share of the time
		std::vector<std::string> a2;
		for (const auto& l : a)
			if (l.rfind("rate:", 0) != 0) a2.push_back(std::regex_replace(l, std::regex(R"( = [0-9.]+% of [0-9]+ ms)"), ""));
		for (const auto& l : a2) la.push_back(digits_masked(l.substr(0, l.find(':'))));
		for (const auto& l : b) lb.push_back(digits_masked(l.substr(0, l.find(':'))));
		CHECK(la == lb);
		for (size_t i = 1; i < b.size(); i++) {
			const std::string l = b[i];
			bool found = false;
			for (const auto& x : a2) found = found || digits_masked(x) == digits_masked(l);
			CAPTURE(l);
			CHECK(found);
		}
	}
	SUBCASE("I: the same threads, priorities and order") {
		const auto a = vt::lines(board_reply("I")), b = vt::lines(bd.cmd("I", 40));
		REQUIRE(a.size() == b.size());
		CHECK(a[0] == b[0]);
		for (size_t i = 1; i < a.size(); i++) {
			if (a[i].empty()) continue;
			const Fields fa(a[i]), fb(b[i]);
			CAPTURE(a[i]);
			CAPTURE(b[i]);
			CHECK(fa.t[4] == fb.t[4]);         // refs
			CHECK(fa.t[5] == fb.t[5]);         // prio
			CHECK(fa.t.back() == fb.t.back()); // name ("send messages" ends in "messages")
		}
	}
	MESSAGE("live values that differ from the virtual board's defaults: " << value_diffs);
}
