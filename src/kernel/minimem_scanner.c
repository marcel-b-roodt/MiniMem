/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * minimem_scanner.c — Idle page scanner daemon for MiniMem
 *
 * Background kernel thread that periodically scans pages, identifies
 * cold ones, and transparently compresses them.
 *
 * Two scanning modes:
 *   1. PFN-based (legacy): iterates physical pages, checks idle flag,
 *      compresses data via minimem_compress_and_store()
 *   2. VMA-based (transparent): iterates process VMAs, walks page tables,
 *      uses two-pass mark-and-sweep to find cold pages, then compresses
 *      them and replaces PTEs via minimem_compress_and_replace_pte()
 *
 * When hook symbols are resolved (kprobe on do_swap_page registered),
 * the scanner uses VMA-based scanning for transparent PTE replacement.
 * Otherwise falls back to PFN-based compression-only scanning.
 *
 * Two-pass cold page detection (VMA mode):
 *   Mark pass: set idle flag + clear young flag on anonymous pages.
 *   Sweep pass: pages still idle with young clear are cold → compress.
 *
 * Sysfs controls:
 *   scanner_enabled      — enable/disable (0/1)
 *   scanner_interval_ms — scan interval in milliseconds (100-60000)
 *   min_savings_pct     — minimum savings % threshold for compression
 */

#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/mm.h>
#include <linux/page-flags.h>
#include <linux/atomic.h>
#include <linux/sched.h>
#include <linux/mmu_context.h>

#ifdef CONFIG_PAGE_IDLE_FLAG
#include <linux/page_idle.h>
#endif

#include "minimem_scanner.h"
#include "minimem_zswap.h"
#include "minimem_hook.h"

static struct task_struct *scanner_task;
static atomic_t scanner_enabled = ATOMIC_INIT(0);
static atomic64_t scanner_interval_ms = ATOMIC64_INIT(1000);
static atomic64_t scanner_min_savings_pct = ATOMIC64_INIT(13);
static atomic64_t scanner_pages_scanned;
static atomic64_t scanner_pages_idle;
static atomic64_t scanner_pages_compressed;
static atomic64_t scanner_pages_skipped;
static atomic64_t scanner_pfn_cursor;

static atomic_t scanner_phase;

bool minimem_scanner_is_enabled(void)
{
	return atomic_read(&scanner_enabled) == 1;
}

long minimem_scanner_interval_ms(void)
{
	return atomic64_read(&scanner_interval_ms);
}

long minimem_scanner_min_savings_pct(void)
{
	return atomic64_read(&scanner_min_savings_pct);
}

void minimem_scanner_set_enabled(int val)
{
	atomic_set(&scanner_enabled, val ? 1 : 0);
	if (val)
		atomic_set(&scanner_phase, 0);
}

void minimem_scanner_set_interval_ms(long ms)
{
	if (ms < 100)
		ms = 100;
	if (ms > 60000)
		ms = 60000;
	atomic64_set(&scanner_interval_ms, ms);
}

void minimem_scanner_set_min_savings_pct(long pct)
{
	if (pct < 0)
		pct = 0;
	if (pct > 90)
		pct = 90;
	atomic64_set(&scanner_min_savings_pct, pct);
}

unsigned long minimem_scanner_pages_scanned(void)
{
	return atomic64_read(&scanner_pages_scanned);
}

unsigned long minimem_scanner_pages_idle(void)
{
	return atomic64_read(&scanner_pages_idle);
}

unsigned long minimem_scanner_pages_compressed(void)
{
	return atomic64_read(&scanner_pages_compressed);
}

unsigned long minimem_scanner_pages_skipped(void)
{
	return atomic64_read(&scanner_pages_skipped);
}

#ifndef CONFIG_PAGE_IDLE_FLAG

static unsigned long minimem_scan_batch(void)
{
	pr_warn_once("minimem: scanner requires CONFIG_PAGE_IDLE_FLAG\n");
	return 0;
}

#else

static unsigned long minimem_scan_batch_pfn(void)
{
	unsigned long cursor, end;
	unsigned long scanned = 0, idle = 0;
	unsigned long compressed = 0, skipped = 0;
	unsigned long max_pfn_val = totalram_pages();
	unsigned long batch_size = 256;
	struct page *page;

	cursor = atomic64_read(&scanner_pfn_cursor);
	if (cursor >= max_pfn_val)
		cursor = 0;

	end = min(cursor + batch_size, max_pfn_val);

	for (; cursor < end; cursor++) {
		if (!pfn_valid(cursor))
			continue;

		page = pfn_to_page(cursor);

		if (PageCompound(page))
			continue;

		if (!PageLRU(page))
			continue;

		if (!folio_test_idle(page_folio(page)))
			continue;

		if (folio_test_clear_young(page_folio(page)))
			continue;

		scanned++;
		idle++;

		if (minimem_compress_and_store((unsigned long)cursor << PAGE_SHIFT,
					       page) == MINIMEM_OK)
			compressed++;
		else
			skipped++;
	}

	atomic64_set(&scanner_pfn_cursor, end);
	atomic64_add(scanned, &scanner_pages_scanned);
	atomic64_add(idle, &scanner_pages_idle);
	atomic64_add(compressed, &scanner_pages_compressed);
	atomic64_add(skipped, &scanner_pages_skipped);

	return scanned;
}

/*
 * Mark pass: walk a VMA's page tables and set the idle flag on all
 * present anonymous pages, then clear the young flag.
 *
 * IMPORTANT: folio_set_idle() and folio_test_clear_young() are called
 * while the PTL is still held, before pte_unmap_unlock(). This prevents
 * the page from being freed between the PTE check and the folio call.
 */
static unsigned long minimem_mark_vma(struct mm_struct *mm,
				       struct vm_area_struct *vma)
{
	unsigned long addr, end;
	unsigned long marked = 0;
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *ptep, pte;
	spinlock_t *ptl;
	struct page *page;
	struct folio *folio;

	addr = vma->vm_start;
	end = vma->vm_end;

	for (; addr < end; addr += PAGE_SIZE) {
		pgd = pgd_offset(mm, addr);
		if (pgd_none(*pgd) || pgd_bad(*pgd))
			continue;

		p4d = p4d_offset(pgd, addr);
		if (p4d_none(*p4d) || p4d_bad(*p4d))
			continue;

		pud = pud_offset(p4d, addr);
		if (pud_none(*pud) || pud_bad(*pud))
			continue;

		pmd = pmd_offset(pud, addr);
		if (pmd_none(*pmd))
			continue;

		if (pmd_trans_huge(*pmd))
			continue;

		ptep = minimem_pte_offset_map_lock(mm, pmd, addr, &ptl);
		if (!ptep)
			continue;

		pte = *ptep;

		if (!pte_present(pte)) {
			pte_unmap_unlock(ptep, ptl);
			continue;
		}

		page = pte_page(pte);
		if (!page || IS_ERR(page) || PageCompound(page)) {
			pte_unmap_unlock(ptep, ptl);
			continue;
		}

		if (!PageAnon(page)) {
			pte_unmap_unlock(ptep, ptl);
			continue;
		}

		if (!PageLRU(page)) {
			pte_unmap_unlock(ptep, ptl);
			continue;
		}

		folio = page_folio(page);
		folio_set_idle(folio);
		folio_test_clear_young(folio);

		pte_unmap_unlock(ptep, ptl);
		marked++;
	}

	return marked;
}

/*
 * Sweep pass: walk a VMA's page tables and compress any pages whose
 * idle flag is set and young flag is clear (cold pages).
 *
 * The idle/young checks are done while PTL is held to prevent the page
 * from being freed between the check and the folio operation.
 */
static unsigned long minimem_sweep_vma(struct mm_struct *mm,
					struct vm_area_struct *vma)
{
	unsigned long addr, end;
	unsigned long scanned = 0, compressed = 0, skipped = 0;
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *ptep, pte;
	spinlock_t *ptl;
	struct page *page;
	struct folio *folio;

	addr = vma->vm_start;
	end = vma->vm_end;

	for (; addr < end; addr += PAGE_SIZE) {
		pgd = pgd_offset(mm, addr);
		if (pgd_none(*pgd) || pgd_bad(*pgd))
			continue;

		p4d = p4d_offset(pgd, addr);
		if (p4d_none(*p4d) || p4d_bad(*p4d))
			continue;

		pud = pud_offset(p4d, addr);
		if (pud_none(*pud) || pud_bad(*pud))
			continue;

		pmd = pmd_offset(pud, addr);
		if (pmd_none(*pmd))
			continue;

		if (pmd_trans_huge(*pmd))
			continue;

		ptep = minimem_pte_offset_map_lock(mm, pmd, addr, &ptl);
		if (!ptep)
			continue;

		pte = *ptep;

		if (!pte_present(pte)) {
			pte_unmap_unlock(ptep, ptl);
			continue;
		}

		page = pte_page(pte);
		if (!page || IS_ERR(page) || PageCompound(page)) {
			pte_unmap_unlock(ptep, ptl);
			continue;
		}

		if (!PageAnon(page)) {
			pte_unmap_unlock(ptep, ptl);
			continue;
		}

		if (!PageLRU(page)) {
			pte_unmap_unlock(ptep, ptl);
			continue;
		}

		folio = page_folio(page);

		if (!folio_test_idle(folio)) {
			pte_unmap_unlock(ptep, ptl);
			continue;
		}

		if (folio_test_clear_young(folio)) {
			folio_clear_idle(folio);
			pte_unmap_unlock(ptep, ptl);
			continue;
		}

		pte_unmap_unlock(ptep, ptl);

		scanned++;

		if (minimem_compress_and_replace_pte(mm, vma, addr) == 0)
			compressed++;
		else
			skipped++;

		if (kthread_should_stop())
			break;
	}

	atomic64_add(scanned, &scanner_pages_scanned);
	atomic64_add(scanned, &scanner_pages_idle);
	atomic64_add(compressed, &scanner_pages_compressed);
	atomic64_add(skipped, &scanner_pages_skipped);

	return scanned;
}

static unsigned long minimem_scan_batch_vma(void)
{
	struct task_struct *task;
	unsigned long total = 0;
	int phase = atomic_read(&scanner_phase);
	bool is_sweep = (phase != 0);

	/*
	 * The sweep pass (transparent PTE replacement) requires the kernel
	 * to properly handle MiniMem PTE marker faults. On unpatched kernels,
	 * do_swap_page processes our swap entries and may return VM_FAULT_SIGBUS
	 * which kills the process. Only run the sweep pass when we have
	 * confirmed that fault decompression works (via compress_vaddr E2E
	 * test) or when the kernel patches are applied.
	 *
	 * For now, the sweep pass is disabled on unpatched kernels.
	 * The mark pass still runs to set idle flags for future use.
	 * The compress_vaddr debugfs interface works because it targets
	 * specific pages and the test process can handle the fault path.
	 *
	 * TODO: Enable sweep pass when kernel patches are applied or when
	 * a reliable fault interception mechanism is available.
	 */
	if (is_sweep) {
		if (!minimem_hook_marker_ready()) {
			pr_info_once("minimem: scanner sweep disabled — "
				     "kretprobe on do_swap_page not registered\n");
			atomic_set(&scanner_phase, 0);
			return 0;
		}
	}

	rcu_read_lock();
	for_each_process(task) {
		struct mm_struct *mm;
		struct vm_area_struct *vma;

		if (kthread_should_stop())
			break;

		mm = get_task_mm(task);
		if (!mm)
			continue;

		if (!mmget_not_zero(mm)) {
			mmput(mm);
			continue;
		}

		mmap_read_lock(mm);

		VMA_ITERATOR(vmi, mm, 0);
		for_each_vma(vmi, vma) {
			if (!vma)
				break;
			if (!vma_is_anonymous(vma))
				continue;
			if (!vma->vm_start || !vma->vm_end)
				continue;
			if (vma->vm_flags & (VM_PFNMAP | VM_IO))
				continue;

			if (kthread_should_stop())
				break;

			if (is_sweep)
		total += minimem_sweep_vma(mm, vma);
	else
		total += minimem_mark_vma(mm, vma);
		}

		mmap_read_unlock(mm);
		mmput(mm);
	}
	rcu_read_unlock();

	atomic_set(&scanner_phase, is_sweep ? 0 : 1);

	if (!is_sweep)
		pr_info("minimem: scanner mark: %lu pages marked idle\n", total);
	else
		pr_info("minimem: scanner sweep: %lu cold pages\n", total);

	return total;
}

static unsigned long minimem_scan_batch(void)
{
	if (minimem_hook_symbols_resolved())
		return minimem_scan_batch_vma();
	else
		return minimem_scan_batch_pfn();
}

#endif /* CONFIG_PAGE_IDLE_FLAG */

static int scanner_thread(void *data)
{
	pr_info("minimem: scanner thread started\n");

	while (!kthread_should_stop()) {
		if (!atomic_read(&scanner_enabled)) {
			msleep_interruptible(1000);
			continue;
		}

		minimem_scan_batch();

		msleep_interruptible(atomic64_read(&scanner_interval_ms));
	}

	pr_info("minimem: scanner thread stopped\n");
	return 0;
}

int minimem_scanner_init(void)
{
	atomic64_set(&scanner_pages_scanned, 0);
	atomic64_set(&scanner_pages_idle, 0);
	atomic64_set(&scanner_pages_compressed, 0);
	atomic64_set(&scanner_pages_skipped, 0);
	atomic64_set(&scanner_pfn_cursor, 0);
	atomic_set(&scanner_phase, 0);

	scanner_task = kthread_create(scanner_thread, NULL, "minimem_scand");
	if (IS_ERR(scanner_task))
		return PTR_ERR(scanner_task);

	wake_up_process(scanner_task);
	return 0;
}

void minimem_scanner_exit(void)
{
	struct task_struct *t = scanner_task;

	if (!t)
		return;

	scanner_task = NULL;
	atomic_set(&scanner_enabled, 0);

	if (!IS_ERR(t))
		kthread_stop(t);
}