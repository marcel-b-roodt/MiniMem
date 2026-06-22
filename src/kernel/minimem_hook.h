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

/*
 * Check if the handle_pte_marker kprobe is registered.
 * This is required for the scanner sweep pass to safely
 * compress pages — without it, MiniMem PTE marker faults
 * return VM_FAULT_SIGBUS which kills the process.
 *
 * Returns true if either kernel patches or kprobe fallback
 * is available to handle MiniMem PTE marker faults.
 */
bool minimem_hook_fault_handler_ready(void);

/*
 * Check if kernel patches are detected (minimem_register_fault_handler
 * symbol resolved). This is a subset of hook_fault_handler_ready.
 */
bool minimem_hook_marker_ready(void);

/*
 * Resolved pte_offset_map_lock via kallsyms.
 * Returns NULL if symbols not resolved.
 * Modules must use this instead of the inline pte_offset_map_lock()
 * because __pte_offset_map_lock is not exported.
 */
pte_t *minimem_pte_offset_map_lock(struct mm_struct *mm, pmd_t *pmd,
				   unsigned long addr, spinlock_t **ptlp);

/*
 * Set a PTE at the given address. Uses the resolved set_ptes symbol
 * or WRITE_ONCE on x86-64 (since set_pte_at is a macro there).
 */
void minimem_set_pte_at(struct mm_struct *mm, unsigned long addr,
			pte_t *ptep, pte_t pte);

/*
 * Inline PTE helpers. On x86-64, many PTE operations are macros or
 * inline functions that can't be resolved via kallsyms. We provide
 * our own implementations using resolved symbols where needed.
 */

/*
 * Construct a PTE from a page and page protection flags.
 * Uses pfn_pte() which is always available.
 */
static inline pte_t minimem_mk_pte(struct page *page, pgprot_t pgprot)
{
	return pfn_pte(page_to_pfn(page), pgprot);
}

/*
 * Make a PTE writable. Uses the resolved pte_mkwrite_novla symbol
 * on kernels that have it (6.x+), otherwise falls back.
 */
pte_t minimem_pte_mkwrite_func(pte_t pte);

static inline pte_t minimem_pte_mkwrite(pte_t pte)
{
	return minimem_pte_mkwrite_func(pte);
}

/*
 * Unlock a PTE. pte_unmap_unlock is a macro on most architectures.
 */
static inline void minimem_pte_unmap_unlock(pte_t *ptep, spinlock_t *ptl)
{
	pte_unmap(ptep);
	spin_unlock(ptl);
}

#endif /* MINIMEM_KERNEL_HOOK_H */