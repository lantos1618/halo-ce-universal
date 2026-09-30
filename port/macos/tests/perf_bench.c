/*
PERF_BENCH.C

Microbenchmarks for the perf lab (docs/perf-lab.md), run as a guest image
of the native macOS build, so they time what the game runs: arm64_32 code,
rebased (every memory access through x28), compiled with the game's flags.
ninja macos_perf_bench builds it; port/macos/tests/run_perf_bench.sh runs it.

- the memory functions the renderer and the game call most (the renderer
  compares a 252-byte pixel shader key and 308 bytes of uniform inputs per
  draw with memcmp): the guest's (port/macos/guest/libc/string/
  memory_wide.c) against musl's portable C, which the guest used before,
  after checking that the two compute the same results;
- the game's CRC (source/memory/crc.c, checkpoints' 16 MB game state)
  against the byte table it had;
- skinning a model the way the ray tracing's object meshes are made
  (port/linux/game/raytrace_world.c): each strip corner transformed, as it
  was, against each vertex transformed once;
- the game's matrix maths and halo_ maths (the determinism test's
  functions), to compare compiler flags (-O2 against -O3);
- the game's matrix transforms and products, NEON's (source/math/
  matrix_math.c) against the C they were, after checking the two alike on
  millions of matrices and points (every exponent, zeros, denormals,
  infinities and NaNs);
- the renderer's loops the guest runs with NEON (port/linux/src/
  d3d8_simd.h: an indexed draw's smallest and largest index) against the
  C, checked alike first.

Each line is the best of several runs, in nanoseconds per call (or per
item).
*/

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* musl's portable string functions, which the guest's libc had before
memory_wide.c, under other names (this file is compiled -fno-builtin, so
they stay the loops they are) */
#define memcpy musl_memcpy
#include "memcpy.c"
#undef memcpy
#define memmove musl_memmove
#include "memmove.c"
#undef memmove
#define memset musl_memset
#include "memset.c"
#undef memset
#define memcmp musl_memcmp
#include "memcmp.c"
#undef memcmp

typedef struct { float scale; float n[4][3]; } matrix4x3;
typedef struct { float x, y, z; } vector3;

void matrix4x3_multiply(const matrix4x3 *a, const matrix4x3 *b, matrix4x3 *result);
vector3 *matrix4x3_transform_point(const matrix4x3 *matrix, const vector3 *point, vector3 *result);
void matrix4x3_inverse(const matrix4x3 *matrix, matrix4x3 *result);
void matrix4x3_rotation_from_angles(matrix4x3 *matrix, float yaw, float pitch, float roll);
double halo_sin(double x);
double halo_atan2(double y, double x);
double halo_pow(double x, double y);

#define MIN_OF(a, b) ((a) < (b) ? (a) : (b))

static uint32_t seed = 20260930;

static uint32_t random_next(void)
{
	seed = seed * 1664525u + 1013904223u;
	return seed;
}

static float random_real(float low, float high)
{
	return low + (high - low) * (float)(random_next() >> 8) / 16777216.0f;
}

static double now_ns(void)
{
	struct timespec time;

	clock_gettime(CLOCK_MONOTONIC, &time);
	return (double)time.tv_sec * 1e9 + (double)time.tv_nsec;
}

static volatile uint64_t sink;
static int failures;

/* ---------- the memory functions */

typedef void *(*copy_function)(void *, const void *, size_t);
typedef void *(*set_function)(void *, int, size_t);
typedef int (*compare_function)(const void *, const void *, size_t);

static int sign(int value)
{
	return value < 0 ? -1 : value > 0;
}

/* the guest's against musl's: every size to 300 at every offset to 16, with
overlaps for memmove and a difference at every position for memcmp */
static void check_memory_functions(void)
{
	static unsigned char source[1024], a[1024], b[1024];
	copy_function volatile copy_wide = memcpy, copy_musl = musl_memcpy;
	copy_function volatile move_wide = memmove, move_musl = musl_memmove;
	set_function volatile set_wide = memset, set_musl = musl_memset;
	compare_function volatile compare_wide = memcmp, compare_musl = musl_memcmp;
	size_t size, offset, other, index;
	int checks = 0;

	for (index = 0; index < sizeof(source); index++)
		source[index] = (unsigned char)random_next();
	for (size = 0; size <= 300; size++)
	{
		for (offset = 0; offset < 16; offset++)
		{
			other = (offset * 7) % 16;
			musl_memset(a, 0x5a, sizeof(a));
			musl_memset(b, 0x5a, sizeof(b));
			copy_wide(a + offset, source + other, size);
			copy_musl(b + offset, source + other, size);
			failures += musl_memcmp(a, b, sizeof(a)) != 0;
			set_wide(a + other, (int)(size * 13 + offset), size);
			set_musl(b + other, (int)(size * 13 + offset), size);
			failures += musl_memcmp(a, b, sizeof(a)) != 0;
			/* overlapping, both ways */
			musl_memcpy(a, source, sizeof(a));
			musl_memcpy(b, source, sizeof(b));
			move_wide(a + 64 + offset, a + 64 + other, size);
			move_musl(b + 64 + offset, b + 64 + other, size);
			failures += musl_memcmp(a, b, sizeof(a)) != 0;
			move_wide(a + 64 + other, a + 64 + offset + 20, size);
			move_musl(b + 64 + other, b + 64 + offset + 20, size);
			failures += musl_memcmp(a, b, sizeof(a)) != 0;
			/* equal, and different at each position (either way) */
			musl_memcpy(a, source, sizeof(a));
			musl_memcpy(b, source, sizeof(b));
			failures += compare_wide(a + offset, b + offset, size) != compare_musl(a + offset, b + offset, size);
			for (index = 0; index < size; index++)
			{
				b[offset + index] ^= (unsigned char)(1 + (index & 0x7f));
				failures += compare_wide(a + offset, b + offset, size) != compare_musl(a + offset, b + offset, size);
				failures += sign(compare_wide(b + offset, a + offset, size)) !=
					sign(compare_musl(b + offset, a + offset, size));
				b[offset + index] = a[offset + index];
			}
			checks += 5 + 2 * (int)size;
		}
	}
	printf("memory functions: %d checks against musl's, %d different\n", checks, failures);
}

static double time_copy(copy_function volatile function, void *destination, const void *source, size_t size,
	long repeats)
{
	double best = 1e30;
	int run;

	for (run = 0; run < 9; run++)
	{
		double start = now_ns(), elapsed;
		long index;

		for (index = 0; index < repeats; index++)
			function(destination, source, size);
		elapsed = (now_ns() - start) / (double)repeats;
		if (elapsed < best)
			best = elapsed;
	}
	return best;
}

static double time_set(set_function volatile function, void *destination, size_t size, long repeats)
{
	double best = 1e30;
	int run;

	for (run = 0; run < 9; run++)
	{
		double start = now_ns(), elapsed;
		long index;

		for (index = 0; index < repeats; index++)
			function(destination, (int)index, size);
		elapsed = (now_ns() - start) / (double)repeats;
		if (elapsed < best)
			best = elapsed;
	}
	return best;
}

static double time_compare(compare_function volatile function, const void *a, const void *b, size_t size,
	long repeats)
{
	double best = 1e30;
	int run;

	for (run = 0; run < 9; run++)
	{
		double start = now_ns(), elapsed;
		long index;
		int total = 0;

		for (index = 0; index < repeats; index++)
			total += function(a, b, size);
		elapsed = (now_ns() - start) / (double)repeats;
		sink += (uint64_t)total;
		if (elapsed < best)
			best = elapsed;
	}
	return best;
}

static void bench_memory_functions(void)
{
	static unsigned char a[65536 + 64], b[65536 + 64], equal0[65536], equal1[65536];
	static const size_t sizes[] = { 12, 64, 252, 308, 1024, 4096, 65536 };
	size_t index;

	for (index = 0; index < sizeof(a); index++)
		a[index] = b[index] = (unsigned char)index;
	for (index = 0; index < sizeof(equal0); index++)
		equal0[index] = equal1[index] = (unsigned char)(index * 7);
	printf("%-8s %7s %10s %10s %7s\n", "function", "bytes", "musl ns", "wide ns", "speedup");
	for (index = 0; index < sizeof(sizes) / sizeof(sizes[0]); index++)
	{
		size_t size = sizes[index];
		long repeats = size >= 65536 ? 5000 : size >= 4096 ? 100000 : 1000000;
		double before, after;

		/* equal buffers: the renderer's state compares, which mostly match */
		before = time_compare(musl_memcmp, equal0, equal1, size, repeats);
		after = time_compare(memcmp, equal0, equal1, size, repeats);
		printf("%-8s %7lu %10.1f %10.1f %6.1fx\n", "memcmp", (unsigned long)size, before, after, before / after);
		before = time_copy(musl_memcpy, a + 1, b + 3, size, repeats);
		after = time_copy(memcpy, a + 1, b + 3, size, repeats);
		printf("%-8s %7lu %10.1f %10.1f %6.1fx\n", "memcpy", (unsigned long)size, before, after, before / after);
		before = time_copy(musl_memcpy, a, b, size, repeats);
		after = time_copy(memcpy, a, b, size, repeats);
		printf("%-8s %7lu %10.1f %10.1f %6.1fx\n", "memcpy/a", (unsigned long)size, before, after, before / after);
		before = time_copy(musl_memmove, a + 8, a, size, repeats);
		after = time_copy(memmove, a + 8, a, size, repeats);
		printf("%-8s %7lu %10.1f %10.1f %6.1fx\n", "memmove", (unsigned long)size, before, after, before / after);
		before = time_set(musl_memset, a, size, repeats);
		after = time_set(memset, a, size, repeats);
		printf("%-8s %7lu %10.1f %10.1f %6.1fx\n", "memset", (unsigned long)size, before, after, before / after);
	}
}

/* ---------- the game's CRC (source/memory/crc.c) */

void crc_checksum_buffer(unsigned long *crc_reference, void const *buffer, long buffer_size);

/* crc.c's table, byte by byte, as the game computed it before the CRC32
instructions */
static unsigned long reference_crc(unsigned long crc, const unsigned char *bytes, long size)
{
	static unsigned long table[256];
	long index;

	if (!table[1])
	{
		for (index = 0; index < 256; index++)
		{
			unsigned long value = (unsigned long)index;
			int bit;

			for (bit = 0; bit < 8; bit++)
				value = value & 1 ? (value >> 1) ^ 0xEDB88320UL : value >> 1;
			table[index] = value;
		}
	}
	for (index = 0; index < size; index++)
		crc = table[(bytes[index] ^ crc) & 0xFF] ^ (crc >> 8);
	return crc;
}

static void bench_crc(void)
{
	static unsigned char state[16 * 1024 * 1024];
	long index, size, checks = 0, different = 0;
	double start, table_ms = 1e30, instructions_ms = 1e30;
	unsigned long table_crc = 0, instructions_crc = 0;
	int run;

	for (index = 0; index < (long)sizeof(state); index++)
		state[index] = (unsigned char)(random_next() >> 13);
	for (size = 0; size < 600; size++)
	{
		for (index = 0; index < 8; index++)
		{
			unsigned long crc = random_next();
			unsigned long expected = reference_crc(crc, state + index * 1000 + size, size);

			crc_checksum_buffer(&crc, state + index * 1000 + size, size);
			different += crc != expected;
			checks++;
		}
	}
	for (run = 0; run < 3; run++)
	{
		unsigned long crc = 0xFFFFFFFFUL;

		start = now_ns();
		table_crc = reference_crc(0xFFFFFFFFUL, state, (long)sizeof(state));
		table_ms = MIN_OF(table_ms, (now_ns() - start) / 1e6);
		start = now_ns();
		crc_checksum_buffer(&crc, state, (long)sizeof(state));
		instructions_crc = crc;
		instructions_ms = MIN_OF(instructions_ms, (now_ns() - start) / 1e6);
	}
	different += table_crc != instructions_crc;
	failures += different != 0;
	printf("crc_checksum_buffer: %ld checks against the table, %ld different; 16 MB (a checkpoint's game state) "
		"%.1f ms by table, %.1f ms by the game's (%.1fx)\n", checks + 1, different, table_ms, instructions_ms,
		table_ms / instructions_ms);
}

/* ---------- skinning, as raytrace_world.c's model_triangles */

#define SKIN_NODES 40
#define SKIN_VERTICES 1200
#define SKIN_STRIP 2600

struct skin_vertex
{
	vector3 position;
	unsigned char node_indices[2];
	short node_weight;
};

static struct skin_vertex skin_vertices[SKIN_VERTICES];
static unsigned short skin_strip[SKIN_STRIP];
static matrix4x3 skin_matrices[SKIN_NODES];
static float skin_out_corners[SKIN_STRIP * 9], skin_out_vertices[SKIN_STRIP * 9];
static vector3 skin_cache[SKIN_VERTICES];

static void skin_point(const struct skin_vertex *vertex, float *q)
{
	short node0 = (short)(vertex->node_indices[0] / 3), node1 = (short)(vertex->node_indices[1] / 3);
	float weight0 = (float)vertex->node_weight * (1.0f / 32767.0f);
	vector3 point0 = vertex->position, point1 = vertex->position;

	if (node0 >= 0 && node0 < SKIN_NODES)
		matrix4x3_transform_point(&skin_matrices[node0], &vertex->position, &point0);
	if (node1 >= 0 && node1 < SKIN_NODES)
		matrix4x3_transform_point(&skin_matrices[node1], &vertex->position, &point1);
	else
		point1 = point0;
	q[0] = point0.x * weight0 + point1.x * (1.0f - weight0);
	q[1] = point0.y * weight0 + point1.y * (1.0f - weight0);
	q[2] = point0.z * weight0 + point1.z * (1.0f - weight0);
}

/* each strip triangle's corners skinned (a vertex three times) */
static long skin_by_corner(float *out)
{
	long count = 0, index;
	int corner;

	for (index = 0; index + 2 < SKIN_STRIP; index++)
	{
		unsigned short corners[3] = { skin_strip[index], skin_strip[index + 1], skin_strip[index + 2] };

		if (corners[0] == corners[1] || corners[1] == corners[2] || corners[0] == corners[2])
			continue;
		for (corner = 0; corner < 3; corner++)
			skin_point(&skin_vertices[corners[corner]], out + count * 9 + corner * 3);
		count++;
	}
	return count;
}

/* each vertex skinned once, the corners copied */
static long skin_by_vertex(float *out)
{
	long count = 0, index;
	int corner;

	for (index = 0; index < SKIN_VERTICES; index++)
		skin_point(&skin_vertices[index], &skin_cache[index].x);
	for (index = 0; index + 2 < SKIN_STRIP; index++)
	{
		unsigned short corners[3] = { skin_strip[index], skin_strip[index + 1], skin_strip[index + 2] };

		if (corners[0] == corners[1] || corners[1] == corners[2] || corners[0] == corners[2])
			continue;
		for (corner = 0; corner < 3; corner++)
		{
			const vector3 *point = &skin_cache[corners[corner]];

			out[count * 9 + corner * 3 + 0] = point->x;
			out[count * 9 + corner * 3 + 1] = point->y;
			out[count * 9 + corner * 3 + 2] = point->z;
		}
		count++;
	}
	return count;
}

static void bench_skinning(void)
{
	long index, triangles = 0;
	double before = 1e30, after = 1e30;
	int run;

	for (index = 0; index < SKIN_NODES; index++)
	{
		matrix4x3_rotation_from_angles(&skin_matrices[index], random_real(-3, 3), random_real(-1.5f, 1.5f),
			random_real(-3, 3));
		skin_matrices[index].scale = 1.0f;
		skin_matrices[index].n[3][0] = random_real(-5, 5);
		skin_matrices[index].n[3][1] = random_real(-5, 5);
		skin_matrices[index].n[3][2] = random_real(-5, 5);
	}
	for (index = 0; index < SKIN_VERTICES; index++)
	{
		skin_vertices[index].position.x = random_real(-1, 1);
		skin_vertices[index].position.y = random_real(-1, 1);
		skin_vertices[index].position.z = random_real(-1, 1);
		skin_vertices[index].node_indices[0] = (unsigned char)(3 * (random_next() % SKIN_NODES));
		skin_vertices[index].node_indices[1] = (unsigned char)(3 * (random_next() % SKIN_NODES));
		skin_vertices[index].node_weight = (short)(random_next() % 32768);
	}
	/* a strip that walks the mesh, with the odd degenerate restart */
	for (index = 0; index < SKIN_STRIP; index++)
		skin_strip[index] = (unsigned short)((index / 2 + (index & 1) * 3 + (index % 97 == 0)) % SKIN_VERTICES);
	for (run = 0; run < 20; run++)
	{
		double start = now_ns(), elapsed;

		triangles = skin_by_corner(skin_out_corners);
		elapsed = now_ns() - start;
		if (elapsed < before)
			before = elapsed;
		start = now_ns();
		skin_by_vertex(skin_out_vertices);
		elapsed = now_ns() - start;
		if (elapsed < after)
			after = elapsed;
	}
	failures += memcmp(skin_out_corners, skin_out_vertices, (size_t)triangles * 9 * sizeof(float)) != 0;
	printf("skinning %ld triangles of %d vertices: by corner %.1f us, by vertex %.1f us (%.1fx), results %s\n",
		triangles, SKIN_VERTICES, before / 1000.0, after / 1000.0, before / after,
		memcmp(skin_out_corners, skin_out_vertices, (size_t)triangles * 9 * sizeof(float)) ? "DIFFERENT" : "identical");
}

/* ---------- the game's maths, for compiler flags */

static void bench_maths(void)
{
	static matrix4x3 matrices[256];
	static vector3 points[256];
	double best_multiply = 1e30, best_transform = 1e30, best_inverse = 1e30, best_transcendental = 1e30;
	int run, index;

	for (index = 0; index < 256; index++)
	{
		matrix4x3_rotation_from_angles(&matrices[index], random_real(-3, 3), random_real(-1.5f, 1.5f),
			random_real(-3, 3));
		matrices[index].scale = random_real(0.5f, 2.0f);
		points[index].x = random_real(-100, 100);
		points[index].y = random_real(-100, 100);
		points[index].z = random_real(-100, 100);
	}
	for (run = 0; run < 7; run++)
	{
		matrix4x3 product;
		vector3 out;
		double start, total = 0;
		int repeat;

		start = now_ns();
		for (repeat = 0; repeat < 400; repeat++)
			for (index = 0; index < 255; index++)
			{
				matrix4x3_multiply(&matrices[index], &matrices[index + 1], &product);
				total += product.n[3][0];
			}
		best_multiply = MIN_OF(best_multiply, (now_ns() - start) / (400.0 * 255));
		start = now_ns();
		for (repeat = 0; repeat < 400; repeat++)
			for (index = 0; index < 256; index++)
			{
				matrix4x3_transform_point(&matrices[index], &points[(index + repeat) & 255], &out);
				total += out.x;
			}
		best_transform = MIN_OF(best_transform, (now_ns() - start) / (400.0 * 256));
		start = now_ns();
		for (repeat = 0; repeat < 100; repeat++)
			for (index = 0; index < 256; index++)
			{
				matrix4x3_inverse(&matrices[index], &product);
				total += product.n[0][0];
			}
		best_inverse = MIN_OF(best_inverse, (now_ns() - start) / (100.0 * 256));
		start = now_ns();
		for (repeat = 0; repeat < 20000; repeat++)
		{
			double x = (double)(repeat & 1023) * 0.01 - 5.0;

			total += halo_sin(x) + halo_atan2(x, 1.5) + halo_pow(x * x + 0.5, 0.75);
		}
		best_transcendental = MIN_OF(best_transcendental, (now_ns() - start) / 20000.0);
		sink += (uint64_t)total;
	}
	printf("matrix4x3_multiply %.1f ns, transform_point %.1f ns, inverse %.1f ns, sin+atan2+pow %.1f ns\n",
		best_multiply, best_transform, best_inverse, best_transcendental);
}

/* ---------- the game's matrix maths: NEON against the C it replaced */

vector3 *matrix4x3_transform_vector(const matrix4x3 *matrix, const vector3 *vector, vector3 *result);
vector3 *matrix4x3_transform_normal(const matrix4x3 *matrix, const vector3 *normal, vector3 *result);

/* source/math/matrix_math.c's C, as it was before its NEON (compiled with
the game's flags: no fused multiply-add) */
__attribute__((noinline)) static vector3 *c_transform_point(const matrix4x3 *matrix, const vector3 *point, vector3 *result)
{
	float x = point->x, y = point->y, z = point->z;

	if (matrix->scale != 1.f)
	{
		x *= matrix->scale;
		y *= matrix->scale;
		z *= matrix->scale;
	}
	result->x = matrix->n[2][0]*z + matrix->n[1][0]*y + matrix->n[0][0]*x + matrix->n[3][0];
	result->y = matrix->n[2][1]*z + matrix->n[1][1]*y + matrix->n[0][1]*x + matrix->n[3][1];
	result->z = matrix->n[2][2]*z + matrix->n[1][2]*y + matrix->n[0][2]*x + matrix->n[3][2];
	return result;
}

__attribute__((noinline)) static vector3 *c_transform_vector(const matrix4x3 *matrix, const vector3 *vector, vector3 *result)
{
	float i = vector->x, j = vector->y, k = vector->z;

	if (matrix->scale != 1.f)
	{
		i *= matrix->scale;
		j *= matrix->scale;
		k *= matrix->scale;
	}
	result->x = i*matrix->n[0][0] + j*matrix->n[1][0] + k*matrix->n[2][0];
	result->y = i*matrix->n[0][1] + j*matrix->n[1][1] + k*matrix->n[2][1];
	result->z = i*matrix->n[0][2] + j*matrix->n[1][2] + k*matrix->n[2][2];
	return result;
}

__attribute__((noinline)) static vector3 *c_transform_normal(const matrix4x3 *matrix, const vector3 *normal, vector3 *result)
{
	float i = normal->x, j = normal->y, k = normal->z;

	result->x = i*matrix->n[0][0] + j*matrix->n[1][0] + k*matrix->n[2][0];
	result->y = i*matrix->n[0][1] + j*matrix->n[1][1] + k*matrix->n[2][1];
	result->z = i*matrix->n[0][2] + j*matrix->n[1][2] + k*matrix->n[2][2];
	return result;
}

__attribute__((noinline)) static void c_multiply(const matrix4x3 *a, const matrix4x3 *b, matrix4x3 *result)
{
	const float *a_elements = a->n[0], *b_elements = b->n[0];
	float product[12];
	long row, column;

	for (row = 0; row < 4; row++)
	{
		for (column = 0; column < 3; column++)
		{
			float value = b_elements[row * 3 + 0] * a_elements[column] +
				b_elements[row * 3 + 1] * a_elements[3 + column] +
				b_elements[row * 3 + 2] * a_elements[6 + column];

			if (row == 3)
				value = value * a->scale + a_elements[9 + column];
			product[row * 3 + column] = value;
		}
	}
	memcpy(result->n[0], product, sizeof(product));
	result->scale = a->scale * b->scale;
}

/* any float: every exponent, both signs, zeros, denormals, infinities and
(rarely) NaNs, or a value of a game's size */
static float random_any_real(void)
{
	uint32_t kind = random_next() >> 24, bits = random_next();
	float value;

	if (kind < 128)
		return random_real(-1000.0f, 1000.0f);
	if (kind < 136)
		return (kind & 1) ? 0.0f : -0.0f;
	if (kind == 136)
		bits = 0x7f800000u | (bits & 0x80000000u); /* infinity */
	else if (kind == 137)
		bits = 0x7fc00000u | (bits & 0x803fffffu); /* NaN */
	else if (kind < 142)
		bits &= 0x807fffffu; /* denormal */
	else if ((bits & 0x7f800000u) == 0x7f800000u)
		bits ^= 0x40000000u; /* no infinity or NaN here */
	memcpy(&value, &bits, sizeof(value));
	return value;
}

static void random_matrix(matrix4x3 *matrix, int kind)
{
	int index;

	if (kind == 0)
	{
		/* as the game has them: a rotation, a position, a scale of 1 or not */
		matrix4x3_rotation_from_angles(matrix, random_real(-3.2f, 3.2f), random_real(-1.6f, 1.6f),
			random_real(-3.2f, 3.2f));
		matrix->scale = (random_next() & 1) ? 1.0f : random_real(0.01f, 4.0f);
		for (index = 0; index < 3; index++)
			matrix->n[3][index] = random_real(-2000.0f, 2000.0f);
		return;
	}
	matrix->scale = (random_next() & 3) ? random_any_real() : 1.0f;
	for (index = 0; index < 12; index++)
		matrix->n[0][index] = random_any_real();
}

/* NaNs alike when both are NaN: which NaN's payload a sum of two keeps is
not the game's concern (and not what x86 and ARM agree on either) */
static int same_reals(const float *a, const float *b, int count, long *nan_only)
{
	int index, different = 0, nans = 0;

	for (index = 0; index < count; index++)
	{
		uint32_t x, y;

		memcpy(&x, &a[index], 4);
		memcpy(&y, &b[index], 4);
		if (x == y)
			continue;
		if (a[index] != a[index] && b[index] != b[index])
			nans = 1;
		else
			different = 1;
	}
	*nan_only += !different && nans;
	return !different;
}

static void check_matrix_maths(void)
{
	static matrix4x3 matrices[257];
	static vector3 points[256];
	long checks = 0, different = 0, nan_only = 0, index;
	double c_ns[4] = { 1e30, 1e30, 1e30, 1e30 }, neon_ns[4] = { 1e30, 1e30, 1e30, 1e30 };
	int run, function;

	for (index = 0; index < 4000000; index++)
	{
		matrix4x3 a, b, expected, got, aliased;
		vector3 point, expected_point, got_point;

		random_matrix(&a, (int)(index & 1));
		random_matrix(&b, (int)((index >> 1) & 1));
		point.x = (index & 4) ? random_any_real() : random_real(-100.0f, 100.0f);
		point.y = (index & 4) ? random_any_real() : random_real(-100.0f, 100.0f);
		point.z = (index & 4) ? random_any_real() : random_real(-100.0f, 100.0f);
		switch (index % 5)
		{
		case 0:
			c_multiply(&a, &b, &expected);
			matrix4x3_multiply(&a, &b, &got);
			different += !same_reals(&expected.scale, &got.scale, 13, &nan_only);
			/* in place, as the game does (result the same as a, or as b) */
			aliased = a;
			matrix4x3_multiply(&aliased, &b, &aliased);
			different += !same_reals(&expected.scale, &aliased.scale, 13, &nan_only);
			aliased = b;
			matrix4x3_multiply(&a, &aliased, &aliased);
			different += !same_reals(&expected.scale, &aliased.scale, 13, &nan_only);
			checks += 2;
			break;
		case 1:
			c_transform_point(&a, &point, &expected_point);
			matrix4x3_transform_point(&a, &point, &got_point);
			different += !same_reals(&expected_point.x, &got_point.x, 3, &nan_only);
			/* in place */
			got_point = point;
			matrix4x3_transform_point(&a, &got_point, &got_point);
			different += !same_reals(&expected_point.x, &got_point.x, 3, &nan_only);
			checks++;
			break;
		case 2:
			c_transform_vector(&a, &point, &expected_point);
			matrix4x3_transform_vector(&a, &point, &got_point);
			different += !same_reals(&expected_point.x, &got_point.x, 3, &nan_only);
			break;
		default:
			c_transform_normal(&a, &point, &expected_point);
			matrix4x3_transform_normal(&a, &point, &got_point);
			different += !same_reals(&expected_point.x, &got_point.x, 3, &nan_only);
			break;
		}
		checks++;
	}
	failures += different != 0;
	printf("matrix maths: %ld checks against the C, %ld different (%ld alike but for a NaN's payload)\n",
		checks, different, nan_only);

	for (index = 0; index < 257; index++)
		random_matrix(&matrices[index], 0);
	for (index = 0; index < 256; index++)
	{
		points[index].x = random_real(-100, 100);
		points[index].y = random_real(-100, 100);
		points[index].z = random_real(-100, 100);
	}
	for (run = 0; run < 9; run++)
	{
		for (function = 0; function < 4; function++)
		{
			int neon;

			for (neon = 0; neon < 2; neon++)
			{
				double start = now_ns(), elapsed, total = 0;
				int repeat;

				for (repeat = 0; repeat < 200; repeat++)
				{
					for (index = 0; index < 256; index++)
					{
						const matrix4x3 *matrix = &matrices[index];
						const vector3 *point = &points[(index + repeat) & 255];
						matrix4x3 product;
						vector3 out;

						switch (function * 2 + neon)
						{
						case 0: c_multiply(matrix, matrix + 1, &product); total += product.n[3][0]; break;
						case 1: matrix4x3_multiply(matrix, matrix + 1, &product); total += product.n[3][0]; break;
						case 2: c_transform_point(matrix, point, &out); total += out.x; break;
						case 3: matrix4x3_transform_point(matrix, point, &out); total += out.x; break;
						case 4: c_transform_vector(matrix, point, &out); total += out.y; break;
						case 5: matrix4x3_transform_vector(matrix, point, &out); total += out.y; break;
						case 6: c_transform_normal(matrix, point, &out); total += out.z; break;
						default: matrix4x3_transform_normal(matrix, point, &out); total += out.z; break;
						}
					}
				}
				elapsed = (now_ns() - start) / (200.0 * 256);
				sink += (uint64_t)total;
				if (neon)
					neon_ns[function] = MIN_OF(neon_ns[function], elapsed);
				else
					c_ns[function] = MIN_OF(c_ns[function], elapsed);
			}
		}
	}
	{
		static const char *names[4] = { "matrix4x3_multiply", "matrix4x3_transform_point",
			"matrix4x3_transform_vector", "matrix4x3_transform_normal" };

		for (function = 0; function < 4; function++)
		{
			printf("%-27s C %5.2f ns, the game's %5.2f ns (%.2fx)\n", names[function], c_ns[function],
				neon_ns[function], c_ns[function] / neon_ns[function]);
		}
	}
}

/* ---------- the renderer's loops (port/linux/src/d3d8_simd.h) */

/* the game's, as the macOS guest has them (NEON), and the C, which the
other ports compile and which the guest had */
#define D3D8_SIMD_SCALAR
#define d3d8_index_extent c_index_extent
#include "../../linux/src/d3d8_simd.h"
#undef d3d8_index_extent
#undef D3D8_SIMD_SCALAR
#undef __D3D8_SIMD_H
#include "../../linux/src/d3d8_simd.h"

typedef void (*extent_function)(const unsigned short *, unsigned long, unsigned long *, unsigned long *);

static void bench_index_extent(void)
{
	/* the draws' index counts: a sprite's quad to a level section's strips */
	static const unsigned long sizes[] = { 4, 6, 14, 36, 120, 400, 1500, 6000 };
	static unsigned short indices[8192];
	static volatile extent_function functions[2] = { c_index_extent, d3d8_index_extent };
	long checks = 0, different = 0;
	unsigned long size, offset, index;

	for (index = 0; index < 8192; index++)
		indices[index] = (unsigned short)(random_next() >> 16);
	/* every count to 600 at every offset to 16, and runs of ascending
	indices (the smallest and the largest at the ends) */
	for (size = 0; size <= 600; size++)
	{
		for (offset = 0; offset < 16; offset++)
		{
			unsigned long c_low, c_high, low, high;
			unsigned short saved[2];

			c_index_extent(indices + offset, size, &c_low, &c_high);
			d3d8_index_extent(indices + offset, size, &low, &high);
			different += c_low != low || c_high != high;
			checks++;
			if (size < 2)
				continue;
			saved[0] = indices[offset];
			saved[1] = indices[offset + size - 1];
			indices[offset] = (unsigned short)offset;
			indices[offset + size - 1] = (unsigned short)(0xfff0 + offset);
			c_index_extent(indices + offset, size, &c_low, &c_high);
			d3d8_index_extent(indices + offset, size, &low, &high);
			different += c_low != low || c_high != high;
			checks++;
			indices[offset] = saved[0];
			indices[offset + size - 1] = saved[1];
		}
	}
	failures += different != 0;
	printf("index extent: %ld checks against the C, %ld different\n", checks, different);
	printf("function         indices       C ns    game ns speedup\n");
	for (size = 0; size < sizeof(sizes) / sizeof(sizes[0]); size++)
	{
		double best[2] = { 1e30, 1e30 };
		long repeats = 4000000 / (long)(sizes[size] + 16);
		int run, which;

		for (run = 0; run < 7; run++)
		{
			for (which = 0; which < 2; which++)
			{
				extent_function function = functions[which];
				unsigned long low = 0, high = 0, total = 0;
				double start = now_ns();
				long repeat;

				for (repeat = 0; repeat < repeats; repeat++)
				{
					function(indices + (repeat & 15), sizes[size], &low, &high);
					total += low + high;
				}
				best[which] = MIN_OF(best[which], (now_ns() - start) / (double)repeats);
				sink += total;
			}
		}
		printf("index_extent  %10lu %10.1f %10.1f %6.1fx\n", sizes[size], best[0], best[1], best[0] / best[1]);
	}
}

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	check_memory_functions();
	bench_memory_functions();
	bench_crc();
	bench_skinning();
	bench_maths();
	check_matrix_maths();
	bench_index_extent();
	printf("%s\n", failures ? "FAILED" : "ok");
	return failures ? 1 : 0;
}
