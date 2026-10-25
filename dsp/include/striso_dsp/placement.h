// Where the board puts a DSP buffer: one of its linker sections (ld/STM32H743xI_uf2.ld, e.g.
// ".ram1_clear", AHB SRAM1/2, zeroed at boot). Only builds for the board (ARM, ELF: the firmware
// and the QEMU target renders) have those sections; a host build (the SDK, the renderers, the
// virtual board) places the buffer as any other, so its linker never sees the name (Mach-O
// rejects it: its section names are "segment,section").
#ifndef STRISO_DSP_PLACEMENT_H_
#define STRISO_DSP_PLACEMENT_H_

#if defined(__arm__) && defined(__ELF__)
#define STRISO_DSP_SECTION(name) __attribute__((section(name)))
#else
#define STRISO_DSP_SECTION(name)
#endif

#endif
