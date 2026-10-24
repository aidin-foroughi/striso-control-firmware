// The buttons' calibration (src/drivers/key_calibration.c): at boot, the factory c_force and the
// user's per-key factors and zero offsets from the config (fCfac<key>, fCoff<key>), and StcC's
// reset. S11: the factors must apply at boot whether or not the board has its calibration
// record, with the range the live setter (buttonSetKeyFactor) accepts, and no c_force may go to
// the positions without a key. The flash settings table is mapped at its board address.
#include <sys/mman.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

#include "doctest/doctest.h"

extern "C" {
#include "config_store.h"
#include "key_scanner.h"
}

namespace {

constexpr int32_t kCalibForce = (1 << 18) / 64 + 1; // config.h CALIB_FORCE
constexpr std::array<int, 7> kNoKey = {52, 54, 57, 59, 61, 64, 66};

// the board's config flash page (0x08020000), shared with test_config_store.cpp's
char* flash() {
	static char* page = [] {
		void* want = reinterpret_cast<void*>(flash_config);
		void* p = mmap(want, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
		if (p == MAP_FAILED && errno == EEXIST) return static_cast<char*>(want);
		if (p == MAP_FAILED || p != want) return static_cast<char*>(nullptr);
		return static_cast<char*>(p);
	}();
	return page;
}

// user settings: the given records between the start and end tags
void settings(const std::vector<std::pair<std::string, std::string>>& recs) {
	char* f = flash();
	REQUIRE_MESSAGE(f != nullptr, "0x08020000 is not mappable on this host");
	std::memset(f, 0xff, 4096);
	std::vector<ConfigParam> t;
	auto add = [&](const std::string& k, const std::string& v) {
		ConfigParam p{};
		std::memset(&p, ' ', sizeof p);
		std::memcpy(p.key, k.data(), std::min<size_t>(8, k.size()));
		std::memcpy(p.value, v.data(), std::min<size_t>(8, v.size()));
		t.push_back(p);
	};
	add("MConfig ", "v1.0");
	for (const auto& [k, v] : recs) add(k, v);
	add("MCfgEnd ", "");
	std::memcpy(f, t.data(), t.size() * sizeof(ConfigParam));
}

std::string key_name(int n) {
	char k[4] = {};
	put_button_name(n, k);
	return std::string(k, 3);
}

struct Boot {
	std::array<scan_cal_t, SCAN_BUTTONS> cal{};
	std::array<int32_t, SCAN_BUTTONS> factory{};
	std::array<float, SCAN_KEYS> factor{};
	std::array<int32_t, SCAN_BUTTONS> zero{};
};

// ks_init's defaults, then the boot calibration
Boot boot(bool calibrated, unsigned type, const std::array<uint16_t, SCAN_BUTTONS>& calib) {
	Boot b;
	for (int n = 0; n < SCAN_BUTTONS; n++) b.cal[n] = {kCalibForce, INT32_MAX, 0};
	for (int n : kNoKey) b.cal[n].c_force = 0;
	ks_boot_calibration(calibrated, type, calib.data(), b.cal.data(), b.factory.data(), b.factor.data(), b.zero.data());
	return b;
}

std::array<uint16_t, SCAN_BUTTONS> factory_calib() {
	std::array<uint16_t, SCAN_BUTTONS> c{};
	for (int n = 0; n < SCAN_BUTTONS; n++) c[n] = static_cast<uint16_t>(3000 + 17 * n);
	for (int n : kNoKey) c[n] = 0;
	return c;
}

} // namespace

TEST_CASE("key calibration: a calibrated board, the user's key factors and zero offsets") {
	const int k = 20; // a key and its button
	const int but = button_number_map[k];
	settings({{"fCfac" + key_name(k), "2.5"}, {"fCoff" + key_name(k), "0.25"}});
	const Boot b = boot(true, 0x02, factory_calib());
	CHECK(b.factory[but] == 2 * (3000 + 17 * but));
	CHECK(b.cal[but].c_force == static_cast<int32_t>(b.factory[but] * 2.5f + 0.5f));
	CHECK(b.factor[k] == 2.5f);
	CHECK(b.zero[but] == (1 << 24) / 4);
	const int other = button_number_map[k + 1];
	CHECK(b.cal[other].c_force == b.factory[other]);
	CHECK(b.factor[k + 1] == 1.0f);
	std::memset(flash(), 0xff, 4096);
}

TEST_CASE("key calibration S11: the key factors apply on a board without its calibration record") {
	const int k = 33;
	const int but = button_number_map[k];
	settings({{"fCfac" + key_name(k), "3.0"}, {"fCoff" + key_name(k), "0.5"}});
	const Boot b = boot(false, 0, factory_calib());
	CHECK(b.factory[but] == kCalibForce);
	CHECK(b.factor[k] == 3.0f); // what Striso Live reads back (StcK)
	CHECK(b.cal[but].c_force == static_cast<int32_t>(kCalibForce * 3.0f + 0.5f));
	CHECK(b.zero[but] == (1 << 24) / 2);
	std::memset(flash(), 0xff, 4096);
}

TEST_CASE("key calibration S11: a factor the live setter refuses doesn't apply at boot either") {
	// buttonSetKeyFactor accepts 0.1 .. 10; 0 would make the key dead
	const int k = 5;
	const int but = button_number_map[k];
	settings({{"fCfac" + key_name(k), "0.0"}, {"fCfac" + key_name(k + 1), "0.05"}, {"fCfac" + key_name(k + 2), "12.0"}});
	const Boot b = boot(true, 0x02, factory_calib());
	CHECK(b.cal[but].c_force == b.factory[but]);
	CHECK(b.factor[k] == 1.0f);
	CHECK(b.cal[button_number_map[k + 1]].c_force == b.factory[button_number_map[k + 1]]);
	CHECK(b.cal[button_number_map[k + 2]].c_force == b.factory[button_number_map[k + 2]]);
	std::memset(flash(), 0xff, 4096);
}

TEST_CASE("key calibration S11: no c_force for the positions without a key") {
	std::memset(flash(), 0xff, 4096);
	std::array<uint16_t, SCAN_BUTTONS> calib = factory_calib();
	for (int n : kNoKey) calib[n] = 1234; // a calibration record with values there
	const Boot b = boot(true, 0x02, calib);
	for (int n : kNoKey) CHECK(b.cal[n].c_force == 0);
	// StcC: the same base c_force for every key, none for the positions without one
	std::array<scan_cal_t, SCAN_BUTTONS> cal = b.cal;
	ks_reset_calibration(cal.data(), 4096);
	for (int n = 0; n < SCAN_BUTTONS; n++) {
		CAPTURE(n);
		const bool nokey = std::find(kNoKey.begin(), kNoKey.end(), n) != kNoKey.end();
		CHECK(cal[n].c_force == (nokey ? 0 : 4096));
		CHECK(cal[n].c_breakpoint == INT32_MAX);
	}
}
