# 018 — Memory Overhead Analysis for MiniMem Kernel Module

## Summary

Analysis of memory overhead introduced by the MiniMem kernel module, including per-CPU buffers, zsmalloc metadata, compression map entries, and compressed data size inflation for incompressible pages. Recommendations for minimum savings thresholds and overhead accounting.

## Key Findings

### Per-Component Overhead

| Component | Size | Per-CPU? | Notes |
|---|---|---|---|
| Compress buffer | 8 KB | Yes (×N CPUs) | Per-CPU scratch buffer for compression |
| Decompress buffer | 8 KB | Yes (×N CPUs) | Per-CPU scratch buffer for decompression |
| XArray entry | 56 bytes | No | `struct minimem_map_entry` per compressed page |
| kmem_cache overhead | ~64 bytes | No | Slab allocator metadata per entry |
| zsmalloc metadata | ~32 bytes | No | Per-object zsmalloc header |
| Parallel decompress buf | 4 KB per worker | No | kmalloc'd per work item, freed after use |
| Workqueue struct | ~256 bytes | No | `struct minimem_cluster_work` per parallel item |

### Per-Page Overhead

For each compressed page stored in zsmalloc:
- **Compression map entry**: ~56 bytes (algo_id + compressed_len + zs_handle)
- **zsmalloc object header**: ~32 bytes
- **Total metadata per page**: ~88 bytes

For a 4 KB page compressed to, say, 2 KB:
- **Original**: 4 KB
- **Compressed data**: 2 KB
- **Metadata**: 88 bytes
- **Net savings**: 4096 - 2048 - 88 = 1960 bytes (47.8% savings)
- **Overhead ratio**: 88/4096 = 2.1%

### Worst Case: Incompressible Pages

If a page cannot be compressed (compressed size ≥ original), MiniMem returns `MINIMEM_INCOMPRESSIBLE` and does NOT store it. This means:
- **No memory is wasted on incompressible pages**
- The compression advisor skips pages that are clearly incompressible (uniform, random data)
- Pages compressed to >90% of original are rejected

### Memory Accounting

The sysfs attributes provide full accounting:
- `zswap_pages`: Number of compressed pages stored
- `zswap_bytes`: Total compressed bytes in zsmalloc
- `zswap_saved`: Total bytes saved (original - compressed)
- `pool_pages`: Total physical pages used by zsmalloc pool

The `pool_pages` × 4096 gives total physical memory used by the zsmalloc pool. This should be less than `zswap_bytes + zswap_saved` if compression is effective, because zsmalloc packs small objects efficiently.

### Minimum Savings Threshold

Recommended: **reject compression if savings < 12.5%** (i.e., compressed size > 87.5% of original).

Reasoning:
- Metadata overhead: ~88 bytes per page
- For a 4 KB page compressed to 3.5 KB: savings = 512 bytes, but overhead = 88 bytes
- Net savings = 512 - 88 = 424 bytes (10.3% effective savings)
- At 87.5% threshold: savings = 512 bytes, overhead = 88 bytes, effective = 424 bytes (10.3%)
- Below this threshold, the zsmalloc fragmentation overhead reduces effective savings further

### Recommendations

1. **Minimum savings ratio**: Skip compression if `compressed_size >= original_size * 7/8`. The `MINIMEM_INCOMPRESSIBLE` return already handles this (returns incompressible if compressed >= original).
2. **Add `min_savings_ratio` sysfs knob**: Allow tuning the threshold from userspace.
3. **Add `overhead_bytes` sysfs attribute**: Report total metadata overhead (entries × 88 bytes).
4. **Cap pool size**: Add `max_pool_pages` sysfs attribute to limit total zsmalloc memory usage.
5. **Shrink on memory pressure**: Register a shrinker callback (`register_shrinker`) to decompress pages under memory pressure.

## Relevance to MiniMem

- **RAM compression**: Overhead is minimal for pages that compress well (>50% savings). Pages with <12.5% savings should be rejected.
- **VRAM compression**: GPU VRAM is more expensive, so lower thresholds may be acceptable.
- **Parallel decompression**: The 4 KB per-worker decompress buffer is freed after use. Workqueue items are short-lived.

## Open Questions

1. Should we use `zsmalloc` class size rounding in our savings calculation? zsmalloc rounds up to the nearest class size, which can add 10-20% overhead for small objects.
2. Should we implement `register_shrinker` to decompress pages under memory pressure?
3. What's the optimal cluster size for parallel decompression? 32 pages matches Linux swap readahead, but smaller clusters may be better for random access patterns.

## References

- Linux zsmalloc documentation: `Documentation/mm/zsmalloc.rst`
- zswap memory accounting: `mm/zswap.c`
- Linux shrinker API: `include/linux/shrinker.h`