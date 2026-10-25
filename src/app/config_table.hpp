// The configuration in C++: the settings table (the user's page over the defaults), its key names
// and value parsers, and the default records built from the schema (config_schema.hpp).
//
//   const settings::ConfigTable cfg = settings::board();
//   if (auto i = cfg.get_int(settings::KeyName::preset(n, "Mint")); i && *i >= 1) ...
//   if (cfg.is(settings::KeyName::general("altkey"), "pedal")) ...
//
// A key's name is checked against the schema at compile time: KeyName::preset(n, "Mnit") does not
// compile. A getter gives nullopt when neither the user's page nor the defaults have the key, when
// the value is blank (not set) or when it is not a number of the getter's type.
#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <span>
#include <string_view>

#include "config_schema.hpp"
#include "config_store.h"

namespace settings {

using config_schema::Field;

// --- the values: 8 characters, a number then spaces (or NULs) -----------------------------------
// (a shorter view reads as if padded with spaces)

namespace detail {
constexpr char at(std::string_view v, std::size_t i) { return i < v.size() ? v[i] : ' '; }
constexpr bool rest_blank(std::string_view v, std::size_t i) {
	for (; i < 8; i++)
		if (at(v, i) != ' ' && at(v, i) != '\0') return false;
	return true;
}
} // namespace detail

// a decimal number with an optional sign and fraction ("-1.25", ".5", "3."); not "-", ".", "1e5" or
// "1-2" (R12). The arithmetic is the firmware's of old (tunings and goldens depend on it).
constexpr std::optional<float> parse_float(std::string_view v) {
	bool neg = false;
	std::size_t i = 0;
	int digits = 0;
	if (detail::at(v, i) == '-') {
		neg = true;
		++i;
	} else if (detail::at(v, i) == '+') {
		++i;
	}
	float value = 0;
	while (i < 8 && detail::at(v, i) >= '0' && detail::at(v, i) <= '9') {
		value *= 10;
		value += static_cast<float>(detail::at(v, i) - '0');
		++i;
		++digits;
	}
	if (i < 8 && detail::at(v, i) == '.') {
		++i;
		float weight = static_cast<float>(0.1);
		while (i < 8 && detail::at(v, i) >= '0' && detail::at(v, i) <= '9') {
			value += weight * static_cast<float>(detail::at(v, i) - '0');
			weight = static_cast<float>(weight * 0.1);
			++i;
			++digits;
		}
	}
	if (digits == 0 || !detail::rest_blank(v, i)) return std::nullopt;
	return neg ? -value : value;
}

// a decimal integer with an optional sign
constexpr std::optional<int> parse_int(std::string_view v) {
	bool neg = false;
	std::size_t i = 0;
	int digits = 0;
	if (detail::at(v, i) == '-') {
		neg = true;
		++i;
	} else if (detail::at(v, i) == '+') {
		++i;
	}
	int value = 0;
	while (i < 8 && detail::at(v, i) >= '0' && detail::at(v, i) <= '9') {
		value = value * 10 + (detail::at(v, i) - '0');
		++i;
		++digits;
	}
	if (digits == 0 || !detail::rest_blank(v, i)) return std::nullopt;
	return neg ? -value : value;
}

// hexadecimal, optionally after '#' or "0x" ("#380000", "0x1F", "C0FFEE"): up to 8 digits, unsigned (R11)
constexpr std::optional<uint32_t> parse_hex(std::string_view v) {
	std::size_t i = 0;
	if (detail::at(v, 0) == '#') {
		i = 1;
	} else if (detail::at(v, 0) == '0' && detail::at(v, 1) == 'x') {
		i = 2;
	}
	uint32_t value = 0;
	int digits = 0;
	for (; i < 8; ++i, ++digits) {
		const char c = detail::at(v, i);
		uint32_t d = 0;
		if (c >= '0' && c <= '9') {
			d = static_cast<uint32_t>(c - '0');
		} else if (c >= 'A' && c <= 'F') {
			d = static_cast<uint32_t>(c - 'A' + 10);
		} else if (c >= 'a' && c <= 'f') {
			d = static_cast<uint32_t>(c - 'a' + 10);
		} else {
			break;
		}
		value = value * 16 + d;
	}
	if (digits == 0 || !detail::rest_blank(v, i)) return std::nullopt;
	return value;
}

// a value without its padding (trailing spaces and NULs)
constexpr std::string_view trimmed(std::string_view v) {
	while (!v.empty() && (v.back() == ' ' || v.back() == '\0')) v.remove_suffix(1);
	return v;
}

// --- key names -----------------------------------------------------------------------------------

// key number n (0..60): its 3-character name, F_1 .. B_4; any other n: "---"
constexpr void write_key_name(int n, char* out) {
	if (n < 0 || n >= config_schema::kKeys) { // no key (R15: n < -7 read before the names)
		out[0] = out[1] = out[2] = '-';
		return;
	}
	const int note = (n + 7) % 17;
	const int octave = (n + 7) / 17;
	out[0] = config_schema::kNoteNames[note * 2];
	out[1] = config_schema::kNoteNames[note * 2 + 1];
	out[2] = static_cast<char>('1' + octave);
}

namespace detail {
// the group's character in the key
enum class Group : char {
	General = 'G',
	Preset = 'P',
	Tuning = 'T',
	Calibration = 'C',
};
template <Group G, bool PerKey>
struct FieldName;
} // namespace detail

using GeneralField = detail::FieldName<detail::Group::General, false>;
using PresetField = detail::FieldName<detail::Group::Preset, false>;
using TuningField = detail::FieldName<detail::Group::Tuning, false>;
using CalibrationField = detail::FieldName<detail::Group::Calibration, true>;

static_assert(std::endian::native == std::endian::little, "KeyName: character i is byte i of the word");

// A record's key: 8 characters, space padded, as one 64-bit word (character i in byte i), so a key
// lives in registers (the firmware's -O1 kept every char-array key and its copies on the stack: a
// preset load took 700 bytes of the 1 kB thread stack).
struct KeyName {
	uint64_t bits = 0x2020202020202020ull; // 8 spaces

	constexpr bool operator==(const KeyName&) const = default;
	constexpr char operator[](std::size_t i) const { return static_cast<char>(bits >> (8 * i)); }
	// the key with character i set to c
	constexpr KeyName with(std::size_t i, char c) const {
		return {(bits & ~(uint64_t{0xff} << (8 * i))) | uint64_t{static_cast<uint8_t>(c)} << (8 * i)};
	}
	// the key with key number key's name (3 characters) at character i
	constexpr KeyName with_key_name(std::size_t i, int key) const {
		const int note = (key + 7) % 17;
		const char octave = static_cast<char>('1' + (key + 7) / 17);
		return with(i, config_schema::kNoteNames[note * 2]).with(i + 1, config_schema::kNoteNames[note * 2 + 1]).with(i + 2, octave);
	}
	constexpr std::array<char, 8> chars() const {
		std::array<char, 8> c{};
		for (std::size_t i = 0; i < 8; i++) c[i] = (*this)[i];
		return c;
	}
	// it is the 8 characters at key (any alignment)
	constexpr bool matches(const char (&key)[8]) const { return std::bit_cast<uint64_t>(key) == bits; }

	// the 8 characters at key as they are (e.g. a key from C code or a record)
	static constexpr KeyName from(const char* key) {
		KeyName k{0};
		for (std::size_t i = 0; i < 8; i++) k.bits |= uint64_t{static_cast<uint8_t>(key[i])} << (8 * i);
		return k;
	}
	// iG<name>: a general setting ("preset" -> iGpreset)
	static constexpr KeyName general(GeneralField f);
	// <type>P<n><name>: a setting of preset n (1-8)
	static constexpr KeyName preset(int n, PresetField f);
	// <type>T<n><name>: a setting of tuning n (1-8)
	static constexpr KeyName tuning(int n, TuningField f);
	// fT<n><key name>: tuning n's offset of key number key (0..60)
	static constexpr KeyName tuning_key(int n, int key);
	// fC<name><key name>: the calibration setting of key number key (0..60)
	static constexpr KeyName calibration(CalibrationField f, int key);
};

namespace detail {
// not defined: a call of it in a constant expression is the compile error of an unknown name
void unknown_config_setting(const char* name);

constexpr std::span<const Field> fields(Group g) {
	switch (g) {
	case Group::General: return config_schema::kGeneral;
	case Group::Preset: return config_schema::kPreset;
	case Group::Tuning: return config_schema::kTuning;
	case Group::Calibration: return config_schema::kCalibration;
	}
	return {};
}

constexpr bool name_is(const Field& f, std::string_view name) { return std::string_view(f.name) == trimmed(name); }

// the key of field f of group g, '0' for the preset or tuning number; at: where a key name goes
constexpr KeyName field_key(const Field& f, Group g, std::size_t* at = nullptr) {
	KeyName k;
	std::size_t i = 0;
	k = k.with(i++, f.type);
	k = k.with(i++, static_cast<char>(g));
	if (g == Group::Preset || g == Group::Tuning) k = k.with(i++, '0');
	for (const char* p = f.name; *p != '\0'; p++) k = k.with(i++, *p);
	if (at != nullptr) *at = i;
	return k;
}

// A field of a group by its name (trailing spaces allowed: "Mint "), per key or not: at compile
// time the whole key ("iP0Mint ") and where a per-key field's key name goes.
template <Group G, bool PerKey>
struct FieldName {
	uint64_t key = 0;
	uint32_t key_at = 0;

	consteval FieldName(const char* n) { // implicit: KeyName::preset(n, "Mint")
		for (const Field& f : fields(G)) {
			if (f.per_key == PerKey && name_is(f, n)) {
				std::size_t at = 0;
				key = field_key(f, G, &at).bits;
				key_at = static_cast<uint32_t>(at);
				return;
			}
		}
		unknown_config_setting(n);
	}
};
} // namespace detail

constexpr KeyName KeyName::general(GeneralField f) { return {f.key}; }
constexpr KeyName KeyName::preset(int n, PresetField f) { return KeyName{f.key}.with(2, static_cast<char>('0' + n)); }
constexpr KeyName KeyName::tuning(int n, TuningField f) { return KeyName{f.key}.with(2, static_cast<char>('0' + n)); }
constexpr KeyName KeyName::tuning_key(int n, int key) {
	constexpr detail::FieldName<detail::Group::Tuning, true> f("");
	return KeyName{f.key}.with(2, static_cast<char>('0' + n)).with_key_name(f.key_at, key);
}
constexpr KeyName KeyName::calibration(CalibrationField f, int key) { return KeyName{f.key}.with_key_name(f.key_at, key); }

// --- the table -----------------------------------------------------------------------------------

constexpr bool same8(const char* a, const char* b) {
	for (std::size_t i = 0; i < 8; i++)
		if (a[i] != b[i]) return false;
	return true;
}

// The settings: the user's page of records over the defaults. A page is used only if it starts
// with the header record (MConfig v1.0); it is read up to the end record (MCfgEnd), a key that
// isn't printable text (an erased or partly written page) or the end of the span (R13).
class ConfigTable {
public:
	constexpr ConfigTable(std::span<const ConfigParam> user, std::span<const ConfigParam> defaults) :
	    user_(records(user)), defaults_(records(defaults)) {}

	// the 8 characters of key's value: the user's, else the default; nullopt if neither has it
	constexpr std::optional<std::string_view> raw(KeyName key) const {
		if (const char* v = find(user_, key)) return std::string_view(v, 8);
		if (const char* v = find(defaults_, key)) return std::string_view(v, 8);
		return std::nullopt;
	}
	// the value without its padding ("" if blank)
	constexpr std::optional<std::string_view> text(KeyName key) const {
		const auto v = raw(key);
		return v ? std::optional<std::string_view>(trimmed(*v)) : std::nullopt;
	}
	// the value without its padding is text (a choice: "mpe", "normal" ...)
	constexpr bool is(KeyName key, std::string_view text) const {
		const auto v = raw(key);
		return v && trimmed(*v) == text;
	}
	constexpr std::optional<float> get_float(KeyName key) const {
		const auto v = raw(key);
		return v ? parse_float(*v) : std::nullopt;
	}
	constexpr std::optional<int> get_int(KeyName key) const {
		const auto v = raw(key);
		return v ? parse_int(*v) : std::nullopt;
	}
	constexpr std::optional<uint32_t> get_hex(KeyName key) const {
		const auto v = raw(key);
		return v ? parse_hex(*v) : std::nullopt;
	}
	// the key has a default and its value is it (the user's page doesn't change it)
	constexpr bool is_default(KeyName key) const {
		const char* d = find(defaults_, key);
		return d != nullptr && same8(raw(key)->data(), d);
	}

private:
	static constexpr bool printable(char c) { return c >= 32 && c < 127; }

	static constexpr std::span<const ConfigParam> records(std::span<const ConfigParam> page) {
		if (page.empty() || !same8(page[0].key, config_schema::kHeaderKey) || !same8(page[0].value, config_schema::kHeaderValue)) return {};
		return page.subspan(1);
	}
	// (one copy of the scan in the firmware, not one per lookup)
	[[gnu::noinline]] static constexpr const char* find(std::span<const ConfigParam> recs, KeyName key) {
		constexpr KeyName end = KeyName::from(config_schema::kEndKey);
		for (const ConfigParam& r : recs) {
			if (end.matches(r.key) || !printable(r.key[0])) break;
			if (key.matches(r.key)) return r.value;
		}
		return nullptr;
	}

	std::span<const ConfigParam> user_;
	std::span<const ConfigParam> defaults_;
};

// the user's settings page: on the board its flash sector (config_flash.cpp: empty if it can't be
// read), on the virtual board its flash, in a test what the test gives
std::span<const ConfigParam> user_page();

// the firmware's default records
inline std::span<const ConfigParam> defaults() { return default_config.records; }

// the board's settings, as they are now
inline ConfigTable board() { return {user_page(), defaults()}; }

// --- the default records, from the schema --------------------------------------------------------

namespace detail {
constexpr std::size_t length(const char* s) { return std::string_view(s).size(); }

constexpr std::size_t count(std::span<const Field> fs) {
	std::size_t n = 0;
	for (const Field& f : fs)
		if (!f.deprecated) n += f.per_key ? config_schema::kKeys : 1;
	return n;
}

struct Builder {
	ConfigDefaults d{};
	std::size_t n = 0;

	constexpr void add(KeyName key, const char* value) {
		ConfigParam& r = d.records[n++];
		for (std::size_t i = 0; i < 8; i++) r.key[i] = key[i];
		const std::size_t len = length(value);
		for (std::size_t i = 0; i < 8; i++) r.value[i] = i < len ? value[i] : ' ';
	}
	constexpr void add(const char* key, const char* value) { add(KeyName::from(key), value); }

	// the fields of a group (n: the preset or tuning number, 0: none); value(i, field, key number)
	// is the default of field i (key: -1 if the field isn't per key)
	template <typename Value>
	constexpr void fields(Group group, int n, std::span<const Field> fs, Value value) {
		for (std::size_t i = 0; i < fs.size(); i++) {
			const Field& f = fs[i];
			if (f.deprecated) continue;
			std::size_t at = 0;
			KeyName k = field_key(f, group, &at);
			if (n > 0) k = k.with(2, static_cast<char>('0' + n));
			if (!f.per_key) {
				add(k, value(i, f, -1));
				continue;
			}
			for (int key = 0; key < config_schema::kKeys; key++) add(k.with_key_name(at, key), value(i, f, key));
		}
	}
};
} // namespace detail

// the number of default records: the header, the settings, the end record
constexpr std::size_t default_record_count() {
	using namespace config_schema;
	return 2 + detail::count(kGeneral) + kPresets * detail::count(kPreset) + kTunings * detail::count(kTuning) + detail::count(kCalibration);
}

// default_config: the header; the general settings; presets 1-8 (their name and colour, preset 1
// also the fields' defaults); tunings 1-8 (their values, the per-key offsets by note or the
// field's); the calibration; the end record. tools/config_schema.py builds the same records.
constexpr ConfigDefaults make_defaults() {
	using namespace config_schema;
	detail::Builder b;
	b.add(kHeaderKey, kHeaderValue);
	b.fields(detail::Group::General, 0, kGeneral, [](std::size_t, const Field& f, int) { return f.value; });
	for (int p = 1; p <= kPresets; p++) {
		b.fields(detail::Group::Preset, p, kPreset, [p](std::size_t i, const Field& f, int) {
			constexpr std::size_t own = std::size(kPresetValues[0]);
			if (i < own) return kPresetValues[p - 1][i];
			return p == 1 ? f.value : "";
		});
	}
	for (int t = 1; t <= kTunings; t++) {
		b.fields(detail::Group::Tuning, t, kTuning, [t](std::size_t i, const Field& f, int key) {
			constexpr std::size_t own = std::size(kTuningValues[0]);
			if (i < own) return kTuningValues[t - 1][i];
			const char* note = kTuningNotes[t - 1][(key + 7) % 17];
			return f.per_key && note != nullptr ? note : f.value;
		});
	}
	b.fields(detail::Group::Calibration, 0, kCalibration, [](std::size_t, const Field& f, int) { return f.value; });
	b.add(kEndKey, kEndValue);
	return b.d;
}

} // namespace settings
