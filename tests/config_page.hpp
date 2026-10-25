// The user's settings page in the host tests (settings::user_page, config_table.hpp): the records a
// test sets, none at first (the defaults apply). On the board it is the flash sector at 0x08020000.
#pragma once

#include <span>
#include <string>
#include <utility>
#include <vector>

#include "config_store.h"

// the page settings::user_page() returns
extern std::span<const ConfigParam> config_test_page;

// a page of the records the config editor writes: the header, (key, value) padded to 8, the end
std::vector<ConfigParam> config_page(const std::vector<std::pair<std::string, std::string>>& recs);
