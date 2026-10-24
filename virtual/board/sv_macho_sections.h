/* macOS: Mach-O section names are "segment,section"; the firmware's DTCM/SRAM placements
 * (__attribute__((section(".ram5_clear"))) and the like) go to one data section instead.
 * Force-included on Apple targets only. */
#ifndef SV_MACHO_SECTIONS_H
#define SV_MACHO_SECTIONS_H
#define section(name) __section__("__DATA,__striso_ram")
#endif
