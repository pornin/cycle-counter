/*
 * This test program demonstrates access to the cycle counter. It should
 * work on x86 (32-bit and 64-bit), aarch64 and riscv64, though in all
 * three cases some superuser-level operations must first be done to allow
 * access to the counter.
 *
 * The core_cycles() function returns the current value of the cycle
 * counter. The test program uses core_cycles() to perform a measurement
 * of the cost (latency) of integer multiplications; a base integer
 * value should be provided as starting point, then the program
 * multiplies it with itself repeatedly. The starting point is obtained
 * as a program argument to prevent the compiler from optimizing it.
 * Relevant argument values are 0, 1 and 3; 0 and 1 exercise "special cases"
 * (i.e. values for which a variable-time multiplier is likely to return
 * "early") while 3 will use more-or-less pseudorandom values and should thus
 * exercise the "general case".
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#if defined __x86_64__ || defined _M_X64 || defined __i386__ || defined _M_IX86
/*
 * x86: the cycle counter is accessible with the rdpmc instruction. Access
 * must first be allowed, which, on Linux, is done with:
 *   echo 2 > /sys/bus/event_source/devices/cpu/rdpmc
 * (only root can perform this write; the setting is "volatile" in that
 * it lasts only until next reboot).
 */
#include <immintrin.h>
#ifdef _MSC_VER
/* On Windows, the intrinsic is called __readpmc(), not __rdpmc(). But it
   will usually imply a crash, since Windows does no enable access to the
   performance counters. */
#ifndef __rdpmc
#define __rdpmc   __readpmc
#endif
#else
#include <x86intrin.h>
#endif
#if defined __GNUC__ || defined __clang__
__attribute__((target("sse2")))
#endif
static inline uint64_t
core_cycles(void)
{
	_mm_lfence();
	return __rdpmc(0x40000001);
}

#elif defined __aarch64__ && (defined __GNUC__ || defined __clang__)
/*
 * ARMv8, 64-bit (aarch64): the cycle counter is pmccntr_el0; it must be
 * enabled through dedicated kernel code.
 */
static inline uint64_t
core_cycles(void)
{
	uint64_t x;
	__asm__ __volatile__ ("dsb sy\n\tmrs %0, pmccntr_el0" : "=r" (x) : : );
	return x;
}

#elif defined __riscv && defined __riscv_xlen && __riscv_xlen >= 64
/*
 * RISC-V, 64-bit (rv64gc): the cycle counter is read with the
 * pseudo-instruction rdcycle (which is just an alias for csrrs with
 * the appropriate register identifier). The cycle counter must be enabled
 * and its userland access allowed, which requires machine-level actions
 * that can be triggered from dedicated kernel code.
 */
static inline uint64_t
core_cycles(void)
{
	/* We don't use a memory fence here because the RISC-V ISA
	   already requires the CPU to enforce appropriate ordering for
	   this access. */
	uint64_t x;
	__asm__ __volatile__ ("rdcycle %0" : "=r" (x));
	return x;
}

#else
#error Architecture is not supported.
#endif

static int
cmp_u64(const void *v1, const void *v2)
{
	uint64_t x1 = *(const uint64_t *)v1;
	uint64_t x2 = *(const uint64_t *)v2;
	if (x1 < x2) {
		return -1;
	} else if (x1 == x2) {
		return 0;
	} else {
		return 1;
	}
}

int
main(int argc, char *argv[])
{
	if (argc != 2) {
		fprintf(stderr, "usage: test_cycle [ 0 | 1 | 3 ]\n");
		exit(EXIT_FAILURE);
	}
	int st = atoi(argv[1]);

	uint64_t tt[100];

	/* 32-bit multiplications. */
	uint32_t x32 = (uint32_t)st;
	uint32_t y32 = x32;
	for (int i = 0; i < 100; i ++) {
		y32 *= x32;
	}
	x32 = y32;
	for (size_t i = 0; i < 120; i ++) {
		uint64_t begin = core_cycles();
		for (int j = 0; j < 1000; j ++) {
			x32 *= y32;
			y32 *= x32;
			x32 *= y32;
			y32 *= x32;
			x32 *= y32;
			y32 *= x32;
			x32 *= y32;
			y32 *= x32;
			x32 *= y32;
			y32 *= x32;
			x32 *= y32;
			y32 *= x32;
			x32 *= y32;
			y32 *= x32;
			x32 *= y32;
			y32 *= x32;
			x32 *= y32;
			y32 *= x32;
			x32 *= y32;
			y32 *= x32;
		}
		uint64_t end = core_cycles();
		if (i >= 20) {
			tt[i - 20] = end - begin;
		}
	}
	qsort(tt, 100, sizeof(uint64_t), &cmp_u64);
	printf("32x32->32 muls:  %7.3f\n", (double)tt[50] / 20000.0);

	/* 64-bit multiplications. */
	uint64_t x64 = (uint64_t)x32;
	x64 *= x64 * x64;
	uint64_t y64 = x64;
	for (size_t i = 0; i < 120; i ++) {
		uint64_t begin = core_cycles();
		for (int j = 0; j < 1000; j ++) {
			x64 *= y64;
			y64 *= x64;
			x64 *= y64;
			y64 *= x64;
			x64 *= y64;
			y64 *= x64;
			x64 *= y64;
			y64 *= x64;
			x64 *= y64;
			y64 *= x64;
			x64 *= y64;
			y64 *= x64;
			x64 *= y64;
			y64 *= x64;
			x64 *= y64;
			y64 *= x64;
			x64 *= y64;
			y64 *= x64;
			x64 *= y64;
			y64 *= x64;
		}
		uint64_t end = core_cycles();
		if (i >= 20) {
			tt[i - 20] = end - begin;
		}
	}
	qsort(tt, 100, sizeof(uint64_t), &cmp_u64);
	printf("64x64->64 muls:  %7.3f\n", (double)tt[50] / 20000.0);

#if (defined __GNUC__ || defined __clang) && defined __SIZEOF_INT128__
	/* 64x64->128 multiplications.
	   We really measure latency to access to the upper half of the
	   result. To avoid the value to become too small, we ensure
	   every 20 operations that the top bit is set (unless source
	   was 0 or 1). */
	uint64_t t64 = (uint64_t)((y64 >> 1) != 0) << 63;
	x64 |= t64;
	y64 |= t64;
	uint64_t x64orig = x64;
	uint64_t y64orig = y64;
	for (size_t i = 0; i < 120; i ++) {
		uint64_t begin = core_cycles();
		for (int j = 0; j < 1000; j ++) {
			x64 ^= x64orig;
			y64 ^= y64orig;
			x64 = ((unsigned __int128)x64 * y64) >> 64;
			y64 = ((unsigned __int128)y64 * x64) >> 64;
			x64 = ((unsigned __int128)x64 * y64) >> 64;
			y64 = ((unsigned __int128)y64 * x64) >> 64;
			x64 = ((unsigned __int128)x64 * y64) >> 64;
			y64 = ((unsigned __int128)y64 * x64) >> 64;
			x64 = ((unsigned __int128)x64 * y64) >> 64;
			y64 = ((unsigned __int128)y64 * x64) >> 64;
		}
		uint64_t end = core_cycles();
		if (i >= 20) {
			tt[i - 20] = end - begin;
		}
	}
	qsort(tt, 100, sizeof(uint64_t), &cmp_u64);
	printf("64x64->128 muls: %7.3f\n", (double)tt[50] / 8000.0);
#endif

	/* Get some bytes from the final value and print them out; this
	   should prevent the compiler from optimizing away the
	   multiplications. */
	unsigned x = 0;
	for (int i = 0; i < 8; i ++) {
		x ^= (unsigned)x64;
		x64 >>= 8;
	}
	printf("(%u)\n", x & 0xFF);
	return 0;
}
