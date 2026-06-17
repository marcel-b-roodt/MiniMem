/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * minimem_parallel.h — Parallel cluster decompression for MiniMem
 *
 * Decompresses clusters of pages in parallel using per-CPU workqueues.
 * When swap readahead brings in a batch of pages, MiniMem dispatches
 * decompression work items to a workqueue. Each worker decompresses
 * one page into a pre-allocated page frame. After all workers complete,
 * PTEs are batch-updated with a single TLB flush.
 *
 * Performance target: 4.5-7× latency reduction on 32-page clusters.
 * Safety: Each page is independently compressed (per-page compressors),
 * so concurrent decompression needs no read-side locking on data.
 */

#ifndef MINIMEM_KERNEL_PARALLEL_H
#define MINIMEM_KERNEL_PARALLEL_H

#include <linux/types.h>
#include <linux/completion.h>
#include <linux/atomic.h>

/*
 * Initialize the parallel decompression workqueue.
 * Returns 0 on success, negative errno on failure.
 */
int minimem_parallel_init(void);

/*
 * Destroy the parallel decompression workqueue.
 * Waits for all in-flight work to complete.
 */
void minimem_parallel_exit(void);

/*
 * Result structure for a single page decompression.
 * Used by the work item to report success/failure.
 */
struct minimem_decompress_result_km {
	unsigned long vaddr;
	int algo_id;
	size_t compressed_len;
	unsigned long zs_handle;
	int status;
	u64 decompress_ns;
};

/*
 * Submit a cluster of compressed pages for parallel decompression.
 * Blocks until all pages are decompressed (or errored).
 *
 * @vaddrs:    Array of page-aligned virtual addresses (compressed pages)
 * @pages:     Array of struct page pointers to write decompressed data
 * @count:     Number of pages in the cluster (1-32)
 * @results:   Optional array to receive per-page results (may be NULL)
 * @returns:   0 on success, negative errno on first error
 */
int minimem_parallel_decompress(unsigned long *vaddrs,
				struct page **pages,
				unsigned int count,
				struct minimem_decompress_result_km *results);

/*
 * Stats for parallel decompression.
 */
struct minimem_parallel_stats {
	atomic64_t clusters_decompressed;
	atomic64_t pages_decompressed;
	atomic64_t total_ns;
};

extern struct minimem_parallel_stats minimem_par_stats;

/*
 * Parallel decompression mode:
 *   0 = disabled (always serial)
 *   1 = enabled (always parallel)
 *   2 = auto (enabled when num_online_cpus >= 2, else serial)
 * Default: auto (2)
 */
#define MINIMEM_PARALLEL_DISABLED	0
#define MINIMEM_PARALLEL_ENABLED	1
#define MINIMEM_PARALLEL_AUTO		2

int minimem_parallel_get_mode(void);
void minimem_parallel_set_mode(int mode);
bool minimem_parallel_is_active(void);

#endif /* MINIMEM_KERNEL_PARALLEL_H */