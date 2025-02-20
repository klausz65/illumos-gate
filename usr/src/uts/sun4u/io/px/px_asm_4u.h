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
 * Copyright 2005 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#ifndef	_SYS_PX_ASM_4U_H
#define	_SYS_PX_ASM_4U_H

#ifdef	__cplusplus
extern "C" {
#endif

extern int px_phys_peek_4u(size_t size, uint64_t paddr, uint64_t *value,
    int type);
extern int px_phys_poke_4u(size_t size, uint64_t paddr, uint64_t *value,
    int type);

#ifdef	__cplusplus
}
#endif

#endif	/* _SYS_PX_ASM_4U_H */
