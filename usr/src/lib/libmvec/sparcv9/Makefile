#
# This file and its contents are supplied under the terms of the
# Common Development and Distribution License ("CDDL"), version 1.0.
# You may only use this file in accordance with the terms of version
# 1.0 of the CDDL.
#
# A full copy of the text of the CDDL should have accompanied this
# source.  A copy of the CDDL is also available via the Internet at
# http://www.illumos.org/license/CDDL.
#

#
# Copyright 2011 Nexenta Systems, Inc.  All rights reserved.
# Copyright 2026 Klaus Ziegler  All rights reserved.
#

TARGET_ARCH	= sparcv9

LIBRARY		= libmvec.a
VERS		= .1

OBJECTS		= $(mvecOBJS)
pics/__vsin.o := sparcv9_COPTFLAG = -xO1
pics/__vsincos.o := sparcv9_COPTFLAG = -xO1

include ../Makefile.com
include	$(SRC)/lib/Makefile.lib.64

CHIP		= ultra

SRCS		= $(SRCS_mvec)

install: all $(ROOTLIBS64) $(ROOTLINKS64)  $(ROOTCOMPATLINKS64)

include $(SRC)/lib/libm/Makefile.targ
