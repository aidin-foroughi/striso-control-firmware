/* src/app/config_store.c as the board builds it, reading the virtual board's config flash. */
#include "ch.h"
#include "sv_flash.h"

#include "config_store.c"

/* config_store.c defines cmp8 'inline'; GCC emits it (config_store.h declares it without), clang
 * for Windows only where it is used: taking its address emits it there too */
bool (*const svh_cmp8_emit)(const char* a, const char* b) = cmp8;
