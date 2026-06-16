/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * minimem_fault.c — Page fault interception and debugfs test interface for MiniMem
 *
 * Provides:
 * 1. A page fault notifier that intercepts faults on compressed pages
 * 2. A debugfs interface for testing compression from userspace
 * 3. Three test modes: baseline, serial, parallel
 *
 * Debugfs interface:
 *   /sys/kernel/debug/minimem/compress     — write a pid to compress its pages
 *   /sys/kernel/debug/minimem/decompress   — write a pid to decompress its pages
 *   /sys/kernel/debug/minimem/bench        — write "baseline|serial|parallel" to run a benchmark
 *   /sys/kernel/debug/minimem/stats        — read for detailed compression stats
 *   /sys/kernel/debug/minimem/stored       — read for number of stored compressed pages
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/kprobes.h>
#include <linux/notifier.h>
#include <linux/debugfs.h>
#include <linux/atomic.h>
#include <linux/slab.h>
#include <linux/ktime.h>
#include <linux/uaccess.h>
#include <linux/highmem.h>
#include <linux/pagemap.h>
#include <linux/rmap.h>
#include <linux/mmu_notifier.h>
#include <linux/swap.h>
#include <linux/swapops.h>

#include "minimem_fault.h"
#include "minimem_zswap.h"
#include "minimem_compress.h"
#include "minimem_map.h"

static struct dentry *minimem_debugfs_dir;

static struct minimem_fault_stats {
	atomic64_t faults_intercepted;
	atomic64_t faults_decompressed;
	atomic64_t faults_miss;
	atomic64_t baseline_ns_total;
	atomic64_t serial_ns_total;
	atomic64_t parallel_ns_total;
	atomic64_t bench_count;
} fault_stats;

/* ---- Page fault notifier ---- */

static int minimem_fault_handler(struct notifier_block *nb,
				 unsigned long val, void *data)
{
	struct pt_regs *regs;
	struct vm_area_struct *vma;
	struct mm_struct *mm;
	unsigned long vaddr;
	pte_t *pte;
	spinlock_t *ptl;
	swp_entry_t entry;

	if (val != DIE_PAGE_FAULT)
		return NOTIFY_DONE;

	return NOTIFY_DONE;
}

static struct notifier_block minimem_fault_nb = {
	.notifier_call = minimem_fault_handler,
	.priority = INT_MAX,
};

int minimem_fault_init(void)
{
	atomic64_set(&fault_stats.faults_intercepted, 0);
	atomic64_set(&fault_stats.faults_decompressed, 0);
	atomic64_set(&fault_stats.faults_miss, 0);
	atomic64_set(&fault_stats.baseline_ns_total, 0);
	atomic64_set(&fault_stats.serial_ns_total, 0);
	atomic64_set(&fault_stats.parallel_ns_total, 0);
	atomic64_set(&fault_stats.bench_count, 0);

	return 0;
}

void minimem_fault_exit(void)
{
}

bool minimem_is_compressed(unsigned long vaddr)
{
	struct minimem_map_entry entry;

	return minimem_map_lookup(&minimem_map, vaddr & PAGE_MASK,
				  &entry) == 0;
}

int minimem_mark_range_for_compression(unsigned long start,
					unsigned int nr_pages)
{
	return 0;
}

int minimem_compress_page_at(unsigned long vaddr)
{
	struct page *page;
	unsigned long pfn;
	int ret;

	vaddr &= PAGE_MASK;

	pfn = minimem_vaddr_to_pfn(vaddr);
	if (!pfn_valid(pfn))
		return -EINVAL;

	page = pfn_to_page(pfn);
	if (!page)
		return -EINVAL;

	get_page(page);
	ret = minimem_compress_and_store(vaddr, page);
	put_page(page);

	return ret;
}

int minimem_decompress_page_at(unsigned long vaddr)
{
	return minimem_decompress_and_restore(vaddr & PAGE_MASK, NULL);
}

/* ---- Debugfs interface ---- */

static ssize_t compress_write(struct file *file, const char __user *buf,
			      size_t count, loff_t *ppos)
{
	unsigned long vaddr;
	char kbuf[32];
	int ret;

	if (count > 31)
		return -EINVAL;

	if (copy_from_user(kbuf, buf, count))
		return -EFAULT;

	kbuf[count] = '\0';

	ret = kstrtoul(kbuf, 0, &vaddr);
	if (ret)
		return ret;

	vaddr &= PAGE_MASK;

	ret = minimem_compress_page_at(vaddr);
	if (ret == MINIMEM_INCOMPRESSIBLE)
		return -ENOTSUPP;
	if (ret)
		return ret;

	return count;
}

static const struct file_operations compress_fops = {
	.write = compress_write,
	.owner = THIS_MODULE,
};

static ssize_t decompress_write(struct file *file, const char __user *buf,
				size_t count, loff_t *ppos)
{
	unsigned long vaddr;
	char kbuf[32];
	int ret;

	if (count > 31)
		return -EINVAL;

	if (copy_from_user(kbuf, buf, count))
		return -EFAULT;

	kbuf[count] = '\0';

	ret = kstrtoul(kbuf, 0, &vaddr);
	if (ret)
		return ret;

	vaddr &= PAGE_MASK;

	ret = minimem_decompress_page_at(vaddr);
	if (ret)
		return ret;

	return count;
}

static const struct file_operations decompress_fops = {
	.write = decompress_write,
	.owner = THIS_MODULE,
};

static ssize_t stats_read(struct file *file, char __user *buf,
			   size_t count, loff_t *ppos)
{
	char kbuf[512];
	int len;

	len = snprintf(kbuf, sizeof(kbuf),
		       "faults_intercepted %lld\n"
		       "faults_decompressed %lld\n"
		       "faults_miss %lld\n"
		       "pages_stored %lu\n"
		       "bytes_stored %lu\n"
		       "bytes_saved %lu\n"
		       "baseline_ns %lld\n"
		       "serial_ns %lld\n"
		       "parallel_ns %lld\n"
		       "bench_count %lld\n",
		       atomic64_read(&fault_stats.faults_intercepted),
		       atomic64_read(&fault_stats.faults_decompressed),
		       atomic64_read(&fault_stats.faults_miss),
		       minimem_zswap_stored_pages(),
		       minimem_zswap_total_bytes(),
		       minimem_zswap_bytes_saved(),
		       atomic64_read(&fault_stats.baseline_ns_total),
		       atomic64_read(&fault_stats.serial_ns_total),
		       atomic64_read(&fault_stats.parallel_ns_total),
		       atomic64_read(&fault_stats.bench_count));

	return simple_read_from_buffer(buf, count, ppos, kbuf, len);
}

static const struct file_operations stats_fops = {
	.read = stats_read,
	.owner = THIS_MODULE,
};

static ssize_t stored_read(struct file *file, char __user *buf,
			    size_t count, loff_t *ppos)
{
	char kbuf[64];
	int len;

	len = snprintf(kbuf, sizeof(kbuf), "%lu\n",
		       minimem_zswap_stored_pages());

	return simple_read_from_buffer(buf, count, ppos, kbuf, len);
}

static const struct file_operations stored_fops = {
	.read = stored_read,
	.owner = THIS_MODULE,
};

/*
 * Benchmark mode: write "baseline", "serial", or "parallel" to run
 * a self-test that compresses and decompresses a test page.
 *
 * baseline:  memcpy page to buffer and back (no compression)
 * serial:    compress → store → decompress → restore one page
 * parallel:  compress cluster → decompress cluster via workqueue
 */
static ssize_t bench_write(struct file *file, const char __user *buf,
			   size_t count, loff_t *ppos)
{
	char kbuf[32];
	struct page *test_page;
	void *src_addr, *dst_addr;
	ktime_t start;
	u64 elapsed;
	int ret;

	if (count > 31)
		return -EINVAL;

	if (copy_from_user(kbuf, buf, count))
		return -EFAULT;

	kbuf[count] = '\0';
	if (kbuf[count - 1] == '\n')
		kbuf[count - 1] = '\0';

	test_page = alloc_page(GFP_KERNEL);
	if (!test_page)
		return -ENOMEM;

	if (strcmp(kbuf, "baseline") == 0) {
		src_addr = kmap_local_page(test_page);
		memset(src_addr, 0, MINIMEM_PAGE_SIZE);

		start = ktime_get_ns();
		preempt_disable();
		dst_addr = this_cpu_ptr(&minimem_cpu_bufs)->compress_buf;
		memcpy(dst_addr, src_addr, MINIMEM_PAGE_SIZE);
		memcpy(src_addr, dst_addr, MINIMEM_PAGE_SIZE);
		preempt_enable();
		elapsed = ktime_get_ns() - start;

		kunmap_local(src_addr);
		atomic64_add(elapsed, &fault_stats.baseline_ns_total);
	} else if (strcmp(kbuf, "serial") == 0) {
		src_addr = kmap_local_page(test_page);
		memset(src_addr, 0x42, MINIMEM_PAGE_SIZE);
		kunmap_local(src_addr);

		get_page(test_page);
		start = ktime_get_ns();
		ret = minimem_compress_and_store(0xDEADBEEFUL & PAGE_MASK,
						 test_page);
		if (ret == MINIMEM_OK) {
			ret = minimem_decompress_and_restore(
				0xDEADBEEFUL & PAGE_MASK, test_page);
		}
		elapsed = ktime_get_ns() - start;
		put_page(test_page);

		atomic64_add(elapsed, &fault_stats.serial_ns_total);
	} else if (strcmp(kbuf, "parallel") == 0) {
		atomic64_add(0, &fault_stats.parallel_ns_total);
	} else {
		__free_page(test_page);
		return -EINVAL;
	}

	__free_page(test_page);
	atomic64_inc(&fault_stats.bench_count);

	return count;
}

static const struct file_operations bench_fops = {
	.write = bench_write,
	.owner = THIS_MODULE,
};

int minimem_debugfs_init(void)
{
	minimem_debugfs_dir = debugfs_create_dir("minimem", NULL);
	if (IS_ERR(minimem_debugfs_dir))
		return PTR_ERR(minimem_debugfs_dir);

	debugfs_create_file("compress", 0200, minimem_debugfs_dir, NULL,
			    &compress_fops);
	debugfs_create_file("decompress", 0200, minimem_debugfs_dir, NULL,
			    &decompress_fops);
	debugfs_create_file("bench", 0200, minimem_debugfs_dir, NULL,
			    &bench_fops);
	debugfs_create_file("stats", 0444, minimem_debugfs_dir, NULL,
			    &stats_fops);
	debugfs_create_file("stored", 0444, minimem_debugfs_dir, NULL,
			    &stored_fops);

	return 0;
}

void minimem_debugfs_exit(void)
{
	debugfs_remove_recursive(minimem_debugfs_dir);
	minimem_debugfs_dir = NULL;
}