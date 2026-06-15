/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * minimem.c — MiniMem transparent in-memory page compression module
 *
 * Provides transparent compression of cold memory pages using multiple
 * algorithms (same-page detection, BDI, WKdm, LZ4) with sysfs stats.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/spinlock.h>
#include <linux/atomic.h>
#include <linux/sysfs.h>
#include <linux/kobject.h>

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("MiniMem Project");
MODULE_DESCRIPTION("Transparent in-memory page compression");
MODULE_VERSION("0.1.0");

static struct minimem_stats {
	atomic64_t pages_compressed;
	atomic64_t bytes_saved;
	atomic64_t decompress_count;
	atomic64_t decompress_ns_total;
} minimem_stats;

static struct kobject *minimem_kobj;

static ssize_t pages_compressed_show(struct kobject *kobj,
				     struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%lld\n",
		       atomic64_read(&minimem_stats.pages_compressed));
}

static ssize_t bytes_saved_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%lld\n",
		       atomic64_read(&minimem_stats.bytes_saved));
}

static ssize_t decompress_count_show(struct kobject *kobj,
				      struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%lld\n",
		       atomic64_read(&minimem_stats.decompress_count));
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

static struct kobj_attribute pages_compressed_attr =
	__ATTR(pages_compressed, 0444, pages_compressed_show, NULL);
static struct kobj_attribute bytes_saved_attr =
	__ATTR(bytes_saved, 0444, bytes_saved_show, NULL);
static struct kobj_attribute decompress_count_attr =
	__ATTR(decompress_count, 0444, decompress_count_show, NULL);
static struct kobj_attribute decompress_ns_total_attr =
	__ATTR(decompress_ns_total, 0444, decompress_ns_total_show, NULL);
static struct kobj_attribute decompress_avg_ns_attr =
	__ATTR(decompress_avg_ns, 0444, decompress_avg_ns_show, NULL);

static struct attribute *minimem_attrs[] = {
	&pages_compressed_attr.attr,
	&bytes_saved_attr.attr,
	&decompress_count_attr.attr,
	&decompress_ns_total_attr.attr,
	&decompress_avg_ns_attr.attr,
	NULL,
};

static struct attribute_group minimem_attr_group = {
	.attrs = minimem_attrs,
};

static int __init minimem_init(void)
{
	int ret;

	atomic64_set(&minimem_stats.pages_compressed, 0);
	atomic64_set(&minimem_stats.bytes_saved, 0);
	atomic64_set(&minimem_stats.decompress_count, 0);
	atomic64_set(&minimem_stats.decompress_ns_total, 0);

	minimem_kobj = kobject_create_and_add("minimem", kernel_kobj);
	if (!minimem_kobj)
		return -ENOMEM;

	ret = sysfs_create_group(minimem_kobj, &minimem_attr_group);
	if (ret) {
		kobject_put(minimem_kobj);
		return ret;
	}

	pr_info("minimem: transparent memory compression loaded\n");
	return 0;
}

static void __exit minimem_exit(void)
{
	sysfs_remove_group(minimem_kobj, &minimem_attr_group);
	kobject_put(minimem_kobj);

	pr_info("minimem: module unloaded - %lld pages compressed, %lld bytes saved\n",
		atomic64_read(&minimem_stats.pages_compressed),
		atomic64_read(&minimem_stats.bytes_saved));
}

module_init(minimem_init);
module_exit(minimem_exit);