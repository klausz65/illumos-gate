/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License, Version 1.0 only
 * (the "License").  You may not use this file except in compliance
 * with the License.
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
 * Copyright 2004 Sun Microsystems, Inc. All rights reserved.
 * Use is subject to license terms.
 */

#include <sys/asm_linkage.h>

	ENTRY(cas)
	casx	[%o0], %o1, %o2
	retl
	mov	%o2, %o0
	SET_SIZE(cas)

	ENTRY(flush_windows)
	save
	flushw
	restore
	retl
	nop
	SET_SIZE(flush_windows)

	/*
	 * US I has a problem with membars in the delay slot. We don't care 
	 * about performance here, so for safety's sake, we'll assume that all 
	 * the world's an US I.
	 */
	ENTRY(membar_producer)
	membar	#StoreStore
	retl
	nop
	SET_SIZE(membar_producer)

	ENTRY_NP(rdasi)
	rd	%asi, %o3
	wr	%o0, %g0, %asi
	ldxa	[%o1] %asi, %o0
	retl
	wr	%o3, %g0, %asi
	SET_SIZE(rdasi)

	ENTRY_NP(wrasi)
	rd	%asi, %o3
	wr	%o0, %g0, %asi
	stxa	%o2, [%o1] %asi
	retl
	wr	%o3, %g0, %asi
	SET_SIZE(wrasi)
