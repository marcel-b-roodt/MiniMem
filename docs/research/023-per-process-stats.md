# 023 — Per-Process Compression Statistics

## Summary

MiniMem currently tracks global compression statistics (total pages compressed, bytes saved, latencies). This research doc explores adding optional per-process (and per-UID) statistics so users can see which applications benefit most from compression, without introducing data leakage or significant overhead.

## Key Findings

### Why per-process stats?
- Users want to know "how much memory is MiniMem saving me?" for their specific workload
- A DBA running PostgreSQL wants to see that 4GB of their 16GB instance is compressed
- An ML engineer wants to see their model weights being compressed during inference gaps
- This is also useful for us during local testing to validate expected gains

### Implementation approach: debugfs, not sysfs
- sysfs attributes are visible to all users by default (mode 0444) — exposing per-process info there would leak process existence and memory patterns
- debugfs is root-only by default and explicitly not a stable ABI — perfect for optional diagnostics
- Per-process stats live in `/sys/kernel/debug/minimem/per_process/` (only mounted by root)
- A separate sysfs boolean `per_process_stats` (default: 0) gates whether collection is active

### Data model

```c
struct minimem_proc_stats {
    pid_t   pid;
    uid_t   uid;
    char    comm[TASK_COMM_LEN];  /* 16 bytes */
    atomic64_t pages_compressed;
    atomic64_t pages_decompressed;
    atomic64_t bytes_saved;
    atomic64_t compress_ns;
    atomic64_t decompress_ns;
};
```

- Stored in an xarray keyed by PID
- Allocated lazily (only when `per_process_stats=1`)
- Freed when process exits (via task_work_add or rcu callback from mm_release)
- Max entries capped (default 1024) to bound memory

### Reading stats

Root can read all processes:
```bash
sudo cat /sys/kernel/debug/minimem/per_process
# PID    COMM            UID  PAGES_CMP  PAGES_DECMP  BYTES_SAVED  CMP_NS    DECMP_NS
# 1234   postgres        70   1024       512           2097152      204800    102400
# 5678   python3         1000 2048        1024          4194304      409600    204800
```

A specific process:
```bash
sudo cat /sys/kernel/debug/minimem/per_process/1234
```

### Summary mode (safe for non-root)
A sysfs attribute provides aggregate anonymized stats:
```bash
cat /sys/kernel/minimem/stats_summary
# top_processes: 5
# top_uid_bytes_saved: uid=70 bytes=2097152, uid=1000 bytes=4194304
# avg_compress_ns: 200
# avg_decompress_ns: 100
# total_bytes_saved: 6291456
```
This is mode 0444 (world-readable) but contains NO process names or PIDs — only UIDs and aggregate numbers. The `top_processes` count is configurable via `per_process_top_n` sysfs knob (default 5, max 20).

### Overhead
- Per-process stats add ~128 bytes per tracked process
- At default cap of 1024 processes = 128KB max overhead
- Atomic operations on the fast path: ~2ns per compress/decompress (incrementing 3 atomic64_t counters)
- XArray lookup by PID: ~50ns (acceptable since it only happens when `per_process_stats=1`)
- When disabled (default): zero overhead — the xarray is empty and the per-process lookup is a single branch

### Privacy / data leakage concerns
- debugfs entries are root-only (mode 0400)
- `per_process_stats` defaults to 0 (off) — must be explicitly enabled
- `stats_summary` is world-readable but contains only UID aggregates and counts — no process names or PIDs
- Process names (comm) are only visible in debugfs (root-only)
- Stats are zeroed when a process exits — no historical data leakage
- The feature can be compiled out with `CONFIG_MINIMEM_PROC_STATS=n`

## Relevance to MiniMem

This directly addresses the user's need for observability. During local testing, the per-process debugfs view shows exactly which applications benefit. In production, the anonymized `stats_summary` gives admins confidence without exposing process details.

## Open Questions

- Should we track per-cgroup stats as well (for containerized workloads)?
- What's the right default for `per_process_top_n`? 5 seems conservative but useful.
- Should expired process entries be kept for a configurable TTL (e.g., 60s) to catch short-lived processes?
- Is there a need for a histogram of compression ratios per-process?

## References

- Linux kernel `task_struct->comm`, `task_work_add` API
- `/proc/<pid>/smaps` as a precedent for per-process memory stats
- `debugfs_create_dir`, `debugfs_create_file` for per-process directory creation