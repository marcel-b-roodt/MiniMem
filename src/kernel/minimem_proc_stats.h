/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * minimem_proc_stats.h — Per-process compression statistics for MiniMem
 *
 * Optional subsystem (default off) that tracks compression and
 * decompression stats per PID. Exposes:
 *   - /sys/kernel/minimem/per_process_stats  (0/1 toggle)
 *   - /sys/kernel/minimem/per_process_top_n  (how many in summary)
 *   - /sys/kernel/minimem/stats_summary       (anonymized, world-readable)
 *   - /sys/kernel/debug/minimem/per_process   (detailed, root-only)
 *
 * When disabled (default), overhead is a single branch on each
 * compress/decompress call.
 */

#ifndef MINIMEM_KERNEL_PROC_STATS_H
#define MINIMEM_KERNEL_PROC_STATS_H

#include <linux/types.h>
#include <linux/sysfs.h>

/*
 * Record one compression or decompression event for the current process.
 * Called from the compress/decompress hot paths.
 * These are inline wrappers that branch on the global enable flag
 * so the overhead is near-zero when disabled.
 */
void minimem_proc_stats_compress(size_t bytes_saved, u64 ns_elapsed);
void minimem_proc_stats_decompress(u64 ns_elapsed);

/*
 * Init/exit — called from module init/exit.
 */
int minimem_proc_stats_init(void);
void minimem_proc_stats_exit(void);

/*
 * Garbage collection — remove entries for exited processes.
 * Called periodically from the scanner thread.
 */
void minimem_proc_stats_gc(void);

/*
 * Query whether per-process stats are enabled.
 */
bool minimem_proc_stats_enabled(void);

/*
 * Accessor functions for sysfs and debugfs integration.
 * Called from minimem_main.c to create/remove files.
 */
struct kobj_attribute **minimem_proc_stats_get_sysfs_attrs(void);
const struct file_operations *minimem_proc_stats_get_debugfs_fops(void);

#endif /* MINIMEM_KERNEL_PROC_STATS_H */