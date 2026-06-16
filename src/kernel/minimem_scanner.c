/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * minimem_scanner.c — Idle page scanner daemon for MiniMem
 *
 * Background kernel thread that periodically scans pages, identifies
 * idle ones, and compresses them into zsmalloc storage.
 *
 * Compression flow:
 *   1. Scan PFNs in batches of 256
 *   2. For each idle page, attempt compress-and-store
 *   3. Store compressed data with PFN as key in the xarray map
 *   4. Skip pages that don't meet min_savings_pct threshold
 *
 * PTE replacement (installing MiniMem markers) requires kernel mm/
 * integration and is not done by the scanner. The shrinker can free
 * compressed pages under memory pressure.
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

#ifdef CONFIG_PAGE_IDLE_FLAG
#include <linux/page_idle.h>
#endif

#include "minimem_scanner.h"
#include "minimem_zswap.h"

static struct task_struct *scanner_task;
static atomic_t scanner_enabled = ATOMIC_INIT(0);
static atomic64_t scanner_interval_ms = ATOMIC64_INIT(1000);
static atomic64_t scanner_min_savings_pct = ATOMIC64_INIT(13);
static atomic64_t scanner_pages_scanned;
static atomic64_t scanner_pages_idle;
static atomic64_t scanner_pages_compressed;
static atomic64_t scanner_pages_skipped;
static atomic64_t scanner_pfn_cursor;

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

static unsigned long minimem_scan_batch(void)
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

	scanner_task = kthread_create(scanner_thread, NULL, "minimem_scand");
	if (IS_ERR(scanner_task))
		return PTR_ERR(scanner_task);

	wake_up_process(scanner_task);
	return 0;
}

void minimem_scanner_exit(void)
{
	if (scanner_task) {
		kthread_stop(scanner_task);
		scanner_task = NULL;
	}
}