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
 * Copyright (c) 2000 by Sun Microsystems, Inc.
 * All rights reserved.
 */

#ifndef	_LM75_H
#define	_LM75_H

#ifdef	__cplusplus
extern "C" {
#endif

#define	LM75_IOCTL		('L' << 8)

#define	LM75_GET_HYST			(LM75_IOCTL | 0)  /* (int16_t *) */
#define	LM75_SET_HYST			(LM75_IOCTL | 1)  /* (int16_t *) */
#define	LM75_GET_OVERTEMP_SHUTDOWN	(LM75_IOCTL | 2)  /* (int16_t *) */
#define	LM75_SET_OVERTEMP_SHUTDOWN	(LM75_IOCTL | 3)  /* (int16_t *) */
#define	LM75_GET_CONFIG			(LM75_IOCTL | 4)  /* (uint8_t *) */
#define	LM75_SET_CONFIG			(LM75_IOCTL | 5)  /* (uint8_t *) */

#ifdef	__cplusplus
}
#endif

#endif	/* _LM75_H */
