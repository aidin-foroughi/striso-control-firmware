// The configuration: the default records built from the schema (config_schema.hpp), checked at
// compile time, and the C files' view of the settings (config_store.h). The user's settings page
// comes from settings::user_page() (config_flash.cpp on the board).
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "config_table.hpp"

using settings::KeyName;

// --- the records: the format and the default table, checked here ---------------------------------

static_assert(sizeof(ConfigParam) == 16, "a record is 16 bytes");
static_assert(offsetof(ConfigParam, key) == 0 && offsetof(ConfigParam, value) == 8, "an 8-character key, then an 8-character value");
static_assert(sizeof(ConfigDefaults) == CONFIG_DEFAULT_RECORDS * sizeof(ConfigParam), "default_config is its records");
static_assert(CONFIG_FLASH_ADDRESS == config_schema::kFlashAddress, "the user's page is where the schema says");
static_assert(settings::default_record_count() == CONFIG_DEFAULT_RECORDS, "CONFIG_DEFAULT_RECORDS (config_store.h) is the schema's count");

namespace {

constexpr ConfigDefaults kDefaults = settings::make_defaults();

constexpr bool fits8(const char* s) { return std::string_view(s).size() <= 8; }
constexpr bool printable8(const char* s) {
	for (std::size_t i = 0; i < 8; i++)
		if (s[i] < 32 || s[i] >= 127) return false;
	return true;
}

// the schema: names that fit their key, defaults that fit 8 characters and parse as their type
constexpr bool fields_ok(std::span<const config_schema::Field> fs, std::size_t room) {
	for (const auto& f : fs) {
		const std::size_t len = std::string_view(f.name).size();
		if (len + (f.per_key ? 3 : 0) > room || !fits8(f.value)) return false;
		if (f.type != 's' && f.type != 'i' && f.type != 'f' && f.type != 'h') return false;
	}
	// unique names within the group
	for (std::size_t i = 0; i < fs.size(); i++)
		for (std::size_t j = i + 1; j < fs.size(); j++)
			if (std::string_view(fs[i].name) == std::string_view(fs[j].name) && fs[i].per_key == fs[j].per_key) return false;
	return true;
}
static_assert(fields_ok(config_schema::kGeneral, 6), "general: names up to 6 characters, values up to 8");
static_assert(fields_ok(config_schema::kPreset, 5), "preset: names up to 5 characters, values up to 8");
static_assert(fields_ok(config_schema::kTuning, 5), "tuning: names up to 5 characters (2 per key), values up to 8");
static_assert(fields_ok(config_schema::kCalibration, 6), "calibration: names of 3 characters per key, values up to 8");

constexpr bool own_values_ok() {
	using namespace config_schema;
	// the presets' and tunings' own values are their first fields: name and colour; off, oct, fifth
	if (std::string_view(kPreset[0].name) != "name" || std::string_view(kPreset[1].name) != "color") return false;
	const char* tuning[] = {"name", "color", "off", "oct", "fifth"};
	for (std::size_t i = 0; i < std::size(tuning); i++)
		if (std::string_view(kTuning[i].name) != tuning[i] || kTuning[i].per_key) return false;
	for (const auto& p : kPresetValues)
		for (const char* v : p)
			if (!fits8(v)) return false;
	for (const auto& t : kTuningValues)
		for (const char* v : t)
			if (!fits8(v)) return false;
	for (const auto& t : kTuningNotes)
		for (const char* v : t)
			if (v != nullptr && !fits8(v)) return false;
	return true;
}
static_assert(own_values_ok(), "kPresetValues and kTuningValues give the first fields of kPreset and kTuning, up to 8 characters");

// every record: printable, a key of its type ("MConfig"/"MCfgEnd" at the ends), a default value
// that is blank or parses as the key's type
constexpr bool records_ok() {
	const auto& r = kDefaults.records;
	if (!settings::same8(r[0].key, config_schema::kHeaderKey) || !settings::same8(r[0].value, config_schema::kHeaderValue)) return false;
	const ConfigParam& end = r[CONFIG_DEFAULT_RECORDS - 1];
	if (!settings::same8(end.key, config_schema::kEndKey) || !settings::same8(end.value, config_schema::kEndValue)) return false;
	for (std::size_t n = 1; n + 1 < CONFIG_DEFAULT_RECORDS; n++) {
		if (!printable8(r[n].key) || !printable8(r[n].value)) return false;
		const std::string_view v(r[n].value, 8);
		if (settings::trimmed(v).empty()) continue;
		switch (r[n].key[0]) {
		case 's': break;
		case 'i':
			if (!settings::parse_int(v)) return false;
			break;
		case 'f':
			if (!settings::parse_float(v)) return false;
			break;
		case 'h':
			if (!settings::parse_hex(v)) return false;
			break;
		default: return false;
		}
	}
	return true;
}
static_assert(records_ok(), "default_config: header, printable records with values of their key's type, end record");

constexpr uint64_t key64(const char* k) {
	uint64_t v = 0;
	for (std::size_t i = 0; i < 8; i++) v = v << 8 | static_cast<uint8_t>(k[i]);
	return v;
}
constexpr bool keys_unique() {
	std::array<uint64_t, CONFIG_DEFAULT_RECORDS> keys{};
	for (std::size_t n = 0; n < keys.size(); n++) keys[n] = key64(kDefaults.records[n].key);
	std::sort(keys.begin(), keys.end());
	return std::adjacent_find(keys.begin(), keys.end()) == keys.end();
}
static_assert(keys_unique(), "default_config: every key once");

// the firmware's own lookups find their defaults (KeyName checks the names, this the numbers)
constexpr settings::ConfigTable kDefaultTable({}, kDefaults.records);
static_assert(kDefaultTable.get_int(KeyName::preset(1, "Mint")) == 1);
static_assert(kDefaultTable.text(KeyName::preset(8, "name")) == "preset8");
static_assert(kDefaultTable.get_hex(KeyName::tuning(8, "color")) == 0x00aa55u);
static_assert(kDefaultTable.get_float(KeyName::tuning(8, "fifth")) == settings::parse_float("696.7741"));
static_assert(kDefaultTable.raw(KeyName::tuning_key(8, 60)) == "+4.39774");
static_assert(kDefaultTable.get_float(KeyName::calibration("fac", 0)) == 1.0f);
static_assert(kDefaultTable.text(KeyName::general("altkey")) == "settings");
static_assert(!kDefaultTable.raw(KeyName::general("motion")), "deprecated: no default");
static_assert(KeyName::tuning_key(1, 0) == KeyName::from("fT1F_1  ") && KeyName::calibration("off", 60) == KeyName::from("fCoffB_4"));

} // namespace

// the firmware's default records (the config editor shows them, .fwinfo points to them)
extern "C" constinit const ConfigDefaults default_config = kDefaults;

// --- keys and the C files' settings -------------------------------------------------------------
// (out of line: the C callers run at boot on main's 1 kB stack, LTO would put a table in its frame)

// key number = 17 * buttons[but].coord0 + 10 * buttons[but].coord1 + 30
extern "C" const int button_number_map[61] = {
    56, 51, 63, 58, 53, 65, 60, 55, 67, 62, 40, 35, 47, 42, 37, 49, 44,
    39, 34, 46, 41, 36, 48, 43, 38, 50, 45, 23, 18, 30, 25, 20, 32, 27,
    22, 17, 29, 24, 19, 31, 26, 21, 33, 28, 6, 1, 13, 8, 3, 15, 10,
    5, 0, 12, 7, 2, 14, 9, 4, 16, 11};

extern "C" void put_button_name(int n, char* name) { settings::write_key_name(n, name); }

extern "C" __attribute__((noinline)) bool config_get_float(const char* key, float* value) {
	const auto v = settings::board().get_float(KeyName::from(key));
	if (v) *value = *v;
	return v.has_value();
}

extern "C" __attribute__((noinline)) bool config_text_is(const char* key, const char* text) {
	return settings::board().is(KeyName::from(key), text);
}

extern "C" __attribute__((noinline)) bool config_calibration_float(const char* name, int key, float* value) {
	for (const auto& f : config_schema::kCalibration) {
		if (std::string_view(f.name) != name) continue;
		std::size_t at = 0;
		const KeyName k = settings::detail::field_key(f, settings::detail::Group::Calibration, &at).with_key_name(at, key);
		const auto v = settings::board().get_float(k);
		if (v) *value = *v;
		return v.has_value();
	}
	return false;
}
