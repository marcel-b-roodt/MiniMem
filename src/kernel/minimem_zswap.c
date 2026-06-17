/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * minimem_zswap.c — zsmalloc-backed compressed page storage for MiniMem
 *
 * Manages compressed page storage in a zsmalloc pool. Each page is
 * compressed using the advisor-selected algorithm, stored in zsmalloc,
 * and tracked in the compression map (xarray).
 *
 * Uses the 6.x zsmalloc API:
 *   zs_malloc(pool, size, flags, nid)        — allocate
 *   zs_obj_write(pool, handle, buf, len)     — write compressed data
 *   zs_obj_read_begin/end(pool, handle, buf) — read compressed data
 *   zs_free(pool, handle)                    — free
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/highmem.h>
#include <linux/zsmalloc.h>
#include <linux/ktime.h>
#include <linux/cpumask.h>

#include "minimem_zswap.h"
#include "minimem_compress.h"
#include "minimem_map.h"
#include "minimem_scanner.h"
#include "minimem_proc_stats.h"

static struct zs_pool *minimem_pool;
static struct minimem_map minimem_map;

static atomic64_t mm_max_pool_pages = ATOMIC64_INIT(0);
static atomic64_t mm_stored_pages;
static atomic64_t mm_total_bytes;
static atomic64_t mm_bytes_saved;

int minimem_zswap_init(void)
{
	minimem_pool = zs_create_pool("minimem");
	if (!minimem_pool)
		return -ENOMEM;

	atomic64_set(&mm_stored_pages, 0);
	atomic64_set(&mm_total_bytes, 0);
	atomic64_set(&mm_bytes_saved, 0);

	return minimem_map_init(&minimem_map);
}

void minimem_zswap_exit(void)
{
	minimem_map_remove_all(&minimem_map);
	minimem_map_destroy(&minimem_map);

	if (minimem_pool) {
		zs_destroy_pool(minimem_pool);
		minimem_pool = NULL;
	}
}

struct zs_pool *minimem_zswap_pool(void)
{
	return minimem_pool;
}

struct minimem_map *minimem_zswap_map(void)
{
	return &minimem_map;
}

int minimem_compress_and_store(unsigned long vaddr, struct page *page)
{
	struct minimem_compress_result res;
	void *src_addr, *dst_buf, *local_buf;
	unsigned long handle;
	size_t copy_len;
	int ret, nid;

	if (!minimem_pool || !page)
		return -EINVAL;

	if (atomic64_read(&mm_max_pool_pages) > 0 &&
	    atomic64_read(&mm_stored_pages) >= atomic64_read(&mm_max_pool_pages))
		return -ENOSPC;

	src_addr = kmap_local_page(page);
	if (!src_addr)
		return -EFAULT;

	preempt_disable();
	dst_buf = minimem_get_compress_buf();
	if (!dst_buf) {
		preempt_enable();
		kunmap_local(src_addr);
		return -EINVAL;
	}

	ret = minimem_compress_page(src_addr, MINIMEM_PAGE_SIZE, &res);
	if (ret != MINIMEM_OK) {
		preempt_enable();
		kunmap_local(src_addr);
		if (ret == MINIMEM_INCOMPRESSIBLE)
			return MINIMEM_INCOMPRESSIBLE;
		return ret;
	}

	if (res.compressed_size == 0 ||
	    res.compressed_size >= MINIMEM_PAGE_SIZE) {
		preempt_enable();
		kunmap_local(src_addr);
		return MINIMEM_INCOMPRESSIBLE;
	}

	{
		long min_pct = minimem_scanner_min_savings_pct();
		long savings_pct = (100L * (MINIMEM_PAGE_SIZE - res.compressed_size))
				   / MINIMEM_PAGE_SIZE;

		if (savings_pct < min_pct) {
			preempt_enable();
			kunmap_local(src_addr);
			return MINIMEM_INCOMPRESSIBLE;
		}
	}

	copy_len = res.compressed_size;

	local_buf = kmalloc(copy_len, GFP_ATOMIC);
	if (!local_buf) {
		preempt_enable();
		kunmap_local(src_addr);
		return -ENOMEM;
	}
	memcpy(local_buf, dst_buf, copy_len);
	preempt_enable();
	kunmap_local(src_addr);

	nid = page_to_nid(page);
	handle = zs_malloc(minimem_pool, copy_len, GFP_NOIO, nid);
	if (!handle) {
		kfree(local_buf);
		return -ENOMEM;
	}

	zs_obj_write(minimem_pool, handle, local_buf, copy_len);
	kfree(local_buf);

	ret = minimem_map_store(&minimem_map, vaddr, res.algo_id,
				res.compressed_size, handle);
	if (ret) {
		zs_free(minimem_pool, handle);
		return ret;
	}

	atomic64_inc(&mm_stored_pages);
	atomic64_add(res.compressed_size, &mm_total_bytes);
	atomic64_add(res.original_size - res.compressed_size,
		     &mm_bytes_saved);

	minimem_proc_stats_compress(res.original_size - res.compressed_size,
				    res.compress_ns);

	return MINIMEM_OK;
}

int minimem_decompress_and_restore(unsigned long vaddr, struct page *page)
{
	struct minimem_map_entry entry;
	struct minimem_decompress_result dres;
	void *local_buf, *dst_addr;
	unsigned long handle;
	int ret;

	if (!minimem_pool)
		return -EINVAL;

	ret = minimem_map_lookup(&minimem_map, vaddr, &entry);
	if (ret)
		return ret;

	handle = entry.zs_handle;

	local_buf = kmalloc(entry.compressed_len, GFP_KERNEL);
	if (!local_buf)
		return -ENOMEM;

	zs_obj_read_begin(minimem_pool, handle, local_buf);

	if (page) {
		dst_addr = kmap_local_page(page);
		if (!dst_addr) {
			zs_obj_read_end(minimem_pool, handle, local_buf);
			kfree(local_buf);
			return -EFAULT;
		}
	} else {
		preempt_disable();
		dst_addr = minimem_get_decompress_buf();
	}

	ret = minimem_decompress_page(local_buf, entry.compressed_len,
				      entry.algo_id, dst_addr,
				      MINIMEM_PAGE_SIZE, &dres);

	if (page)
		kunmap_local(dst_addr);
	else
		preempt_enable();

	zs_obj_read_end(minimem_pool, handle, local_buf);
	kfree(local_buf);

	if (ret != MINIMEM_OK)
		return ret;

	minimem_proc_stats_decompress(dres.decompress_ns);

	minimem_map_remove(&minimem_map, vaddr);
	zs_free(minimem_pool, handle);

	atomic64_dec(&mm_stored_pages);
	atomic64_sub(entry.compressed_len, &mm_total_bytes);

	return MINIMEM_OK;
}

int minimem_zswap_remove(unsigned long vaddr)
{
	struct minimem_map_entry entry;
	int ret;

	ret = minimem_map_lookup(&minimem_map, vaddr, &entry);
	if (ret)
		return ret;

	ret = minimem_map_remove(&minimem_map, vaddr);
	if (ret == 0) {
		zs_free(minimem_pool, entry.zs_handle);
		atomic64_dec(&mm_stored_pages);
		atomic64_sub(entry.compressed_len, &mm_total_bytes);
	}

	return ret;
}

unsigned long minimem_zswap_total_bytes(void)
{
	return (unsigned long)atomic64_read(&mm_total_bytes);
}

unsigned long minimem_zswap_stored_pages(void)
{
	return (unsigned long)atomic64_read(&mm_stored_pages);
}

unsigned long minimem_zswap_bytes_saved(void)
{
	return (unsigned long)atomic64_read(&mm_bytes_saved);
}

unsigned long minimem_zswap_max_pool_pages(void)
{
	return (unsigned long)atomic64_read(&mm_max_pool_pages);
}

void minimem_zswap_set_max_pool_pages(unsigned long max)
{
	atomic64_set(&mm_max_pool_pages, (s64)max);
}