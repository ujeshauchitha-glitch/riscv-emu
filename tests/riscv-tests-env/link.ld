/* Link script for the riscv-tests suite.
 *
 * Everything sits at the start of DRAM, where the emulator begins executing.
 * .text.init must come first because it contains _start and the trap vector.
 * .tohost gets its own aligned section so the result word is easy to find. */
OUTPUT_ARCH(riscv)
ENTRY(_start)

SECTIONS
{
  . = 0x80000000;

  .text.init : { *(.text.init) }

  . = ALIGN(0x1000);
  .tohost : { *(.tohost) }

  . = ALIGN(0x1000);
  .text : { *(.text) *(.text.*) }

  . = ALIGN(0x1000);
  .data : { *(.data) *(.data.*) *(.rodata) *(.rodata.*) *(.sdata) *(.sdata.*) }

  .bss : { *(.bss) *(.bss.*) *(.sbss) *(.sbss.*) }

  _end = .;
}
