/* noknok USB STAGE-1 linker - CH32V203, flash layout 4 (DEV-237)
 * Based on the ch32fun-generated CH32V203 linker, with these changes:
 *   - FLASH = the stage-1 region 0x0400-0x1FFF (7 KB), above the frozen 1 KB
 *     stage-0. The application stays at 0x2000 (unchanged since 1.0).
 *     0x0400 is 1 KB aligned (the vector table base, set by ch32fun into mtvec).
 *   - a 20-byte image header at a FIXED base + 0x100, right after the vector
 *     table: VERIFY_STAGE1 reads it in a STAGED image before arming stage-0.
 *   - RAM reduced by 16 B so the no-init cells at 0x200027F0 (handoff, F4
 *     diagnostic, F8 stage-0 attempts) sit ABOVE the stack.
 * Flash is addressed via the 0x00000000 alias (as ch32fun does for V20x).
 */

ENTRY( InterruptVector )
MEMORY
{
  FLASH (rx) : ORIGIN = 0x00000400, LENGTH = 7K       /* stage-1 region                */
  EXT (rx)   : ORIGIN = 0x08008000, LENGTH = 192K     /* extended flash (unused here)  */
  RAM (xrw)  : ORIGIN = 0x20000000, LENGTH = 10224    /* 10 KB - 16 B (no-init cells)  */
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
  /* Explicit AT(): "AT>FLASH" would continue the load address from the end of
   * .init and put the header at the wrong file offset (I2C stage-1 lesson). */
  .stage1_hdr ORIGIN(FLASH) + 0x100 : AT(ORIGIN(FLASH) + 0x100)
  {
   KEEP(*(.stage1_hdr))
  } >FLASH
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
ASSERT( ORIGIN(FLASH) == 0x400 && (ADDR(.init) & 0x3FF) == 0, "stage-1 vector table must sit at 0x400 (1 KB aligned)" )
ASSERT( SIZEOF(.init) <= 0x100, "vector table has grown into the stage-1 header slot" )
ASSERT( ADDR(.stage1_hdr) == 0x500 && LOADADDR(.stage1_hdr) == 0x500 && SIZEOF(.stage1_hdr) == 20,
        "stage-1 header must be 20 bytes at base + 0x100" )
