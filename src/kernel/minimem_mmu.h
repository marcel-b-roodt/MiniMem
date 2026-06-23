/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * minimem_mmu.h — MMU notifier for MiniMem process exit cleanup
 *
 * On stock kernels (without CONFIG_MINIMEM), the kernel's zap_pte_range()
 * doesn't know how to handle PTE_MARKER_MINIMEM entries. When a process
 * exits with compressed pages, the zsmalloc allocations would leak and
 * "unrecognized swap entry" warnings would appear.
 *
 * This module registers an MMU notifier for each process that has compressed
 * pages. The release callback (process exit) walks the page tables, finds
 * MiniMem PTE markers, clears them, and frees the zsmalloc allocations.
 *
 * IMPORTANT: mmu_notifier_get() internally takes mmap_write_lock(mm), so
 * we must NOT call it while holding mmap_read_lock(mm) on the same mm.
 * minimem_mmu_register_deferred() queues the mm for later registration in
 * a workqueue context where mmap_write_lock can be safely acquired.
 */

#ifndef MINIMEM_KERNEL_MMU_H
#define MINIMEM_KERNEL_MMU_H

#include <linux/mm.h>

/*
 * Register an MMU notifier for the given mm.
 * Caller must NOT hold mmap_lock for this mm (mmu_notifier_get takes
 * mmap_write_lock internally).
 * Returns 0 on success, negative errno on failure.
 */
int minimem_mmu_register(struct mm_struct *mm);

/*
 * Register MMU notifier with mmap_write_lock already held.
 * Calls mmu_notifier_get_locked internally.
 * Used by the deferred registration work handler.
 */
int minimem_mmu_register_locked(struct mm_struct *mm);

/*
 * Deferred MMU notifier registration. Safe to call from any context,
 * including under mmap_read_lock. Queues the mm for later registration
 * in a workqueue context where mmap_write_lock can be safely acquired.
 */
void minimem_mmu_register_deferred(struct mm_struct *mm);

int minimem_mmu_init(void);
void minimem_mmu_exit(void);

unsigned long minimem_mmu_release_count(void);
unsigned long minimem_mmu_release_pages(void);
unsigned long minimem_mmu_invalidate_count(void);

#endif /* MINIMEM_KERNEL_MMU_H */