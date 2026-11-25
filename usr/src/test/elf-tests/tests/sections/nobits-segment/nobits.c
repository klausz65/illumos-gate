/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 */

/*
 * Copyright 2026 The OpenIndiana SPARC desktop contributors
 */

/*
 * A thread-local variable (.tbss) ahead of .bss in a NOBITS-only segment,
 * a .bss object with a large alignment, and data from libc that the link
 * editor copy-relocates into .bss (errno, the stdio _iob array).
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>

__thread char tl[3] __attribute__((aligned(128)));
char page[100] __attribute__((aligned(4096)));

int
main(void)
{
	tl[1] = 1;
	if (((uintptr_t)page & 4095) != 0) {
		(void) fprintf(stderr, "page is misaligned: %p\n", page);
		return (1);
	}
	(void) printf("%d %d\n", errno, tl[1] + page[5]);
	return (0);
}
