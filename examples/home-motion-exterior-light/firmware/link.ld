MEMORY {
  FLASH (rx) : ORIGIN = 0, LENGTH = 64K
  RAM (rwx) : ORIGIN = 0x20000000, LENGTH = 12K
}
ENTRY(Reset_Handler)
SECTIONS {
  .vectors : { KEEP(*(.vectors)) } > FLASH
  .text : { *(.text*) *(.rodata*) . = ALIGN(4); } > FLASH
  .data : { _sdata = .; *(.data*) . = ALIGN(4); _edata = .; } > RAM AT> FLASH
  _sidata = LOADADDR(.data);
  .bss (NOLOAD) : { _sbss = .; *(.bss*) *(COMMON) . = ALIGN(4); _ebss = .; } > RAM
  _stack_top = ORIGIN(RAM) + LENGTH(RAM);
  ASSERT(_ebss + 1024 <= _stack_top, "Insufficient stack space")
}
