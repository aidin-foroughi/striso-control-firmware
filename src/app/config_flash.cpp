// The board's settings page: the config sector at 0x08020000 (128 KB, ld/STM32H743xI_uf2.ld),
// read only if it can be read safely.
#include <span>

#include "ch.h"
#include "hal.h"

#include "config_table.hpp"

static_assert(config_schema::kFlashSize % sizeof(ConfigParam) == 0);

namespace {

// A power loss while the bootloader programs the config sector can leave a flash word with an
// uncorrectable ECC error; reading it is a bus fault (R14). Read the sector once with data bus
// faults ignored (FAULTMASK + BFHFNMIGN: a faulting load returns garbage instead of faulting) and
// check bank 1's double error flag: if it is set, the user's settings are not used (the defaults are).
bool flash_readable() {
	static int readable = -1;
	if (readable < 0) {
		const volatile uint32_t* w = reinterpret_cast<const volatile uint32_t*>(CONFIG_FLASH_ADDRESS);
		FLASH->CCR1 = FLASH_CCR_CLR_DBECCERR | FLASH_CCR_CLR_SNECCERR;
		__set_FAULTMASK(1);
		SCB->CCR |= SCB_CCR_BFHFNMIGN_Msk;
		__DSB();
		__ISB();
		// one word of every 32-byte flash word (ECC is per 256-bit flash word)
		for (unsigned i = 0; i < config_schema::kFlashSize / 4; i += 8) {
			(void)w[i];
		}
		__DSB();
		SCB->CCR &= ~SCB_CCR_BFHFNMIGN_Msk;
		__DSB();
		__ISB();
		__set_FAULTMASK(0);
		readable = (FLASH->SR1 & FLASH_SR_DBECCERR) == 0;
		FLASH->CCR1 = FLASH_CCR_CLR_DBECCERR | FLASH_CCR_CLR_SNECCERR;
	}
	return readable != 0;
}

} // namespace

std::span<const ConfigParam> settings::user_page() {
	if (!flash_readable()) return {};
	return {flash_config, config_schema::kFlashSize / sizeof(ConfigParam)};
}
