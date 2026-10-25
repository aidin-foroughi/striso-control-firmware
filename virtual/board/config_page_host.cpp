/* The user's settings page of the virtual board (settings::user_page, config_table.hpp): its
 * config flash, the front's sv_shared (fw_api.h), which a reset leaves alone. On the board it is
 * the config sector (src/app/config_flash.cpp). */
#include <span>

#include "config_table.hpp"
#include "fw_api.h"
#include "sv_host.h"

std::span<const ConfigParam> settings::user_page() {
	return {reinterpret_cast<const ConfigParam*>(svh_shared->flash), SV_FLASH_SIZE / sizeof(ConfigParam)};
}
