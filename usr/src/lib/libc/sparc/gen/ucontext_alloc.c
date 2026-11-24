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
 * Copyright 2023 Oxide Computer Company
 */

/*
 * This is a copy of the ucontext_alloc() function written by
 * Oxide Computer Company in 2023 out of i386/gen/makecontext.c
 * Allocate and free a SPARC user context.
 * SPARC does not require a separate XSAVE area.
 */
#include <sys/types.h>
#include <errno.h>
#include <stdlib.h>
#include <ucontext.h>

ucontext_t *
ucontext_alloc(uint32_t flags)
{
	if (flags != 0) {
		errno = EINVAL;
		return (NULL);
	}

	return (calloc(1, sizeof (ucontext_t)));
}

void
ucontext_free(ucontext_t *ucp)
{
	free(ucp);
}
