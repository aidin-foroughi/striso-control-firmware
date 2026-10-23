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

// the board's config flash page, mapped once at its address (0x08020000), erased (0xFF: no user
// settings); nullptr if the address isn't available on this host
char* flash_page() {
	static char* page = [] {
		void* want = reinterpret_cast<void*>(flash_config);
		void* p = mmap(want, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
		if (p == MAP_FAILED || p != want) return static_cast<char*>(nullptr);
		std::memset(p, 0xff, 4096);
		return static_cast<char*>(p);
	}();
	return page;
}

}  // namespace

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
	std::memcpy(recs[0].key, "MConfig ", 8); std::memcpy(recs[0].value, "v1.0    ", 8);
	std::memcpy(recs[1].key, d.key, 8);      std::memcpy(recs[1].value, "override", 8);
	std::memcpy(recs[2].key, "MCfgEnd ", 8); std::memcpy(recs[2].value, "        ", 8);
	char* flash = flash_page();
	REQUIRE_MESSAGE(flash != nullptr, "0x08020000 is not mappable on this host");
	std::memcpy(flash, recs, sizeof recs);
	CHECK(field(getConfigSetting(d.key)) == "override");
	CHECK(std::string(getConfigSetting("NoSuchK ")) == "(ERROR!)");
	CHECK(getConfigSetting(default_config[2].key) == default_config[2].value);
	std::memset(flash, 0xff, 4096);  // erased again: the defaults apply
	CHECK(getConfigSetting(d.key) == d.value);
}
