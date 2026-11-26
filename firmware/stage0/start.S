/* SPDX-License-Identifier: MIT
 * Copyright (c) noknok
 *
 * noknok USB stage-0 - minimal startup (CH32V203, rv32imac). DEV-237.
 * Same idea as the I2C stage-0: no ch32fun.c (its vector table and reset
 * handler are code stage-0 never uses), no .data/.bss (stage0.ld asserts it).
 * Point gp and sp somewhere valid, call main(). The CH32V203 starts executing
 * at 0x0000_0000 on every reset.
 */

	.section .init, "ax"
	.global InterruptVector
	.align 2
InterruptVector:
	j       handle_reset

	.section .text.handle_reset, "ax"
	.global handle_reset
	.align 2
handle_reset:
.option push
.option norelax
	la      gp, __global_pointer$
.option pop
	la      sp, _eusrstack        /* = 0x200027F0, below the reserved 16 B */
	jal     ra, main
1:	j       1b
