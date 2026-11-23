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
 * Copyright 2010 Sun Microsystems, Inc. All rights reserved.
 * Use is subject to license terms.
 */

/*
 * Utility Assembly routines used by the debugger.
 */

#include <sys/asm_linkage.h>
#include <sys/privregs.h>
#include "mach_asmutil.h"

	/* GNU assembler */
	.register	%g3, #scratch

	ENTRY(get_nwin)
	GET_NWIN(%g4, %g3);	/* %g4 is scratch, %g3 set to nwin-1 */
	mov	%g3, %o0
	retl
	add	%o0, 1, %o0
	SET_SIZE(get_nwin)

	ENTRY(get_fp)
	retl
	mov	%fp, %o0
	SET_SIZE(get_fp)

	ENTRY(interrupts_on)
	rdpr	%pstate, %o0
	bset	PSTATE_IE, %o0
	retl
	wrpr	%o0, %pstate
	SET_SIZE(interrupts_on)

	ENTRY(interrupts_off)
	rdpr	%pstate, %o0
	bclr	PSTATE_IE, %o0
	retl
	wrpr	%o0, %pstate
	SET_SIZE(interrupts_off)

	ENTRY(get_tba)
	retl
	rdpr	%tba, %o0
	SET_SIZE(get_tba)

	ENTRY(set_tba)
	retl
	wrpr	%o0, %tba
	SET_SIZE(set_tba)
