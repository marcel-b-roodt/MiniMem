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
 *   - CPU budget: each phase (mark/sweep) has a wall-clock time limit
 *     (scanner_cpu_budget_ms, default 50ms). If exceeded, the phase
 *     aborts and resumes next cycle via a saved cursor.
 *   - Mark batch limit: caps pages examined per mark cycle
 *     (scanner_mark_budget_pages, default 65536). Prevents unbounded
 *     CPU on large-memory systems.
 *   - Sweep batch limit: max compressed pages per sweep cycle (8192).
 *   - Cursor-based resumption: both mark and sweep save position
 *     (pid + addr) so work continues where it left off next cycle.
 *   - Adaptive interval: backs off when no compression, resets when active
 *   - cond_resched() in all inner loops to yield CPU
 *   - Skips VMAs that are mlock'd, shared, or device-mapped
 *   - Skips pages that are shared (mapcount > 1) or mlocked
 *   - Bloom filter skip-list for recently incompressible pages
 *
 * Sysfs controls:
 *   scanner_enabled           — enable/disable (0/1)
 *   scanner_interval_ms       — scan interval in milliseconds (100-60000)
 *   min_savings_pct           — minimum savings % threshold for compression
 *   scanner_cpu_budget_ms     — max wall-clock ms per phase (1-1000)
 *   scanner_mark_budget_pages — max pages examined per mark phase (4096-524288)
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
#include <linux/timekeeping.h>

#ifdef CONFIG_PAGE_IDLE_FLAG
#include <linux/page_idle.h>
#endif

#include "minimem_scanner.h"
#include "minimem_zswap.h"
#include "minimem_hook.h"
#include "minimem_mmu.h"
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
static atomic64_t scanner_cpu_budget_ms = ATOMIC64_INIT(50);
static atomic64_t scanner_mark_budget_pages = ATOMIC64_INIT(65536);
static atomic64_t scanner_mark_yielded;
static atomic64_t scanner_sweep_yielded;

#define MINIMEM_MAX_BATCH_PAGES		8192
#define MINIMEM_ADAPTIVE_MIN_MS		1000
#define MINIMEM_ADAPTIVE_MAX_MS		30000
#define MINIMEM_ADAPTIVE_BACKOFF_MS	2000
#define MINIMEM_SKIP_LIST_BITS		14
#define MINIMEM_SKIP_LIST_SIZE		(1 << MINIMEM_SKIP_LIST_BITS)
#define MINIMEM_SKIP_LIST_MASK		(MINIMEM_SKIP_LIST_SIZE - 1)
#define MINIMEM_SKIP_DECAY_CYCLES	8
#define MINIMEM_CPU_BUDGET_MIN_MS	1
#define MINIMEM_CPU_BUDGET_MAX_MS	1000
#define MINIMEM_MARK_BUDGET_MIN		4096
#define MINIMEM_MARK_BUDGET_MAX		524288

static atomic_t scanner_phase;

static DECLARE_BITMAP(skip_list, MINIMEM_SKIP_LIST_SIZE);

/*
 * Mark cursor: saves position across mark cycles so we don't restart
 * from the beginning every time. On large-memory systems with millions
 * of pages, this prevents CPU spikes by spreading the mark work across
 * multiple cycles.
 *
 * cursor_pid: PID of the process we were scanning (0 = start from beginning)
 * cursor_addr: virtual address within that process's VMA to resume from
 * cursor_valid: if true, resume from saved position; if false, start over
 */
static pid_t mark_cursor_pid;
static unsigned long mark_cursor_addr;
static bool mark_cursor_valid;

/*
 * Sweep cursor: same idea for the sweep phase.
 */
static pid_t sweep_cursor_pid;
static unsigned long sweep_cursor_addr;
static bool sweep_cursor_valid;

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
		mark_cursor_valid = false;
		sweep_cursor_valid = false;
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

unsigned long minimem_scanner_current_interval_ms(void)
{
	return atomic64_read(&scanner_current_interval_ms);
}

long minimem_scanner_cpu_budget_ms(void)
{
	return atomic64_read(&scanner_cpu_budget_ms);
}

void minimem_scanner_set_cpu_budget_ms(long ms)
{
	if (ms < MINIMEM_CPU_BUDGET_MIN_MS)
		ms = MINIMEM_CPU_BUDGET_MIN_MS;
	if (ms > MINIMEM_CPU_BUDGET_MAX_MS)
		ms = MINIMEM_CPU_BUDGET_MAX_MS;
	atomic64_set(&scanner_cpu_budget_ms, ms);
}

long minimem_scanner_mark_budget_pages(void)
{
	return atomic64_read(&scanner_mark_budget_pages);
}

void minimem_scanner_set_mark_budget_pages(long pages)
{
	if (pages < MINIMEM_MARK_BUDGET_MIN)
		pages = MINIMEM_MARK_BUDGET_MIN;
	if (pages > MINIMEM_MARK_BUDGET_MAX)
		pages = MINIMEM_MARK_BUDGET_MAX;
	atomic64_set(&scanner_mark_budget_pages, pages);
}

unsigned long minimem_scanner_mark_yielded(void)
{
	return atomic64_read(&scanner_mark_yielded);
}

unsigned long minimem_scanner_sweep_yielded(void)
{
	return atomic64_read(&scanner_sweep_yielded);
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

static bool cpu_budget_expired(ktime_t start_time)
{
	s64 elapsed_ms = ktime_ms_delta(ktime_get(), start_time);

	return elapsed_ms >= atomic64_read(&scanner_cpu_budget_ms);
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
				       unsigned long start_addr,
				       unsigned long *nr_marked,
				       unsigned long *pages_remaining,
				       ktime_t start_time,
				       unsigned long *end_addr_out)
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

	addr = max(start_addr, vma->vm_start);
	end = vma->vm_end;

	for (; addr < end; addr += PAGE_SIZE) {
		if (kthread_should_stop())
			break;

		if (*pages_remaining == 0)
			break;

		if (pages_examined % 256 == 0 && pages_examined > 0 &&
		    cpu_budget_expired(start_time))
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

		folio = page_folio(page);
		if (folio_test_mlocked(folio)) {
			atomic64_inc(&scanner_skip_page_mlocked);
			pte_unmap_unlock(ptep, ptl);
			continue;
		}

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
		(*pages_remaining)--;

		if (pages_examined % 256 == 0)
			cond_resched();
	}

	*end_addr_out = addr;
	return pages_examined;
}

static unsigned long minimem_sweep_vma(struct mm_struct *mm,
				       struct vm_area_struct *vma,
				       unsigned long start_addr,
				       unsigned long *total_batch,
				       ktime_t start_time,
				       unsigned long *end_addr_out)
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
	unsigned long pages_since_yield = 0;

	addr = max(start_addr, vma->vm_start);
	end = vma->vm_end;

	for (; addr < end; addr += PAGE_SIZE) {
		if (kthread_should_stop())
			break;

		if (*total_batch == 0)
			break;

		if (pages_since_yield > 0 && pages_since_yield % 64 == 0 &&
		    cpu_budget_expired(start_time))
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
			(*total_batch)--;
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

		(*total_batch)--;
		pages_since_yield++;

		if (scanned % 64 == 0)
			cond_resched();
	}

	atomic64_add(scanned, &scanner_pages_scanned);
	atomic64_add(scanned, &scanner_pages_idle);
	atomic64_add(compressed, &scanner_pages_compressed);
	atomic64_add(skipped, &scanner_pages_skipped);

	*end_addr_out = addr;
	return scanned;
}

static unsigned long minimem_scan_batch_vma(void)
{
	struct task_struct *task;
	unsigned long total = 0;
	unsigned long total_marked = 0;
	int phase = atomic_read(&scanner_phase);
	bool is_sweep = (phase != 0);
	bool budget_exhausted = false;
	unsigned long pages_remaining = atomic64_read(&scanner_mark_budget_pages);
	unsigned long sweep_batch = MINIMEM_MAX_BATCH_PAGES;
	ktime_t start_time;
	pid_t *cursor_pid;
	unsigned long *cursor_addr;
	bool *cursor_valid;

	if (is_sweep) {
		if (!minimem_hook_marker_ready()) {
			pr_debug("minimem: scanner sweep skipped — "
				  "PTE marker handling requires kernel patches "
				  "(kprobe fallback cannot safely replace PTEs)\n");
			atomic_set(&scanner_phase, 0);
			return 0;
		}
	}

	start_time = ktime_get();

	if (is_sweep) {
		cursor_pid = &sweep_cursor_pid;
		cursor_addr = &sweep_cursor_addr;
		cursor_valid = &sweep_cursor_valid;
	} else {
		cursor_pid = &mark_cursor_pid;
		cursor_addr = &mark_cursor_addr;
		cursor_valid = &mark_cursor_valid;
	}

	rcu_read_lock();
	for_each_process(task) {
		struct mm_struct *mm;
		struct vm_area_struct *vma;
		unsigned long vma_count = 0;

		if (kthread_should_stop())
			break;

		if (budget_exhausted)
			break;

		if (*cursor_valid && task->pid < *cursor_pid)
			continue;

		mm = get_task_mm(task);
		if (!mm)
			continue;

		if (!mmget_not_zero(mm)) {
			mmput(mm);
			continue;
		}

		if (!mmap_read_trylock(mm)) {
			mmput(mm);
			continue;
		}

		{
			unsigned long vma_start = 0;

			if (*cursor_valid && task->pid == *cursor_pid)
				vma_start = *cursor_addr;

			VMA_ITERATOR(vmi, mm, vma_start);
			for_each_vma(vmi, vma) {
				unsigned long end_addr = 0;

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

				if (is_sweep) {
					unsigned long start = vma->vm_start;

					if (*cursor_valid &&
					    task->pid == *cursor_pid &&
					    *cursor_addr >= vma->vm_start &&
					    *cursor_addr < vma->vm_end)
						start = *cursor_addr;

					total += minimem_sweep_vma(
						mm, vma, start,
						&sweep_batch,
						start_time, &end_addr);
					if (sweep_batch == 0 ||
					    cpu_budget_expired(start_time)) {
						budget_exhausted = true;
						*cursor_pid = task->pid;
						*cursor_addr = end_addr;
						*cursor_valid = true;
						break;
					}
				} else {
					unsigned long nr_marked = 0;
					unsigned long start = vma->vm_start;

					if (*cursor_valid &&
					    task->pid == *cursor_pid &&
					    *cursor_addr >= vma->vm_start &&
					    *cursor_addr < vma->vm_end)
						start = *cursor_addr;

					minimem_mark_vma(
						mm, vma, start,
						&nr_marked,
						&pages_remaining,
						start_time, &end_addr);
					total_marked += nr_marked;
					if (pages_remaining == 0 ||
					    cpu_budget_expired(start_time)) {
						budget_exhausted = true;
						*cursor_pid = task->pid;
						*cursor_addr = end_addr;
						*cursor_valid = true;
						break;
					}
				}

				if (*cursor_valid &&
				    task->pid == *cursor_pid)
					*cursor_addr = 0;

				vma_count++;
				if (vma_count % 16 == 0)
					cond_resched();
			}
		}

		mmap_read_unlock(mm);

		if (vma_count > 0 && is_sweep && !budget_exhausted)
			minimem_mmu_register_deferred(mm);

		mmput(mm);
	}
	rcu_read_unlock();

	if (budget_exhausted) {
		if (is_sweep)
			atomic64_inc(&scanner_sweep_yielded);
		else
			atomic64_inc(&scanner_mark_yielded);
	} else {
		*cursor_valid = false;
		atomic_set(&scanner_phase, is_sweep ? 0 : 1);
	}

	if (!is_sweep)
		atomic64_set(&scanner_mark_pages, total_marked);

	pr_debug("minimem: scanner %s: %lu pages %s%s\n",
		 is_sweep ? "sweep" : "mark",
		 is_sweep ? total : total_marked,
		 is_sweep ? "compressed/skipped" : "marked idle",
		 budget_exhausted ? " (yielded)" : "");

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
			atomic64_inc(&scanner_cycles_empty);
			if (empty_cycles >= MINIMEM_SKIP_DECAY_CYCLES) {
				skip_list_decay();
				empty_cycles = 0;
			}
		} else {
			empty_cycles = 0;
		}

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
	atomic64_set(&scanner_mark_yielded, 0);
	atomic64_set(&scanner_sweep_yielded, 0);
	atomic_set(&scanner_phase, 0);
	bitmap_zero(skip_list, MINIMEM_SKIP_LIST_SIZE);
	mark_cursor_valid = false;
	sweep_cursor_valid = false;

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