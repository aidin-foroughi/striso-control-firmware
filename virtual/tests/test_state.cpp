// What the board tells the PC about itself: the settings-key layout (StcX), the state dump (StcY)
// and its change counter (Stcy).
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "board.h"
#include "doctest/doctest.h"

using vt::Board;
using vt::lines;

namespace {

struct SettingsKey {
	int id = -1;
	std::string kind, group, engine, led, flags, label, title, knob;
};

std::vector<std::string> split(const std::string& s, char sep) {
	std::vector<std::string> out;
	size_t p = 0;
	for (size_t e; (e = s.find(sep, p)) != std::string::npos; p = e + 1) out.push_back(s.substr(p, e - p));
	out.push_back(s.substr(p));
	return out;
}

// StcX, checked line by line: the header's count, ascending ids, every field's form
std::vector<SettingsKey> layout(Board& bd) {
	const std::vector<std::string> l = lines(bd.cmd("X", 40));
	REQUIRE(l.size() >= 2);
	REQUIRE(l[0].rfind("X keys ", 0) == 0);
	const int n = std::atoi(l[0].c_str() + 7);
	REQUIRE(static_cast<int>(l.size()) == n + 2);
	CHECK(l.back() == "X end");
	static const std::set<std::string> kinds = {"switch", "dial", "action"};
	static const std::set<std::string> groups = {"midi", "layout", "sens", "voice", "tuning", "preset", "midi2", "system"};
	std::vector<SettingsKey> keys;
	for (int i = 1; i <= n; i++) {
		CAPTURE(l[i]);
		const std::vector<std::string> bar = split(l[i], '|');
		REQUIRE(bar.size() == 3);
		std::istringstream is(bar[0]);
		std::string x;
		SettingsKey k;
		is >> x >> k.id >> k.kind >> k.group >> k.engine >> k.led >> k.flags >> k.label;
		std::string rest;
		CHECK(!(is >> rest)); // the label is the last word before the first '|'
		k.title = bar[1];
		k.knob = bar[2];
		CHECK(x == "X");
		CHECK(k.id >= 0);
		CHECK(k.id < 68);
		if (!keys.empty()) CHECK(k.id > keys.back().id);
		CHECK(kinds.count(k.kind) == 1);
		CHECK(groups.count(k.group) == 1);
		CHECK(!k.label.empty());
		CHECK(k.label.size() <= 6);
		CHECK(!k.title.empty());
		CHECK(k.flags.find_first_not_of(k.flags == "-" ? "-" : "kd") == std::string::npos);
		if (k.group == "voice") {
			CHECK(k.engine != "-");
			CHECK(k.led.size() == 6);
			CHECK(k.led.find_first_not_of("0123456789ABCDEFabcdef") == std::string::npos);
		} else {
			CHECK(k.engine == "-");
			CHECK(k.led == "-");
		}
		keys.push_back(k);
	}
	return keys;
}

bool has_flag(const SettingsKey& k, char f) { return k.flags != "-" && k.flags.find(f) != std::string::npos; }

std::string k_cmd(int id) { return std::string("K") + static_cast<char>(id); }

} // namespace

TEST_CASE("state: StcX, the settings-key layout: its k flags are exactly what StcK accepts") {
	Board bd;
	const std::vector<SettingsKey> keys = layout(bd);
	std::map<int, SettingsKey> by_id;
	for (const SettingsKey& k : keys) by_id[k.id] = k;
	for (int id = 0; id < 68; id++) {
		CAPTURE(id);
		const std::string r = bd.cmd(k_cmd(id), 20);
		const bool ok = r.find("K ok " + std::to_string(id)) != std::string::npos;
		CHECK((ok || r.find("K err " + std::to_string(id)) != std::string::npos));
		CHECK(ok == (by_id.count(id) && has_flag(by_id[id], 'k')));
	}
	// the dangerous key: only system reset, and the console can't press it
	for (const SettingsKey& k : keys) CHECK(has_flag(k, 'd') == (k.id == 67));
	CHECK(by_id.count(67) == 1);
}

TEST_CASE("state: StcX, every voice key's engine is in the voice list (Stcv), every engine has its key") {
	Board bd;
	std::set<std::string> engines;
	for (const std::string& l : lines(bd.cmd("v", 40))) {
		if (l.size() < 4) continue;
		const std::string name = l.substr(l.find(' ', 2) + 1);
		engines.insert(name.substr(0, name.find(':')));
	}
	std::set<std::string> keyed;
	for (const SettingsKey& k : layout(bd)) {
		if (k.group != "voice") continue;
		CAPTURE(k.id);
		CHECK(engines.count(k.engine) == 1);
		CHECK(k.kind == "switch");
		CHECK(has_flag(k, 'k'));
		keyed.insert(k.engine);
	}
	CHECK(keyed == engines);
	// the colour config_voice shows: bowed red
	for (const SettingsKey& k : layout(bd))
		if (k.engine == "bowed") CHECK(k.led == "400000");
}

TEST_CASE("state: StcX, a renamed tuning and preset in the config show their config names") {
	// a config with tuning 1 changed (a 17-TET fifth, its own name) and preset 2 named
	std::string data;
	auto field = [&](std::string s) {
		s.resize(8, ' ');
		data += s;
	};
	field("MConfig ");
	field("v1.0");
	for (const auto& [k, v] : std::vector<std::pair<std::string, std::string>>{
	         {"sT1name ", "17 tet"}, {"fT1fifth", "705.8824"}, {"sP2name ", "stage"}}) {
		field(k);
		field(v);
	}
	field("MCfgEnd ");
	field("");
	sv_options o{};
	o.size = sizeof o;
	o.flash = reinterpret_cast<const uint8_t*>(data.data());
	o.flash_len = static_cast<uint32_t>(data.size());
	Board bd(&o);
	for (const SettingsKey& k : layout(bd)) {
		if (k.id == 36) {
			CHECK(k.label == "17tet");
			CHECK(k.title == "Tuning 1: 17 tet (from the config)");
		}
		if (k.id == 38) CHECK(k.title == "Tuning 2: 19-TET");
		if (k.id == 37) CHECK(k.title == "Load settings preset 2: stage");
		if (k.id == 39) CHECK(k.title == "Load settings preset 3");
	}
}

// --- StcY and Stcy ------------------------------------------------------------------------------------

namespace {

constexpr int IDC_OCT_UP = 1, IDC_ALT = 3; // striso.h

// StcY: "Y <counter> key=value ..."
struct Z {
	uint32_t counter = 0;
	std::map<std::string, std::string> f;
	std::string line;
	std::string operator[](const std::string& k) const {
		const auto it = f.find(k);
		return it == f.end() ? "<none>" : it->second;
	}
	// the fields without the counter
	bool same_state(const Z& o) const { return f == o.f; }
};

Z state(Board& bd) {
	const std::string r = bd.cmd("Y", 20);
	REQUIRE(r.size() > 2);
	CHECK(r.find("\r\n") == r.size() - 2); // exactly one line
	const std::vector<std::string> l = lines(r);
	Z z;
	z.line = l[0];
	std::istringstream is(l[0]);
	std::string w;
	is >> w;
	REQUIRE(w == "Y");
	is >> z.counter;
	while (is >> w) {
		const size_t e = w.find('=');
		REQUIRE(e != std::string::npos);
		z.f[w.substr(0, e)] = w.substr(e + 1);
	}
	return z;
}

uint32_t counter(Board& bd) {
	const std::string r = bd.cmd("y", 20);
	REQUIRE(r.size() > 2);
	CHECK(r.find("\r\n") == r.size() - 2); // exactly one line
	const std::vector<std::string> l = lines(r);
	REQUIRE(l[0].rfind("y ", 0) == 0);
	return static_cast<uint32_t>(std::strtoul(l[0].c_str() + 2, nullptr, 10));
}

std::string engine_of(Board& bd, int voice) {
	for (const std::string& l : lines(bd.cmd("v", 40))) {
		if (l.size() < 4 || std::atoi(l.c_str() + 2) != voice) continue;
		const std::string name = l.substr(l.find(' ', 2) + 1);
		return name.substr(0, name.find(':'));
	}
	return "";
}

std::string q(int target, int index, float value) {
	uint32_t u;
	std::memcpy(&u, &value, 4);
	std::string s = "Q";
	s += static_cast<char>(target);
	s += static_cast<char>(index);
	for (int k = 0; k < 4; k++) s += static_cast<char>((u >> (8 * k)) & 0xff);
	return s;
}

} // namespace

TEST_CASE("state: StcY has every field, its counter is Stcy's; idle reads don't count") {
	Board bd;
	const Z z = state(bd);
	MESSAGE(z.line);
	for (const char* k : {"voice", "midi", "ch", "voices", "bend", "tuning", "fine", "transpose", "pres", "velo", "tilt",
	         "bendsens", "thres", "vol", "sustain", "rate", "alt", "porta", "flip", "led", "updown"})
		CHECK_MESSAGE(z.f.count(k) == 1, "no " << k);
	CHECK(z.f.size() == 21);
	CHECK(z["voice"] == "1");
	CHECK(z["midi"] == "mpe");
	CHECK(z["ch"] == "2");
	CHECK(z["voices"] == "10");
	CHECK(z["bend"] == "48");
	CHECK(z["tuning"] == "0");
	CHECK(z["fine"] == "0.00");
	CHECK(z["transpose"] == "0.000");
	CHECK(z["pres"] == "1.000");
	CHECK(z["bendsens"] == "0.500");
	CHECK(z["vol"] == "90");
	CHECK(z["alt"] == "0");
	CHECK(z["led"].size() == 6);
	CHECK(z["updown"] == "0000");
	CHECK(counter(bd) == z.counter);
	bd.run_ms(300);
	const uint32_t c = counter(bd);
	bd.run_ms(300);
	CHECK(counter(bd) == c);
	CHECK(state(bd).counter == c);
}

TEST_CASE("state: Stcy goes up after a tuning key, a voice select, a Q write and Settings + octave up") {
	Board bd;
	uint32_t c = counter(bd);
	auto went_up = [&] {
		const uint32_t n = counter(bd);
		const bool up = n > c;
		c = n;
		return up;
	};

	bd.cmd(k_cmd(36)); // tuning 1
	CHECK(went_up());
	CHECK(state(bd)["tuning"] == "1");
	c = counter(bd);

	bd.send(std::string("Stc") + static_cast<char>(0x80 + 24)); // a voice
	CHECK(went_up());
	CHECK(state(bd)["voice"] == "24");
	bd.run_ms(20); // the engine starts at the next block: its parameters change then (counted once more)
	c = counter(bd);
	bd.run_ms(50);
	CHECK(counter(bd) == c);

	bd.cmd(q(0, 0, 0.77f)); // the voice's first parameter
	CHECK(went_up());
	bd.cmd(q(2, 0, 2.0f)); // touch: pressure
	CHECK(went_up());
	CHECK(state(bd)["pres"] == "2.000");
	c = counter(bd);

	sv_inject_control(bd.b, IDC_ALT, 1); // hold Settings
	bd.run_ms(10);
	CHECK(went_up());
	CHECK(state(bd)["alt"] == "1");
	c = counter(bd);
	sv_inject_control(bd.b, IDC_OCT_UP, 1); // with Settings: a semitone up
	bd.run_ms(10);
	sv_inject_control(bd.b, IDC_OCT_UP, 0);
	bd.run_ms(10);
	CHECK(went_up());
	const Z z = state(bd);
	CHECK(z["alt"] == "1");
	// a semitone in tuning 1 (31-TET): 3 octaves less 5 fifths
	CHECK(z["transpose"] == "1.161");
	sv_inject_control(bd.b, IDC_ALT, 0);
	bd.run_ms(10);
	CHECK(went_up());
	CHECK(state(bd)["alt"] == "0");
	sv_inject_control(bd.b, IDC_OCT_UP, 1); // an octave up
	bd.run_ms(10);
	sv_inject_control(bd.b, IDC_OCT_UP, 0);
	bd.run_ms(10);
	CHECK(went_up());
	const Z z2 = state(bd);
	CHECK(z2["transpose"] == "13.161");
	CHECK(z2["updown"] != "0000"); // the octave leds
}

TEST_CASE("state: every switch StcK may press changes what StcY shows (or is panic)") {
	// the settings presets of the factory config are mostly empty: give each its own volume
	std::string data;
	auto field = [&](std::string s) {
		s.resize(8, ' ');
		data += s;
	};
	field("MConfig ");
	field("v1.0");
	for (int n = 1; n <= 8; n++) {
		field("fP" + std::to_string(n) + "volum");
		field(std::to_string(10 + 10 * n) + ".0");
	}
	field("MCfgEnd ");
	field("");
	sv_options o{};
	o.size = sizeof o;
	o.flash = reinterpret_cast<const uint8_t*>(data.data());
	o.flash_len = static_cast<uint32_t>(data.size());
	Board bd(&o);
	const std::vector<SettingsKey> keys = layout(bd);
	int pressed = 0;
	for (const SettingsKey& k : keys) {
		if (!has_flag(k, 'k')) continue;
		CAPTURE(k.id);
		CAPTURE(k.title);
		if (k.label == "Panic") {
			CHECK(bd.cmd(k_cmd(k.id)) == "K ok 63\r\n");
			continue;
		}
		// another key of the group first, so this one has something to change
		const SettingsKey* other = nullptr;
		for (const SettingsKey& x : keys)
			if (x.group == k.group && x.id != k.id && has_flag(x, 'k') && (k.group != "voice" || x.engine != k.engine)) {
				other = &x;
				break;
			}
		REQUIRE(other != nullptr);
		bd.cmd(k_cmd(other->id), 30);
		const Z before = state(bd);
		REQUIRE(bd.cmd(k_cmd(k.id), 30) == "K ok " + std::to_string(k.id) + "\r\n");
		const Z after = state(bd);
		CHECK(!after.same_state(before));
		CHECK(after.counter > before.counter);
		if (k.group == "tuning") CHECK("Tuning " + after["tuning"] + ":" == k.title.substr(0, k.title.find(':') + 1));
		if (k.group == "voice") CHECK(engine_of(bd, std::atoi(after["voice"].c_str())) == k.engine);
		if (k.group == "preset") CHECK(after["vol"] == std::to_string(10 + 10 * std::atoi(k.label.c_str() + 1)));
		if (k.id == 0) CHECK(after["midi"] == "mpe");
		if (k.id == 2) CHECK(after["midi"] == "poly");
		if (k.id == 4) CHECK(after["midi"] == "mono");
		pressed++;
	}
	CHECK(pressed == 3 + 8 + 9 + 8); // MIDI modes, voices, tunings, presets
}
