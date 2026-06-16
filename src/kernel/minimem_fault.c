/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * minimem_fault.c — Page fault handler stub for MiniMem compressed pages
 *
 * Provides:
 * 1. minimem_handle_fault() — stub for decompress-on-fault (needs kernel patch)
 * 2. minimem_is_compressed() — check if a vaddr is in the compression map
 * 3. PTE marker encoding test interface (debugfs)
 *
 * The actual kprobe-based fault handler lives in minimem_hook.c.
 * PTE replacement is implemented in minimem_hook.c via
 * minimem_compress_and_replace_pte() which uses resolved kernel symbols.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/debugfs.h>
#include <linux/atomic.h>
#include <linux/uaccess.h>
#include <asm/pgtable.h>

#include "minimem_fault.h"
#include "minimem_pte.h"
#include "minimem_zswap.h"
#include "minimem_map.h"

static struct minimem_fault_stats {
	atomic64_t faults_intercepted;
} fault_stats;

int minimem_handle_fault(struct vm_area_struct *vma,
			 unsigned long addr, swp_entry_t entry)
{
	atomic64_inc(&fault_stats.faults_intercepted);
	return -EOPNOTSUPP;
}

bool minimem_is_compressed(unsigned long vaddr)
{
	struct minimem_map_entry entry;

	return minimem_map_lookup(minimem_zswap_map(),
				  vaddr & PAGE_MASK, &entry) == 0;
}

int minimem_fault_init(void)
{
	atomic64_set(&fault_stats.faults_intercepted, 0);
	return 0;
}

void minimem_fault_exit(void)
{
}

/* ---- PTE marker test (called from main module debugfs) ---- */

static unsigned long last_encoded_index;

ssize_t minimem_pte_test_write(const char __user *buf, size_t count)
{
	char kbuf[64];
	unsigned long val;
	swp_entry_t entry;
	pte_t pte;

	if (count > 63)
		return -EINVAL;

	if (copy_from_user(kbuf, buf, count))
		return -EFAULT;

	kbuf[count] = '\0';
	if (count > 0 && kbuf[count - 1] == '\n')
		kbuf[count - 1] = '\0';

	if (strncmp(kbuf, "encode ", 7) == 0) {
		if (kstrtoul(kbuf + 7, 0, &val))
			return -EINVAL;

		entry = make_minimem_entry(val);
		pte = minimem_entry_to_pte(entry);

		if (!is_minimem_pte(pte)) {
			pr_err("minimem: PTE marker roundtrip FAILED for index %lu\n",
			       val);
			return -EIO;
		}

		if (minimem_pte_index(pte) != val) {
			pr_err("minimem: PTE marker index mismatch: encoded %lu, decoded %lu\n",
			       val, minimem_pte_index(pte));
			return -EIO;
		}

		last_encoded_index = val;
		pr_info("minimem: PTE marker roundtrip OK for index %lu (entry.val=0x%lx)\n",
			val, entry.val);
	} else if (strcmp(kbuf, "decode") == 0) {
		entry = make_minimem_entry(last_encoded_index);
		pr_info("minimem: last encoded index=%lu entry.val=0x%lx\n",
			last_encoded_index, entry.val);
	} else {
		return -EINVAL;
	}

	return count;
}

ssize_t minimem_fault_stats_read(char *kbuf, size_t kbuf_size)
{
	int len;

	len = snprintf(kbuf, kbuf_size,
		       "faults_intercepted %lld\n",
		       atomic64_read(&fault_stats.faults_intercepted));

	return len;
}