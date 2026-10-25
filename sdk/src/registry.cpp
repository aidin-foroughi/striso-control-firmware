// Lookups over the engines linked in (engines() is generated: sdk/CMakeLists.txt).
#include <charconv>

#include "striso_sdk/engines.h"

namespace striso::sdk {

const EngineEntry* find_engine(std::string_view name) {
	for (const EngineEntry* e : engines())
		if (name == e->info->name) return e;
	return nullptr;
}

int find_preset(const EngineEntry& e, std::string_view s) {
	const int n = e.info->num_presets;
	int i = -1;
	const auto r = std::from_chars(s.data(), s.data() + s.size(), i);
	if (r.ec == std::errc() && r.ptr == s.data() + s.size()) return i >= 0 && i < n ? i : -1;
	for (int k = 0; k < n; k++)
		if (s == e.preset_name(k)) return k;
	return -1;
}

} // namespace striso::sdk
