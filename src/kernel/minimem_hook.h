/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * minimem_hook.h — Page fault hook for MiniMem transparent compression
 *
 * Uses kprobes on do_swap_page() to intercept MiniMem PTE markers.
 * When a fault hits a compressed page, the hook decompresses it
 * transparently and installs a present PTE.
 *
 * Also provides minimem_compress_and_replace_pte() which compresses
 * a page and replaces its PTE with a MiniMem marker, using resolved
 * kernel symbols (pte_offset_map_lock, set_pte_at, etc.).
 */

#ifndef MINIMEM_KERNEL_HOOK_H
#define MINIMEM_KERNEL_HOOK_H

#include <linux/mm.h>
#include <asm/pgtable.h>

int minimem_hook_init(void);
void minimem_hook_exit(void);

unsigned long minimem_hook_faults_handled(void);
unsigned long minimem_hook_faults_missed(void);

/*
 * Compress the page at @addr in @mm and replace its PTE with a
 * MiniMem swap entry marker. Returns 0 on success, -EOPNOTSUPP
 * if kernel symbols are not resolved, negative errno on error.
 *
 * Requires symbols_resolved == true (set during hook init).
 */
int minimem_compress_and_replace_pte(struct mm_struct *mm,
				     struct vm_area_struct *vma,
				     unsigned long addr);

/*
 * Check if the kprobe hook's kernel symbols were resolved.
 * Returns true if PTE manipulation functions are available.
 */
bool minimem_hook_symbols_resolved(void);

#endif /* MINIMEM_KERNEL_HOOK_H */