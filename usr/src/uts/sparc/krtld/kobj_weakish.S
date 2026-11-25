/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

/*
 * Copyright 2007 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright 2026 Klaus Ziegler
 * created using ChatGPT plus.
 */

/*
 * SPARC V9 implementation of get_weakish_int().
 *
 * During standalone krtld execution, symbols from genunix
 * may not yet be relocated. Do not dereference such symbols
 * until standalone has been cleared.
 */

	.section ".text"
	.align	4
	.global	get_weakish_int
	.type	get_weakish_int, #function

get_weakish_int:
	sethi	%hi(standalone), %g1
	ld	[%g1 + %lo(standalone)], %g1

	/* standalone != 0: return 0 */
	brnz,pn	%g1, .Lweakish_zero
	nop

/* ip == NULL: return 0 */
	brz,pn	%o0, .Lweakish_zero
	nop

	/* standalone == 0 && ip != NULL */
	retl
	ldsw	[%o0], %o0

.Lweakish_zero:
	retl
	clr	%o0

	.size	get_weakish_int, . - get_weakish_int
