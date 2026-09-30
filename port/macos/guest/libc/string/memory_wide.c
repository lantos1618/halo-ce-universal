/*
MEMORY_WIDE.C

memcpy, memmove, memset and memcmp for the native macOS guest (arm64_32),
in place of musl's portable C (tools/macos_build.py leaves those out).

musl's are written for any machine: memcmp compares a byte at a time, and
memcpy, memmove and memset move 4 bytes at a time (a size_t, on ILP32). On
the rebased guest every memory access costs an extra instruction
(tools/macos_arm64_rebase.py: add x27, x28, wN, uxtw), so a byte loop costs
four instructions a byte before its compare and branch. These move 16 bytes
a register (NEON's q registers, through the compiler's vector types: the
guest is compiled with -nostdinc, without arm_neon.h) and compare 8 bytes a
register, with unaligned accesses, which Apple silicon does at full speed.

They compute what musl's do, bit for bit: the same bytes written, and
memcmp's result is the difference of the first differing bytes, as musl's.
Nothing about the game's arithmetic changes (port/macos/tests/
math_determinism_test.c; port/macos/tests/perf_bench.c times them).

They are compiled like musl, freestanding: the compiler does not turn their
loops back into calls to themselves.
*/

#include <stddef.h>
#include <stdint.h>

typedef unsigned char block16 __attribute__((vector_size(16), aligned(1), may_alias));
typedef uint64_t __attribute__((aligned(1), may_alias)) unaligned64;
typedef uint32_t __attribute__((aligned(1), may_alias)) unaligned32;
typedef uint16_t __attribute__((aligned(1), may_alias)) unaligned16;

#define LOAD16(p) (*(const block16 *)(p))
#define STORE16(p, v) (*(block16 *)(p) = (v))
#define LOAD8(p) (*(const unaligned64 *)(p))
#define STORE8(p, v) (*(unaligned64 *)(p) = (v))
#define LOAD4(p) (*(const unaligned32 *)(p))
#define STORE4(p, v) (*(unaligned32 *)(p) = (v))
#define LOAD2(p) (*(const unaligned16 *)(p))
#define STORE2(p, v) (*(unaligned16 *)(p) = (v))

/* copies n <= 32 bytes, all of them read before any is written (so it is
also memmove's) */
static inline __attribute__((always_inline)) void copy_small(unsigned char *d, const unsigned char *s, size_t n)
{
	if (n >= 16)
	{
		block16 a = LOAD16(s), b = LOAD16(s + n - 16);

		STORE16(d, a);
		STORE16(d + n - 16, b);
	}
	else if (n >= 8)
	{
		uint64_t a = LOAD8(s), b = LOAD8(s + n - 8);

		STORE8(d, a);
		STORE8(d + n - 8, b);
	}
	else if (n >= 4)
	{
		uint32_t a = LOAD4(s), b = LOAD4(s + n - 4);

		STORE4(d, a);
		STORE4(d + n - 4, b);
	}
	else if (n >= 2)
	{
		uint16_t a = LOAD2(s), b = LOAD2(s + n - 2);

		STORE2(d, a);
		STORE2(d + n - 2, b);
	}
	else if (n)
	{
		*d = *s;
	}
}

/* copies n > 32 bytes forwards, 64 a step, then 32. The last 32 source
bytes are read first and written last, so this is also memmove's when d is
below s: each step reads before it writes, and writes only below what it
reads. */
static inline __attribute__((always_inline)) void copy_forward(unsigned char *d, const unsigned char *s, size_t n)
{
	const unsigned char *last = s + n - 32;
	unsigned char *last_d = d + n - 32;
	block16 t0 = LOAD16(last), t1 = LOAD16(last + 16);

	while (s + 32 < last)
	{
		block16 a = LOAD16(s), b = LOAD16(s + 16), c = LOAD16(s + 32), e = LOAD16(s + 48);

		STORE16(d, a);
		STORE16(d + 16, b);
		STORE16(d + 32, c);
		STORE16(d + 48, e);
		s += 64;
		d += 64;
	}
	if (s < last)
	{
		block16 a = LOAD16(s), b = LOAD16(s + 16);

		STORE16(d, a);
		STORE16(d + 16, b);
	}
	STORE16(last_d, t0);
	STORE16(last_d + 16, t1);
}

void *memcpy(void *restrict dest, const void *restrict src, size_t n)
{
	unsigned char *d = dest;
	const unsigned char *s = src;
	size_t skip;

	if (n <= 32)
	{
		copy_small(d, s, n);
		return dest;
	}
	if (n < 256)
	{
		copy_forward(d, s, n);
		return dest;
	}
	/* long copies: the first 16 bytes, then from the next 16-byte boundary
	of the destination (stores that cross cache lines cost two); the
	regions do not overlap, so this is not memmove's */
	STORE16(d, LOAD16(s));
	skip = 16 - ((uintptr_t)d & 15);
	copy_forward(d + skip, s + skip, n - skip);
	return dest;
}

void *memmove(void *dest, const void *src, size_t n)
{
	unsigned char *d = dest;
	const unsigned char *s = src;

	if (n <= 32)
	{
		copy_small(d, s, n);
	}
	else if ((uintptr_t)d - (uintptr_t)s >= n)
	{
		/* d below s, or no overlap */
		copy_forward(d, s, n);
	}
	else
	{
		/* d above s, overlapping: backwards, the first 32 bytes read
		first and written last */
		const unsigned char *end = s + n;
		unsigned char *end_d = d + n;
		block16 h0 = LOAD16(s), h1 = LOAD16(s + 16);

		while (end > s + 96)
		{
			block16 a, b, c, e;

			end -= 64;
			end_d -= 64;
			a = LOAD16(end);
			b = LOAD16(end + 16);
			c = LOAD16(end + 32);
			e = LOAD16(end + 48);
			STORE16(end_d, a);
			STORE16(end_d + 16, b);
			STORE16(end_d + 32, c);
			STORE16(end_d + 48, e);
		}
		while (end > s + 32)
		{
			block16 a, b;

			end -= 32;
			end_d -= 32;
			a = LOAD16(end);
			b = LOAD16(end + 16);
			STORE16(end_d, a);
			STORE16(end_d + 16, b);
		}
		STORE16(d, h0);
		STORE16(d + 16, h1);
	}
	return dest;
}

void *memset(void *dest, int c, size_t n)
{
	unsigned char *d = dest;
	uint64_t fill8 = 0x0101010101010101ULL * (unsigned char)c;

	if (n <= 16)
	{
		if (n >= 8)
		{
			STORE8(d, fill8);
			STORE8(d + n - 8, fill8);
		}
		else if (n >= 4)
		{
			STORE4(d, (uint32_t)fill8);
			STORE4(d + n - 4, (uint32_t)fill8);
		}
		else if (n)
		{
			d[0] = (unsigned char)c;
			d[n - 1] = (unsigned char)c;
			if (n == 3)
				d[1] = (unsigned char)c;
		}
		return dest;
	}
	{
		block16 fill = (block16){ 0 } + (unsigned char)c;
		unsigned char *last = d + n - 32;

		if (n <= 32)
		{
			STORE16(d, fill);
			STORE16(d + n - 16, fill);
			return dest;
		}
		/* the first 32 bytes, then from a 16-byte boundary of the
		destination, 128 bytes a step */
		STORE16(d, fill);
		STORE16(d + 16, fill);
		d = (unsigned char *)(((uintptr_t)d + 32) & ~(uintptr_t)15);
		while (d + 96 < last)
		{
			STORE16(d, fill);
			STORE16(d + 16, fill);
			STORE16(d + 32, fill);
			STORE16(d + 48, fill);
			STORE16(d + 64, fill);
			STORE16(d + 80, fill);
			STORE16(d + 96, fill);
			STORE16(d + 112, fill);
			d += 128;
		}
		while (d < last)
		{
			STORE16(d, fill);
			d += 16;
		}
		STORE16(last, fill);
		STORE16(last + 16, fill);
	}
	return dest;
}

/* the difference of the first differing bytes of two little-endian words
that differ */
static inline __attribute__((always_inline)) int word_difference(uint64_t a, uint64_t b)
{
	int shift = __builtin_ctzll(a ^ b) & ~7;

	return (int)((a >> shift) & 0xff) - (int)((b >> shift) & 0xff);
}

int memcmp(const void *vl, const void *vr, size_t n)
{
	const unsigned char *l = vl, *r = vr;

	for (; n >= 32; n -= 32, l += 32, r += 32)
	{
		uint64_t a0 = LOAD8(l), a1 = LOAD8(l + 8), a2 = LOAD8(l + 16), a3 = LOAD8(l + 24);
		uint64_t b0 = LOAD8(r), b1 = LOAD8(r + 8), b2 = LOAD8(r + 16), b3 = LOAD8(r + 24);

		if ((a0 ^ b0) | (a1 ^ b1) | (a2 ^ b2) | (a3 ^ b3))
		{
			if (a0 != b0)
				return word_difference(a0, b0);
			if (a1 != b1)
				return word_difference(a1, b1);
			if (a2 != b2)
				return word_difference(a2, b2);
			return word_difference(a3, b3);
		}
	}
	if (n >= 16)
	{
		uint64_t a0 = LOAD8(l), a1 = LOAD8(l + 8), b0 = LOAD8(r), b1 = LOAD8(r + 8);

		if ((a0 ^ b0) | (a1 ^ b1))
			return a0 != b0 ? word_difference(a0, b0) : word_difference(a1, b1);
		n -= 16;
		l += 16;
		r += 16;
	}
	if (n >= 8)
	{
		uint64_t a = LOAD8(l), b = LOAD8(r);

		if (a != b)
			return word_difference(a, b);
		n -= 8;
		l += 8;
		r += 8;
	}
	if (n >= 4)
	{
		uint64_t a = LOAD4(l), b = LOAD4(r);

		if (a != b)
			return word_difference(a, b);
		n -= 4;
		l += 4;
		r += 4;
	}
	for (; n; n--, l++, r++)
	{
		if (*l != *r)
			return *l - *r;
	}
	return 0;
}
