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
# Copyright 2019 Joyent, Inc.
#

UTSBASE = ../..

MODULE          = overlay
OBJECTS         = $(OVERLAY_OBJS:%=$(OBJS_DIR)/%)
ROOTMODULE      = $(ROOT_DRV_DIR)/$(MODULE)

include $(UTSBASE)/sparc/Makefile.sparc

ALL_TARGET      = $(BINARY) $(SRC_CONFFILE)
INSTALL_TARGET  = $(BINARY) $(ROOTMODULE) $(ROOT_CONFFILE)
CONF_SRCDIR	= $(UTSBASE)/common/io/overlay
MAPFILE		= $(UTSBASE)/common/io/overlay/overlay.mapfile

LDFLAGS += -Nmisc/mac -Ndrv/dld -Nmisc/dls -Nmisc/ksocket

# needs work
SMATCH=off

.KEEP_STATE:

def:            $(DEF_DEPS)

all:            $(ALL_DEPS)

clean:          $(CLEAN_DEPS)

clobber:        $(CLOBBER_DEPS)

install:        $(INSTALL_DEPS)

include $(UTSBASE)/sparc/Makefile.targ
