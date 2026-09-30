/*
D3D8_SIMD.H

Loops of the renderer (d3d8_gl.c) that the native macOS guest runs with
NEON (Apple silicon), kept apart so port/macos/tests/perf_bench.c can
check them against the C, which every other build keeps, and time both.
The guest is compiled without arm_neon.h (-nostdinc): these are the
compiler's vector types, which it gives NEON's instructions.
*/

#ifndef __D3D8_SIMD_H
#define __D3D8_SIMD_H

/* (D3D8_SIMD_SCALAR: the C there too, for perf_bench.c) */
#if defined(HALO_MACOS) && defined(__ARM_NEON) && !defined(D3D8_SIMD_SCALAR)
#define D3D8_SIMD_NEON
typedef unsigned short d3d8_simd_index8 __attribute__((vector_size(16), aligned(2), may_alias));
typedef unsigned short d3d8_simd_index4 __attribute__((vector_size(8), aligned(2), may_alias));
#else
#undef D3D8_SIMD_NEON
#endif

/* the smallest and the largest of count 16-bit indices (0xffff and 0 for
none): the vertices an indexed draw uploads. NEON compares eight indices an
instruction, sixteen a step, and ends on the last eight (again comparing
some: a minimum and a maximum do not mind) rather than one at a time. */
static void d3d8_index_extent(const unsigned short *indices, unsigned long count, unsigned long *minimum,
	unsigned long *maximum)
{
	unsigned long index, low = 0xffff, high = 0;

#ifdef D3D8_SIMD_NEON
	if (count >= 8)
	{
		d3d8_simd_index8 low8 = *(const d3d8_simd_index8 *)indices, high8 = low8;
		d3d8_simd_index8 low8_odd = low8, high8_odd = low8;

		for (index = 8; index + 16 <= count; index += 16)
		{
			d3d8_simd_index8 even = *(const d3d8_simd_index8 *)(indices + index);
			d3d8_simd_index8 odd = *(const d3d8_simd_index8 *)(indices + index + 8);

			low8 = __builtin_elementwise_min(low8, even);
			high8 = __builtin_elementwise_max(high8, even);
			low8_odd = __builtin_elementwise_min(low8_odd, odd);
			high8_odd = __builtin_elementwise_max(high8_odd, odd);
		}
		if (index + 8 <= count)
		{
			d3d8_simd_index8 next = *(const d3d8_simd_index8 *)(indices + index);

			low8 = __builtin_elementwise_min(low8, next);
			high8 = __builtin_elementwise_max(high8, next);
		}
		{
			d3d8_simd_index8 last = *(const d3d8_simd_index8 *)(indices + count - 8);

			low8 = __builtin_elementwise_min(__builtin_elementwise_min(low8, low8_odd), last);
			high8 = __builtin_elementwise_max(__builtin_elementwise_max(high8, high8_odd), last);
		}
		*minimum = __builtin_reduce_min(low8);
		*maximum = __builtin_reduce_max(high8);
		return;
	}
	if (count >= 4)
	{
		d3d8_simd_index4 first = *(const d3d8_simd_index4 *)indices;
		d3d8_simd_index4 last = *(const d3d8_simd_index4 *)(indices + count - 4);

		*minimum = __builtin_reduce_min(__builtin_elementwise_min(first, last));
		*maximum = __builtin_reduce_max(__builtin_elementwise_max(first, last));
		return;
	}
#endif
	for (index = 0; index < count; index++)
	{
		if (indices[index] < low)
			low = indices[index];
		if (indices[index] > high)
			high = indices[index];
	}
	*minimum = low;
	*maximum = high;
}

#endif
