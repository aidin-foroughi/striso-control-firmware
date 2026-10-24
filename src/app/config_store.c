#include <string.h>

#include "ch.h"
#if defined(__arm__) // the board (the host tests have no HAL)
#include "hal.h"
#endif
#include "config.h"

/* Button names */
// key number = 17 * buttons[but].coord0 + 10 * buttons[but].coord1 + 30
const int button_number_map[61] = {
  56, 51, 63, 58, 53, 65, 60, 55, 67, 62, 40, 35, 47, 42, 37, 49, 44,
  39, 34, 46, 41, 36, 48, 43, 38, 50, 45, 23, 18, 30, 25, 20, 32, 27,
  22, 17, 29, 24, 19, 31, 26, 21, 33, 28,  6,  1, 13,  8,  3, 15, 10,
   5,  0, 12,  7,  2, 14,  9,  4, 16, 11};

// const char note_names[2][17] = {"C_","Db","C#","D_","Eb","D#","E_",
//     "F_","Gb","F#","G_","Ab","G#","A_","Bb","A#","B_"};
const char note_names[] = "C_DbC#D_EbD#E_F_GbF#G_AbG#A_BbA#B_";

void put_button_name(int n, char* key) {
  int note = (n + 7) % 17;
  int oct = (n + 7) / 17;
  key[0] = note_names[note*2];
  key[1] = note_names[note*2+1];
  key[2] = '1' + oct;
}

void strset(char* key, unsigned int start, const char* str) {
  for (int n = start; n < 8 && str[n-start]; n++) {
    key[n] = str[n-start];
  }
}

// The rest of an 8-character value field after a number: spaces (or NULs) only.
static bool field_rest_blank(const char* str, int i) {
  for (; i < 8; i++) {
    if (str[i] != ' ' && str[i] != '\0') return false;
  }
  return true;
}

// A decimal number with an optional sign and fraction ("-1.25", ".5", "3."), then spaces;
// anything else ("-", ".", "1e5", "1-2") is CONFIG_UNDEFINED, as an empty field is.
float atof8(const char* str) {
  bool neg = false;
  int i = 0, digits = 0;

  if (str[i] == '-') {
    neg = true;
    ++i;
  } else if (str[i] == '+') {
    ++i;
  }

  float value = 0;
  while (i < 8 && str[i] >= '0' && str[i] <= '9') {
    value *= 10;
    value += str[i] - '0';
    ++i;
    ++digits;
  }

  if (i < 8 && str[i] == '.') {
    ++i;
    float weight = 0.1;
    while (i < 8 && str[i] >= '0' && str[i] <= '9') {
      value += weight * (str[i] - '0');
      weight *= 0.1;
      ++i;
      ++digits;
    }
  }

  if (digits == 0 || !field_rest_blank(str, i)) {
    return CONFIG_UNDEFINED;
  }
  return neg ? -value : value;
}

// A decimal integer with an optional sign, then spaces; anything else is CONFIG_UNDEFINED.
int atoi8(const char* str) {
  bool neg = false;
  int i = 0, digits = 0;

  if (str[i] == '-') {
    neg = true;
    ++i;
  } else if (str[i] == '+') {
    ++i;
  }

  int value = 0;
  while (i < 8 && str[i] >= '0' && str[i] <= '9') {
    value *= 10;
    value += str[i] - '0';
    ++i;
    ++digits;
  }

  if (digits == 0 || !field_rest_blank(str, i)) {
    return CONFIG_UNDEFINED;
  }
  return neg ? -value : value;
}

unsigned int atox8(const char* str) {
  int i = 0;

  uint32_t value = 0; // unsigned: 8 hex digits >= 0x80000000 overflowed an int
  if (str[i] == '#') {
    i++;
  } else if (str[i] == '0' && str[i+1] == 'x') {
    i += 2;
  }
  while (i < 8) {
    if (str[i] >= '0' && str[i] <= '9') {
      value *= 16;
      value += str[i] - '0';
      ++i;
    }
    else if (str[i] >= 'A' && str[i] <= 'F') {
      value *= 16;
      value += str[i] - 'A' + 10;
      ++i;
    }
    else if (str[i] >= 'a' && str[i] <= 'f') {
      value *= 16;
      value += str[i] - 'a' + 10;
      ++i;
    }
    else {
      break;
    }
  }

  return value;
}

// the 8 characters of two keys or values (B12: no longer read through a uint64_t pointer, which
// was undefined behaviour and needed 8-byte aligned keys)
bool cmp8(const char* a, const char* b) {
  return memcmp(a, b, 8) == 0;
}

// The config sector (0x08020000, 128 KB, ld/STM32H743xI_uf2.ld): the scan never leaves it.
#define CONFIG_SECTOR_RECORDS (128 * 1024 / sizeof (ConfigParam))

#if defined(STM32H7XX)
// A power loss while the bootloader programs the config sector can leave a flash word with an
// uncorrectable ECC error; reading it is a bus fault. Read the sector once with data bus faults
// ignored (FAULTMASK + BFHFNMIGN: a faulting load returns garbage instead of faulting) and check
// bank 1's double error flag: if it is set, the flash config is not used (the defaults are).
static bool config_flash_readable(void) {
  static int readable = -1;
  if (readable < 0) {
    const volatile uint32_t* w = (const volatile uint32_t*)flash_config;
    FLASH->CCR1 = FLASH_CCR_CLR_DBECCERR | FLASH_CCR_CLR_SNECCERR;
    __set_FAULTMASK(1);
    SCB->CCR |= SCB_CCR_BFHFNMIGN_Msk;
    __DSB();
    __ISB();
    // one word of every 32-byte flash word (ECC is per 256-bit flash word)
    for (unsigned i = 0; i < CONFIG_SECTOR_RECORDS * sizeof (ConfigParam) / 4; i += 8) {
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
  return readable;
}
#else
static bool config_flash_readable(void) { return true; }
#endif

const char* getConfigSetting(const char* name) {
  // the user's settings: only a readable sector that starts with the header of this format, and
  // never past the sector (a page without the end marker ran into the firmware's .fwinfo)
  const ConfigParam* cfg = flash_config;
  if (config_flash_readable() && cmp8(cfg[0].key, "MConfig ") && cmp8(cfg[0].value, "v1.0    ")) {
    for (unsigned n = 0; n < CONFIG_SECTOR_RECORDS && !cmp8(cfg->key, "MCfgEnd ") &&
                         cfg->key[0] >= 32 && cfg->key[0] < 127; n++, cfg++) {
      if (cmp8(cfg->key, name))
        return cfg->value;
    }
  }

  cfg = default_config;
  while (!cmp8(cfg->key, "MCfgEnd ") && cfg->key[0] >= 32 && cfg->key[0] < 127) {
    if (cmp8(cfg->key, name))
      return cfg->value;
    cfg++;
  }

  return "(ERROR!)";
}

float getConfigFloat(const char* name) {
  return atof8(getConfigSetting(name));
}

int getConfigInt(const char* name) {
  return atoi8(getConfigSetting(name));
}

unsigned int getConfigHex(const char* name) {
  return atox8(getConfigSetting(name));
}
