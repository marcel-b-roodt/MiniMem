/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * minimem_scanner.h — Idle page scanner daemon for MiniMem
 *
 * Background kernel thread that scans for idle pages and compresses
 * them transparently. Uses PG_idle/PG_young page flags to identify
 * cold pages.
 *
 * Sysfs controls:
 *   scanner_enabled      — enable/disable (0/1)
 *   scanner_interval_ms — scan interval in milliseconds
 *   min_savings_pct     — minimum savings percentage (0-100)
 */

#ifndef MINIMEM_KERNEL_SCANNER_H
#define MINIMEM_KERNEL_SCANNER_H

#include <linux/types.h>

int minimem_scanner_init(void);
void minimem_scanner_exit(void);

bool minimem_scanner_is_enabled(void);
long minimem_scanner_interval_ms(void);
long minimem_scanner_min_savings_pct(void);
void minimem_scanner_set_enabled(int val);
void minimem_scanner_set_interval_ms(long ms);
void minimem_scanner_set_min_savings_pct(long pct);
unsigned long minimem_scanner_pages_scanned(void);
unsigned long minimem_scanner_pages_idle(void);
unsigned long minimem_scanner_pages_compressed(void);
unsigned long minimem_scanner_pages_skipped(void);
unsigned long minimem_scanner_mark_pages(void);
unsigned long minimem_scanner_skip_vma_locked(void);
unsigned long minimem_scanner_skip_page_shared(void);
unsigned long minimem_scanner_skip_page_mlocked(void);
unsigned long minimem_scanner_skip_incompressible(void);
unsigned long minimem_scanner_cycles_total(void);
unsigned long minimem_scanner_cycles_empty(void);

#endif /* MINIMEM_KERNEL_SCANNER_H */