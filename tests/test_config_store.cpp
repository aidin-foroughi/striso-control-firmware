// The configuration record format (src/app/config_store.c, config.h): 16-byte records, an 8-char
// key and an 8-char value, space padded, from "MConfig " to "MCfgEnd ". The user's settings are in
// flash at 0x08020000 (the config editor writes them as CONFIG.UF2) and override the firmware's
// defaults; lookups fall back to default_config, and unknown keys give "(ERROR!)". The flash
// table is mapped at its board address for the test.
#include <sys/mman.h>

#include <cmath>
#include <cstring>
#include <set>
#include <string>

#include "doctest/doctest.h"

extern "C" {
#include "config_store.h"
float atof8(const char* str);
int atoi8(const char* str);
unsigned int atox8(const char* str);
}

namespace {

std::string field(const char* s) { return std::string(s, 8); }

constexpr size_t kSector = 128 * 1024; // the config sector, 0x08020000-0x0803FFFF

// the board's config flash sector, mapped once at its address (0x08020000), erased (0xFF: no user
// settings), and after it the firmware's .fwinfo text as on the board (0x08040000); nullptr if the
// address isn't available on this host
char* flash_page() {
	static char* page = [] {
		void* want = reinterpret_cast<void*>(flash_config);
		void* p = mmap(want, kSector + 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
		    -1, 0);
		if (p == MAP_FAILED || p != want) return static_cast<char*>(nullptr);
		auto* c = static_cast<char*>(p);
		std::memset(c, 0xff, kSector);
		std::memset(c + kSector, 0, 4096);
		static const char fw[] = "Firmware version: striso_control_2026-10-24_v2.2.4_all\r\n";
		std::memcpy(c + kSector, fw, sizeof fw - 1);
		return c;
	}();
	return page;
}

} // namespace

TEST_CASE("config records are 16 bytes: an 8-char key and an 8-char value") {
	CHECK(sizeof(ConfigParam) == 16);
	CHECK(offsetof(ConfigParam, value) == 8);
}

TEST_CASE("default config: MConfig v1.0 ... MCfgEnd, printable unique keys, every key found") {
	REQUIRE_MESSAGE(flash_page() != nullptr, "0x08020000 is not mappable on this host");
	CHECK(field(default_config[0].key) == "MConfig ");
	CHECK(field(default_config[0].value) == "v1.0    ");
	std::set<std::string> keys;
	size_t n = 1;
	for (; n < 4096 && field(default_config[n].key) != "MCfgEnd "; n++) {
		const ConfigParam& c = default_config[n];
		CAPTURE(field(c.key));
		for (char ch : field(c.key)) CHECK((ch >= 32 && ch < 127));
		for (char ch : field(c.value)) CHECK((ch >= 32 && ch < 127));
		CHECK(keys.insert(field(c.key)).second);
		// the lookup of every key returns its value (no other record shadows it)
		CHECK(getConfigSetting(c.key) == c.value);
	}
	CHECK(n < 4096);
	const std::string msg = std::to_string(n - 1) + " default records";
	MESSAGE(msg);
}

TEST_CASE("value parsing: atoi8, atof8, atox8 read at most 8 characters") {
	CHECK(atoi8("42      ") == 42);
	CHECK(atoi8("-17     ") == -17);
	CHECK(atoi8("+3      ") == 3);
	CHECK(atoi8("12345678") == 12345678);
	CHECK(atoi8("        ") == CONFIG_UNDEFINED);
	CHECK(atof8("0.5     ") == doctest::Approx(0.5f));
	CHECK(atof8("-1.25   ") == doctest::Approx(-1.25f));
	CHECK(atof8("3       ") == doctest::Approx(3.0f));
	CHECK(atof8("abc     ") == CONFIG_UNDEFINED);
	CHECK(atox8("#ff00aa ") == 0xff00aau);
	CHECK(atox8("0x1F    ") == 0x1fu);
	CHECK(atox8("C0FFEE  ") == 0xc0ffeeu);
}

TEST_CASE("lookup: the flash settings override the defaults, unknown keys are an error") {
	// the first default after the start tag, overridden in the flash table
	const ConfigParam& d = default_config[1];
	ConfigParam recs[3];
	std::memcpy(recs[0].key, "MConfig ", 8);
	std::memcpy(recs[0].value, "v1.0    ", 8);
	std::memcpy(recs[1].key, d.key, 8);
	std::memcpy(recs[1].value, "override", 8);
	std::memcpy(recs[2].key, "MCfgEnd ", 8);
	std::memcpy(recs[2].value, "        ", 8);
	char* flash = flash_page();
	REQUIRE_MESSAGE(flash != nullptr, "0x08020000 is not mappable on this host");
	std::memcpy(flash, recs, sizeof recs);
	CHECK(field(getConfigSetting(d.key)) == "override");
	CHECK(std::string(getConfigSetting("NoSuchK ")) == "(ERROR!)");
	CHECK(getConfigSetting(default_config[2].key) == default_config[2].value);
	std::memset(flash, 0xff, 4096); // erased again: the defaults apply
	CHECK(getConfigSetting(d.key) == d.value);
}

TEST_CASE("value parsing is strict: a number, then only spaces (R12)") {
	// no digits
	CHECK(atoi8("-       ") == CONFIG_UNDEFINED);
	CHECK(atoi8("+       ") == CONFIG_UNDEFINED);
	CHECK(atof8("-       ") == CONFIG_UNDEFINED);
	CHECK(atof8("+       ") == CONFIG_UNDEFINED);
	CHECK(atof8(".       ") == CONFIG_UNDEFINED);
	CHECK(atof8("-.      ") == CONFIG_UNDEFINED);
	// trailing garbage
	CHECK(atoi8("1-2     ") == CONFIG_UNDEFINED);
	CHECK(atoi8("1.2.3   ") == CONFIG_UNDEFINED);
	CHECK(atoi8("-.5     ") == CONFIG_UNDEFINED);
	CHECK(atof8("1-2     ") == CONFIG_UNDEFINED);
	CHECK(atof8("1.2.3   ") == CONFIG_UNDEFINED);
	CHECK(atof8("1e5     ") == CONFIG_UNDEFINED);
	CHECK(atof8("5 0     ") == CONFIG_UNDEFINED);
	// well formed: unchanged
	CHECK(atof8("-.5     ") == doctest::Approx(-0.5f));
	CHECK(atof8("1.      ") == doctest::Approx(1.0f));
	CHECK(atof8("696.7742") == doctest::Approx(696.7742f));
	CHECK(atoi8("-0      ") == 0);
	CHECK(atoi8("7\0\0\0\0\0\0\0") == 7);
	// every default value parses as its key's type (i, f, h) or is blank
	for (size_t n = 1; n < 4096 && field(default_config[n].key) != "MCfgEnd "; n++) {
		const ConfigParam& c = default_config[n];
		CAPTURE(field(c.key));
		CAPTURE(field(c.value));
		const bool blank = field(c.value) == "        ";
		if (c.key[0] == 'i') CHECK((blank || atoi8(c.value) != CONFIG_UNDEFINED));
		if (c.key[0] == 'f') CHECK((blank || atof8(c.value) != CONFIG_UNDEFINED));
	}
}

TEST_CASE("cmp8: all 8 characters, at any alignment (B12)") {
	// keys at every offset from an 8-byte boundary: UBSan (host-asan) reported the old uint64_t
	// read of an unaligned key
	alignas(8) char buf[32];
	for (int off = 0; off < 8; off++) {
		CAPTURE(off);
		std::memcpy(buf + off, "fCfacC_3", 8);
		std::memcpy(buf + 16 + (7 - off), "fCfacC_3", 8);
		CHECK(cmp8(buf + off, buf + 16 + (7 - off)));
		CHECK(cmp8(buf + off, "fCfacC_3"));
		buf[off + 7] = '4'; // differs only in the last character
		CHECK_FALSE(cmp8(buf + off, "fCfacC_3"));
		buf[off + 7] = '3';
		buf[off] = 'i'; // only in the first
		CHECK_FALSE(cmp8(buf + off, "fCfacC_3"));
	}
	// a lookup with an unaligned name finds the default
	std::memcpy(buf + 3, "iGoct   ", 8);
	CHECK(field(getConfigSetting(buf + 3)) != "(ERROR!)");
	CHECK(field(getConfigSetting(buf + 3)) == field(getConfigSetting("iGoct   ")));
}

TEST_CASE("atox8: 8 hex digits, no signed overflow (R11)") {
	CHECK(atox8("FFFFFFFF") == 0xffffffffu);
	CHECK(atox8("80000000") == 0x80000000u);
	CHECK(atox8("99999999") == 0x99999999u);
	CHECK(atox8("#FFFFFFF") == 0x0fffffffu);
}

TEST_CASE("lookup stays in the config sector and needs the MConfig v1.0 header (R13)") {
	char* flash = flash_page();
	REQUIRE_MESSAGE(flash != nullptr, "0x08020000 is not mappable on this host");
	const ConfigParam& d = default_config[1];
	// a whole sector of printable records without the end marker: the scan must not run into .fwinfo
	std::memcpy(flash, "MConfig v1.0    ", 16);
	for (size_t o = 16; o < kSector; o += 16) std::memcpy(flash + o, "xPadding........", 16);
	const char* v = getConfigSetting("Firmware");
	CHECK(std::string(v) == "(ERROR!)");
	CHECK(getConfigSetting(d.key) == d.value);
	// a page that doesn't start with the header (garbage, or a partial write) is not used
	std::memset(flash, 0xff, kSector);
	std::memcpy(flash, "xGarbage", 8);
	std::memcpy(flash + 8, "        ", 8);
	std::memcpy(flash + 16, d.key, 8);
	std::memcpy(flash + 24, "override", 8);
	std::memcpy(flash + 32, "MCfgEnd         ", 16);
	CHECK(getConfigSetting(d.key) == d.value);
	// with the header it is
	std::memcpy(flash, "MConfig v1.0    ", 16);
	CHECK(field(getConfigSetting(d.key)) == "override");
	// a power loss while the page was programmed: the header and one record, then erased
	std::memset(flash + 32, 0xff, 16);
	CHECK(field(getConfigSetting(d.key)) == "override");
	CHECK(getConfigSetting(default_config[2].key) == default_config[2].value);
	std::memset(flash, 0xff, kSector);
}
