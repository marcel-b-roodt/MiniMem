/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * minimem_proc_stats.c — Per-process compression statistics for MiniMem
 *
 * Tracks compression/decompression stats per PID using an xarray.
 * Entries are allocated lazily when per_process_stats=1 and freed
 * either on process exit (via periodic GC from the scanner) or
 * when per_process_stats is disabled.
 *
 * Memory overhead: ~128 bytes per tracked process, capped at
 * MINIMEM_PROC_STATS_MAX_ENTRIES (1024 by default).
 *
 * Privacy:
 *   - per_process_stats defaults to 0 (off)
 *   - Detailed per-PID data is only in debugfs (root-only, mode 0400)
 *   - stats_summary is world-readable (0444) but contains only
 *     UID aggregates and top-N — no PIDs or process names
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/xarray.h>
#include <linux/ktime.h>
#include <linux/atomic.h>
#include <linux/sched.h>
#include <linux/cred.h>
#include <linux/seq_file.h>
#include <linux/debugfs.h>
#include <linux/sort.h>
#include <linux/pid.h>

#include "minimem_proc_stats.h"

#define MINIMEM_PROC_STATS_MAX_ENTRIES	1024
#define MINIMEM_PROC_STATS_DEFAULT_TOP_N	5
#define MINIMEM_PROC_STATS_MAX_TOP_N		20

struct minimem_proc_entry {
	pid_t	pid;
	uid_t	uid;
	char	comm[16];
	atomic64_t pages_compressed;
	atomic64_t pages_decompressed;
	atomic64_t bytes_saved;
	atomic64_t compress_ns;
	atomic64_t decompress_ns;
	struct rcu_head rcu;
};

static DEFINE_XARRAY(minimem_proc_stats_xa);
static atomic_t mm_proc_stats_enabled = ATOMIC_INIT(0);
static atomic_t mm_proc_stats_top_n = ATOMIC_INIT(MINIMEM_PROC_STATS_DEFAULT_TOP_N);
static atomic_t mm_proc_stats_count = ATOMIC_INIT(0);
static DEFINE_SPINLOCK(minimem_proc_stats_lock);

static struct minimem_proc_entry *minimem_proc_stats_find(pid_t pid)
{
	return xa_load(&minimem_proc_stats_xa, pid);
}

static struct minimem_proc_entry *minimem_proc_stats_alloc(pid_t pid,
							    uid_t uid,
							    const char *comm)
{
	struct minimem_proc_entry *entry;
	unsigned long flags;

	if (atomic_read(&mm_proc_stats_count) >=
	    MINIMEM_PROC_STATS_MAX_ENTRIES)
		return NULL;

	entry = kzalloc(sizeof(*entry), GFP_ATOMIC);
	if (!entry)
		return NULL;

	entry->pid = pid;
	entry->uid = uid;
	strscpy(entry->comm, comm, sizeof(entry->comm));
	atomic64_set(&entry->pages_compressed, 0);
	atomic64_set(&entry->pages_decompressed, 0);
	atomic64_set(&entry->bytes_saved, 0);
	atomic64_set(&entry->compress_ns, 0);
	atomic64_set(&entry->decompress_ns, 0);

	spin_lock_irqsave(&minimem_proc_stats_lock, flags);
	if (xa_load(&minimem_proc_stats_xa, pid)) {
		spin_unlock_irqrestore(&minimem_proc_stats_lock, flags);
		kfree(entry);
		return xa_load(&minimem_proc_stats_xa, pid);
	}
	if (atomic_read(&mm_proc_stats_count) >=
	    MINIMEM_PROC_STATS_MAX_ENTRIES) {
		spin_unlock_irqrestore(&minimem_proc_stats_lock, flags);
		kfree(entry);
		return NULL;
	}
	xa_store(&minimem_proc_stats_xa, pid, entry, GFP_ATOMIC);
	atomic_inc(&mm_proc_stats_count);
	spin_unlock_irqrestore(&minimem_proc_stats_lock, flags);

	return entry;
}

void minimem_proc_stats_compress(size_t bytes_saved, u64 ns_elapsed)
{
	struct minimem_proc_entry *entry;
	pid_t pid;
	uid_t uid;

	if (!atomic_read(&mm_proc_stats_enabled))
		return;

	pid = current->pid;
	uid = from_kuid(current_user_ns(), current_uid());

	entry = minimem_proc_stats_find(pid);
	if (!entry)
		entry = minimem_proc_stats_alloc(pid, uid, current->comm);

	if (entry) {
		atomic64_inc(&entry->pages_compressed);
		atomic64_add(bytes_saved, &entry->bytes_saved);
		atomic64_add(ns_elapsed, &entry->compress_ns);
	}
}

void minimem_proc_stats_decompress(u64 ns_elapsed)
{
	struct minimem_proc_entry *entry;
	pid_t pid;
	uid_t uid;

	if (!atomic_read(&mm_proc_stats_enabled))
		return;

	pid = current->pid;
	uid = from_kuid(current_user_ns(), current_uid());

	entry = minimem_proc_stats_find(pid);
	if (!entry)
		entry = minimem_proc_stats_alloc(pid, uid, current->comm);

	if (entry) {
		atomic64_inc(&entry->pages_decompressed);
		atomic64_add(ns_elapsed, &entry->decompress_ns);
	}
}

bool minimem_proc_stats_enabled(void)
{
	return atomic_read(&mm_proc_stats_enabled) != 0;
}

void minimem_proc_stats_gc(void)
{
	struct minimem_proc_entry *entry;
	unsigned long pid;

	if (!atomic_read(&mm_proc_stats_enabled))
		return;

	xa_for_each(&minimem_proc_stats_xa, pid, entry) {
		struct pid *pidp;

		rcu_read_lock();
		pidp = find_vpid(pid);
		if (!pidp || !pid_task(pidp, PIDTYPE_PID)) {
			rcu_read_unlock();
			spin_lock(&minimem_proc_stats_lock);
			xa_erase(&minimem_proc_stats_xa, pid);
			atomic_dec(&mm_proc_stats_count);
			spin_unlock(&minimem_proc_stats_lock);
			kfree_rcu(entry, rcu);
		} else {
			rcu_read_unlock();
		}
	}
}

/* ---- Sysfs: per_process_stats toggle ---- */

static ssize_t per_process_stats_show(struct kobject *kobj,
				       struct kobj_attribute *attr,
				       char *buf)
{
	return sprintf(buf, "%d\n", atomic_read(&mm_proc_stats_enabled));
}

static ssize_t per_process_stats_store(struct kobject *kobj,
					struct kobj_attribute *attr,
					const char *buf, size_t count)
{
	long val;

	if (kstrtol(buf, 10, &val))
		return -EINVAL;

	if (val == 0) {
		struct minimem_proc_entry *entry;
		unsigned long pid;

		atomic_set(&mm_proc_stats_enabled, 0);

		spin_lock(&minimem_proc_stats_lock);
		xa_for_each(&minimem_proc_stats_xa, pid, entry) {
			xa_erase(&minimem_proc_stats_xa, pid);
			kfree(entry);
		}
		atomic_set(&mm_proc_stats_count, 0);
		spin_unlock(&minimem_proc_stats_lock);
	} else if (val == 1) {
		atomic_set(&mm_proc_stats_enabled, 1);
	} else {
		return -EINVAL;
	}

	return count;
}

static struct kobj_attribute per_process_stats_attr =
	__ATTR(per_process_stats, 0644,
	       per_process_stats_show, per_process_stats_store);

/* ---- Sysfs: per_process_top_n ---- */

static ssize_t per_process_top_n_show(struct kobject *kobj,
				       struct kobj_attribute *attr,
				       char *buf)
{
	return sprintf(buf, "%d\n", atomic_read(&mm_proc_stats_top_n));
}

static ssize_t per_process_top_n_store(struct kobject *kobj,
					struct kobj_attribute *attr,
					const char *buf, size_t count)
{
	long val;

	if (kstrtol(buf, 10, &val))
		return -EINVAL;

	if (val < 1 || val > MINIMEM_PROC_STATS_MAX_TOP_N)
		return -EINVAL;

	atomic_set(&mm_proc_stats_top_n, val);
	return count;
}

static struct kobj_attribute per_process_top_n_attr =
	__ATTR(per_process_top_n, 0644,
	       per_process_top_n_show, per_process_top_n_store);

/* ---- Sysfs: stats_summary (anonymized, world-readable) ---- */

struct uid_summary {
	uid_t	uid;
	s64	bytes_saved;
	s64	pages_compressed;
	s64	pages_decompressed;
	s64	compress_ns;
	s64	decompress_ns;
};

static int uid_summary_cmp(const void *a, const void *b)
{
	const struct uid_summary *sa = a;
	const struct uid_summary *sb = b;

	if (sa->bytes_saved > sb->bytes_saved)
		return -1;
	if (sa->bytes_saved < sb->bytes_saved)
		return 1;
	return 0;
}

static ssize_t stats_summary_show(struct kobject *kobj,
				   struct kobj_attribute *attr,
				   char *buf)
{
	struct minimem_proc_entry *entry;
	unsigned long pid;
	struct uid_summary *uid_arr;
	int uid_count = 0;
	int uid_capacity = 64;
	int top_n;
	int len = 0;
	int i, j;

	if (!atomic_read(&mm_proc_stats_enabled))
		return sprintf(buf, "per_process_stats disabled\n");

	uid_arr = kmalloc_array(uid_capacity, sizeof(*uid_arr), GFP_KERNEL);
	if (!uid_arr)
		return -ENOMEM;

	xa_for_each(&minimem_proc_stats_xa, pid, entry) {
		bool found = false;

		for (j = 0; j < uid_count; j++) {
			if (uid_arr[j].uid == entry->uid) {
				uid_arr[j].bytes_saved +=
					atomic64_read(&entry->bytes_saved);
				uid_arr[j].pages_compressed +=
					atomic64_read(&entry->pages_compressed);
				uid_arr[j].pages_decompressed +=
					atomic64_read(&entry->pages_decompressed);
				uid_arr[j].compress_ns +=
					atomic64_read(&entry->compress_ns);
				uid_arr[j].decompress_ns +=
					atomic64_read(&entry->decompress_ns);
				found = true;
				break;
			}
		}

		if (!found) {
			if (uid_count >= uid_capacity) {
				struct uid_summary *new_arr;

				uid_capacity *= 2;
				new_arr = krealloc(uid_arr,
						   uid_capacity *
						   sizeof(*uid_arr),
						   GFP_KERNEL);
				if (!new_arr) {
					kfree(uid_arr);
					return -ENOMEM;
				}
				uid_arr = new_arr;
			}

			uid_arr[uid_count].uid = entry->uid;
			uid_arr[uid_count].bytes_saved =
				atomic64_read(&entry->bytes_saved);
			uid_arr[uid_count].pages_compressed =
				atomic64_read(&entry->pages_compressed);
			uid_arr[uid_count].pages_decompressed =
				atomic64_read(&entry->pages_decompressed);
			uid_arr[uid_count].compress_ns =
				atomic64_read(&entry->compress_ns);
			uid_arr[uid_count].decompress_ns =
				atomic64_read(&entry->decompress_ns);
			uid_count++;
		}
	}

	sort(uid_arr, uid_count, sizeof(*uid_arr), uid_summary_cmp, NULL);

	top_n = atomic_read(&mm_proc_stats_top_n);
	if (top_n > uid_count)
		top_n = uid_count;

	len += sprintf(buf + len,
		       "tracked_processes: %d\n"
		       "tracked_uids: %d\n"
		       "top_processes_by_bytes_saved:\n",
		       atomic_read(&mm_proc_stats_count),
		       uid_count);

	for (i = 0; i < top_n; i++) {
		s64 avg_cmp = uid_arr[i].pages_compressed > 0 ?
			      uid_arr[i].compress_ns /
			      uid_arr[i].pages_compressed : 0;
		s64 avg_decmp = uid_arr[i].pages_decompressed > 0 ?
				uid_arr[i].decompress_ns /
				uid_arr[i].pages_decompressed : 0;

		len += sprintf(buf + len,
			       "  uid=%u bytes_saved=%lld "
			       "pages_compressed=%lld "
			       "pages_decompressed=%lld "
			       "avg_compress_ns=%lld "
			       "avg_decompress_ns=%lld\n",
			       uid_arr[i].uid,
			       uid_arr[i].bytes_saved,
			       uid_arr[i].pages_compressed,
			       uid_arr[i].pages_decompressed,
			       avg_cmp, avg_decmp);
	}

	kfree(uid_arr);
	return len;
}

static struct kobj_attribute stats_summary_attr =
	__ATTR(stats_summary, 0444, stats_summary_show, NULL);

/* ---- Debugfs: per_process (root-only, detailed) ---- */

static int minimem_proc_stats_show(struct seq_file *m, void *v)
{
	struct minimem_proc_entry *entry;
	unsigned long pid;

	if (!atomic_read(&mm_proc_stats_enabled)) {
		seq_puts(m, "per_process_stats disabled\n");
		return 0;
	}

	seq_puts(m, "PID\tCOMM\t\t\tUID\tPAGES_CMP\tPAGES_DECMP\t"
		 "BYTES_SAVED\tAVG_CMP_NS\tAVG_DECMP_NS\n");

	rcu_read_lock();
	xa_for_each(&minimem_proc_stats_xa, pid, entry) {
		s64 cmp = atomic64_read(&entry->pages_compressed);
		s64 decmp = atomic64_read(&entry->pages_decompressed);
		s64 avg_cmp = cmp > 0 ?
			      atomic64_read(&entry->compress_ns) / cmp : 0;
		s64 avg_decmp = decmp > 0 ?
				atomic64_read(&entry->decompress_ns) / decmp : 0;

		seq_printf(m, "%d\t%-16s\t%u\t%lld\t\t%lld\t\t%lld\t\t%lld\t\t%lld\n",
			   entry->pid,
			   entry->comm,
			   entry->uid,
			   cmp,
			   decmp,
			   atomic64_read(&entry->bytes_saved),
			   avg_cmp,
			   avg_decmp);
	}
	rcu_read_unlock();

	return 0;
}

static int minimem_proc_stats_open(struct inode *inode, struct file *file)
{
	return single_open(file, minimem_proc_stats_show, NULL);
}

static const struct file_operations per_process_fops = {
	.owner		= THIS_MODULE,
	.open		= minimem_proc_stats_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

/* ---- Init/Exit ---- */

int minimem_proc_stats_init(void)
{
	xa_init(&minimem_proc_stats_xa);
	atomic_set(&mm_proc_stats_enabled, 0);
	atomic_set(&mm_proc_stats_top_n, MINIMEM_PROC_STATS_DEFAULT_TOP_N);
	atomic_set(&mm_proc_stats_count, 0);

	return 0;
}

void minimem_proc_stats_exit(void)
{
	struct minimem_proc_entry *entry;
	unsigned long pid;

	xa_for_each(&minimem_proc_stats_xa, pid, entry)
		kfree(entry);

	xa_destroy(&minimem_proc_stats_xa);
}

struct kobj_attribute **minimem_proc_stats_get_sysfs_attrs(void)
{
	static struct kobj_attribute *attrs[] = {
		&per_process_stats_attr,
		&per_process_top_n_attr,
		&stats_summary_attr,
		NULL,
	};

	return attrs;
}

const struct file_operations *minimem_proc_stats_get_debugfs_fops(void)
{
	return &per_process_fops;
}