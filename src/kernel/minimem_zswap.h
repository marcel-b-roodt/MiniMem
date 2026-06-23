/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * minimem_zswap.h — zsmalloc-backed compressed page storage for MiniMem
 *
 * Manages a zsmalloc pool that stores compressed page data. Each compressed
 * page is allocated from the pool and tracked in the compression map.
 * The pool is created during module init and destroyed on module exit.
 */

#ifndef MINIMEM_KERNEL_ZSWAP_H
#define MINIMEM_KERNEL_ZSWAP_H

#include <linux/types.h>
#include <linux/zsmalloc.h>
#include "minimem_compress.h"
#include "minimem_map.h"

/*
 * Initialize the zsmalloc pool and compression map.
 * Returns 0 on success, negative errno on failure.
 */
int minimem_zswap_init(void);

/*
 * Destroy the zsmalloc pool and free all stored pages.
 */
void minimem_zswap_exit(void);

/*
 * Decompress all stored pages and restore their PTEs to present.
 * Walks all processes' page tables, finds MiniMem PTE markers,
 * decompresses the corresponding data, allocates new pages,
 * and installs present PTEs. Called during module unload to
 * ensure no data is lost.
 *
 * Returns the number of pages successfully restored, or a negative
 * errno on failure.
 */
long minimem_zswap_drain_and_restore(void);

/*
 * Compress a page and store it in the zsmalloc pool.
 * Uses the advisor to select the best algorithm.
 * Returns 0 on success, MINIMEM_INCOMPRESSIBLE if no compression possible,
 * negative errno on error.
 */
int minimem_compress_and_store(unsigned long vaddr, struct page *page);

/*
 * Decompress a page from the pool and write it into the target page.
 * Removes the entry from the map and frees the zsmalloc allocation.
 * Returns 0 on success, negative errno on error.
 */
int minimem_decompress_and_restore(unsigned long vaddr, struct page *page);

/*
 * Remove a compressed page from storage without decompressing.
 * Frees the zsmalloc allocation and removes the map entry.
 */
int minimem_zswap_remove(unsigned long vaddr);

/*
 * Get the zsmalloc pool pointer. Used by the parallel decompression
 * worker to map/unmap zsmalloc objects.
 */
struct zs_pool *minimem_zswap_pool(void);

/*
 * Get the compression map pointer. Used by the parallel decompression
 * worker to look up entries.
 */
struct minimem_map *minimem_zswap_map(void);

/*
 * Zap callback for MiniMem PTE markers.
 * Called from zap_pte_range() when a process exits or munmaps
 * a region with MiniMem-compressed pages. Frees the zsmalloc
 * allocation and removes the map entry.
 */
void minimem_zswap_zap_cb(struct vm_area_struct *vma,
			  unsigned long addr, swp_entry_t entry);


/* Stats */
unsigned long minimem_zswap_total_bytes(void);
unsigned long minimem_zswap_stored_pages(void);
unsigned long minimem_zswap_bytes_saved(void);
unsigned long minimem_zswap_max_pool_pages(void);
void minimem_zswap_set_max_pool_pages(unsigned long max);
unsigned long minimem_zswap_zap_cb_count(void);
unsigned long minimem_zswap_zap_cb_miss_count(void);

#endif /* MINIMEM_KERNEL_ZSWAP_H */