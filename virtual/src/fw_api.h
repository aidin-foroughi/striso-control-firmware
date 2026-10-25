/*
 * The interface between the library's front (api.cpp: boards, loading, real time, the bootloader
 * drive) and one firmware instance (instance.cpp: the firmware on the hal stand-in). Every board
 * loads its own copy of the library and calls that copy's svfw_api(): the firmware's globals are
 * the copy's, fresh after each load. What must survive a reset lives in sv_shared, owned by the
 * front.
 */
#ifndef SV_FW_API_H
#define SV_FW_API_H

#include <stdint.h>

#include "striso_virtual.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SV_FW_API_VERSION 2
#define SV_DEVSPEC_SIZE 1024 /* 0x081e0000: devspec_id, calib_dis_force, _breakpoint, _force2 */
#define SV_BOOTLOADER_SIGNATURE 0x71a21877u /* console.cpp kBootloaderRtcSignature */

typedef struct sv_shared {
	uint8_t flash[SV_FLASH_SIZE];      /* 0x08020000: the user config (flash_config) */
	uint8_t devspec[SV_DEVSPEC_SIZE];  /* 0x081e0000: device id and calibration */
	uint32_t uid[3];                   /* UID_BASE */
	uint32_t rtc_bkp[32];              /* RTC->BKPxR: survive a reset */
	int32_t port_key_held;             /* LINE_BUTTON_PORT reads 0 at boot */
} sv_shared;

typedef struct sv_fw_api {
	uint32_t version;
	/* boots the firmware (power on); 0 ok */
	int32_t (*boot)(sv_shared* shared);
	/* stops every firmware thread (before the copy is unloaded) */
	void (*shutdown)(void);
	/* runs `frames` of virtual time, writing the taps (each may be NULL); returns the frames run:
	 * fewer when the firmware reset itself (reset_requested) */
	int32_t (*advance)(int32_t frames, float* usb, float* engine, float* fx);
	uint64_t (*now)(void);
	int32_t (*reset_requested)(void);
	int32_t (*usb_attached)(void);
	int32_t (*console_write)(const uint8_t* bytes, int32_t n);
	int32_t (*console_read)(uint8_t* out, int32_t max);
	int32_t (*midi_write)(const uint8_t* bytes, int32_t n);
	int32_t (*midi_read)(uint8_t* out, int32_t max);
	int32_t (*ump_write)(const uint32_t* words, int32_t n);
	int32_t (*inject_key)(int32_t src, int32_t id, int32_t pres, int32_t velo, int32_t x, int32_t y);
	int32_t (*inject_control)(int32_t id, int32_t value);
	int32_t (*inject_motion)(const int16_t raw[6]);
	int32_t (*set_engine_controls)(int32_t v, const float* values);
	int32_t (*select_voice)(int32_t n);
	int32_t (*get_leds)(sv_leds* out);
	int32_t (*get_state)(sv_state* out);
	int32_t (*test)(int32_t what, int32_t arg);
	const char* (*firmware_version)(void);
} sv_fw_api;

SV_API const sv_fw_api* svfw_api(void);

#ifdef __cplusplus
}
#endif

#endif
