# 004 — zram & zswap Architecture

## Summary

zram and zswap are the Linux kernel's two mainline mechanisms for compressed memory. Both compress pages, but only pages that are being evicted to swap — not pages that remain mapped in a process's address space. Understanding their architecture is essential for MiniMem because: (1) MiniMem should reuse their proven infrastructure (zsmalloc, multi-algorithm dispatch), and (2) MiniMem fills the gap they leave (in-memory compression of still-mapped pages).

## Key Findings

### zram

- Creates RAM-based block devices (`/dev/zramX`). Pages written to these devices are compressed and stored in zsmalloc.
- Default compressor: LZO-RLE (since kernel 5.1), selectable per-device via sysfs
- Same-page detection: pages filled with a single repeated value are stored with zero memory allocation (just a flag)
- Multi-algorithm recompression (CONFIG_ZRAM_MULTI_COMP, since ~6.x): up to 4 algorithms simultaneously. Primary (fast, e.g., LZ4) + up to 3 secondary (slower but denser, e.g., zstd). Cold/idle pages recompressed with secondary algorithms.
- Dictionary-trained zstd: zram supports `algorithm_params` sysfs attribute for dictionary-based compression
- Writeback (CONFIG_ZRAM_WRITEBACK, since 4.14): idle/incompressible pages written to backing storage. Compressed writeback (newer): writes pages to backing storage in compressed form.
- Internal table (`zram_table_entry`): indexed by block number, stores zsmalloc handle, compressed length, flags (same-page, huge, idle, etc.)

### zswap

- Compressed cache layer **in front of** a swap device. Not a swap device itself.
- Intercepts pages being swapped out, compresses them into a dynamically-allocated RAM pool.
- Uses zsmalloc as memory allocator (since kernel 6.18; previously zbud)
- Compression map: per-swap-type xarray of `zswap_entry` indexed by swap offset
- LRU eviction to swap device when pool reaches max size (configurable via `max_pool_percent`)
- Proactive shrinker (since 6.x): writes back cold pages before pool is full
- Per-cgroup control: `memory.zswap.writeback` to disable writeback for specific cgroups
- Pages remain in zswap tree after load (not removed until invalidated by swap subsystem)

### zsmalloc

- Slab-based allocator designed for compressed pages. Allows objects to span non-contiguous pages.
- 254 size classes for objects < PAGE_SIZE.
- Objects may cross page boundaries; must be mapped via `zs_map_object()`/`zs_unmap_object()` before access.
- Returns opaque "handle" (not a direct pointer). Handle is the lookup key for the compression map.
- Used by both zram and zswap (and will be used by MiniMem).

### The Critical Gap

**Neither zram nor zswap compresses pages that are still mapped in a process's page table.** The flow is:

```
Running process page → Page reclaim selects it → Page evicted → [zswap compresses / zram receives as swap]
```

MiniMem needs to insert a compression step **before eviction**:

```
Running process page → Page goes cold → MiniMem compresses in-place → Page remains "mapped" via custom PTE
```

## Relevance to MiniMem

- **Reuse zsmalloc** for MiniMem's compressed page storage. It's already in mainline, well-tested, and designed for this exact use case.
- **Reuse multi-algorithm dispatch** pattern from zram's CONFIG_ZRAM_MULTI_COMP. The same LZ4-for-hot, zstd-for-cold strategy applies.
- **Reuse same-page detection** from zram. Zero-allocation fast path for zero-fill and repeated-value pages.
- **Compression map design** should follow zswap's xarray pattern but keyed by (mm_struct, virtual_address) instead of (swap_type, swap_offset).
- **The gap is clear:** MiniMem's kernel module does something that zram/zswap deliberately do not attempt. This is the project's primary contribution.

## Open Questions

- Can MiniMem share a zsmalloc pool with zswap, or should it have a separate pool? (Separate is safer — avoids cross-dependencies.)
- Can MiniMem coexist with zram used as swap? (Yes — they operate on different pages: MiniMem compresses still-mapped pages, zram compresses swap pages. They should be complementary.)
- Should MiniMem hook into the same page reclaim path as zswap, or have its own idle-page scanner?
- What happens when a MiniMem-compressed page is later selected for swap? The flow would be: compressed-in-RAM → decompress → swap out → zswap compresses again. This double-compression is wasteful. MiniMem should intercept and transfer the already-compressed data directly to zswap.

## References

- Linux kernel source: `drivers/block/zram/`, `mm/zswap.c`, `mm/zsmalloc.c`
- Nitin Gupta. "compcache: in-memory compressed swapping." Linux Symposium 2009.
- Seth Jennings. "zswap: Compressed RAM caching for swap." Linux Plumbers Conference 2013.
- LWN articles: https://lwn.net/Articles/547084/ (zswap), https://lwn.net/Articles/332806/ (compcache/zram)