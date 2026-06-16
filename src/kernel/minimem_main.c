/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * minimem_main.c — MiniMem transparent in-memory page compression module
 *
 * Provides transparent compression of cold memory pages using multiple
 * algorithms (same-page detection, BDI, WKdm, WKdm-64, block classifier,
 * LZ4) with sysfs stats and debugfs test interface.
 *
 * Three test modes via debugfs:
 * - baseline:  memcpy roundtrip (no compression) — raw memory copy latency
 * - serial:    compress → store → decompress → restore, one page at a time
 * - parallel:  compress cluster → decompress cluster via workqueue
 *
 * Architecture:
 *   minimem_zswap  — zsmalloc storage + compression map
 *   minimem_fault  — debugfs interface + fault hook (future)
 *   minimem_parallel — workqueue-based cluster decompression
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/spinlock.h>
#include <linux/atomic.h>
#include <linux/sysfs.h>
#include <linux/kobject.h>
#include <linux/debugfs.h>
#include <linux/slab.h>
#include <linux/ktime.h>
#include <linux/highmem.h>
#include <linux/uaccess.h>
#include <linux/zsmalloc.h>

#include "minimem_compress.h"
#include "minimem_zswap.h"
#include "minimem_parallel.h"
#include "minimem_fault.h"
#include "minimem_shrinker.h"
#include "minimem_scanner.h"
#include "minimem_hook.h"

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("MiniMem Project");
MODULE_DESCRIPTION("Transparent in-memory page compression");
MODULE_VERSION("0.6.0");

#define MINIMEM_VERSION_STR "0.6.0"

static struct minimem_stats {
	atomic64_t pages_compressed;
	atomic64_t pages_decompressed;
	atomic64_t bytes_saved;
	atomic64_t compress_count;
	atomic64_t decompress_count;
	atomic64_t compress_ns_total;
	atomic64_t decompress_ns_total;
} minimem_stats;

static atomic64_t bench_baseline_ns;
static atomic64_t bench_serial_ns;
static atomic64_t bench_parallel_ns;
static atomic64_t bench_pages;

static struct kobject *minimem_kobj;
static struct dentry *minimem_debugfs_dir;

/* ---- Sysfs attributes ---- */

static ssize_t pages_compressed_show(struct kobject *kobj,
				     struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%lld\n",
		       atomic64_read(&minimem_stats.pages_compressed));
}

static ssize_t pages_decompressed_show(struct kobject *kobj,
				       struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%lld\n",
		       atomic64_read(&minimem_stats.pages_decompressed));
}

static ssize_t bytes_saved_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%lu\n", minimem_zswap_bytes_saved());
}

static ssize_t compress_count_show(struct kobject *kobj,
				   struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%lld\n",
		       atomic64_read(&minimem_stats.compress_count));
}

static ssize_t decompress_count_show(struct kobject *kobj,
				      struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%lld\n",
		       atomic64_read(&minimem_stats.decompress_count));
}

static ssize_t compress_ns_total_show(struct kobject *kobj,
				       struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%lld\n",
		       atomic64_read(&minimem_stats.compress_ns_total));
}

static ssize_t decompress_ns_total_show(struct kobject *kobj,
					struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%lld\n",
		       atomic64_read(&minimem_stats.decompress_ns_total));
}

static ssize_t decompress_avg_ns_show(struct kobject *kobj,
				       struct kobj_attribute *attr, char *buf)
{
	s64 count = atomic64_read(&minimem_stats.decompress_count);
	s64 total = atomic64_read(&minimem_stats.decompress_ns_total);

	if (count == 0)
		return sprintf(buf, "0\n");

	return sprintf(buf, "%lld\n", total / count);
}

static ssize_t compress_avg_ns_show(struct kobject *kobj,
				     struct kobj_attribute *attr, char *buf)
{
	s64 count = atomic64_read(&minimem_stats.compress_count);
	s64 total = atomic64_read(&minimem_stats.compress_ns_total);

	if (count == 0)
		return sprintf(buf, "0\n");

	return sprintf(buf, "%lld\n", total / count);
}

static ssize_t zswap_pages_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%lu\n", minimem_zswap_stored_pages());
}

static ssize_t zswap_bytes_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%lu\n", minimem_zswap_total_bytes());
}

static ssize_t zswap_saved_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%lu\n", minimem_zswap_bytes_saved());
}

static ssize_t pool_pages_show(struct kobject *kobj,
			       struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%lu\n", zs_get_total_pages(minimem_zswap_pool()));
}

static ssize_t parallel_clusters_show(struct kobject *kobj,
				      struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%lld\n",
		       atomic64_read(&minimem_par_stats.clusters_decompressed));
}

static ssize_t parallel_pages_show(struct kobject *kobj,
				   struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%lld\n",
		       atomic64_read(&minimem_par_stats.pages_decompressed));
}

static ssize_t bench_baseline_ns_show(struct kobject *kobj,
				      struct kobj_attribute *attr, char *buf)
{
	s64 pages = atomic64_read(&bench_pages);
	s64 total = atomic64_read(&bench_baseline_ns);

	if (pages == 0)
		return sprintf(buf, "0 total_ns 0 avg_ns\n");

	return sprintf(buf, "%lld total_ns %lld avg_ns\n",
		       total, total / pages);
}

static ssize_t bench_serial_ns_show(struct kobject *kobj,
				     struct kobj_attribute *attr, char *buf)
{
	s64 pages = atomic64_read(&bench_pages);
	s64 total = atomic64_read(&bench_serial_ns);

	if (pages == 0)
		return sprintf(buf, "0 total_ns 0 avg_ns\n");

	return sprintf(buf, "%lld total_ns %lld avg_ns\n",
		       total, total / pages);
}

static ssize_t bench_parallel_ns_show(struct kobject *kobj,
				      struct kobj_attribute *attr, char *buf)
{
	s64 pages = atomic64_read(&bench_pages);
	s64 total = atomic64_read(&bench_parallel_ns);

	if (pages == 0)
		return sprintf(buf, "0 total_ns 0 avg_ns\n");

	return sprintf(buf, "%lld total_ns %lld avg_ns\n",
		       total, total / pages);
}

static ssize_t scanner_enabled_show(struct kobject *kobj,
				    struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", minimem_scanner_is_enabled() ? 1 : 0);
}

static ssize_t scanner_enabled_store(struct kobject *kobj,
				     struct kobj_attribute *attr,
				     const char *buf, size_t count)
{
	int val;
	int ret = kstrtoint(buf, 0, &val);
	if (ret)
		return ret;
	minimem_scanner_set_enabled(val);
	return count;
}

static ssize_t scanner_interval_ms_show(struct kobject *kobj,
					struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%ld\n", minimem_scanner_interval_ms());
}

static ssize_t scanner_interval_ms_store(struct kobject *kobj,
					  struct kobj_attribute *attr,
					  const char *buf, size_t count)
{
	long val;
	int ret = kstrtol(buf, 0, &val);
	if (ret)
		return ret;
	minimem_scanner_set_interval_ms(val);
	return count;
}

static ssize_t min_savings_pct_show(struct kobject *kobj,
				    struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%ld\n", minimem_scanner_min_savings_pct());
}

static ssize_t min_savings_pct_store(struct kobject *kobj,
				     struct kobj_attribute *attr,
				     const char *buf, size_t count)
{
	long val;
	int ret = kstrtol(buf, 0, &val);
	if (ret)
		return ret;
	minimem_scanner_set_min_savings_pct(val);
	return count;
}

static ssize_t scanner_pages_scanned_show(struct kobject *kobj,
					   struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%lu\n", minimem_scanner_pages_scanned());
}

static ssize_t scanner_pages_idle_show(struct kobject *kobj,
				       struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%lu\n", minimem_scanner_pages_idle());
}

static ssize_t scanner_pages_compressed_show(struct kobject *kobj,
					      struct kobj_attribute *attr,
					      char *buf)
{
	return sprintf(buf, "%lu\n", minimem_scanner_pages_compressed());
}

static ssize_t scanner_pages_skipped_show(struct kobject *kobj,
					   struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%lu\n", minimem_scanner_pages_skipped());
}

static ssize_t hook_faults_show(struct kobject *kobj,
				 struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%lu\n", minimem_hook_faults_handled());
}

static ssize_t kernel_patches_show(struct kobject *kobj,
				    struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", minimem_hook_marker_ready() ? 1 : 0);
}

static ssize_t max_pool_pages_show(struct kobject *kobj,
				    struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%lu\n", minimem_zswap_max_pool_pages());
}

static ssize_t max_pool_pages_store(struct kobject *kobj,
				     struct kobj_attribute *attr,
				     const char *buf, size_t count)
{
	unsigned long val;
	int ret = kstrtoul(buf, 0, &val);
	if (ret)
		return ret;
	minimem_zswap_set_max_pool_pages(val);
	return count;
}

static struct kobj_attribute pages_compressed_attr =
	__ATTR(pages_compressed, 0444, pages_compressed_show, NULL);
static struct kobj_attribute pages_decompressed_attr =
	__ATTR(pages_decompressed, 0444, pages_decompressed_show, NULL);
static struct kobj_attribute bytes_saved_attr =
	__ATTR(bytes_saved, 0444, bytes_saved_show, NULL);
static struct kobj_attribute compress_count_attr =
	__ATTR(compress_count, 0444, compress_count_show, NULL);
static struct kobj_attribute decompress_count_attr =
	__ATTR(decompress_count, 0444, decompress_count_show, NULL);
static struct kobj_attribute compress_ns_total_attr =
	__ATTR(compress_ns_total, 0444, compress_ns_total_show, NULL);
static struct kobj_attribute decompress_ns_total_attr =
	__ATTR(decompress_ns_total, 0444, decompress_ns_total_show, NULL);
static struct kobj_attribute decompress_avg_ns_attr =
	__ATTR(decompress_avg_ns, 0444, decompress_avg_ns_show, NULL);
static struct kobj_attribute compress_avg_ns_attr =
	__ATTR(compress_avg_ns, 0444, compress_avg_ns_show, NULL);
static struct kobj_attribute zswap_pages_attr =
	__ATTR(zswap_pages, 0444, zswap_pages_show, NULL);
static struct kobj_attribute zswap_bytes_attr =
	__ATTR(zswap_bytes, 0444, zswap_bytes_show, NULL);
static struct kobj_attribute zswap_saved_attr =
	__ATTR(zswap_saved, 0444, zswap_saved_show, NULL);
static struct kobj_attribute pool_pages_attr =
	__ATTR(pool_pages, 0444, pool_pages_show, NULL);
static struct kobj_attribute parallel_clusters_attr =
	__ATTR(parallel_clusters, 0444, parallel_clusters_show, NULL);
static struct kobj_attribute parallel_pages_attr =
	__ATTR(parallel_pages, 0444, parallel_pages_show, NULL);
static struct kobj_attribute bench_baseline_ns_attr =
	__ATTR(bench_baseline_ns, 0444, bench_baseline_ns_show, NULL);
static struct kobj_attribute bench_serial_ns_attr =
	__ATTR(bench_serial_ns, 0444, bench_serial_ns_show, NULL);
static struct kobj_attribute bench_parallel_ns_attr =
	__ATTR(bench_parallel_ns, 0444, bench_parallel_ns_show, NULL);
static struct kobj_attribute scanner_enabled_attr =
	__ATTR(scanner_enabled, 0644, scanner_enabled_show, scanner_enabled_store);
static struct kobj_attribute scanner_interval_ms_attr =
	__ATTR(scanner_interval_ms, 0644, scanner_interval_ms_show, scanner_interval_ms_store);
static struct kobj_attribute min_savings_pct_attr =
	__ATTR(min_savings_pct, 0644, min_savings_pct_show, min_savings_pct_store);
static struct kobj_attribute scanner_pages_scanned_attr =
	__ATTR(scanner_pages_scanned, 0444, scanner_pages_scanned_show, NULL);
static struct kobj_attribute scanner_pages_idle_attr =
	__ATTR(scanner_pages_idle, 0444, scanner_pages_idle_show, NULL);
static struct kobj_attribute scanner_pages_compressed_attr =
	__ATTR(scanner_pages_compressed, 0444, scanner_pages_compressed_show, NULL);
static struct kobj_attribute scanner_pages_skipped_attr =
	__ATTR(scanner_pages_skipped, 0444, scanner_pages_skipped_show, NULL);
static struct kobj_attribute hook_faults_attr =
	__ATTR(hook_faults, 0444, hook_faults_show, NULL);
static struct kobj_attribute kernel_patches_attr =
	__ATTR(kernel_patches, 0444, kernel_patches_show, NULL);
static struct kobj_attribute max_pool_pages_attr =
	__ATTR(max_pool_pages, 0644, max_pool_pages_show, max_pool_pages_store);

static struct attribute *minimem_attrs[] = {
	&pages_compressed_attr.attr,
	&pages_decompressed_attr.attr,
	&bytes_saved_attr.attr,
	&compress_count_attr.attr,
	&decompress_count_attr.attr,
	&compress_ns_total_attr.attr,
	&decompress_ns_total_attr.attr,
	&decompress_avg_ns_attr.attr,
	&compress_avg_ns_attr.attr,
	&zswap_pages_attr.attr,
	&zswap_bytes_attr.attr,
	&zswap_saved_attr.attr,
	&pool_pages_attr.attr,
	&parallel_clusters_attr.attr,
	&parallel_pages_attr.attr,
	&bench_baseline_ns_attr.attr,
	&bench_serial_ns_attr.attr,
	&bench_parallel_ns_attr.attr,
	&scanner_enabled_attr.attr,
	&scanner_interval_ms_attr.attr,
	&min_savings_pct_attr.attr,
	&scanner_pages_scanned_attr.attr,
	&scanner_pages_idle_attr.attr,
	&scanner_pages_compressed_attr.attr,
	&scanner_pages_skipped_attr.attr,
	&hook_faults_attr.attr,
	&kernel_patches_attr.attr,
	&max_pool_pages_attr.attr,
	NULL,
};

static struct attribute_group minimem_attr_group = {
	.attrs = minimem_attrs,
};

/* ---- Debugfs test interface ---- */

#define MINIMEM_BENCH_PAGES 32

static ssize_t bench_write(struct file *file, const char __user *buf,
			   size_t count, loff_t *ppos)
{
	char kbuf[32];
	ktime_t start;
	u64 elapsed;
	struct page **pages;
	unsigned long *vaddrs;
	int i, ret;

	if (count > 31)
		return -EINVAL;

	if (copy_from_user(kbuf, buf, count))
		return -EFAULT;

	kbuf[count] = '\0';
	if (count > 0 && kbuf[count - 1] == '\n')
		kbuf[count - 1] = '\0';

	pages = kmalloc_array(MINIMEM_BENCH_PAGES, sizeof(*pages), GFP_KERNEL);
	if (!pages)
		return -ENOMEM;

	vaddrs = kmalloc_array(MINIMEM_BENCH_PAGES, sizeof(*vaddrs),
			       GFP_KERNEL);
	if (!vaddrs) {
		kfree(pages);
		return -ENOMEM;
	}

	for (i = 0; i < MINIMEM_BENCH_PAGES; i++) {
		pages[i] = alloc_page(GFP_KERNEL);
		if (!pages[i]) {
			for (int j = 0; j < i; j++)
				__free_page(pages[j]);
			kfree(pages);
			kfree(vaddrs);
			return -ENOMEM;
		}
		vaddrs[i] = 0xDEAD0000UL + (i * PAGE_SIZE);
	}

	if (strcmp(kbuf, "baseline") == 0) {
		void *src;
		void *tmpbuf;

		tmpbuf = kmalloc(MINIMEM_PAGE_SIZE, GFP_KERNEL);
		if (!tmpbuf) {
			for (i = 0; i < MINIMEM_BENCH_PAGES; i++)
				__free_page(pages[i]);
			kfree(pages);
			kfree(vaddrs);
			return -ENOMEM;
		}

		start = ktime_get_ns();
		for (i = 0; i < MINIMEM_BENCH_PAGES; i++) {
			src = kmap_local_page(pages[i]);
			memset(src, i & 0xFF, MINIMEM_PAGE_SIZE);
			memcpy(tmpbuf, src, MINIMEM_PAGE_SIZE);
			memcpy(src, tmpbuf, MINIMEM_PAGE_SIZE);
			kunmap_local(src);
		}
		elapsed = ktime_get_ns() - start;
		kfree(tmpbuf);

		atomic64_set(&bench_baseline_ns, elapsed);
	} else if (strcmp(kbuf, "serial") == 0) {
		start = ktime_get_ns();
		for (i = 0; i < MINIMEM_BENCH_PAGES; i++) {
			void *src = kmap_local_page(pages[i]);
			memset(src, i & 0xFF, MINIMEM_PAGE_SIZE);
			kunmap_local(src);
			get_page(pages[i]);

			ret = minimem_compress_and_store(vaddrs[i], pages[i]);
			if (ret == MINIMEM_OK) {
				ret = minimem_decompress_and_restore(
					vaddrs[i], pages[i]);
			}
			put_page(pages[i]);
		}
		elapsed = ktime_get_ns() - start;

		atomic64_set(&bench_serial_ns, elapsed);
	} else if (strcmp(kbuf, "parallel") == 0) {
		for (i = 0; i < MINIMEM_BENCH_PAGES; i++) {
			void *src = kmap_local_page(pages[i]);
			memset(src, i & 0xFF, MINIMEM_PAGE_SIZE);
			kunmap_local(src);
			get_page(pages[i]);

			ret = minimem_compress_and_store(vaddrs[i], pages[i]);
			put_page(pages[i]);
			if (ret != MINIMEM_OK && ret != MINIMEM_INCOMPRESSIBLE) {
				for (int j = i + 1; j < MINIMEM_BENCH_PAGES; j++)
					__free_page(pages[j]);
				kfree(pages);
				kfree(vaddrs);
				return ret;
			}
		}

		start = ktime_get_ns();
		ret = minimem_parallel_decompress(vaddrs, NULL,
							  MINIMEM_BENCH_PAGES, NULL);
		elapsed = ktime_get_ns() - start;

		for (i = 0; i < MINIMEM_BENCH_PAGES; i++)
			minimem_zswap_remove(vaddrs[i]);

		atomic64_set(&bench_parallel_ns, elapsed);
	} else {
		for (i = 0; i < MINIMEM_BENCH_PAGES; i++)
			__free_page(pages[i]);
		kfree(pages);
		kfree(vaddrs);
		return -EINVAL;
	}

	atomic64_set(&bench_pages, MINIMEM_BENCH_PAGES);

	for (i = 0; i < MINIMEM_BENCH_PAGES; i++)
		__free_page(pages[i]);
	kfree(pages);
	kfree(vaddrs);

	return count;
}

static const struct file_operations bench_fops = {
	.write = bench_write,
	.owner = THIS_MODULE,
};

static ssize_t compress_write(struct file *file, const char __user *buf,
			      size_t count, loff_t *ppos)
{
	unsigned long vaddr;
	char kbuf[32];
	int ret;
	struct page *page;

	if (count > 31)
		return -EINVAL;

	if (copy_from_user(kbuf, buf, count))
		return -EFAULT;

	kbuf[count] = '\0';
	ret = kstrtoul(kbuf, 0, &vaddr);
	if (ret)
		return ret;

	vaddr &= PAGE_MASK;

	page = alloc_page(GFP_KERNEL);
	if (!page)
		return -ENOMEM;

	{
		void *addr = kmap_local_page(page);
		memset(addr, 0x42, MINIMEM_PAGE_SIZE);
		kunmap_local(addr);
	}

	get_page(page);
	ret = minimem_compress_and_store(vaddr, page);
	put_page(page);

	__free_page(page);

	return count;
}

static const struct file_operations compress_fops = {
	.write = compress_write,
	.owner = THIS_MODULE,
};

static ssize_t stats_read(struct file *file, char __user *buf,
			   size_t count, loff_t *ppos)
{
	char kbuf[1024];
	int len;

	len = snprintf(kbuf, sizeof(kbuf),
		       "pages_compressed %lld\n"
		       "pages_decompressed %lld\n"
		       "bytes_saved %lu\n"
		       "compress_count %lld\n"
		       "decompress_count %lld\n"
		       "compress_ns_total %lld\n"
		       "decompress_ns_total %lld\n"
		       "compress_avg_ns %lld\n"
		       "decompress_avg_ns %lld\n"
		       "zswap_pages %lu\n"
		       "zswap_bytes %lu\n"
		       "zswap_saved %lu\n"
		       "pool_pages %lu\n"
		       "parallel_clusters %lld\n"
		       "parallel_pages %lld\n",
		       atomic64_read(&minimem_stats.pages_compressed),
		       atomic64_read(&minimem_stats.pages_decompressed),
		       minimem_zswap_bytes_saved(),
		       atomic64_read(&minimem_stats.compress_count),
		       atomic64_read(&minimem_stats.decompress_count),
		       atomic64_read(&minimem_stats.compress_ns_total),
		       atomic64_read(&minimem_stats.decompress_ns_total),
		       atomic64_read(&minimem_stats.compress_count) ?
		       atomic64_read(&minimem_stats.compress_ns_total) /
		       atomic64_read(&minimem_stats.compress_count) : 0,
		       atomic64_read(&minimem_stats.decompress_count) ?
		       atomic64_read(&minimem_stats.decompress_ns_total) /
		       atomic64_read(&minimem_stats.decompress_count) : 0,
		       minimem_zswap_stored_pages(),
		       minimem_zswap_total_bytes(),
		       minimem_zswap_bytes_saved(),
		       zs_get_total_pages(minimem_zswap_pool()),
		       atomic64_read(&minimem_par_stats.clusters_decompressed),
		       atomic64_read(&minimem_par_stats.pages_decompressed));

	len += minimem_fault_stats_read(kbuf + len, sizeof(kbuf) - len);

	return simple_read_from_buffer(buf, count, ppos, kbuf, len);
}

static const struct file_operations stats_fops = {
	.read = stats_read,
	.owner = THIS_MODULE,
};

static ssize_t pte_test_write(struct file *file, const char __user *buf,
			      size_t count, loff_t *ppos)
{
	return minimem_pte_test_write(buf, count);
}

static const struct file_operations pte_test_fops = {
	.write = pte_test_write,
	.owner = THIS_MODULE,
};

/*
 * compress_vaddr: Takes a userspace virtual address, finds the corresponding
 * VMA and page, compresses the page, stores it in zsmalloc, and replaces
 * the PTE with a MiniMem swap entry. On next access, the kprobe hook
 * will intercept the fault and decompress the page.
 *
 * Usage: echo "0x7f0000" > /sys/kernel/debug/minimem/compress_vaddr
 * Returns: number of bytes written on success, negative errno on failure.
 */
static ssize_t compress_vaddr_write(struct file *file,
				    const char __user *buf,
				    size_t count, loff_t *ppos)
{
	unsigned long vaddr;
	char kbuf[32];
	int ret;
	struct mm_struct *mm;
	struct vm_area_struct *vma;

	if (count > 31)
		return -EINVAL;

	if (copy_from_user(kbuf, buf, count))
		return -EFAULT;

	kbuf[count] = '\0';
	ret = kstrtoul(kbuf, 0, &vaddr);
	if (ret)
		return ret;

	vaddr &= PAGE_MASK;

	mm = current->mm;
	if (!mm)
		return -EINVAL;

	mmap_read_lock(mm);

	vma = find_vma(mm, vaddr);
	if (!vma || vaddr < vma->vm_start) {
		mmap_read_unlock(mm);
		return -ENOENT;
	}

	ret = minimem_compress_and_replace_pte(mm, vma, vaddr);

	mmap_read_unlock(mm);

	if (ret == 0)
		pr_info("minimem: compress_vaddr: compressed vaddr=0x%lx\n",
			vaddr);
	else if (ret != -EOPNOTSUPP)
		pr_warn("minimem: compress_vaddr: failed for vaddr=0x%lx: %d\n",
			vaddr, ret);

	return (ret < 0 && ret != -EOPNOTSUPP) ? ret : count;
}

static const struct file_operations compress_vaddr_fops = {
	.write = compress_vaddr_write,
	.owner = THIS_MODULE,
};

/* ---- Module init/exit ---- */

static int __init minimem_init(void)
{
	int ret;

	atomic64_set(&minimem_stats.pages_compressed, 0);
	atomic64_set(&minimem_stats.pages_decompressed, 0);
	atomic64_set(&minimem_stats.bytes_saved, 0);
	atomic64_set(&minimem_stats.compress_count, 0);
	atomic64_set(&minimem_stats.decompress_count, 0);
	atomic64_set(&minimem_stats.compress_ns_total, 0);
	atomic64_set(&minimem_stats.decompress_ns_total, 0);

	ret = minimem_compress_init();
	if (ret) {
		pr_err("minimem: failed to allocate per-CPU buffers\n");
		return ret;
	}

	ret = minimem_zswap_init();
	if (ret) {
		pr_err("minimem: failed to initialize zswap storage\n");
		minimem_compress_exit();
		return ret;
	}

	ret = minimem_parallel_init();
	if (ret) {
		pr_err("minimem: failed to initialize parallel decompression\n");
		minimem_zswap_exit();
		minimem_compress_exit();
		return ret;
	}

	ret = minimem_fault_init();
	if (ret) {
		pr_err("minimem: failed to initialize fault handler\n");
		minimem_parallel_exit();
		minimem_zswap_exit();
		minimem_compress_exit();
		return ret;
	}

	ret = minimem_shrinker_init();
	if (ret) {
		pr_err("minimem: failed to initialize shrinker\n");
		minimem_fault_exit();
		minimem_parallel_exit();
		minimem_zswap_exit();
		minimem_compress_exit();
		return ret;
	}

	ret = minimem_scanner_init();
	if (ret) {
		pr_err("minimem: failed to initialize scanner\n");
		minimem_shrinker_exit();
		minimem_fault_exit();
		minimem_parallel_exit();
		minimem_zswap_exit();
		minimem_compress_exit();
		return ret;
	}

	ret = minimem_hook_init();
	if (ret) {
		pr_warn("minimem: failed to initialize page fault hook (non-fatal)\n");
		/* Hook is non-fatal — module still works without transparent faults */
	}

	minimem_kobj = kobject_create_and_add("minimem", kernel_kobj);
	if (!minimem_kobj) {
		minimem_hook_exit();
		minimem_scanner_exit();
		minimem_shrinker_exit();
		minimem_fault_exit();
		minimem_parallel_exit();
		minimem_zswap_exit();
		minimem_compress_exit();
		return -ENOMEM;
	}

	ret = sysfs_create_group(minimem_kobj, &minimem_attr_group);
	if (ret) {
		kobject_put(minimem_kobj);
		minimem_hook_exit();
		minimem_scanner_exit();
		minimem_shrinker_exit();
		minimem_fault_exit();
		minimem_parallel_exit();
		minimem_zswap_exit();
		minimem_compress_exit();
		return ret;
	}

	minimem_debugfs_dir = debugfs_create_dir("minimem", NULL);
	if (!IS_ERR(minimem_debugfs_dir)) {
		debugfs_create_file("compress", 0200, minimem_debugfs_dir,
				    NULL, &compress_fops);
		debugfs_create_file("bench", 0200, minimem_debugfs_dir,
				    NULL, &bench_fops);
		debugfs_create_file("stats", 0444, minimem_debugfs_dir,
				    NULL, &stats_fops);
		debugfs_create_file("pte_test", 0200, minimem_debugfs_dir,
				    NULL, &pte_test_fops);
		debugfs_create_file("compress_vaddr", 0200, minimem_debugfs_dir,
				    NULL, &compress_vaddr_fops);
	}

	pr_info("minimem: transparent memory compression loaded (v" MINIMEM_VERSION_STR ")\n");
	pr_info("minimem: algorithms: same_page, BDI, WKdm, WKdm-64, block_class, LZ4, delta\n");
	pr_info("minimem: PTE markers, shrinker, idle scanner\n");
	pr_info("minimem: sysfs at /sys/kernel/minimem/, debugfs at /sys/kernel/debug/minimem/\n");
	return 0;
}

static void __exit minimem_exit(void)
{
	debugfs_remove_recursive(minimem_debugfs_dir);
	sysfs_remove_group(minimem_kobj, &minimem_attr_group);
	kobject_put(minimem_kobj);

	minimem_hook_exit();
	minimem_scanner_exit();
	minimem_shrinker_exit();
	minimem_fault_exit();
	minimem_parallel_exit();
	minimem_zswap_exit();
	minimem_compress_exit();

	pr_info("minimem: module unloaded - %lld pages compressed, %lld bytes saved\n",
		atomic64_read(&minimem_stats.pages_compressed),
		atomic64_read(&minimem_stats.bytes_saved));
}

module_init(minimem_init);
module_exit(minimem_exit);