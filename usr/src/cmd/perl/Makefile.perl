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
# Copyright 2015, OmniTI Computer Consulting, Inc. All rights reserved.
# Copyright 2016 RackTop Systems.
#

include $(SRC)/lib/Makefile.lib
$(BUILDPERL64)include $(SRC)/lib/Makefile.lib.64

PERLINCDIR_cmd = $(PERL) -MConfig -e 'print $$Config{archlib} . "/CORE"'
PERLINCDIR = $(PERLINCDIR_cmd:sh)
PERLPRIVLIBDIR_cmd = $(PERL) -MConfig -e 'print $$Config{installprivlib}'
PERLPRIVLIBDIR = $(PERLPRIVLIBDIR_cmd:sh)

PERLMOD = $(MODULE).pm
PERLEXT = $(MODULE).so
PERLXS = $(MODULE).xs

ROOTPERLDIR = $(ROOT)/usr/perl5/$(PERL_VERSION)
ROOTPERLLIBDIR = $(ROOTPERLDIR)/lib/$(PERL_ARCH)
ROOTPERLMODDIR = $(ROOTPERLLIBDIR)/Sun/Solaris
ROOTPERLEXTDIR = $(ROOTPERLLIBDIR)/auto/Sun/Solaris/$(MODULE)

ROOTPERLMOD = $(ROOTPERLMODDIR)/$(MODULE).pm
ROOTPERLEXT = $(ROOTPERLEXTDIR)/$(MODULE).so

XSUBPP = $(PERL) $(PERLPRIVLIBDIR)/ExtUtils/xsubpp \
	-typemap $(PERLPRIVLIBDIR)/ExtUtils/typemap

# CFLAGS for perl, specifically.
PCFLAGS = -DPERL_EUPXS_ALWAYS_EXPORT -DPERL_USE_SAFE_PUTENV -D_TS_ERRNO
$(BUILDPERL32)PCFLAGS += -D_LARGEFILE_SOURCE -D_FILE_OFFSET_BITS=64
$(BUILDPERL64)PCFLAGS += -D_LARGEFILE64_SOURCE

CSTD = $(CSTD_GNU99)
ZGUIDANCE =
SONAME = $(PERLEXT)

CLEANFILES += $(PERLEXT) $(MODULE).o $(MODULE).c
