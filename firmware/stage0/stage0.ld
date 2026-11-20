/* noknok USB stage-0 linker - CH32V203, layout 4 (DEV-237)
 * Stage-0 owns the first 1 KB of flash (0x0000-0x03FF) and nothing else.
 * RAM ends 16 B early: 0x200027F0 handoff, F4 stage-1 diagnostic, F8 stage-0
 * install attempts, FC free - the same reserved cells as the apps. */

ENTRY( InterruptVector )

MEMORY
{
	FLASH (rx)  : ORIGIN = 0x00000000, LENGTH = 1K
	RAM   (xrw) : ORIGIN = 0x20000000, LENGTH = 10224
}

SECTIONS
{
	.init :
	{
		. = ALIGN(4);
		KEEP(*(SORT_NONE(.init)))
		. = ALIGN(4);
	} >FLASH AT>FLASH

	.text :
	{
		. = ALIGN(4);
		*(.text)
		*(.text.*)
		*(.rodata)
		*(.rodata*)
		. = ALIGN(4);
	} >FLASH AT>FLASH

	.data :
	{
		. = ALIGN(4);
		*(.data .data.*)
		. = ALIGN(8);
		PROVIDE( __global_pointer$ = . + 0x800 );
		*(.sdata .sdata.*)
		*(.srodata .srodata.*)
		. = ALIGN(4);
	} >RAM AT>FLASH

	.bss :
	{
		. = ALIGN(4);
		*(.sbss*)
		*(.bss*)
		*(COMMON*)
		. = ALIGN(4);
	} >RAM

	PROVIDE( _eusrstack = ORIGIN(RAM) + LENGTH(RAM) );   /* 0x200027F0 */

	/DISCARD/ : {
		*(.note .note.*)
		*(.eh_frame .eh_frame.*)
		*(.comment .comment.*)
	}
}

ASSERT( SIZEOF(.data) == 0, "stage-0 must have no .data - start.S does not copy it" )
ASSERT( SIZEOF(.bss)  == 0, "stage-0 must have no .bss - start.S does not zero it" )
ASSERT( ADDR(.init) == 0x00000000, "stage-0 .init must start at 0x00000000" )
