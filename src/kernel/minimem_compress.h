/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * minimem_compress.h — In-kernel compression dispatch for MiniMem
 *
 * Provides per-CPU buffer compression/decompression with algorithm
 * selection via the advisor. No dynamic allocation in hot paths.
 */

#ifndef MINIMEM_KERNEL_COMPRESS_H
#define MINIMEM_KERNEL_COMPRESS_H

#include <linux/types.h>
#include <linux/sizes.h>

#define MINIMEM_PAGE_SIZE	4096
#define MINIMEM_CACHE_LINE_SIZE	64

#define MINIMEM_ALGO_SAME_PAGE	0
#define MINIMEM_ALGO_BDI	1
#define MINIMEM_ALGO_WKDM	2
#define MINIMEM_ALGO_LZ4	3
#define MINIMEM_ALGO_LZSSE8	4
#define MINIMEM_ALGO_ZSTD_DICT	5
#define MINIMEM_ALGO_DELTA	6
#define MINIMEM_ALGO_WKDM64	7
#define MINIMEM_ALGO_BLOCK_CLASS 8
#define MINIMEM_ALGO_COUNT	9

#define MINIMEM_OK		0
#define MINIMEM_ERROR		(-1)
#define MINIMEM_NOSPACE		(-2)
#define MINIMEM_INCOMPRESSIBLE	(-3)

struct minimem_compress_result {
	int algo_id;
	size_t original_size;
	size_t compressed_size;
	u64 compress_ns;
};

struct minimem_decompress_result {
	size_t decompressed_size;
	u64 decompress_ns;
};

/*
 * Initialize per-CPU compression buffers. Must be called from module init.
 * Returns 0 on success, negative errno on failure.
 */
int minimem_compress_init(void);

/*
 * Free per-CPU compression buffers. Must be called from module exit.
 */
void minimem_compress_exit(void);

/*
 * Compress a page using the best algorithm selected by the advisor.
 * Uses per-CPU buffers — must be called with preemption enabled.
 * Returns 0 on success, stores result in @res.
 * Returns MINIMEM_INCOMPRESSIBLE if no algorithm can compress the page.
 */
int minimem_compress_page(const void *src, size_t src_len,
			  struct minimem_compress_result *res);

/*
 * Compress a page using a specific algorithm.
 * Uses per-CPU buffers — must be called with preemption enabled.
 */
int minimem_compress_page_algo(const void *src, size_t src_len,
			       int algo_id,
			       struct minimem_compress_result *res);

/*
 * Decompress a previously compressed page.
 * Uses per-CPU buffers — must be called with preemption enabled.
 * @algo_id must match the algorithm used to compress.
 * @compressed_len is the size returned in compress_result.compressed_size.
 */
int minimem_decompress_page(const void *compressed, size_t compressed_len,
			    int algo_id, void *dst, size_t dst_len,
			    struct minimem_decompress_result *res);

/*
 * Quick check: can this page potentially be compressed?
 * Returns the recommended algorithm ID, or -1 if the page is incompressible.
 */
int minimem_classify_page(const void *src, size_t src_len);

/*
 * Get the compression buffer for the current CPU.
 * The buffer is at least MINIMEM_COMPRESS_BUF_SIZE bytes.
 * Caller must not sleep while using this buffer.
 */
void *minimem_get_compress_buf(void);
void *minimem_get_decompress_buf(void);

#define MINIMEM_COMPRESS_BUF_SIZE	(SZ_8K)
#define MINIMEM_DECOMPRESS_BUF_SIZE	(SZ_8K)

#endif /* MINIMEM_KERNEL_COMPRESS_H */