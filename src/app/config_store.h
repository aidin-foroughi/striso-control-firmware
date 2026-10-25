/* The configuration records, and the settings for the C files. The schema (every key, its type
 * and default) is config_schema.hpp; the C++ interface, ConfigTable and KeyName, config_table.hpp.
 *
 * A record is 16 bytes: an 8-character key and an 8-character value, padded with spaces. The
 * user's settings are a page of records in flash at 0x08020000 (the config editor writes them as
 * CONFIG.UF2) from "MConfig "/"v1.0    " to "MCfgEnd "; a key there overrides its default. */
#ifndef CONFIG_STORE_H
#define CONFIG_STORE_H

#include <stdbool.h>

typedef struct {
  char key[8];
  char value[8];
} ConfigParam;

/* the firmware's default records (config_schema.hpp; config_store.cpp checks the count) */
#define CONFIG_DEFAULT_RECORDS 880
typedef struct {
  ConfigParam records[CONFIG_DEFAULT_RECORDS];
} ConfigDefaults;

/* the user's settings page on the board (the config sector, 128 KB) */
#define CONFIG_FLASH_ADDRESS 0x08020000
#define flash_config ((const ConfigParam*)CONFIG_FLASH_ADDRESS)

#ifdef __cplusplus
extern "C" {
#endif

extern const ConfigDefaults default_config;

/* key number n (0..60) -> button */
extern const int button_number_map[61];

/* the name of key number n (0..60), 3 characters (F_1 .. B_4), at name (not terminated) */
void put_button_name(int n, char* name);

/* The board's settings for the C files (8-character keys). false: neither the user's page nor the
 * defaults have the key, its value is blank, or not a decimal number. */
bool config_get_float(const char* key, float* value);
/* key's value without its padding is text */
bool config_text_is(const char* key, const char* text);
/* the calibration setting name ("fac", "off") of key number key (0..60): fC<name><key name> */
bool config_calibration_float(const char* name, int key, float* value);

#ifdef __cplusplus
}
#endif

#endif
