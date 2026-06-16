/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * minimem_lzsse8.c — LZSSE8 wrapper for MiniMem
 *
 * LZSSE8 is an SSE4.1-optimized LZ77 compressor with ~4.7 GB/s
 * decompression speed. This wrapper provides:
 *   - Runtime SSE4.1 feature detection
 *   - Fallback (returns 0 / false) when SSE4.1 is unavailable
 *   - MiniMem compressor API integration
 *
 * LZSSE8 source: https://github.com/ConorStokes/LZSSE
 * License: BSD-3-Clause (see vendor/lzsse8/LICENSE)
 */

#if defined(MINIMEM_KERNEL)

#include "lib/compressors/lzsse8.h"

size_t minimem_lzsse8_compress(const uint8_t *src, size_t src_len,
			       uint8_t *dst, size_t dst_cap)
{
	(void)src; (void)src_len; (void)dst; (void)dst_cap;
	return 0;
}

size_t minimem_lzsse8_decompress(const uint8_t *src, size_t src_len,
				 uint8_t *dst, size_t dst_cap)
{
	(void)src; (void)src_len; (void)dst; (void)dst_cap;
	return 0;
}

size_t minimem_lzsse8_compress_bound(size_t src_len)
{
	return src_len + src_len / 16 + 64;
}

bool minimem_lzsse8_can_compress(const uint8_t *src, size_t src_len)
{
	(void)src; (void)src_len;
	return false;
}

#else /* Userspace */

#include "lib/minimem.h"
#include "lib/advisor.h"
#include "lib/compressors/lzsse8.h"

#if defined(__x86_64__) || defined(_M_X64)

static bool sse41_available(void)
{
	unsigned int eax, ebx, ecx, edx;
	__asm__ __volatile__(
		"cpuid"
		: "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
		: "a"(1)
	);
	return (ecx & (1 << 19)) != 0;
}

static bool minimem_lzsse8_available;
static bool minimem_lzsse8_checked;

static bool lzsse8_is_available(void)
{
	if (!minimem_lzsse8_checked) {
		minimem_lzsse8_available = sse41_available();
		minimem_lzsse8_checked = true;
	}
	return minimem_lzsse8_available;
}

extern void *minimem_lzsse8_make_parse_state(void);
extern size_t minimem_lzsse8_compress_fast(void *state,
	const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_cap);
extern size_t minimem_lzsse8_decompress_raw(const uint8_t *src,
	size_t src_len, uint8_t *dst, size_t dst_cap);

static void *lzsse8_parse_state;

static void lzsse8_init(void)
{
	if (!lzsse8_parse_state)
		lzsse8_parse_state = minimem_lzsse8_make_parse_state();
}

size_t minimem_lzsse8_compress(const uint8_t *src, size_t src_len,
			       uint8_t *dst, size_t dst_cap)
{
	if (!lzsse8_is_available())
		return 0;

	if (dst_cap < src_len)
		return 0;

	lzsse8_init();
	if (!lzsse8_parse_state)
		return 0;

	size_t result = minimem_lzsse8_compress_fast(lzsse8_parse_state,
						     src, src_len,
						     dst, dst_cap);
	if (result == 0 || result >= src_len)
		return 0;

	return result;
}

size_t minimem_lzsse8_decompress(const uint8_t *src, size_t src_len,
				 uint8_t *dst, size_t dst_cap)
{
	if (!lzsse8_is_available())
		return 0;

	size_t result = minimem_lzsse8_decompress_raw(src, src_len,
						       dst, dst_cap);
	if (result != dst_cap)
		return 0;

	return result;
}

#else /* Not x86-64 */

static bool lzsse8_is_available(void) { return false; }

size_t minimem_lzsse8_compress(const uint8_t *src, size_t src_len,
			       uint8_t *dst, size_t dst_cap)
{
	(void)src; (void)src_len; (void)dst; (void)dst_cap;
	return 0;
}

size_t minimem_lzsse8_decompress(const uint8_t *src, size_t src_len,
				 uint8_t *dst, size_t dst_cap)
{
	(void)src; (void)src_len; (void)dst; (void)dst_cap;
	return 0;
}

#endif /* __x86_64__ */

size_t minimem_lzsse8_compress_bound(size_t src_len)
{
	return src_len + src_len / 16 + 64;
}

bool minimem_lzsse8_can_compress(const uint8_t *src, size_t src_len)
{
	if (!lzsse8_is_available())
		return false;

	struct minimem_page_stats stats = minimem_analyze_page(src, src_len);
	return minimem_advise_algorithm(&stats) == MINIMEM_ALGO_LZSSE8;
}

const struct minimem_compressor minimem_lzsse8_compressor = {
	.name = "lzsse8",
	.id = MINIMEM_ALGO_LZSSE8,
	.compress = minimem_lzsse8_compress,
	.decompress = minimem_lzsse8_decompress,
	.compress_bound = minimem_lzsse8_compress_bound,
	.can_compress = minimem_lzsse8_can_compress,
};

#endif /* MINIMEM_KERNEL */