/* noknok USB bootloader linker - CH32V203
 * Based on the ch32fun-generated CH32V203 linker, with two changes:
 *   - FLASH limited to the low 8 KB (the bootloader region); the application
 *     lives above it at 0x2000 and is flashed over USB.
 *   - RAM reduced by 16 B so the no-init handoff cell at 0x200027F0 sits ABOVE
 *     the stack and is never clobbered. The same address is valid in the app.
 * Flash is addressed via the 0x00000000 alias (as ch32fun does for V20x).
 */

ENTRY( InterruptVector )
MEMORY
{
  FLASH (rx) : ORIGIN = 0x00000000, LENGTH = 8K       /* 8 KB bootloader region        */
  EXT (rx)   : ORIGIN = 0x08008000, LENGTH = 192K     /* extended flash (unused here)  */
  RAM (xrw)  : ORIGIN = 0x20000000, LENGTH = 10224    /* 10 KB - 16 B (handoff cell)   */
}
SECTIONS
{
  .init :
  {
   _sinit = .;
   . = ALIGN(4);
   KEEP(*(SORT_NONE(.init)))
   . = ALIGN(4);
   _einit = .;
  } >FLASH AT>FLASH
  .text :
  {
   . = ALIGN(4);
   *(.text)
   *(.text.*)
   *(.rodata)
   *(.rodata*)
   *(.gnu.linkonce.t.*)
   . = ALIGN(4);
  } >FLASH AT>FLASH
  .fini :
  {
   KEEP(*(SORT_NONE(.fini)))
   . = ALIGN(4);
  } >FLASH AT>FLASH
  PROVIDE( _etext = . );
  PROVIDE( _eitcm = . );
  .preinit_array :
  {
   PROVIDE_HIDDEN (__preinit_array_start = .);
   KEEP (*(.preinit_array))
   PROVIDE_HIDDEN (__preinit_array_end = .);
  } >FLASH AT>FLASH
  .init_array :
  {
   PROVIDE_HIDDEN (__init_array_start = .);
   KEEP (*(SORT_BY_INIT_PRIORITY(.init_array.*) SORT_BY_INIT_PRIORITY(.ctors.*)))
   KEEP (*(.init_array EXCLUDE_FILE (*crtbegin.o *crtbegin?.o *crtend.o *crtend?.o ) .ctors))
   PROVIDE_HIDDEN (__init_array_end = .);
  } >FLASH AT>FLASH
  .fini_array :
  {
   PROVIDE_HIDDEN (__fini_array_start = .);
   KEEP (*(SORT_BY_INIT_PRIORITY(.fini_array.*) SORT_BY_INIT_PRIORITY(.dtors.*)))
   KEEP (*(.fini_array EXCLUDE_FILE (*crtbegin.o *crtbegin?.o *crtend.o *crtend?.o ) .dtors))
   PROVIDE_HIDDEN (__fini_array_end = .);
  } >FLASH AT>FLASH
  .ctors :
  {
   KEEP (*crtbegin.o(.ctors))
   KEEP (*crtbegin?.o(.ctors))
   KEEP (*(EXCLUDE_FILE (*crtend.o *crtend?.o ) .ctors))
   KEEP (*(SORT(.ctors.*)))
   KEEP (*(.ctors))
  } >FLASH AT>FLASH
  .dtors :
  {
   KEEP (*crtbegin.o(.dtors))
   KEEP (*crtbegin?.o(.dtors))
   KEEP (*(EXCLUDE_FILE (*crtend.o *crtend?.o ) .dtors))
   KEEP (*(SORT(.dtors.*)))
   KEEP (*(.dtors))
  } >FLASH AT>FLASH
  .dalign :
  {
   . = ALIGN(4);
   PROVIDE(_data_vma = .);
  } >RAM AT>FLASH
  .dlalign :
  {
   . = ALIGN(4);
   PROVIDE(_data_lma = .);
  } >FLASH AT>FLASH
  .data :
  {
   . = ALIGN(4);
   __global_pointer$ = . + 0x3fc;
   *(.gnu.linkonce.r.*)
   *(.data .data.*)
   *(.gnu.linkonce.d.*)
   . = ALIGN(8);
   *(.sdata .sdata.*)
   *(.sdata2*)
   *(.gnu.linkonce.s.*)
   . = ALIGN(8);
   *(.srodata.cst16)
   *(.srodata.cst8)
   *(.srodata.cst4)
   *(.srodata.cst2)
   *(.srodata .srodata.*)
   . = ALIGN(4);
   PROVIDE( _edata = .);
  } >RAM AT>FLASH
  .bss :
  {
   . = ALIGN(4);
   PROVIDE( _sbss = .);
   *(.sbss*)
   *(.gnu.linkonce.sb.*)
   *(.bss*)
   *(.gnu.linkonce.b.*)
   *(COMMON*)
   . = ALIGN(4);
   PROVIDE( _ebss = .);
  } >RAM
  PROVIDE( _end = _ebss);
  PROVIDE( end = . );
  PROVIDE( _eusrstack = ORIGIN(RAM) + LENGTH(RAM));   /* = 0x200027F0 (handoff cell just above) */
  /DISCARD/ : {
   *(.note .note.*)
   *(.eh_frame .eh_frame.*)
   *(.comment .comment.*)
   *(.ARM.extab* .gnu.linkonce.armextab.*)
   *(.ARM.exidx*)
  }
}
