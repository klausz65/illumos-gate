/*
 * SPARC GCC integer bit-operation runtime helpers.
 *
 * SI = 32 bits, DI = 64 bits, independent of
 * the ILP32/LP64 data model.
 */

static int
clz32(unsigned int x)
{
	int n = 0;

	if (x == 0)
		return (32);

	if (x <= 0xffffU) {
		n += 16;
		x <<= 16;
	}
	if (x <= 0xffffffU) {
		n += 8;
		x <<= 8;
	}
	if (x <= 0xfffffffU) {
		n += 4;
		x <<= 4;
	}
	if (x <= 0x3fffffffU) {
		n += 2;
		x <<= 2;
	}
	if (x <= 0x7fffffffU)
		n++;

	return (n);
}

int
__clzsi2(unsigned int x)
{
	return (clz32(x));
}

int
__clzdi2(unsigned long long x)
{
	unsigned int hi = (unsigned int)(x >> 32);

	if (hi != 0)
		return (clz32(hi));

	return (32 + clz32((unsigned int)x));
}

static int
ctz32(unsigned int x)
{
	int n = 0;

	if (x == 0)
		return (32);

	if ((x & 0xffffU) == 0) {
		n += 16;
		x >>= 16;
	}
	if ((x & 0xffU) == 0) {
		n += 8;
		x >>= 8;
	}
	if ((x & 0xfU) == 0) {
		n += 4;
		x >>= 4;
	}
	if ((x & 3U) == 0) {
		n += 2;
		x >>= 2;
	}
	if ((x & 1U) == 0)
		n++;

	return (n);
}

int
__ctzsi2(unsigned int x)
{
	return (ctz32(x));
}

int
__ctzdi2(unsigned long long x)
{
	unsigned int lo = (unsigned int)x;

	if (lo != 0)
		return (ctz32(lo));

	return (32 + ctz32((unsigned int)(x >> 32)));
}

int
__popcountsi2(unsigned int x)
{
	x -= (x >> 1) & 0x55555555U;
	x = (x & 0x33333333U) +
	    ((x >> 2) & 0x33333333U);
	x = (x + (x >> 4)) & 0x0f0f0f0fU;
	x += x >> 8;
	x += x >> 16;

	return ((int)(x & 0x3fU));
}

int
__popcountdi2(unsigned long long x)
{
	return (__popcountsi2((unsigned int)x) +
	    __popcountsi2((unsigned int)(x >> 32)));
}
