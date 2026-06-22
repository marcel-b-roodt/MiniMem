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
#include <linux/rwsem.h>
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
	struct rw_semaphore rwsem;
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

void minimem_map_read_lock(struct minimem_map *map);
void minimem_map_read_unlock(struct minimem_map *map);

int minimem_map_remove(struct minimem_map *map, unsigned long vaddr);

/*
 * Drain up to @max_entries from the map, calling @callback for each.
 * The callback receives the vaddr and entry for each removed item.
 * Returns the number of entries drained.
 * Holds map->lock during iteration; entries are removed under the lock.
 */
typedef void (*minimem_map_drain_cb)(unsigned long vaddr,
				      struct minimem_map_entry *entry,
				      void *priv);
unsigned long minimem_map_drain(struct minimem_map *map,
				 unsigned long max_entries,
				 minimem_map_drain_cb callback,
				 void *priv);

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