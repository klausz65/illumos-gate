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
# Copyright (c) 2002-2011 Masayuki Murayama.  All rights reserved.
# Copyright 2025 Klaus Ziegler, Inc.  All rights reserved.
# Use is subject to license terms.
#
#	This makefile drives the production of the 2114x fast ethernet chipset
#	Ethernet (TU) driver module in sparc systems
#

#
#	Path to the base of the uts directory tree (usually /usr/src/uts).
#

#

UTSBASE		= ../..

#
#	Define the module and object file sets.
#
PKGVERS		= 2.6.2
MODULE		= tu
OBJECTS		= $(TU_OBJS:%=$(OBJS_DIR)/%)
ROOTMODULE	= $(ROOT_DRV_DIR)/$(MODULE)

#
#	Include common rules.
#
include $(UTSBASE)/sparc/Makefile.sparc

CPPFLAGS  +=-I${UTSBASE}/common/io/tu

DFLAGS  = -DDEBUG -DDEBUG_LEVEL=0 -DGEM_DEBUG_LEVEL=0 -USANITY \
          -UTEST_TXTIMEOUT -UTEST_CRCERR -UTEST_RESET_ON_ERR \
          -UTEST_DM9102A_E3 -UTEST_HASH_FILTERING -UTEST_RX_EMPTY \
          -UDEBUG_MULTIPORT -UDEBUG_RESUME -UDEBUG_TX_WAKEUP \
          -I ${ONUTSDIR}/include

CFGFLAGS = -DGEM_CONFIG_POLLING -DGEM_CONFIG_GLDv3 -DGEM_CONFIG_VLAN_HW \
           -DGEM_CONFIG_CKSUM_OFFLOAD -DSOLARIS10 \
           -UGEM_CONFIG_ND -DGEM_CONFIG_MAC_PROP \
           -Unotdef -UNEVER -UGEM_GCC_RUNTIME \
           -UGEM_COMPAT -USANITY -UGEM_CONFIG_FMA -UMODULE \
           -UGEM_CONFIG_RX_DIRECT -DGEM_CONFIG_TX_DIRECT \
           -UOS_PUTBACK \
           -DCENTAUR_BUG -UMAP_MEM -DCONFIG_MULTIPORT \
           -UCONFIG_POLLING
#
LDFLAGS += -N misc/mac -N drv/ip


#
#	Define targets
#
ALL_TARGET	= $(MODULE)
INSTALL_TARGET	= $(BINARY) $(ROOTMODULE)

CFLAGS		+= $(CCVERBOSE) $(DFLAGS) $(CFGFLAGS)

CERRWARN	+= -_gcc=-Wno-parentheses
CERRWARN	+= -_gcc=-Wno-uninitialized
CERRWARN	+= -_gcc=-Wno-implicit-function-declaration
CERRWARN	+= -_gcc=-Wno-switch
CERRWARN	+= -_gcc=-Wno-format

#
# Driver depends on GLD
#
LDFLAGS		+= -N misc/mac -Nmisc/mii

#
#	Default build targets.
#
.KEEP_STATE:

def:		$(DEF_DEPS)

all:		$(ALL_DEPS)

clean:		$(CLEAN_DEPS)

clobber:	$(CLOBBER_DEPS)

install:	$(INSTALL_DEPS)

#
#	Include common targets.
#
include $(UTSBASE)/sparc/Makefile.targ
