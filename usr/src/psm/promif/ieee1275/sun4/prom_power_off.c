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

/*
 * This interface allows the client to power off the machine.
 * There's no return from this service.
 */
void
prom_power_off()
{
	cell_t ci[3];

	ci[0] = p1275_ptr2cell("SUNW,power-off");	/* Service name */
	ci[1] = (cell_t) 0;				/* #argument cells */
	ci[2] = (cell_t) 0;				/* #result cells */

	promif_preprom();
	(void) p1275_cif_handler(&ci);
	promif_postprom();
}
