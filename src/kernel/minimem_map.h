/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * minimem_map.h — Compression map for MiniMem kernel module
 *
 * Maps virtual addresses to compressed page handles stored in
 * zsmalloc. Uses xarray for RCU-safe concurrent lookups.
 */

#ifndef MINIMEM_KERNEL_MAP_H
#define MINIMEM_KERNEL_MAP_H

#include <linux/types.h>
#include <linux/xarray.h>
#include <linux/spinlock.h>
#include "minimem_compress.h"

/*
 * Compression map entry. Stores the metadata for a compressed page.
 * The actual compressed data lives in a zsmalloc handle (not here).
 */
struct minimem_map_entry {
	int algo_id;
	size_t compressed_len;
	unsigned long zs_handle;
};

/*
 * The compression map. An xarray mapping page-aligned virtual addresses
 * to minimem_map_entry structs.
 */
struct minimem_map {
	struct xarray entries;
	spinlock_t lock;
	atomic64_t count;
};

int minimem_map_init(struct minimem_map *map);
void minimem_map_destroy(struct minimem_map *map);

/*
 * Store a compressed page in the map.
 * Returns 0 on success, negative errno on failure.
 * Caller must provide the zsmalloc handle containing compressed data.
 */
int minimem_map_store(struct minimem_map *map, unsigned long vaddr,
		      int algo_id, size_t compressed_len,
		      unsigned long zs_handle);

/*
 * Look up a compressed page by virtual address.
 * Returns 0 on success, -ENOENT if not found.
 * Caller must not free the entry while using it.
 */
int minimem_map_lookup(struct minimem_map *map, unsigned long vaddr,
		       struct minimem_map_entry *out);

/*
 * Remove a compressed page from the map.
 * Returns 0 on success, -ENOENT if not found.
 * Caller is responsible for freeing the zsmalloc handle.
 */
int minimem_map_remove(struct minimem_map *map, unsigned long vaddr);

/*
 * Remove all entries from the map. Called during module unload
 * to decompress all remaining pages.
 */
void minimem_map_remove_all(struct minimem_map *map);

/*
 * Get the number of entries in the map.
 */
static inline long minimem_map_count(struct minimem_map *map)
{
	return atomic64_read(&map->count);
}

#endif /* MINIMEM_KERNEL_MAP_H */