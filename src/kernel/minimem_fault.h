/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * minimem_fault.h — Page fault interception for MiniMem (future)
 *
 * When MiniMem can modify PTEs (via kernel patch or swap type),
 * this module will intercept page faults on compressed pages and
 * decompress them transparently. For now, the debugfs interface
 * in minimem_main.c provides manual testing.
 *
 * Future: register_page_fault_notifier() hook to intercept faults
 * on compressed pages, decompress, and remap.
 */

#ifndef MINIMEM_KERNEL_FAULT_H
#define MINIMEM_KERNEL_FAULT_H

#include <linux/types.h>

/*
 * Check if a virtual address corresponds to a compressed page.
 * Returns true if the address is in the compression map.
 */
bool minimem_is_compressed(unsigned long vaddr);

/*
 * Immediately compress a page at the given virtual address.
 * For testing and debugfs use.
 */
int minimem_compress_page_at(unsigned long vaddr);

/*
 * Immediately decompress a page at the given virtual address.
 * For testing and debugfs use.
 */
int minimem_decompress_page_at(unsigned long vaddr);

#endif /* MINIMEM_KERNEL_FAULT_H */