/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * minimem_shrinker.h — Memory pressure shrinker for MiniMem
 *
 * Registers a shrinker callback that decompresses pages back to RAM
 * under memory pressure. Decompresses least-recently-accessed pages
 * first to free zsmalloc memory.
 */

#ifndef MINIMEM_KERNEL_SHRINKER_H
#define MINIMEM_KERNEL_SHRINKER_H

int minimem_shrinker_init(void);
void minimem_shrinker_exit(void);

#endif /* MINIMEM_KERNEL_SHRINKER_H */