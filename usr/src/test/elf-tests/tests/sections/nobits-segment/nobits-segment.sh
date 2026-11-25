#!/usr/bin/ksh
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
# Copyright 2026 The OpenIndiana SPARC desktop contributors
#

#
# The sections of a loadable segment without file content (only NOBITS
# sections, as /usr/lib/ld/map.noexbss creates for .tbss and .bss) must
# get addresses that honour their alignment, and the segment must cover
# them.  The link editor used to derive these addresses from the
# sections' file offsets, which are not congruent with the segment's
# address: .bss became misaligned, and the COPY-relocated data placed in
# it (_iob, errno) ended up beyond the end of the segment, where the heap
# begins.
#

TESTDIR=$(dirname $0)

tmpdir=/tmp/test.$$
mkdir $tmpdir
cd $tmpdir

function cleanup {
	cd /
	rm -fr $tmpdir
}

trap 'cleanup' EXIT

function fail {
	print -u2 "$@"
	exit 1
}

for bits in 32 64; do
	# We expect any alternate linker to be in LD_ALTEXEC for us already
	gcc -m$bits -O2 -o nobits$bits ${TESTDIR}/nobits.c \
	    -Wl,-M,${TESTDIR}/mapfile.nobits ||
	    fail "compilation of ${TESTDIR}/nobits.c (-m$bits) failed"

	# Every symbol lies within its section.
	if elfdump -s nobits$bits 2>&1 | grep -q 'lies outside'; then
		elfdump -s nobits$bits 2>&1 | grep 'lies outside' >&2
		fail "nobits$bits: symbols outside of their section"
	fi

	# Every allocated section is aligned, and lies within a PT_LOAD.
	{ elfdump -l nobits$bits; elfdump -c nobits$bits; } | nawk -v obj=nobits$bits '
	    # nawk has no hex()
	    function hex(s,	i, v) {
		s = tolower(s); sub(/^0x/, "", s); v = 0
		for (i = 1; i <= length(s); i++)
			v = v * 16 + index("0123456789abcdef", substr(s, i, 1)) - 1
		return (v)
	    }
	    BEGIN { n = 0 }
	    /^Program Header/ { ph = 1; sh = 0; load = 0 }
	    /^Section Header/ { sh = 1; ph = 0; name = $NF }
	    ph && /p_vaddr:/ { vaddr = hex($2) }
	    ph && /p_type:/ && /PT_LOAD/ { load = 1 }
	    ph && load && /p_memsz:/ { lo[n] = vaddr; hi[n++] = vaddr + hex($4) }
	    sh && /sh_addr:/ { addr = hex($2) }
	    sh && /sh_size:/ { size = hex($2) }
	    sh && /sh_addralign:/ { align[name] = hex($NF);
		saddr[name] = addr; ssize[name] = size }
	    END {
		bad = 0
		for (s in saddr) {
			if (saddr[s] == 0)
				continue
			if (align[s] > 1 && saddr[s] % align[s] != 0) {
				printf("%s: %s at %x, alignment %x\n", obj, s,
				    saddr[s], align[s]); bad = 1
			}
			inside = 0
			for (i = 0; i < n; i++)
				if (saddr[s] >= lo[i] && saddr[s] + ssize[s] <= hi[i])
					inside = 1
			if (!inside) {
				printf("%s: %s not within a PT_LOAD\n", obj, s)
				bad = 1
			}
		}
		exit bad
	    }' >&2 || fail "nobits$bits: misplaced sections"

	out=$(./nobits$bits) || fail "execution of nobits$bits failed"
	[[ "$out" == "0 1" ]] || fail "nobits$bits: unexpected output: $out"
done
