OUTPUT_ARCH( "riscv" )
ENTRY( _start )

MEMORY
{
  ram   (wxa!ri) : ORIGIN = 0x80000000, LENGTH = 128M
}

PHDRS
{
  text PT_LOAD;
  data PT_LOAD;
  bss PT_LOAD;
}

SECTIONS
{
  .text : {
    PROVIDE(_text_start = .);
    *(.text.init) *(.text .text.*)
    PROVIDE(_text_end = .);
  } >ram AT>ram :text

  .rodata : {
    PROVIDE(_rodata_start = .);
    *(.rodata .rodata.*)
    PROVIDE(_rodata_end = .);
  } >ram AT>ram :text

  .data : {
    . = ALIGN(4096);
    PROVIDE(_data_start = .);
    *(.sdata .sdata.*) *(.data .data.*)
    PROVIDE(_data_end = .);
  } >ram AT>ram :data

  .bss : {
    . = ALIGN(4096);   /* CRITICAL: Ensure BSS starts at Page boundary */
    PROVIDE(_bss_start = .);
    *(.sbss .sbss.*) *(.bss .bss.*)
    . = ALIGN(4);      /* Ensure BSS end is 4-byte aligned for start.s loop */
    PROVIDE(_bss_end = .);
  } >ram AT>ram :bss

  . = ALIGN(4096);
  .apppt : {
    PROVIDE(_apppt_start = .);
    *(.apppt)
    PROVIDE(_apppt_end = .);
  } >ram AT>ram :bss

  PROVIDE(_memory_start = ORIGIN(ram));
  PROVIDE(_memory_end = ORIGIN(ram) + LENGTH(ram));
  PROVIDE(_app_size = 16777216);
  PROVIDE(_app_start = ALIGN(_memory_end - _app_size, 4096));
  PROVIDE(_app_end = _memory_end);
  PROVIDE(_heap_start = ALIGN(_bss_end, 4096));
  PROVIDE(_heap_size = _app_start - _heap_start);
}
