#include "config_page.hpp"

#include <algorithm>
#include <cstring>

#include "config_table.hpp"

std::span<const ConfigParam> config_test_page;

std::span<const ConfigParam> settings::user_page() { return config_test_page; }

std::vector<ConfigParam> config_page(const std::vector<std::pair<std::string, std::string>>& recs) {
	std::vector<ConfigParam> page;
	auto add = [&](const std::string& k, const std::string& v) {
		ConfigParam p{};
		std::memset(&p, ' ', sizeof p);
		std::memcpy(p.key, k.data(), std::min<size_t>(8, k.size()));
		std::memcpy(p.value, v.data(), std::min<size_t>(8, v.size()));
		page.push_back(p);
	};
	add(config_schema::kHeaderKey, config_schema::kHeaderValue);
	for (const auto& [k, v] : recs) add(k, v);
	add(config_schema::kEndKey, config_schema::kEndValue);
	return page;
}
