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
 * Copyright 2026 Hans Rosenfeld
 */

/*
 * Dummy pci_prd module for sun4u/sun4v
 */

#include <sys/modctl.h>
#include <sys/plat/pci_prd.h>

pci_prd_compat_flags_t
pci_prd_compat_flags(void)
{
	return (PCI_PRD_COMPAT_NONE);
}

static struct modlmisc pci_prd_modlmisc = {
	.misc_modops = &mod_miscops,
	.misc_linkinfo = "sun4 PCI Resource Discovery"
};

static struct modlinkage pci_prd_modlinkage = {
	.ml_rev = MODREV_1,
	.ml_linkage = { &pci_prd_modlmisc, NULL }
};

int
_init(void)
{
	return (mod_install(&pci_prd_modlinkage));
}

int
_info(struct modinfo *modinfop)
{
	return (mod_info(&pci_prd_modlinkage, modinfop));
}

int
_fini(void)
{
	return (mod_remove(&pci_prd_modlinkage));
}
