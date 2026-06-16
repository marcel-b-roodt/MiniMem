/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * minimem_fault.h — Page fault handler for MiniMem compressed pages
 *
 * When a MiniMem-compressed page is accessed, the CPU faults on the
 * PTE marker. The fault handler extracts the map index from the PTE,
 * decompresses the page from zsmalloc, and installs a present PTE.
 *
 * For v0.5.0, the fault handler is a stub — PTE manipulation requires
 * mm/ internals not exported to modules. A kernel patch to
 * do_swap_page() would call minimem_handle_fault() when it encounters
 * PTE_MARKER_MINIMEM.
 *
 * The kprobe hook in minimem_hook.c provides the actual fault handling.
 * PTE replacement is implemented in minimem_hook.c via
 * minimem_compress_and_replace_pte().
 *
 * The PTE test and fault stats are consolidated into the main module's
 * debugfs directory.
 */

#ifndef MINIMEM_KERNEL_FAULT_H
#define MINIMEM_KERNEL_FAULT_H

#include <linux/types.h>
#include <linux/mm.h>
#include <asm/pgtable.h>
#include "minimem_pte.h"

int minimem_handle_fault(struct vm_area_struct *vma,
			 unsigned long addr, swp_entry_t entry);

bool minimem_is_compressed(unsigned long vaddr);

int minimem_fault_init(void);
void minimem_fault_exit(void);

/* Debugfs interfaces (called from minimem_main.c) */
ssize_t minimem_pte_test_write(const char __user *buf, size_t count);
ssize_t minimem_fault_stats_read(char *kbuf, size_t kbuf_size);

#endif /* MINIMEM_KERNEL_FAULT_H */