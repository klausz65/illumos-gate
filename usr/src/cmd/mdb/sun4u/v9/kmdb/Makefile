#
# CDDL HEADER START
#
# The contents of this file are subject to the terms of the
# Common Development and Distribution License (the "License").
# You may not use this file except in compliance with the License.
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
# Copyright 2009 Sun Microsystems, Inc.  All rights reserved.
# Use is subject to license terms.
#


all: $(PROG)

%.o: ../../../sparc/mdb/%.c
	$(COMPILE.c) $<
	$(CTFCONVERT_O)

kaif.fth: ../../../sparc/v9/kmdb/kaif_regs.h

ROOT_MISC_DIR		= $(ROOT)/platform/sun4u/kernel/misc
ROOT_MISC_DIR_64	= $(ROOT_MISC_DIR)/$(SUBDIR64)

ROOTMISC		= $(ROOT_MISC_DIR)/$(PROG)
ROOTMISC64		= $(ROOT_MISC_DIR_64)/$(PROG)

include ../../../../Makefile.cmd
include ../../../../Makefile.cmd.64
include ../../../sparc/Makefile.sparcv9
include ../../../sparc/Makefile.kmdb.64
include ../../../Makefile.kmdb

STANDLIBS += $(ROOT)/usr/lib/sparcv9/libstanddisasm.so

KMDB_FPTEST = \
	$(FINDFP) -x kaif_resume -x kaif_trap_common $@

#
# Uncomment the following line to enable trap accounting.  A DTLB translation
# will be added for the trap table, and each handler installed by KMDB will use
# its padding to keep a trap count.  See kaif_handlers.S.
#
#TRAPCOUNT = -DKMDB_TRAPCOUNT

CPPFLAGS += -I../../../sparc/mdb -I.. -I$(SRC)/uts/sun4 -I$(SRC)/uts/sun4u $(TRAPCOUNT)
CPPFLAGS += -I$(SRC)/cmd/mdb/sparc/v9 -I$(SRC)/cmd/mdb/sparc/v9/kmdb
CPPFLAGS += -I$(SRC)/cmd/mdb/sparc
ASFLAGS64 += -I$(SRC)/uts/sun4 -I$(SRC)/uts/sun4u $(TRAPCOUNT)
ASFLAGS64 += -I$(SRC)/cmd/mdb/sparc
ASFLAGS64 += -I$(SRC)/cmd/mdb/sparc/v9 -I$(SRC)/cmd/mdb/sparc/v9/kmdb

install: all $(ROOTMISC64) kaif.fth

kaif_off.h: ../../../sparc/kmdb/kaif_off.in
	$(OFFSETS_CREATE) <../../../sparc/kmdb/kaif_off.in >$@

kmdb_context_off.h: ../../../sparc/kmdb/kmdb_context_off.in
	$(OFFSETS_CREATE) <../../../sparc/kmdb/kmdb_context_off.in >$@

#
# Dynamic rules for object construction
#
%.o: ../../../common/kmdb/%.c
	$(COMPILE.c) -o $@ $<
	$(CTFCONVERT_O)

%.o: ../../../common/kmdb/kctl/%.c
	$(COMPILE.c) -o $@ $<
	$(CTFCONVERT_O)

%.o: ../../../common/kmdb/kctl/%.S
	$(COMPILE.s) -o $@ $<

%.o: ../../../common/mdb/%.c
	$(COMPILE.c) -o $@ $<
	$(CTFCONVERT_O)

%.o: ../../../sparc/kmdb/%.c
	$(COMPILE.c) -o $@ $<
	$(CTFCONVERT_O)

%.o: ../../../sparc/kmdb/%.S
	$(COMPILE.s) -o $@ $<

%.o: ../../../sparc/kmdb/kctl/%.c
	$(COMPILE.c) -o $@ $<
	$(CTFCONVERT_O)

%.o: ../../../sparc/kmdb/kctl/%.S
	$(COMPILE.s) -o $@ $<

%.o: ../../../sparc/mdb/%.c
	$(COMPILE.c) -o $@ $<
	$(CTFCONVERT_O)

%.o: ../../../sparc/v9/kmdb/%.c
	$(COMPILE.c) -o $@ $<
	$(CTFCONVERT_O)

%.o: ../../../sparc/v9/kmdb/%.S
	$(COMPILE64.s) -o $@ $<

%.o: ../../../sparc/v9/kmdb/kctl/%.S
	$(COMPILE.s) -o $@ $<

%.o: $(SRC)/common/net/util/%.c
	$(COMPILE.c) $<
	$(CTFCONVERT_O)

#
# Installation targets
#

$(ROOT_MISC_DIR) $(ROOT_MISC_DIR_64):
	-$(INS.dir)

$(ROOT_MISC_DIR)/%: 	% $(ROOT_MISC_DIR)
	$(INS.file)

$(ROOT_MISC_DIR_64)/%:	% $(ROOT_MISC_DIR_64)
	$(INS.file)
