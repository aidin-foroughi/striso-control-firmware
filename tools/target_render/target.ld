/* Target renderer on QEMU mps2-an500: everything in the 4 MB SSRAM at 0 (QEMU loads the ELF). */
ENTRY(Reset_Handler)

MEMORY
{
  RAM (rwx) : ORIGIN = 0x00000000, LENGTH = 4M
}

__stack = ORIGIN(RAM) + LENGTH(RAM);

SECTIONS
{
  .text :
  {
    KEEP(*(.vectors))
    *(.text*)
    KEEP(*(.init))
    KEEP(*(.fini))
    *(.rodata*)
    . = ALIGN(4);
  } > RAM

  .ARM.extab : { *(.ARM.extab* .gnu.linkonce.armextab.*) } > RAM
  .ARM.exidx :
  {
    __exidx_start = .;
    *(.ARM.exidx* .gnu.linkonce.armexidx.*)
    __exidx_end = .;
  } > RAM

  .preinit_array :
  {
    PROVIDE_HIDDEN(__preinit_array_start = .);
    KEEP(*(.preinit_array*))
    PROVIDE_HIDDEN(__preinit_array_end = .);
  } > RAM
  .init_array :
  {
    PROVIDE_HIDDEN(__init_array_start = .);
    KEEP(*(SORT(.init_array.*)))
    KEEP(*(.init_array*))
    PROVIDE_HIDDEN(__init_array_end = .);
  } > RAM
  .fini_array :
  {
    PROVIDE_HIDDEN(__fini_array_start = .);
    KEEP(*(SORT(.fini_array.*)))
    KEEP(*(.fini_array*))
    PROVIDE_HIDDEN(__fini_array_end = .);
  } > RAM

  .data : { *(.data*) . = ALIGN(8); } > RAM

  .bss (NOLOAD) :
  {
    __bss_start__ = .;
    *(.bss*)
    *(.ram*_clear*)
    *(COMMON)
    . = ALIGN(8);
    __bss_end__ = .;
  } > RAM
  end = .;
  __end__ = .;
  PROVIDE(_end = .);
}
