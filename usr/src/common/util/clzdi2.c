//===-- clzdi2.c - Implement __clzdi2 -------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements __clzdi2 for the compiler_rt library.
//
//===----------------------------------------------------------------------===//

#include <sys/isa_defs.h>
#include <limits.h>

typedef union {
	long long all;
	struct {
		int high;
		unsigned low;
	} s;
} dwords;

int
__clzdi2(int val) {
  dwords x;
  x.all = val;
  const int f = -(x.s.high == 0);
  return __builtin_clz((x.s.high & ~f) | (x.s.low & f)) +
         (f & ((int)(sizeof(int) * CHAR_BIT)));
}
