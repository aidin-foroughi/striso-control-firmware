// What the board tells the PC about itself: the settings-key layout (StcX), the state dump (StcZ)
// and its change counter (Stcz).
#include <algorithm>
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
