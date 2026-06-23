/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * minimem_hook.c — Kprobe-based page fault hook for transparent decompression
 *
 * Intercepts do_swap_page() via kprobe. When a MiniMem PTE marker is
 * found, decompresses the page and installs a present PTE.
 *
 * Also provides minimem_compress_and_replace_pte() which walks the
 * page table, compresses a page, and replaces its PTE with a MiniMem
 * swap entry.
 *
 * Symbol resolution:
 *   On x86-64, many PTE manipulation functions are inline or macros that
 *   cannot be resolved via kallsyms_lookup_name. We resolve the
 *   underlying non-inline symbols (__pte_offset_map_lock, set_ptes,
 *   pte_mkwrite_novma) and reimplement inline wrappers locally.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/kprobes.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/highmem.h>
#include <linux/ktime.h>
#include <linux/atomic.h>
#include <linux/spinlock.h>
#include <linux/page-flags.h>
#include <linux/swap.h>
#include <linux/swapops.h>
#include <linux/rmap.h>
#include <linux/zsmalloc.h>

#include "minimem_hook.h"
#include "minimem_compress.h"
#include "minimem_zswap.h"
#include "minimem_map.h"
#include "minimem_pte.h"
#include "minimem_scanner.h"
#include "minimem_parallel.h"
#include "minimem_mmu.h"

/*
 * Resolved kernel symbols.
 *
 * On kernel 6.x+, many PTE helpers are inline or macros:
 *   - pte_offset_map_lock → calls __pte_offset_map_lock (real symbol)
 *   - set_pte_at → macro expanding to set_ptes() (may be real symbol)
 *   - mk_pte → inline (use our own implementation)
 *   - pte_mkwrite → takes (pte_t, struct vm_area_struct *) on 6.x+
 *   - pte_unmap_unlock → macro (use our own implementation)
 *
 * We resolve what we can and implement the rest inline.
 */
static pte_t *(*p__pte_offset_map_lock)(struct mm_struct *, pmd_t *,
					 unsigned long, spinlock_t **);
static pte_t (*p_pte_mkwrite_vma)(pte_t, struct vm_area_struct *);
static void (*p_folio_add_new_anon_rmap)(struct folio *, struct vm_area_struct *,
					unsigned long, rmap_t);
static void (*p_folio_add_lru_vma)(struct folio *, struct vm_area_struct *);

/* mk_pte and pte_unmap_unlock are inline — we implement them locally */

static unsigned long (*p_kallsyms_lookup_name)(const char *name);

static int resolve_kallsyms_lookup_name(void)
{
	struct kprobe kp;
	int ret;

	memset(&kp, 0, sizeof(kp));
	kp.symbol_name = "kallsyms_lookup_name";

	ret = register_kprobe(&kp);
	if (ret) {
		pr_warn("minimem: failed to resolve kallsyms_lookup_name: %d\n",
			ret);
		return ret;
	}

	p_kallsyms_lookup_name = (void *)kp.addr;
	unregister_kprobe(&kp);

	if (!p_kallsyms_lookup_name) {
		pr_warn("minimem: kallsyms_lookup_name resolved to NULL\n");
		return -ENOENT;
	}

	return 0;
}

/*
 * Try to resolve a symbol by name, with fallback names.
 * Returns the address or NULL if none found.
 */
static void *try_resolve_symbol(const char *name)
{
	void *addr;

	addr = (void *)p_kallsyms_lookup_name(name);
	if (addr)
		pr_info("minimem: resolved %s at %px\n", name, addr);
	return addr;
}

static struct kretprobe minimem_kretprobe;
static atomic64_t hook_faults_handled;
static atomic64_t hook_faults_missed;
static atomic64_t hook_faults_ns;
static bool hook_registered;
static bool symbols_resolved;
static bool kernel_patches_detected;

/*
 * Per-instance data for the kretprobe: set by the entry handler when
 * we successfully decompress a MiniMem page, checked by the return
 * handler to override the return value.
 */
struct minimem_kretprobe_data {
	bool handled;
};

typedef int (*minimem_register_fn)(vm_fault_t (*handler)(struct vm_fault *));
typedef void (*minimem_unregister_fn)(void);
static minimem_register_fn p_minimem_register_fault_handler;
static minimem_unregister_fn p_minimem_unregister_fault_handler;

static int resolve_kernel_symbols(void)
{
	int ret;

	ret = resolve_kallsyms_lookup_name();
	if (ret)
		return ret;

	/*
	 * pte_offset_map_lock is inline — resolve the underlying
	 * __pte_offset_map_lock instead.
	 */
	p__pte_offset_map_lock = try_resolve_symbol("__pte_offset_map_lock");
	if (!p__pte_offset_map_lock) {
		pr_warn("minimem: could not resolve __pte_offset_map_lock\n");
		return -ENOENT;
	}

	/*
	 * On x86-64, set_pte_at is a macro expanding to set_ptes(),
	 * which is inline and calls native_set_pte() which is also
	 * inline (just WRITE_ONCE). Since we hold the PTL, we can
	 * write the PTE directly. No symbol resolution needed.
	 *
	 * pte_mkwrite takes two arguments on newer kernels:
	 * pte_mkwrite(pte_t pte, struct vm_area_struct *vma).
	 * pte_mkwrite_novma is static inline and not exported.
	 */
	p_pte_mkwrite_vma = try_resolve_symbol("pte_mkwrite");
	if (!p_pte_mkwrite_vma) {
		pr_warn("minimem: could not resolve pte_mkwrite\n");
		return -ENOENT;
	}

	p_folio_add_new_anon_rmap = try_resolve_symbol("folio_add_new_anon_rmap");
	if (!p_folio_add_new_anon_rmap) {
		pr_warn("minimem: could not resolve folio_add_new_anon_rmap\n");
		return -ENOENT;
	}

	p_folio_add_lru_vma = try_resolve_symbol("folio_add_lru_vma");
	if (!p_folio_add_lru_vma)
		pr_warn("minimem: could not resolve folio_add_lru_vma (pages may not be on LRU)\n");

	/*
	 * mk_pte is inline — we implement it using pfn_to_page and
	 * __pgprot. No symbol resolution needed.
	 *
	 * pte_unmap_unlock is a macro — we implement it using pte_unmap
	 * and spin_unlock. pte_unmap is also inline on x86-64 but
	 * pte_unmap_uncareful or the underlying pte_free_pmd is not
	 * needed since we can just call spin_unlock on the ptl.
	 */

	/*
	 * Detect kernel patches: if minimem_register_fault_handler is
	 * exported, the kernel patches have been applied. This means
	 * handle_pte_marker() will call our registered handler instead
	 * of returning VM_FAULT_SIGBUS for MiniMem markers.
	 */
	p_minimem_register_fault_handler =
		(minimem_register_fn)try_resolve_symbol(
			"minimem_register_fault_handler");
	p_minimem_unregister_fault_handler =
		(minimem_unregister_fn)try_resolve_symbol(
			"minimem_unregister_fault_handler");

	if (p_minimem_register_fault_handler &&
	    p_minimem_unregister_fault_handler) {
		kernel_patches_detected = true;
		pr_info("minimem: kernel patches detected — "
			"fault handler registration available\n");
	} else {
		kernel_patches_detected = false;
		pr_info("minimem: kernel patches not detected — "
			"kprobe-only mode (sweep enabled via kprobe)\n");
	}

	symbols_resolved = true;
	return 0;
}

/*
 * PTE helper implementations.
 * Declarations and inline versions are in minimem_hook.h.
 */

void minimem_set_pte_at(struct mm_struct *mm,
		      unsigned long addr, pte_t *ptep,
		      pte_t pte)
{
	WRITE_ONCE(*ptep, pte);
}

void minimem_folio_add_new_anon_rmap(struct folio *folio,
				     struct vm_area_struct *vma,
				     unsigned long address)
{
	if (p_folio_add_new_anon_rmap)
		p_folio_add_new_anon_rmap(folio, vma, address, RMAP_EXCLUSIVE);
}

void minimem_folio_add_lru_vma(struct folio *folio,
			       struct vm_area_struct *vma)
{
	if (p_folio_add_lru_vma)
		p_folio_add_lru_vma(folio, vma);
}

pte_t minimem_pte_mkwrite_func(pte_t pte, struct vm_area_struct *vma)
{
	if (p_pte_mkwrite_vma)
		return p_pte_mkwrite_vma(pte, vma);
	return pte;
}

static vm_fault_t minimem_vm_fault_handler(struct vm_fault *vmf)
{
	pte_t orig_pte;
	swp_entry_t entry;
	unsigned long map_index, vaddr;
	struct minimem_map_entry map_entry;
	struct page *new_page;
	struct minimem_decompress_result dres;
	void *src_buf, *zs_addr, *dst_addr;
	pte_t new_pte;
	struct vm_area_struct *vma = vmf->vma;
	struct mm_struct *mm = vma->vm_mm;
	spinlock_t *ptl;
	pte_t *ptep;
	ktime_t start;
	int ret;

	if (!symbols_resolved)
		return VM_FAULT_SIGBUS;

	orig_pte = vmf->orig_pte;

	if (!is_minimem_pte(orig_pte)) {
		pr_warn("minimem: vm_fault_handler: not a minimem pte (0x%llx)\n",
			(unsigned long long)pte_val(orig_pte));
		return VM_FAULT_SIGBUS;
	}

	start = ktime_get_ns();
	entry = minimem_pte_to_swp_entry(orig_pte);
	map_index = minimem_entry_index(entry);
	vaddr = vmf->address & PAGE_MASK;

	ret = minimem_map_lookup(minimem_zswap_map(), vaddr, &map_entry);
	if (ret) {
		pr_warn("minimem: vm_fault_handler: map_lookup FAILED for vaddr=0x%lx ret=%d\n",
			vaddr, ret);
		atomic64_inc(&hook_faults_missed);
		return VM_FAULT_SIGBUS;
	}

	new_page = alloc_page_vma(GFP_HIGHUSER_MOVABLE, vma, vmf->address);
	if (!new_page) {
		pr_warn("minimem: vm_fault_handler: alloc_page_vma FAILED\n");
		atomic64_inc(&hook_faults_missed);
		return VM_FAULT_OOM;
	}

	src_buf = kmalloc(map_entry.compressed_len, GFP_ATOMIC);
	if (!src_buf) {
		pr_warn("minimem: vm_fault_handler: kmalloc FAILED (len=%zu)\n",
			map_entry.compressed_len);
		__free_page(new_page);
		atomic64_inc(&hook_faults_missed);
		return VM_FAULT_OOM;
	}

	zs_addr = zs_obj_read_begin(minimem_zswap_pool(), map_entry.zs_handle, src_buf);
	if (!zs_addr) {
		kfree(src_buf);
		__free_page(new_page);
		atomic64_inc(&hook_faults_missed);
		return VM_FAULT_SIGBUS;
	}

	dst_addr = kmap_local_page(new_page);
	ret = minimem_decompress_page(zs_addr, map_entry.compressed_len,
				      map_entry.algo_id, dst_addr,
				      MINIMEM_PAGE_SIZE, &dres);
	kunmap_local(dst_addr);

	zs_obj_read_end(minimem_zswap_pool(), map_entry.zs_handle, zs_addr);
	kfree(src_buf);

	if (ret != MINIMEM_OK) {
		pr_warn("minimem: decompress failed ret=%d algo=%d clen=%zu\n",
			ret, map_entry.algo_id, map_entry.compressed_len);
		__free_page(new_page);
		atomic64_inc(&hook_faults_missed);
		return VM_FAULT_SIGBUS;
	}

	minimem_map_remove(minimem_zswap_map(), vaddr);
	zs_free(minimem_zswap_pool(), map_entry.zs_handle);

	new_pte = minimem_mk_pte(new_page, vma->vm_page_prot);
	if (vma->vm_flags & VM_WRITE)
		new_pte = minimem_pte_mkwrite(new_pte, vma);
	new_pte = pte_mkyoung(new_pte);

	ptep = minimem_pte_offset_map_lock(mm, vmf->pmd, vmf->address, &ptl);
	if (ptep) {
		if (pte_same(*ptep, orig_pte)) {
			minimem_set_pte_at(mm, vmf->address, ptep, new_pte);
			percpu_counter_add(&mm->rss_stat[MM_ANONPAGES], 1);
			minimem_folio_add_new_anon_rmap(page_folio(new_page), vma,
						  vmf->address);
			minimem_folio_add_lru_vma(page_folio(new_page), vma);
		} else {
			pr_warn("minimem: vm_fault_handler: PTE changed! old=0x%llx cur=0x%llx\n",
				(unsigned long long)pte_val(orig_pte),
				(unsigned long long)pte_val(*ptep));
			__free_page(new_page);
		}
		minimem_pte_unmap_unlock(ptep, ptl);
	} else {
		pr_warn("minimem: vm_fault_handler: ptep is NULL\n");
		__free_page(new_page);
	}

	atomic64_inc(&hook_faults_handled);
	atomic64_add(ktime_get_ns() - start, &hook_faults_ns);

	pr_debug("minimem: patched fault handled for vaddr=0x%lx, "
		 "algo=%d compressed=%zu decompressed=%zu\n",
		 vaddr, map_entry.algo_id, map_entry.compressed_len,
		 (size_t)MINIMEM_PAGE_SIZE);

	return VM_FAULT_NOPAGE;
}

static int minimem_handle_swap_fault(struct vm_fault *vmf)
{
	swp_entry_t entry;
	unsigned long map_index, vaddr;
	struct minimem_map_entry map_entry;
	struct page *new_page;
	struct minimem_decompress_result dres;
	void *src_buf, *zs_addr, *dst_addr;
	pte_t new_pte, orig_pte;
	struct vm_area_struct *vma = vmf->vma;
	struct mm_struct *mm = vma->vm_mm;
	spinlock_t *ptl;
	pte_t *ptep;
	ktime_t start;
	int ret;

	orig_pte = vmf->orig_pte;

	if (!is_minimem_pte(orig_pte))
		return 0;

	if (!symbols_resolved)
		return 0;

	start = ktime_get_ns();
	entry = minimem_pte_to_swp_entry(orig_pte);
	map_index = minimem_entry_index(entry);
	vaddr = vmf->address & PAGE_MASK;

	ret = minimem_map_lookup(minimem_zswap_map(), vaddr, &map_entry);
	if (ret) {
		atomic64_inc(&hook_faults_missed);
		return 0;
	}

	new_page = alloc_page_vma(GFP_HIGHUSER_MOVABLE, vma, vmf->address);
	if (!new_page) {
		atomic64_inc(&hook_faults_missed);
		return 0;
	}

	src_buf = kmalloc(map_entry.compressed_len, GFP_ATOMIC);
	if (!src_buf) {
		__free_page(new_page);
		atomic64_inc(&hook_faults_missed);
		return 0;
	}

	zs_addr = zs_obj_read_begin(minimem_zswap_pool(), map_entry.zs_handle, src_buf);
	if (!zs_addr) {
		kfree(src_buf);
		__free_page(new_page);
		atomic64_inc(&hook_faults_missed);
		return 0;
	}

	dst_addr = kmap_local_page(new_page);
	ret = minimem_decompress_page(zs_addr, map_entry.compressed_len,
				      map_entry.algo_id, dst_addr,
				      MINIMEM_PAGE_SIZE, &dres);
	kunmap_local(dst_addr);

	zs_obj_read_end(minimem_zswap_pool(), map_entry.zs_handle, zs_addr);
	kfree(src_buf);

	if (ret != MINIMEM_OK) {
		__free_page(new_page);
		atomic64_inc(&hook_faults_missed);
		return 0;
	}

	minimem_map_remove(minimem_zswap_map(), vaddr);
	zs_free(minimem_zswap_pool(), map_entry.zs_handle);

	new_pte = minimem_mk_pte(new_page, vma->vm_page_prot);
	if (vma->vm_flags & VM_WRITE)
		new_pte = minimem_pte_mkwrite(new_pte, vma);
	new_pte = pte_mkyoung(new_pte);

	ptep = minimem_pte_offset_map_lock(mm, vmf->pmd, vmf->address, &ptl);
	if (ptep) {
		if (pte_same(*ptep, orig_pte)) {
			minimem_set_pte_at(mm, vmf->address, ptep, new_pte);
			percpu_counter_add(&mm->rss_stat[MM_ANONPAGES], 1);
			minimem_folio_add_new_anon_rmap(page_folio(new_page), vma,
						  vmf->address);
			minimem_folio_add_lru_vma(page_folio(new_page), vma);
		} else {
			__free_page(new_page);
		}
		minimem_pte_unmap_unlock(ptep, ptl);
	} else {
		__free_page(new_page);
	}

	atomic64_inc(&hook_faults_handled);
	atomic64_add(ktime_get_ns() - start, &hook_faults_ns);

	pr_debug("minimem: transparent fault handled for vaddr=0x%lx, "
		 "algo=%d compressed=%zu decompressed=%zu\n",
		 vaddr, map_entry.algo_id, map_entry.compressed_len,
		 (size_t)MINIMEM_PAGE_SIZE);

	return 1;
}

/*
 * Kretprobe entry handler for do_swap_page().
 *
 * On stock kernels (without CONFIG_MINIMEM patches), the kprobe approach
 * has a fundamental limitation on x86-64: pte_unmap_same() is a no-op
 * because sizeof(pte_t) == sizeof(unsigned long). This means that after
 * our handler installs a present PTE, do_swap_page() continues processing
 * the original MiniMem marker PTE via handle_pte_marker(), which returns
 * VM_FAULT_SIGBUS and kills the process.
 *
 * The kretprobe return handler (minimem_kretprobe_return) fixes this by
 * changing the return value from VM_FAULT_SIGBUS to VM_FAULT_NOPAGE
 * when our entry handler has successfully resolved the fault. This causes
 * the page fault handler to re-walk the page table, find the present PTE
 * we installed, and complete the fault normally.
 */
static int minimem_kretprobe_entry(struct kretprobe_instance *ri,
				   struct pt_regs *regs)
{
	struct minimem_kretprobe_data *data;
	struct vm_fault *vmf;
	pte_t orig_pte;

	data = (struct minimem_kretprobe_data *)ri->data;
	data->handled = false;

	if (!symbols_resolved)
		return 0;

	vmf = (struct vm_fault *)regs->di;
	if (!vmf || !vmf->vma)
		return 0;

	orig_pte = vmf->orig_pte;

	if (!is_minimem_pte(orig_pte))
		return 0;

	if (minimem_handle_swap_fault(vmf) > 0)
		data->handled = true;

	return 0;
}

/*
 * Kretprobe return handler for do_swap_page().
 *
 * If our entry handler successfully decompressed a MiniMem page, we
 * override the return value: VM_FAULT_SIGBUS → VM_FAULT_NOPAGE.
 *
 * On x86-64, pte_unmap_same() is a no-op (sizeof(pte_t) ==
 * sizeof(unsigned long)), so do_swap_page() does NOT bail out after
 * our entry handler installs the present PTE. Instead it continues
 * to handle_pte_marker(), which returns VM_FAULT_SIGBUS for unknown
 * PTE marker types. We change that to VM_FAULT_NOPAGE so the page
 * fault handler re-walks the page table and finds our installed PTE.
 */
static int minimem_kretprobe_return(struct kretprobe_instance *ri,
				    struct pt_regs *regs)
{
	struct minimem_kretprobe_data *data;
	vm_fault_t ret;

	data = (struct minimem_kretprobe_data *)ri->data;
	if (!data->handled)
		return 0;

	ret = (vm_fault_t)regs_return_value(regs);
	if (ret == VM_FAULT_SIGBUS) {
		regs_set_return_value(regs, VM_FAULT_NOPAGE);
		pr_debug("minimem: kretprobe: changed VM_FAULT_SIGBUS "
			 "to VM_FAULT_NOPAGE\n");
	}

	return 0;
}

int minimem_hook_init(void)
{
	int ret;

	atomic64_set(&hook_faults_handled, 0);
	atomic64_set(&hook_faults_missed, 0);
	atomic64_set(&hook_faults_ns, 0);
	kernel_patches_detected = false;

	ret = resolve_kernel_symbols();
	if (ret) {
		pr_warn("minimem: could not resolve kernel symbols, "
			"transparent fault interception disabled\n");
		return ret;
	}

	if (kernel_patches_detected) {
		ret = p_minimem_register_fault_handler(minimem_vm_fault_handler);
		if (ret) {
			pr_warn("minimem: failed to register fault handler: %d\n",
				ret);
			kernel_patches_detected = false;
		} else {
			void (**zap_cb_ptr)(struct vm_area_struct *,
					   unsigned long, swp_entry_t);

			pr_info("minimem: registered fault handler with kernel "
				"(handle_pte_marker path)\n");

			/* Register the zap callback for process exit/munmap */
			zap_cb_ptr = try_resolve_symbol("minimem_zap_cb");
			if (zap_cb_ptr) {
				*zap_cb_ptr = minimem_zswap_zap_cb;
				pr_info("minimem: registered zap callback with kernel\n");
			}
		}
	}

	if (!kernel_patches_detected) {
		minimem_kretprobe.kp.symbol_name = "do_swap_page";
		minimem_kretprobe.entry_handler = minimem_kretprobe_entry;
		minimem_kretprobe.handler = minimem_kretprobe_return;
		minimem_kretprobe.data_size = sizeof(struct minimem_kretprobe_data);
		minimem_kretprobe.maxactive = 0;

		ret = register_kretprobe(&minimem_kretprobe);
		if (ret) {
			pr_warn("minimem: failed to register kretprobe on do_swap_page: %d\n",
				ret);
			pr_warn("minimem: transparent fault interception disabled\n");
			return ret;
		}

		hook_registered = true;
		pr_info("minimem: kretprobe registered on do_swap_page "
			"(fallback — kernel patches not detected)\n");
	}

	return 0;
}

void minimem_hook_exit(void)
{
	void (**zap_cb_ptr)(struct vm_area_struct *, unsigned long, swp_entry_t);

	if (kernel_patches_detected && p_minimem_unregister_fault_handler) {
		p_minimem_unregister_fault_handler();
		pr_info("minimem: unregistered fault handler from kernel\n");

		zap_cb_ptr = try_resolve_symbol("minimem_zap_cb");
		if (zap_cb_ptr)
			*zap_cb_ptr = NULL;
	}

	if (hook_registered) {
		unregister_kretprobe(&minimem_kretprobe);
		hook_registered = false;
		pr_info("minimem: kretprobe unregistered\n");
	}

	pr_info("minimem: hook faults_handled=%lld faults_missed=%lld ns=%lld\n",
		atomic64_read(&hook_faults_handled),
		atomic64_read(&hook_faults_missed),
		atomic64_read(&hook_faults_ns));
}

unsigned long minimem_hook_faults_handled(void)
{
	return atomic64_read(&hook_faults_handled);
}

unsigned long minimem_hook_faults_missed(void)
{
	return atomic64_read(&hook_faults_missed);
}

bool minimem_hook_symbols_resolved(void)
{
	return symbols_resolved;
}

bool minimem_hook_marker_ready(void)
{
	/*
	 * Returns true if PTE marker replacement is safe during scanner sweep.
	 * On custom kernels (CONFIG_MINIMEM), handle_pte_marker() calls our
	 * fault handler directly. On stock kernels, the kretprobe return
	 * handler changes VM_FAULT_SIGBUS to VM_FAULT_NOPAGE for MiniMem
	 * PTE markers, making marker replacement safe.
	 */
	return kernel_patches_detected || hook_registered;
}

bool minimem_hook_fault_handler_ready(void)
{
	/*
	 * Returns true if either kernel patches or kretprobe fallback is
	 * available. Used by the scanner to decide whether to run at all.
	 * The kretprobe fallback intercepts do_swap_page() entry and return:
	 * entry handler decompresses and installs present PTE, return handler
	 * changes VM_FAULT_SIGBUS to VM_FAULT_NOPAGE for MiniMem faults.
	 */
	return kernel_patches_detected || hook_registered;
}

pte_t *minimem_pte_offset_map_lock(struct mm_struct *mm, pmd_t *pmd,
				   unsigned long addr, spinlock_t **ptlp)
{
	if (!p__pte_offset_map_lock)
		return NULL;
	return p__pte_offset_map_lock(mm, pmd, addr, ptlp);
}

int minimem_compress_and_replace_pte(struct mm_struct *mm,
				     struct vm_area_struct *vma,
				     unsigned long addr)
{
	unsigned long vaddr = addr & PAGE_MASK;
	struct page *page;
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *ptep, old_pte, new_pte;
	swp_entry_t entry;
	spinlock_t *ptl;
	struct minimem_compress_result res;
	void *src_addr, *dst_buf, *local_buf;
	unsigned long handle;
	size_t copy_len;
	int ret, nid;
	unsigned long map_index;

	if (!symbols_resolved)
		return -EOPNOTSUPP;

	if (!mm || !vma)
		return -EINVAL;

	pgd = pgd_offset(mm, vaddr);
	if (pgd_none(*pgd) || pgd_bad(*pgd))
		return -ENOENT;

	p4d = p4d_offset(pgd, vaddr);
	if (p4d_none(*p4d) || p4d_bad(*p4d))
		return -ENOENT;

	pud = pud_offset(p4d, vaddr);
	if (pud_none(*pud) || pud_bad(*pud))
		return -ENOENT;

	pmd = pmd_offset(pud, vaddr);
	if (pmd_none(*pmd))
		return -ENOENT;

	ptep = minimem_pte_offset_map_lock(mm, pmd, vaddr, &ptl);
	if (!ptep)
		return -ENOENT;

	old_pte = *ptep;

	if (!pte_present(old_pte)) {
		minimem_pte_unmap_unlock(ptep, ptl);
		return -ENOENT;
	}

	page = pte_page(old_pte);
	get_page(page);

	minimem_pte_unmap_unlock(ptep, ptl);

	src_addr = kmap_local_page(page);
	if (!src_addr) {
		put_page(page);
		return -EFAULT;
	}

	preempt_disable();
	dst_buf = minimem_get_compress_buf();
	if (!dst_buf) {
		preempt_enable();
		kunmap_local(src_addr);
		put_page(page);
		return -EINVAL;
	}

	ret = minimem_compress_page(src_addr, MINIMEM_PAGE_SIZE, &res);
	if (ret != MINIMEM_OK) {
		preempt_enable();
		kunmap_local(src_addr);
		put_page(page);
		if (ret == MINIMEM_INCOMPRESSIBLE)
			return MINIMEM_INCOMPRESSIBLE;
		return ret;
	}

	if (res.compressed_size == 0 ||
	    res.compressed_size >= MINIMEM_PAGE_SIZE) {
		preempt_enable();
		kunmap_local(src_addr);
		put_page(page);
		return MINIMEM_INCOMPRESSIBLE;
	}

	{
		long min_pct = minimem_scanner_min_savings_pct();
		long savings_pct = (100L * (MINIMEM_PAGE_SIZE - res.compressed_size))
				   / MINIMEM_PAGE_SIZE;

		if (savings_pct < min_pct) {
			preempt_enable();
			kunmap_local(src_addr);
			put_page(page);
			return MINIMEM_INCOMPRESSIBLE;
		}
	}

	copy_len = res.compressed_size;

	local_buf = kmalloc(copy_len, GFP_ATOMIC);
	if (!local_buf) {
		preempt_enable();
		kunmap_local(src_addr);
		put_page(page);
		return -ENOMEM;
	}
	memcpy(local_buf, dst_buf, copy_len);
	preempt_enable();
	kunmap_local(src_addr);

	nid = page_to_nid(page);
	handle = zs_malloc(minimem_zswap_pool(), copy_len, GFP_NOIO, nid);
	if (!handle) {
		kfree(local_buf);
		put_page(page);
		return -ENOMEM;
	}

	zs_obj_write(minimem_zswap_pool(), handle, local_buf, copy_len);
	kfree(local_buf);

	map_index = minimem_vaddr_to_index(vaddr);
	ret = minimem_map_store(minimem_zswap_map(), vaddr, res.algo_id,
				res.compressed_size, handle);
	if (ret) {
		zs_free(minimem_zswap_pool(), handle);
		put_page(page);
		return ret;
	}

	entry = make_minimem_entry(map_index);
	new_pte = minimem_swp_entry_to_pte(entry);

	ptep = minimem_pte_offset_map_lock(mm, pmd, vaddr, &ptl);
	if (!ptep) {
		minimem_map_remove(minimem_zswap_map(), vaddr);
		zs_free(minimem_zswap_pool(), handle);
		put_page(page);
		return -ENOENT;
	}

	if (!pte_same(*ptep, old_pte)) {
		minimem_pte_unmap_unlock(ptep, ptl);
		minimem_map_remove(minimem_zswap_map(), vaddr);
		zs_free(minimem_zswap_pool(), handle);
		put_page(page);
		return -EAGAIN;
	}

	minimem_set_pte_at(mm, vaddr, ptep, new_pte);
	minimem_pte_unmap_unlock(ptep, ptl);

	/*
	 * Decrement the anonymous page RSS counter. We replaced a present
	 * PTE with a swap entry, so the page is no longer mapped. We can't
	 * use dec_mm_counter() because mm_trace_rss_stat is unexported, so
	 * we update the percpu counter directly.
	 */
	percpu_counter_add(&mm->rss_stat[MM_ANONPAGES], -1);
	put_page(page);

	minimem_mmu_register_deferred(mm);

	atomic64_inc(&hook_faults_handled);

	return 0;
}