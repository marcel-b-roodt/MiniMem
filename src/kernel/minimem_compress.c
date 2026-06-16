/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * minimem_compress.c — In-kernel compression dispatch for MiniMem
 *
 * Adapts the userspace algorithm library for kernel context:
 * - Per-CPU scratch buffers (no malloc in hot paths)
 * - Kernel-compatible memory operations
 * - Advisor-based algorithm selection
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/percpu.h>
#include <linux/preempt.h>
#include <linux/ktime.h>
#include <linux/string.h>
#include <linux/bug.h>

#include "minimem_compress.h"

/* Forward declarations for algorithm functions compiled into this module */
extern size_t minimem_bdi_compress(const uint8_t *src, size_t src_len,
				   uint8_t *dst, size_t dst_cap);
extern size_t minimem_bdi_decompress(const uint8_t *src, size_t src_len,
				     uint8_t *dst, size_t dst_cap);
extern size_t minimem_wkdm_compress(const uint8_t *src, size_t src_len,
				     uint8_t *dst, size_t dst_cap);
extern size_t minimem_wkdm_decompress(const uint8_t *src, size_t src_len,
				       uint8_t *dst, size_t dst_cap);
extern size_t minimem_wkdm64_compress(const uint8_t *src, size_t src_len,
				       uint8_t *dst, size_t dst_cap);
extern size_t minimem_wkdm64_decompress(const uint8_t *src, size_t src_len,
					 uint8_t *dst, size_t dst_cap);
extern size_t minimem_block_class_compress(const uint8_t *src, size_t src_len,
					    uint8_t *dst, size_t dst_cap);
extern size_t minimem_block_class_decompress(const uint8_t *src, size_t src_len,
					     uint8_t *dst, size_t dst_cap);
extern size_t minimem_lz4_compress(const uint8_t *src, size_t src_len,
				     uint8_t *dst, size_t dst_cap);
extern size_t minimem_lz4_decompress(const uint8_t *src, size_t src_len,
				       uint8_t *dst, size_t dst_cap);

/* ---- Per-CPU buffers ---- */

DEFINE_PER_CPU(struct minimem_cpu_buf, minimem_cpu_bufs);

/* ---- Kernel-compatible algorithm wrappers ---- */

/*
 * The kernel module links the same algorithm source files as the
 * userspace library, but with kernel headers. The algorithm files
 * use only memcpy/memset and stack variables in their hot paths,
 * so they compile cleanly for both environments.
 *
 * We include the algorithm headers and call their functions directly.
 * The kernel module object links with the compiled algorithm objects.
 */

/* Same-page detection — inlined for speed */
static size_t km_same_page_compress(const void *src, size_t src_len,
				    void *dst, size_t dst_cap)
{
	const uint8_t *s = src;
	uint8_t *d = dst;
	size_t i;

	if (src_len == 0)
		return 0;

	for (i = 1; i < src_len; i++) {
		if (s[i] != s[0])
			return 0;
	}

	if (dst_cap < 1)
		return 0;

	d[0] = s[0];
	return 1;
}

static size_t km_same_page_decompress(const void *src, size_t src_len,
				      void *dst, size_t dst_cap)
{
	const uint8_t *s = src;
	uint8_t *d = dst;

	if (src_len < 1 || dst_cap == 0)
		return 0;

	memset(d, s[0], dst_cap);
	return dst_cap;
}

static bool km_same_page_can_compress(const void *src, size_t src_len)
{
	const uint8_t *s = src;
	size_t i;

	if (src_len == 0)
		return false;

	for (i = 1; i < src_len; i++) {
		if (s[i] != s[0])
			return false;
	}
	return true;
}

/* ---- Page classification (simplified advisor for kernel) ---- */

static int km_classify_page(const void *src, size_t src_len)
{
	const uint8_t *data = src;
	size_t zero_bytes = 0;
	size_t upper32_zero = 0;
	size_t n_words64;
	uint8_t min_byte = 0xFF, max_byte = 0x00;
	size_t i;

	if (src_len != MINIMEM_PAGE_SIZE || src_len == 0)
		return MINIMEM_ALGO_LZ4;

	/* Fast zero-byte count */
	for (i = 0; i < src_len; i++) {
		if (data[i] == 0)
			zero_bytes++;
		if (data[i] < min_byte)
			min_byte = data[i];
		if (data[i] > max_byte)
			max_byte = data[i];
	}

	/* Same-page: all bytes identical */
	if (zero_bytes == src_len || km_same_page_can_compress(src, src_len))
		return MINIMEM_ALGO_SAME_PAGE;

	/* Mostly zeros: BDI or WKdm */
	if (zero_bytes * 256 / src_len >= 128) {
		/* Check if cache-line structured (BDI territory) */
		if (max_byte - min_byte <= 32)
			return MINIMEM_ALGO_BDI;
		return MINIMEM_ALGO_WKDM;
	}

	/* Upper 32 bits zero (pointer-heavy): WKdm-64 */
	n_words64 = src_len / sizeof(uint64_t);
	if (n_words64 > 0) {
		const uint64_t *words = (const uint64_t *)data;
		for (i = 0; i < n_words64; i++) {
			if ((words[i] & 0xFFFFFFFF00000000ULL) == 0)
				upper32_zero++;
		}
		if (upper32_zero > n_words64 / 2)
			return MINIMEM_ALGO_WKDM64;
	}

	/* Default: LZ4 for general data */
	return MINIMEM_ALGO_LZ4;
}

/* ---- Compress / Decompress dispatch ---- */

/*
 * These functions will be replaced by direct calls to the algorithm
 * library once the objects are linked into the module. For now,
 * they serve as the kernel entry points that will dispatch to
 * same_page_compress, wkdm_compress, lz4_compress, etc.
 *
 * The actual algorithm functions are compiled from the same source
 * files in src/lib/compressors/ but with kernel includes.
 */

int minimem_compress_page(const void *src, size_t src_len,
			  struct minimem_compress_result *res)
{
	int algo_id;
	ktime_t start;
	size_t bound, csz;
	void *buf;

	if (!src || !res || src_len != MINIMEM_PAGE_SIZE)
		return MINIMEM_ERROR;

	preempt_disable();
	buf = this_cpu_ptr(&minimem_cpu_bufs)->compress_buf;
	if (!buf) {
		preempt_enable();
		return MINIMEM_ERROR;
	}

	algo_id = km_classify_page(src, src_len);

	start = ktime_get_ns();
	bound = MINIMEM_COMPRESS_BUF_SIZE;

	switch (algo_id) {
	case MINIMEM_ALGO_SAME_PAGE:
		csz = km_same_page_compress(src, src_len, buf, bound);
		break;
	case MINIMEM_ALGO_BDI:
		csz = minimem_bdi_compress(src, src_len, buf, bound);
		break;
	case MINIMEM_ALGO_WKDM:
		csz = minimem_wkdm_compress(src, src_len, buf, bound);
		break;
	case MINIMEM_ALGO_WKDM64:
		csz = minimem_wkdm64_compress(src, src_len, buf, bound);
		break;
	case MINIMEM_ALGO_BLOCK_CLASS:
		csz = minimem_block_class_compress(src, src_len, buf, bound);
		break;
	case MINIMEM_ALGO_LZ4:
		csz = minimem_lz4_compress(src, src_len, buf, bound);
		break;
	default:
		csz = 0;
		break;
	}

	res->algo_id = algo_id;
	res->original_size = src_len;
	res->compressed_size = csz;
	res->compress_ns = (u64)(ktime_get_ns() - start);

	preempt_enable();

	if (csz == 0)
		return MINIMEM_INCOMPRESSIBLE;

	return MINIMEM_OK;
}

int minimem_decompress_page(const void *compressed, size_t compressed_len,
			    int algo_id, void *dst, size_t dst_len,
			    struct minimem_decompress_result *res)
{
	ktime_t start;
	size_t dsz;

	if (!compressed || !dst || dst_len != MINIMEM_PAGE_SIZE)
		return MINIMEM_ERROR;

	start = ktime_get_ns();

	switch (algo_id) {
	case MINIMEM_ALGO_SAME_PAGE:
		dsz = km_same_page_decompress(compressed, compressed_len,
					      dst, dst_len);
		break;
	case MINIMEM_ALGO_BDI:
		dsz = minimem_bdi_decompress(compressed, compressed_len,
					      dst, dst_len);
		break;
	case MINIMEM_ALGO_WKDM:
		dsz = minimem_wkdm_decompress(compressed, compressed_len,
					       dst, dst_len);
		break;
	case MINIMEM_ALGO_WKDM64:
		dsz = minimem_wkdm64_decompress(compressed, compressed_len,
					        dst, dst_len);
		break;
	case MINIMEM_ALGO_BLOCK_CLASS:
		dsz = minimem_block_class_decompress(compressed, compressed_len,
						     dst, dst_len);
		break;
	case MINIMEM_ALGO_LZ4:
		dsz = minimem_lz4_decompress(compressed, compressed_len,
					     dst, dst_len);
		break;
	default:
		return MINIMEM_ERROR;
	}

	res->decompressed_size = dsz;
	res->decompress_ns = (u64)(ktime_get_ns() - start);

	if (dsz != dst_len)
		return MINIMEM_ERROR;

	return MINIMEM_OK;
}

int minimem_classify_page(const void *src, size_t src_len)
{
	return km_classify_page(src, src_len);
}

int minimem_compress_init(void)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		struct minimem_cpu_buf *buf = per_cpu_ptr(&minimem_cpu_bufs, cpu);

		buf->compress_buf = kmalloc_node(MINIMEM_COMPRESS_BUF_SIZE,
						 GFP_KERNEL, cpu_to_node(cpu));
		if (!buf->compress_buf)
			goto fail;

		buf->decompress_buf = kmalloc_node(MINIMEM_DECOMPRESS_BUF_SIZE,
						    GFP_KERNEL, cpu_to_node(cpu));
		if (!buf->decompress_buf) {
			kfree(buf->compress_buf);
			buf->compress_buf = NULL;
			goto fail;
		}
	}

	pr_info("minimem: per-CPU buffers allocated for %d CPUs\n",
		num_possible_cpus());
	return 0;

fail:
	for_each_possible_cpu(cpu) {
		struct minimem_cpu_buf *buf = per_cpu_ptr(&minimem_cpu_bufs, cpu);
		kfree(buf->compress_buf);
		buf->compress_buf = NULL;
		kfree(buf->decompress_buf);
		buf->decompress_buf = NULL;
	}
	return -ENOMEM;
}

void minimem_compress_exit(void)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		struct minimem_cpu_buf *buf = per_cpu_ptr(&minimem_cpu_bufs, cpu);
		kfree(buf->compress_buf);
		buf->compress_buf = NULL;
		kfree(buf->decompress_buf);
		buf->decompress_buf = NULL;
	}
}