#
# CDDL HEADER START
#
# The contents of this file are subject to the terms of the
# Common Development and Distribution License, Version 1.0 only
# (the "License").  You may not use this file except in compliance
# with the License.
#
# You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
# or http://www.opensolaris.org/os/licensing.
# See the License for the specific language governing permissions
# and limitations under the License.
#
# When distributing Covered Code, include this CDDL HEADER in each
# file and include the License file at usr/src/OPENSOLARIS.LICENSE.
# If applicable, add the following below this CDDL HEADER, with the
# fields enclosed by brackets "[]" replaced with your own identifying
# information: Portions Copyright [yyyy] [name of copyright owner]
#
# CDDL HEADER END
#
#
# Copyright 2004 Sun Microsystems, Inc.  All rights reserved.
# Use is subject to license terms.
#

#
# In the mdb world, mdb_ks is delivered as a dmod, albeit one with special
# powers.  Various other dmods use symbols provided by mdb_ks, through the
# magic of dynamic linking.  In the kmdb world, however, things are different.
# kmdb dmods are not allowed to consume symbols exported by anything other
# than the kmdb module.  In order to allow other kmdb dmods to use the mdb_ks
# symbols, we link mdb_ks into kmdb.
#

MODULE = mdb_ks.so
MDBTGT = kvm

MODSRCS = mdb_ks.c

include ../../../../Makefile.cmd
include ../../../../Makefile.cmd.64
include ../../Makefile.sparcv9
include ../../../Makefile.module
include ../../../Makefile.mdb_ks

#
# We are not actually hardwiring some dependency on sun4u, we just need to
# include sys/param.h with _MACHDEP defined, and this forces the inclusion of
# machparam.h, even though we don't use anything there.  This is a temporary
# kludge until we invent -DDONTINCLUDEMACHPARAM or something.
#
CPPFLAGS += -I$(SRC)/uts/sun4u
