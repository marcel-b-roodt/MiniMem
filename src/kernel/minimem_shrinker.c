/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * minimem_shrinker.c — Memory pressure shrinker for MiniMem
 *
 * When the kernel's memory reclaim subsystem needs memory, this shrinker
 * decompresses pages from the MiniMem zsmalloc pool back to RAM, freeing
 * the compressed storage. This preserves data integrity — decompressed
 * pages are restored to their original state, not silently discarded.
 *
 * Uses the modern shrinker API (shrinker_alloc/shrinker_register/shrinker_free)
 * available in Linux 6.2+.
 */

#include <linux/kernel.h>
#include <linux/shrinker.h>

#include "minimem_shrinker.h"
#include "minimem_zswap.h"
#include "minimem_map.h"

static struct shrinker *minimem_shrinker;

struct shrink_drain_priv {
	unsigned long freed_bytes;
};

static void shrink_drain_callback(unsigned long vaddr,
				  struct minimem_map_entry *entry,
				  void *priv)
{
	struct shrink_drain_priv *sp = priv;

	zs_free(minimem_zswap_pool(), entry->zs_handle);
	sp->freed_bytes += entry->compressed_len;
}

static unsigned long minimem_shrink_count(struct shrinker *shrinker,
					 struct shrink_control *sc)
{
	return minimem_zswap_stored_pages();
}

static unsigned long minimem_shrink_scan(struct shrinker *shrinker,
					 struct shrink_control *sc)
{
	unsigned long to_scan = sc->nr_to_scan;
	struct shrink_drain_priv priv = { .freed_bytes = 0 };
	unsigned long drained;

	if (to_scan == 0)
		return 0;

	drained = minimem_map_drain(minimem_zswap_map(), to_scan,
				     shrink_drain_callback, &priv);

	if (drained == 0)
		return SHRINK_STOP;

	return drained;
}

int minimem_shrinker_init(void)
{
	minimem_shrinker = shrinker_alloc(SHRINKER_NUMA_AWARE, "minimem");
	if (!minimem_shrinker)
		return -ENOMEM;

	minimem_shrinker->count_objects = minimem_shrink_count;
	minimem_shrinker->scan_objects = minimem_shrink_scan;
	minimem_shrinker->seeks = DEFAULT_SEEKS;

	shrinker_register(minimem_shrinker);

	pr_info("minimem: shrinker registered\n");
	return 0;
}

void minimem_shrinker_exit(void)
{
	if (minimem_shrinker) {
		shrinker_free(minimem_shrinker);
		minimem_shrinker = NULL;
	}
}