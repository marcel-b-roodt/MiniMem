/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * minimem_parallel.c — Parallel cluster decompression for MiniMem
 *
 * Uses a dedicated workqueue (minimem_wq) to decompress clusters of
 * pages in parallel. Each page gets a work item. An atomic counter
 * tracks remaining work; the caller blocks on a completion until
 * all items finish.
 *
 * Performance target: 4.5-7× latency reduction on 32-page clusters
 * vs serial decompression.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/cpumask.h>
#include <linux/workqueue.h>
#include <linux/completion.h>
#include <linux/atomic.h>
#include <linux/ktime.h>
#include <linux/highmem.h>
#include <linux/mm.h>
#include <linux/zsmalloc.h>

#include "minimem_parallel.h"
#include "minimem_compress.h"
#include "minimem_map.h"
#include "minimem_zswap.h"

#define MINIMEM_MAX_CLUSTER 32

static atomic_t parallel_mode = ATOMIC_INIT(MINIMEM_PARALLEL_AUTO);

struct minimem_cluster_ctx;

struct minimem_cluster_work {
	struct work_struct work;
	unsigned long vaddr;
	struct page *page;
	struct minimem_map_entry entry;
	u64 decompress_ns;
	int status;
	void *decompress_buf;
	struct minimem_cluster_ctx *ctx;
};

struct minimem_cluster_ctx {
	struct completion done;
	atomic_t remaining;
	int first_error;
	unsigned int count;
	struct minimem_cluster_work items[];
};

static struct workqueue_struct *minimem_wq;

struct minimem_parallel_stats minimem_par_stats;

static void minimem_cluster_worker(struct work_struct *work)
{
	struct minimem_cluster_work *item;
	struct minimem_cluster_ctx *ctx;
	struct minimem_decompress_result dres;
	struct zs_pool *pool;
	void *local_buf, *dst_addr;
	int ret;

	item = container_of(work, struct minimem_cluster_work, work);
	ctx = item->ctx;
	pool = minimem_zswap_pool();

	if (!pool) {
		item->status = -ENODEV;
		goto out;
	}

	local_buf = kmalloc(item->entry.compressed_len, GFP_ATOMIC);
	if (!local_buf) {
		item->status = -ENOMEM;
		goto out;
	}

	zs_obj_read_begin(pool, item->entry.zs_handle, local_buf);

	if (item->page) {
		dst_addr = kmap_local_page(item->page);
		if (!dst_addr) {
			zs_obj_read_end(pool, item->entry.zs_handle, local_buf);
			kfree(local_buf);
			item->status = -EFAULT;
			goto out;
		}
	} else {
		dst_addr = item->decompress_buf;
	}

	ret = minimem_decompress_page(local_buf, item->entry.compressed_len,
				      item->entry.algo_id, dst_addr,
				      MINIMEM_PAGE_SIZE, &dres);

	if (item->page)
		kunmap_local(dst_addr);

	zs_obj_read_end(pool, item->entry.zs_handle, local_buf);
	kfree(local_buf);

	item->status = ret;
	item->decompress_ns = dres.decompress_ns;

	atomic64_inc(&minimem_par_stats.pages_decompressed);
	atomic64_add(dres.decompress_ns, &minimem_par_stats.total_ns);

out:
	if (atomic_dec_and_test(&ctx->remaining))
		complete(&ctx->done);
}

int minimem_parallel_get_mode(void)
{
	return atomic_read(&parallel_mode);
}

void minimem_parallel_set_mode(int mode)
{
	if (mode < MINIMEM_PARALLEL_DISABLED || mode > MINIMEM_PARALLEL_AUTO)
		return;
	atomic_set(&parallel_mode, mode);
}

bool minimem_parallel_is_active(void)
{
	int mode = atomic_read(&parallel_mode);

	if (mode == MINIMEM_PARALLEL_DISABLED)
		return false;
	if (mode == MINIMEM_PARALLEL_ENABLED)
		return true;
	/* MINIMEM_PARALLEL_AUTO: enable when >= 2 CPUs available */
	return num_online_cpus() >= 2;
}

int minimem_parallel_init(void)
{
	minimem_wq = alloc_workqueue("minimem_dec",
				     WQ_MEM_RECLAIM | WQ_HIGHPRI, 0);
	if (!minimem_wq)
		return -ENOMEM;

	atomic64_set(&minimem_par_stats.clusters_decompressed, 0);
	atomic64_set(&minimem_par_stats.pages_decompressed, 0);
	atomic64_set(&minimem_par_stats.total_ns, 0);

	return 0;
}

void minimem_parallel_exit(void)
{
	if (minimem_wq) {
		drain_workqueue(minimem_wq);
		destroy_workqueue(minimem_wq);
		minimem_wq = NULL;
	}
}

int minimem_parallel_decompress(unsigned long *vaddrs,
				struct page **pages,
				unsigned int count,
				struct minimem_decompress_result_km *results)
{
	struct minimem_cluster_ctx *ctx;
	struct minimem_map *map;
	unsigned int i;
	int ret = 0;

	if (count == 0 || count > MINIMEM_MAX_CLUSTER)
		return -EINVAL;

	if (!minimem_wq || !minimem_parallel_is_active())
		return -ENODEV;

	map = minimem_zswap_map();

	ctx = kzalloc(struct_size(ctx, items, count), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	init_completion(&ctx->done);
	atomic_set(&ctx->remaining, 0);
	ctx->first_error = 0;
	ctx->count = count;

	for (i = 0; i < count; i++) {
		int err;

		ctx->items[i].ctx = ctx;

		err = minimem_map_lookup(map, vaddrs[i], &ctx->items[i].entry);
		if (err) {
			ctx->items[i].status = err;
			ctx->items[i].decompress_buf = NULL;
			if (ctx->first_error == 0)
				ctx->first_error = err;
			continue;
		}

		INIT_WORK(&ctx->items[i].work, minimem_cluster_worker);
		ctx->items[i].vaddr = vaddrs[i];
		ctx->items[i].page = pages ? pages[i] : NULL;

		if (!ctx->items[i].page) {
			ctx->items[i].decompress_buf =
				kmalloc(MINIMEM_PAGE_SIZE, GFP_KERNEL);
			if (!ctx->items[i].decompress_buf) {
				ctx->items[i].status = -ENOMEM;
				if (ctx->first_error == 0)
					ctx->first_error = -ENOMEM;
				continue;
			}
		}

		atomic_inc(&ctx->remaining);
		queue_work(minimem_wq, &ctx->items[i].work);
	}

	if (atomic_read(&ctx->remaining) > 0)
		wait_for_completion(&ctx->done);

	for (i = 0; i < count; i++) {
		kfree(ctx->items[i].decompress_buf);
		if (results) {
			results[i].vaddr = ctx->items[i].vaddr;
			results[i].algo_id = ctx->items[i].entry.algo_id;
			results[i].compressed_len =
				ctx->items[i].entry.compressed_len;
			results[i].zs_handle = ctx->items[i].entry.zs_handle;
			results[i].status = ctx->items[i].status;
			results[i].decompress_ns =
				ctx->items[i].decompress_ns;
		}
		if (ctx->items[i].status && ctx->first_error == 0)
			ctx->first_error = ctx->items[i].status;
	}

	atomic64_inc(&minimem_par_stats.clusters_decompressed);

	ret = ctx->first_error;
	kfree(ctx);

	return ret;
}