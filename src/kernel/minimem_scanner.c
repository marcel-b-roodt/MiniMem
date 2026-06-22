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
 * When a fault handler is available (kernel patches or kprobe on
 * do_swap_page), the scanner uses VMA-based scanning with PTE
 * replacement. Otherwise falls back to PFN-based compression-only.
 *
 * Two-pass cold page detection (VMA mode):
 *   Mark pass: set idle flag + clear young flag on anonymous pages.
 *   Sweep pass: pages still idle with young clear are cold → compress.
 *
 * Performance safeguards:
 *   - cond_resched() in all inner loops to prevent CPU starvation
 *   - Batch limit: max pages per scan cycle (default 8192)
 *   - Adaptive interval: backs off when no compression, resets when active
 *   - Skips VMAs that are mlock'd, shared, or device-mapped
 *   - Skips pages that are shared (mapcount > 1) or mlocked
 *   - Bloom filter skip-list for recently incompressible pages
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
#include <linux/bitmap.h>
#include <linux/module.h>
#include <linux/rmap.h>

#ifdef CONFIG_PAGE_IDLE_FLAG
#include <linux/page_idle.h>
#endif

#include "minimem_scanner.h"
#include "minimem_zswap.h"
#include "minimem_hook.h"
#include "minimem_proc_stats.h"

static struct task_struct *scanner_task;
static atomic_t scanner_enabled = ATOMIC_INIT(0);
static atomic64_t scanner_interval_ms = ATOMIC64_INIT(1000);
static atomic64_t scanner_min_savings_pct = ATOMIC64_INIT(13);
static atomic64_t scanner_pages_scanned;
static atomic64_t scanner_pages_idle;
static atomic64_t scanner_pages_compressed;
static atomic64_t scanner_pages_skipped;
static atomic64_t scanner_pfn_cursor;
static atomic64_t scanner_mark_pages;
static atomic64_t scanner_skip_vma_locked;
static atomic64_t scanner_skip_page_shared;
static atomic64_t scanner_skip_page_mlocked;
static atomic64_t scanner_skip_incompressible;
static atomic64_t scanner_cycles_total;
static atomic64_t scanner_cycles_empty;
static atomic64_t scanner_current_interval_ms;

#define MINIMEM_MAX_BATCH_PAGES		8192
#define MINIMEM_ADAPTIVE_MIN_MS		1000
#define MINIMEM_ADAPTIVE_MAX_MS		30000
#define MINIMEM_ADAPTIVE_BACKOFF_MS	2000
#define MINIMEM_SKIP_LIST_BITS		14
#define MINIMEM_SKIP_LIST_SIZE		(1 << MINIMEM_SKIP_LIST_BITS)
#define MINIMEM_SKIP_LIST_MASK		(MINIMEM_SKIP_LIST_SIZE - 1)
#define MINIMEM_SKIP_DECAY_CYCLES	8

static atomic_t scanner_phase;

static DECLARE_BITMAP(skip_list, MINIMEM_SKIP_LIST_SIZE);

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
	if (val) {
		atomic_set(&scanner_phase, 0);
		bitmap_zero(skip_list, MINIMEM_SKIP_LIST_SIZE);
	}
}

void minimem_scanner_set_interval_ms(long ms)
{
	if (ms < 100)
		ms = 100;
	if (ms > 60000)
		ms = 60000;
	atomic64_set(&scanner_interval_ms, ms);
	atomic64_set(&scanner_current_interval_ms, ms);
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

unsigned long minimem_scanner_mark_pages(void)
{
	return atomic64_read(&scanner_mark_pages);
}

unsigned long minimem_scanner_skip_vma_locked(void)
{
	return atomic64_read(&scanner_skip_vma_locked);
}

unsigned long minimem_scanner_skip_page_shared(void)
{
	return atomic64_read(&scanner_skip_page_shared);
}

unsigned long minimem_scanner_skip_page_mlocked(void)
{
	return atomic64_read(&scanner_skip_page_mlocked);
}

unsigned long minimem_scanner_skip_incompressible(void)
{
	return atomic64_read(&scanner_skip_incompressible);
}

unsigned long minimem_scanner_cycles_total(void)
{
	return atomic64_read(&scanner_cycles_total);
}

unsigned long minimem_scanner_cycles_empty(void)
{
	return atomic64_read(&scanner_cycles_empty);
}

static void skip_list_add(unsigned long vaddr)
{
	unsigned long idx = (vaddr >> PAGE_SHIFT) & MINIMEM_SKIP_LIST_MASK;

	test_and_set_bit(idx, skip_list);
}

static bool skip_list_check(unsigned long vaddr)
{
	unsigned long idx = (vaddr >> PAGE_SHIFT) & MINIMEM_SKIP_LIST_MASK;

	return test_bit(idx, skip_list);
}

static void skip_list_decay(void)
{
	bitmap_zero(skip_list, MINIMEM_SKIP_LIST_SIZE);
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

		cond_resched();
	}

	atomic64_set(&scanner_pfn_cursor, end);
	atomic64_add(scanned, &scanner_pages_scanned);
	atomic64_add(idle, &scanner_pages_idle);
	atomic64_add(compressed, &scanner_pages_compressed);
	atomic64_add(skipped, &scanner_pages_skipped);

	return scanned;
}

static unsigned long minimem_mark_vma(struct mm_struct *mm,
				       struct vm_area_struct *vma,
				       unsigned long *nr_marked)
{
	unsigned long addr, end;
	unsigned long pages_examined = 0;
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
		if (kthread_should_stop())
			break;

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

		/*
		 * Skip mlocked pages — they must stay resident.
		 * Use folio_test_mlocked() for 6.x+ kernel API.
		 */
		folio = page_folio(page);
		if (folio_test_mlocked(folio)) {
			atomic64_inc(&scanner_skip_page_mlocked);
			pte_unmap_unlock(ptep, ptl);
			continue;
		}

		/*
		 * Skip pages with elevated reference counts — another
		 * path may be using this page. page_mapcount() is not
		 * available to modules, so we use page_count() as a
		 * conservative heuristic: if refcount > baseline (1 for
		 * the page itself + 1 for the PTE reference), skip it.
		 */
		if (page_count(page) > 2) {
			atomic64_inc(&scanner_skip_page_shared);
			pte_unmap_unlock(ptep, ptl);
			continue;
		}

		folio_set_idle(folio);
		folio_test_clear_young(folio);

		pte_unmap_unlock(ptep, ptl);
		(*nr_marked)++;
		pages_examined++;

		if (pages_examined % 256 == 0)
			cond_resched();
	}

	return pages_examined;
}

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
	unsigned long total_batch = MINIMEM_MAX_BATCH_PAGES;

	addr = vma->vm_start;
	end = vma->vm_end;

	for (; addr < end; addr += PAGE_SIZE) {
		if (kthread_should_stop())
			break;

		if (total_batch == 0) {
			cond_resched();
			break;
		}

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

		if (page_count(page) > 2) {
			atomic64_inc(&scanner_skip_page_shared);
			pte_unmap_unlock(ptep, ptl);
			continue;
		}

		if (folio_test_mlocked(page_folio(page))) {
			atomic64_inc(&scanner_skip_page_mlocked);
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

		if (skip_list_check(addr)) {
			atomic64_inc(&scanner_skip_incompressible);
			pte_unmap_unlock(ptep, ptl);
			skipped++;
			total_batch--;
			continue;
		}

		pte_unmap_unlock(ptep, ptl);

		scanned++;

		if (minimem_compress_and_replace_pte(mm, vma, addr) == 0) {
			compressed++;
		} else {
			skipped++;
			skip_list_add(addr);
		}

		total_batch--;

		if (scanned % 64 == 0)
			cond_resched();
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
	unsigned long total_marked = 0;
	int phase = atomic_read(&scanner_phase);
	bool is_sweep = (phase != 0);

	if (is_sweep) {
		if (!minimem_hook_fault_handler_ready()) {
			pr_info_once("minimem: scanner sweep disabled — "
				     "no fault handler available\n");
			atomic_set(&scanner_phase, 0);
			return 0;
		}
	}

	rcu_read_lock();
	for_each_process(task) {
		struct mm_struct *mm;
		struct vm_area_struct *vma;
		unsigned long vma_count = 0;

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
			if (vma->vm_flags & VM_LOCKED) {
				atomic64_inc(&scanner_skip_vma_locked);
				continue;
			}
			if (vma->vm_flags & VM_SHARED)
				continue;

			if (kthread_should_stop())
				break;

			if (is_sweep)
				total += minimem_sweep_vma(mm, vma);
			else
				total += minimem_mark_vma(mm, vma,
							 &total_marked);

			vma_count++;
			if (vma_count % 16 == 0)
				cond_resched();
		}

		mmap_read_unlock(mm);
		mmput(mm);
	}
	rcu_read_unlock();

	atomic_set(&scanner_phase, is_sweep ? 0 : 1);

	if (!is_sweep)
		atomic64_set(&scanner_mark_pages, total_marked);

	pr_debug("minimem: scanner %s: %lu pages %s\n",
		 is_sweep ? "sweep" : "mark",
		 is_sweep ? total : total_marked,
		 is_sweep ? "compressed/skipped" : "marked idle");

	return is_sweep ? total : total_marked;
}

static unsigned long minimem_scan_batch(void)
{
	if (minimem_hook_fault_handler_ready())
		return minimem_scan_batch_vma();
	else if (minimem_hook_symbols_resolved())
		return minimem_scan_batch_vma();
	else
		return minimem_scan_batch_pfn();
}

#endif /* CONFIG_PAGE_IDLE_FLAG */

static int scanner_thread(void *data)
{
	unsigned long empty_cycles = 0;
	long current_interval;

	pr_info("minimem: scanner thread started\n");

	current_interval = atomic64_read(&scanner_interval_ms);

	while (!kthread_should_stop()) {
		unsigned long result;

		if (!atomic_read(&scanner_enabled)) {
			msleep_interruptible(1000);
			continue;
		}

		if (!minimem_hook_fault_handler_ready() &&
		    !minimem_hook_symbols_resolved()) {
			msleep_interruptible(5000);
			continue;
		}

		atomic64_inc(&scanner_cycles_total);

		result = minimem_scan_batch();
		minimem_proc_stats_gc();

		if (result == 0) {
			empty_cycles++;
			if (empty_cycles >= MINIMEM_SKIP_DECAY_CYCLES) {
				skip_list_decay();
				empty_cycles = 0;
			}
		} else {
			empty_cycles = 0;
		}

		atomic64_inc(&scanner_cycles_empty);

		if (result == 0) {
			current_interval = min(current_interval +
					       MINIMEM_ADAPTIVE_BACKOFF_MS,
					       (long)MINIMEM_ADAPTIVE_MAX_MS);
		} else {
			current_interval = atomic64_read(&scanner_interval_ms);
		}

		atomic64_set(&scanner_current_interval_ms, current_interval);

		msleep_interruptible(current_interval);
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
	atomic64_set(&scanner_mark_pages, 0);
	atomic64_set(&scanner_skip_vma_locked, 0);
	atomic64_set(&scanner_skip_page_shared, 0);
	atomic64_set(&scanner_skip_page_mlocked, 0);
	atomic64_set(&scanner_skip_incompressible, 0);
	atomic64_set(&scanner_cycles_total, 0);
	atomic64_set(&scanner_cycles_empty, 0);
	atomic64_set(&scanner_current_interval_ms,
		     atomic64_read(&scanner_interval_ms));
	atomic_set(&scanner_phase, 0);
	bitmap_zero(skip_list, MINIMEM_SKIP_LIST_SIZE);

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