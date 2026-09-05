/* Minimal linker script: place everything at the start of DRAM, where the
   emulator loads a flat binary image and sets the PC. */
OUTPUT_ARCH(riscv)
ENTRY(_start)
SECTIONS {
  . = 0x80000000;
  .text : { *(.text*) }
  .data : { *(.data*) }
  .bss  : { *(.bss*) }
}
