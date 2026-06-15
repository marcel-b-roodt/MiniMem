/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * minimem_map.c — Compression map for MiniMem kernel module
 *
 * Uses xarray for lock-free concurrent reads and spinlock-protected
 * writes. Each entry maps a page-aligned virtual address to the
 * compression metadata (algorithm ID, compressed length, zsmalloc handle).
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/xarray.h>
#include <linux/spinlock.h>
#include <linux/errno.h>

#include "minimem_map.h"

static struct kmem_cache *minimem_entry_cache;

int minimem_map_init(struct minimem_map *map)
{
	xa_init(&map->entries);
	spin_lock_init(&map->lock);
	atomic64_set(&map->count, 0);

	minimem_entry_cache = kmem_cache_create("minimem_map_entry",
						  sizeof(struct minimem_map_entry),
						  0, SLAB_PANIC, NULL);
	if (!minimem_entry_cache)
		return -ENOMEM;

	return 0;
}

void minimem_map_destroy(struct minimem_map *map)
{
	struct minimem_map_entry *entry;
	unsigned long index;

	xa_for_each(&map->entries, index, entry) {
		xa_erase(&map->entries, index);
		kmem_cache_free(minimem_entry_cache, entry);
	}

	xa_destroy(&map->entries);
	kmem_cache_destroy(minimem_entry_cache);
	minimem_entry_cache = NULL;
}

int minimem_map_store(struct minimem_map *map, unsigned long vaddr,
		      int algo_id, size_t compressed_len,
		      unsigned long zs_handle)
{
	struct minimem_map_entry *entry;
	unsigned long index = vaddr >> PAGE_SHIFT;
	int ret;

	entry = kmem_cache_alloc(minimem_entry_cache, GFP_KERNEL);
	if (!entry)
		return -ENOMEM;

	entry->algo_id = algo_id;
	entry->compressed_len = compressed_len;
	entry->zs_handle = zs_handle;

	spin_lock(&map->lock);
	ret = xa_err(xa_store(&map->entries, index, entry, GFP_KERNEL));
	if (ret == 0)
		atomic64_inc(&map->count);
	spin_unlock(&map->lock);

	if (ret) {
		kmem_cache_free(minimem_entry_cache, entry);
		return ret;
	}

	return 0;
}

int minimem_map_lookup(struct minimem_map *map, unsigned long vaddr,
		       struct minimem_map_entry *out)
{
	unsigned long index = vaddr >> PAGE_SHIFT;
	struct minimem_map_entry *entry;

	rcu_read_lock();
	entry = xa_load(&map->entries, index);
	if (!entry) {
		rcu_read_unlock();
		return -ENOENT;
	}

	*out = *entry;
	rcu_read_unlock();

	return 0;
}

int minimem_map_remove(struct minimem_map *map, unsigned long vaddr)
{
	unsigned long index = vaddr >> PAGE_SHIFT;
	struct minimem_map_entry *entry;

	spin_lock(&map->lock);
	entry = xa_erase(&map->entries, index);
	if (entry) {
		atomic64_dec(&map->count);
		kmem_cache_free(minimem_entry_cache, entry);
	}
	spin_unlock(&map->lock);

	return entry ? 0 : -ENOENT;
}

void minimem_map_remove_all(struct minimem_map *map)
{
	struct minimem_map_entry *entry;
	unsigned long index;

	xa_for_each(&map->entries, index, entry) {
		xa_erase(&map->entries, index);
		kmem_cache_free(minimem_entry_cache, entry);
	}

	atomic64_set(&map->count, 0);
}