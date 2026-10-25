// The configuration (src/app/config_table.hpp, config_store.cpp, config_schema.hpp): 16-byte
// records, an 8-char key and an 8-char value, space padded, from "MConfig " to "MCfgEnd ". The
// user's settings page (in flash at 0x08020000 on the board, the config editor writes it as
// CONFIG.UF2) overrides the firmware's defaults; a key neither has is nullopt. The default records
// are built from the schema and must stay byte for byte what tests/golden/firmware_bytes.txt has.
#include <array>
#include <climits>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "config_page.hpp"
#include "config_table.hpp"
#include "doctest/doctest.h"

using settings::ConfigTable;
using settings::KeyName;
using settings::parse_float;
using settings::parse_hex;
using settings::parse_int;

namespace {

std::string field(const char* s) { return std::string(s, 8); }
std::string str(KeyName k) {
	const std::array<char, 8> c = k.chars();
	return std::string(c.data(), c.size());
}

const std::span<const ConfigParam> kDefaults = settings::defaults();

// a page of the user's records (config_page) over the defaults
struct Page {
	std::vector<ConfigParam> recs;
	explicit Page(std::vector<ConfigParam> r) :
	    recs(std::move(r)) {}
	ConfigTable table() const { return {recs, kDefaults}; }
};

// a whole sector, erased
std::vector<ConfigParam> erased_sector() {
	std::vector<ConfigParam> s(config_schema::kFlashSize / sizeof(ConfigParam));
	std::memset(s.data(), 0xff, s.size() * sizeof(ConfigParam));
	return s;
}

void put(ConfigParam& r, const char* key, const char* value) {
	std::memcpy(r.key, key, 8);
	std::memcpy(r.value, value, 8);
}

// tests/golden/firmware_bytes.txt: the bytes of an entry
std::vector<uint8_t> golden(const std::string& name) {
	std::ifstream f(STRISO_GOLDEN_DIR "/firmware_bytes.txt");
	std::string line;
	std::vector<uint8_t> out;
	bool in = false;
	while (std::getline(f, line)) {
		if (line.rfind(name + " ", 0) == 0) {
			in = true;
			continue;
		}
		if (!in) continue;
		if (line.empty()) break;
		for (size_t i = 0; i + 1 < line.size(); i += 2) out.push_back(static_cast<uint8_t>(std::stoi(line.substr(i, 2), nullptr, 16)));
	}
	return out;
}

} // namespace

TEST_CASE("config records are 16 bytes: an 8-char key and an 8-char value") {
	CHECK(sizeof(ConfigParam) == 16);
	CHECK(offsetof(ConfigParam, value) == 8);
	CHECK(sizeof(default_config) == CONFIG_DEFAULT_RECORDS * 16);
}

TEST_CASE("default_config from the schema: byte for byte the golden (the CFG gate on the host)") {
	const std::vector<uint8_t> g = golden("config.default_config");
	REQUIRE(g.size() == sizeof(default_config));
	const auto* d = reinterpret_cast<const uint8_t*>(&default_config);
	size_t first = 0;
	while (first < g.size() && g[first] == d[first]) first++;
	CHECK_MESSAGE(first == g.size(), "first difference at byte " << first << " (record " << first / 16 << ")");
}

TEST_CASE("default config: MConfig v1.0 ... MCfgEnd, printable unique keys, every key found") {
	CHECK(field(kDefaults[0].key) == "MConfig ");
	CHECK(field(kDefaults[0].value) == "v1.0    ");
	CHECK(field(kDefaults.back().key) == "MCfgEnd ");
	const ConfigTable t({}, kDefaults);
	std::set<std::string> keys;
	for (size_t n = 1; n + 1 < kDefaults.size(); n++) {
		const ConfigParam& c = kDefaults[n];
		CAPTURE(field(c.key));
		for (char ch : field(c.key)) CHECK((ch >= 32 && ch < 127));
		for (char ch : field(c.value)) CHECK((ch >= 32 && ch < 127));
		CHECK(keys.insert(field(c.key)).second);
		// the lookup of every key returns its value (no other record shadows it)
		CHECK(t.raw(KeyName::from(c.key))->data() == c.value);
		CHECK(t.is_default(KeyName::from(c.key)));
	}
	const std::string msg = std::to_string(kDefaults.size() - 2) + " default records";
	MESSAGE(msg);
}

TEST_CASE("the schema's keys: KeyName builds what the defaults have") {
	const ConfigTable t({}, kDefaults);
	CHECK(str(KeyName::general("preset")) == "iGpreset");
	CHECK(str(KeyName::general("oct")) == "iGoct   ");
	CHECK(str(KeyName::preset(3, "Mint ")) == "iP3Mint ");
	CHECK(str(KeyName::preset(3, "Mint")) == "iP3Mint ");
	CHECK(str(KeyName::preset(8, "color")) == "hP8color");
	CHECK(str(KeyName::tuning(2, "fifth")) == "fT2fifth");
	CHECK(str(KeyName::tuning(2, "name")) == "sT2name ");
	CHECK(str(KeyName::tuning_key(5, 0)) == "fT5F_1  ");
	CHECK(str(KeyName::calibration("fac", 30)).substr(0, 5) == "fCfac");
	// every preset field of every preset, every tuning field and key, every key's calibration
	for (int p = 1; p <= config_schema::kPresets; p++) {
		CHECK(t.text(KeyName::preset(p, "name")) == "preset" + std::to_string(p));
		CHECK(t.raw(KeyName::preset(p, "decaP")).has_value());
	}
	for (int n = 1; n <= config_schema::kTunings; n++) {
		for (int k = 0; k < config_schema::kKeys; k++) CHECK(t.get_float(KeyName::tuning_key(n, k)).has_value());
		CHECK(t.get_float(KeyName::tuning(n, "oct")).has_value());
	}
	for (int k = 0; k < config_schema::kKeys; k++) {
		CHECK(t.get_float(KeyName::calibration("fac", k)) == 1.0f);
		CHECK(t.get_float(KeyName::calibration("off", k)) == 0.0f);
	}
	// the deprecated settings: read if the user's page has them, no default
	CHECK_FALSE(t.raw(KeyName::general("jack2")).has_value());
	CHECK_FALSE(t.raw(KeyName::general("motion")).has_value());
}

TEST_CASE("key names: put_button_name, the 61 keys F_1 .. B_4, each once") {
	std::set<std::string> names;
	for (int n = 0; n < 61; n++) {
		char k[3];
		put_button_name(n, k);
		names.insert(std::string(k, 3));
	}
	CHECK(names.size() == 61);
	char k[3];
	put_button_name(0, k);
	CHECK(std::string(k, 3) == "F_1");
	put_button_name(60, k);
	CHECK(std::string(k, 3) == "B_4");
}

TEST_CASE("key names: a number that is no key gives \"---\", never a read outside the names (R15)") {
	for (int n : {-1, -7, -8, -100, INT_MIN, 61, 62, 1000, INT_MAX}) {
		CAPTURE(n);
		char k[3] = {'?', '?', '?'};
		put_button_name(n, k);
		CHECK(std::string(k, 3) == "---");
	}
}

TEST_CASE("value parsing: integers, decimals, hexadecimal") {
	CHECK(parse_int("42      ") == 42);
	CHECK(parse_int("-17     ") == -17);
	CHECK(parse_int("+3      ") == 3);
	CHECK(parse_int("12345678") == 12345678);
	CHECK_FALSE(parse_int("        "));
	CHECK(*parse_float("0.5     ") == doctest::Approx(0.5f));
	CHECK(*parse_float("-1.25   ") == doctest::Approx(-1.25f));
	CHECK(*parse_float("3       ") == doctest::Approx(3.0f));
	CHECK_FALSE(parse_float("abc     "));
	CHECK(parse_hex("#ff00aa ") == 0xff00aau);
	CHECK(parse_hex("0x1F    ") == 0x1fu);
	CHECK(parse_hex("C0FFEE  ") == 0xc0ffeeu);
}

TEST_CASE("value parsing is strict: a number, then only spaces (R12)") {
	// no digits
	CHECK_FALSE(parse_int("-       "));
	CHECK_FALSE(parse_int("+       "));
	CHECK_FALSE(parse_float("-       "));
	CHECK_FALSE(parse_float("+       "));
	CHECK_FALSE(parse_float(".       "));
	CHECK_FALSE(parse_float("-.      "));
	CHECK_FALSE(parse_hex("#       "));
	CHECK_FALSE(parse_hex("        "));
	// trailing garbage
	CHECK_FALSE(parse_int("1-2     "));
	CHECK_FALSE(parse_int("1.2.3   "));
	CHECK_FALSE(parse_int("-.5     "));
	CHECK_FALSE(parse_float("1-2     "));
	CHECK_FALSE(parse_float("1.2.3   "));
	CHECK_FALSE(parse_float("1e5     "));
	CHECK_FALSE(parse_float("5 0     "));
	CHECK_FALSE(parse_hex("#38zz00 "));
	CHECK_FALSE(parse_hex("#38 00  "));
	// well formed
	CHECK(*parse_float("-.5     ") == doctest::Approx(-0.5f));
	CHECK(*parse_float("1.      ") == doctest::Approx(1.0f));
	CHECK(*parse_float("696.7742") == doctest::Approx(696.7742f));
	CHECK(parse_int("-0      ") == 0);
	CHECK(parse_int(std::string_view("7\0\0\0\0\0\0\0", 8)) == 7);
	// no magic value: 99999999 is a number like any other (it was CONFIG_UNDEFINED)
	CHECK(parse_int("99999999") == 99999999);
	CHECK(*parse_float("99999999") == doctest::Approx(99999999.0f));
	// every default value parses as its key's type (i, f, h) or is blank
	for (size_t n = 1; n + 1 < kDefaults.size(); n++) {
		const ConfigParam& c = kDefaults[n];
		CAPTURE(field(c.key));
		CAPTURE(field(c.value));
		const std::string_view v(c.value, 8);
		if (settings::trimmed(v).empty()) continue;
		if (c.key[0] == 'i') CHECK(parse_int(v));
		if (c.key[0] == 'f') CHECK(parse_float(v));
		if (c.key[0] == 'h') CHECK(parse_hex(v));
	}
}

TEST_CASE("decimals: the firmware's arithmetic of old, bit for bit (the tunings and goldens use it)") {
	// atof8: value*10 + digit, then weight = 0.1 (a double, as float), weight *= 0.1 in double
	auto old_atof8 = [](const char* str) {
		float value = 0;
		int i = 0;
		while (i < 8 && str[i] >= '0' && str[i] <= '9') {
			value *= 10;
			value += str[i] - '0';
			++i;
		}
		if (i < 8 && str[i] == '.') {
			++i;
			float weight = 0.1;
			while (i < 8 && str[i] >= '0' && str[i] <= '9') {
				value += weight * (str[i] - '0');
				weight *= 0.1;
				++i;
			}
		}
		return value;
	};
	for (const char* v : {"696.7742", "694.7368", "701.9550", "685.7142", "1024.130", "585.2169", "1901.955", "696.7741", "0.25    ", "10.3616 ", "5.96386 "}) {
		CAPTURE(v);
		const float a = *parse_float(v), b = old_atof8(v);
		CHECK(std::memcmp(&a, &b, sizeof a) == 0);
	}
}

TEST_CASE("atox8 R11: 8 hex digits, unsigned") {
	CHECK(parse_hex("FFFFFFFF") == 0xffffffffu);
	CHECK(parse_hex("80000000") == 0x80000000u);
	CHECK(parse_hex("99999999") == 0x99999999u);
	CHECK(parse_hex("#FFFFFFF") == 0x0fffffffu);
}

TEST_CASE("lookup: the user's page overrides the defaults, unknown keys are nullopt") {
	const ConfigParam& d = kDefaults[1];
	const Page p(config_page({{field(d.key), "override"}}));
	const ConfigTable t = p.table();
	CHECK(t.raw(KeyName::from(d.key)) == "override");
	CHECK_FALSE(t.is_default(KeyName::from(d.key)));
	CHECK_FALSE(t.raw(KeyName::from("NoSuchK ")).has_value());
	CHECK(t.raw(KeyName::from(kDefaults[2].key))->data() == kDefaults[2].value);
	// a blank value on the user's page is "not set": no number
	const Page blank(config_page({{"iP1Mint ", ""}}));
	CHECK(blank.table().text(KeyName::preset(1, "Mint")) == "");
	CHECK_FALSE(blank.table().get_int(KeyName::preset(1, "Mint")).has_value());
	// text: without the padding
	const Page mode(config_page({{"sP2Mmode", "normal"}}));
	CHECK(mode.table().text(KeyName::preset(2, "Mmode")) == "normal");
	CHECK(mode.table().text(KeyName::preset(3, "Mmode")) == "");
	// no page: the defaults
	const ConfigTable none({}, kDefaults);
	CHECK(none.get_int(KeyName::preset(1, "Mint")) == 1);
}

TEST_CASE("the board's table: settings::user_page() over the defaults (C files: config_get_float ...)") {
	char name[3];
	put_button_name(19, name);
	const std::string fac19 = "fCfac" + std::string(name, 3);
	const std::vector<ConfigParam> page = config_page({{fac19, "2.5"}, {"sGjack2", "midi"}});
	config_test_page = page;
	float f = 0;
	CHECK(config_get_float(fac19.c_str(), &f));
	CHECK(f == 2.5f);
	f = 0;
	CHECK(config_calibration_float("fac", 19, &f));
	CHECK(f == 2.5f);
	CHECK(config_calibration_float("off", 19, &f));
	CHECK(f == 0.0f);
	CHECK_FALSE(config_calibration_float("xyz", 19, &f));
	CHECK(config_text_is("sGjack2 ", "midi"));
	CHECK_FALSE(config_text_is("sGjack2 ", "mid"));
	CHECK_FALSE(config_get_float("NoSuchK ", &f));
	config_test_page = {};
	CHECK_FALSE(config_text_is("sGjack2 ", "midi"));
}

TEST_CASE("lookup with keys at any alignment (B12: memcmp, no 8-byte reads)") {
	alignas(8) char buf[32];
	const ConfigTable t({}, kDefaults);
	for (int off = 0; off < 8; off++) {
		CAPTURE(off);
		std::memcpy(buf + off, "iGoct   ", 8);
		CHECK(t.raw(KeyName::from(buf + off)) == t.raw(KeyName::general("oct")));
		CHECK(t.raw(KeyName::from(buf + off)).has_value());
	}
}

TEST_CASE("lookup stays in the page and needs the MConfig v1.0 header (R13)") {
	const ConfigParam& d = kDefaults[1];
	std::vector<ConfigParam> sector = erased_sector();
	// a whole sector of printable records without the end marker: the lookup ends with the sector
	put(sector[0], "MConfig ", "v1.0    ");
	for (size_t n = 1; n < sector.size(); n++) put(sector[n], "xPadding", "........");
	CHECK_FALSE(ConfigTable(sector, kDefaults).raw(KeyName::from("Firmware")).has_value());
	CHECK(ConfigTable(sector, kDefaults).raw(KeyName::from(d.key))->data() == d.value);
	// a page that doesn't start with the header (garbage, or a partial write) is not used
	sector = erased_sector();
	put(sector[0], "xGarbage", "        ");
	put(sector[1], d.key, "override");
	put(sector[2], "MCfgEnd ", "        ");
	CHECK(ConfigTable(sector, kDefaults).raw(KeyName::from(d.key))->data() == d.value);
	// with the header it is
	put(sector[0], "MConfig ", "v1.0    ");
	CHECK(ConfigTable(sector, kDefaults).raw(KeyName::from(d.key)) == "override");
	// a power loss while the page was programmed: the header and one record, then erased
	std::memset(&sector[2], 0xff, sizeof(ConfigParam));
	CHECK(ConfigTable(sector, kDefaults).raw(KeyName::from(d.key)) == "override");
	CHECK(ConfigTable(sector, kDefaults).raw(KeyName::from(kDefaults[2].key))->data() == kDefaults[2].value);
	// an empty page (the board: a page with an ECC error, R14) gives the defaults
	CHECK(ConfigTable({}, kDefaults).raw(KeyName::from(d.key))->data() == d.value);
	// a header of another format version is not used
	put(sector[0], "MConfig ", "v2.0    ");
	CHECK(ConfigTable(sector, kDefaults).raw(KeyName::from(d.key))->data() == d.value);
}
