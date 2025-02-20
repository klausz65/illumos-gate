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
 * Copyright (c) 1994, by Sun Microsystems, Inc.
 * All rights reserved.
 */

#include <sys/promif.h>
#include <sys/promimpl.h>

static char *stdinpath;
static char buffer[OBP_MAXPATHLEN];

char *
prom_stdinpath(void)
{
	ihandle_t	istdin;

	if (stdinpath != (char *) 0)		/* Got it already? */
		return (stdinpath);

	istdin = prom_stdin_ihandle();

	if (istdin != (ihandle_t)-1)
		if (prom_ihandle_to_path(istdin, buffer,
		    OBP_MAXPATHLEN - 1) > 0)
			return (stdinpath = buffer);
	return ((char *)0);
}
