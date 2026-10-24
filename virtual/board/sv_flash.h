/* The board's fixed flash addresses (config.h, config_store.h) moved to the virtual board's flash:
 * the front's sv_shared (fw_api.h), which a reset leaves alone. Included after config.h by the
 * files that compile a firmware file with its flash reads (fw_*.c) and by the board glue. */
#ifndef SV_FLASH_H
#define SV_FLASH_H

#include "config.h"
#include "config_store.h"
#include "fw_api.h"
#include "sv_host.h"

#undef flash_config
#define flash_config ((ConfigParam*)(svh_shared->flash))
#undef devspec_id
#define devspec_id ((devspec_id_t*)(svh_shared->devspec))
#undef calib_dis_force
#define calib_dis_force ((calib_t*)(svh_shared->devspec + 256))
#undef calib_dis_breakpoint
#define calib_dis_breakpoint ((calib_t*)(svh_shared->devspec + 2 * 256))
#undef calib_dis_force2
#define calib_dis_force2 ((calib_t*)(svh_shared->devspec + 3 * 256))

#endif
