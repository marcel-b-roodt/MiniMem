# 016 — Parallel Decompression Swarming

## Summary

Investigation of multi-threaded "swarming" techniques for parallel memory page decompression and wake-up in the Linux kernel, GPU, and hardware acceleration contexts. The core question: when a process faults on compressed memory, can we decompress a cluster of pages in parallel to reduce total stall latency? Research covers kernel swap-in paths, page fault clustering, concurrency safety models, GPU-parallel decompression (nvCOMP/Blackwell DE), hardware acceleration (QAT, CXL, FPGA), prefetching strategies, and real-world implementations. **Verdict: parallel cluster decompression is feasible and high-impact for MiniMem, but requires careful design around kernel concurrency primitives, per-page independence constraints, and the choice of CPU- vs GPU- vs HW-accelerated backend.**

---

## 1 — Kernel-Level Parallel Decompression

### Linux swap-in path

The Linux swap-in path (`do_swap_page()` → `lookup_swap_cache()` → `swap_readpage()`) is **fundamentally single-threaded per fault**. Each page fault is handled by the faulting thread in process context. The kernel does not spawn additional threads to decompress multiple swap pages concurrently for a single fault.

However, **multiple processes can independently fault on different swap pages simultaneously**, and each will execute `do_swap_page()` on its own CPU. This provides natural inter-process parallelism but no intra-fault parallelism.

### zram and zswap parallelism

- **zram**: Decompression happens inline in `zram_read_page()`. Each read call decompresses one page using the selected compressor (LZ4, LZ4HC, Zstd, 842). There is **no multi-page batch decompression**. Multiple threads reading from the same zram device will decompress in parallel on different CPUs, but each decompresses exactly one page. zram uses per-CPU compression streams (`struct zram_comp`) with `mutex` locking — each CPU gets its own compressor context, avoiding contention. This is per-CPU parallelism, not per-fault parallelism.

- **zswap**: Similar to zram — each page is decompressed individually in the swap-in path. zswap stores compressed pages in a per-swap-slot rbtree. Decompression is single-page, inline. No batching.

### kswapd

`kswapd` runs **one thread per NUMA node** (`kswapd0`, `kswapd1`, ...). It handles background reclaim (writing pages out, not reading them in). For decompression work, kswapd is not relevant — decompression only happens on the fault path. There is no kernel thread dedicated to parallel decompression.

### Kernel patches for parallel swap-in

As of 2026, **no mainline kernel patches exist for parallel multi-page swap-in/decompression**. The closest work:
- **Swap readahead** (see section 2) brings in multiple pages sequentially, not in parallel.
- **Multi-gen LRU** (MGLRU, merged 6.1) changes reclaim policy but does not parallelize swap-in.
- The **"Migrate Pages in lieu of discard"** RFC (Dave Hansen, 2020) addressed page migration to PMEM tiers, not parallel decompression.
- No patch set has proposed spawning worker threads to decompress a cluster of pages in parallel on the swap-in path.

**Key constraint**: The kernel's page fault handler runs in the faulting process's context with the mmap_lock held (at least read). Adding parallelism within a single fault requires either releasing the lock (risky) or using async worker threads with completion callbacks.

---

## 2 — Page Fault Clustering

### Swap readahead

Linux implements **swap readahead** (`swap_ra_info()`, `do_swap_page()` in `mm/memory.c`). When a page fault occurs on a swap entry, the kernel speculatively reads adjacent swap slots. This is analogous to file readahead but for anonymous pages.

Key parameters:
- **Window size**: Default 32 pages (128 KB). Controlled by `/sys/kernel/mm/swap/swap_ra_ratio`.
- **Hit tracking**: The kernel tracks swap readahead hits/misses and adapts the window.
- **Sequential detection**: If the faulting process is scanning through swap sequentially, readahead is aggressive. Random access reduces the window.

**Critical point**: Swap readahead **reads** multiple pages from the swap device but **decompresses them serially**. Each page is brought into the swap cache and decompressed one at a time. This is the primary opportunity for parallelization — the clustering already exists, the parallelism does not.

### mmap readahead

File-backed mmap readahead (`page_cache_sync_readahead()`, `page_cache_async_readahead()`) does apply to file pages but **not to swap pages**. Swap readahead is a separate mechanism. However, the clustering behavior is similar — sequential access patterns trigger larger readahead windows.

### Typical cluster sizes

- Swap readahead default: 32 pages (128 KB window)
- zram: Pages are individually compressed; no inherent clustering
- zswap: Same — individual page compression
- File readahead: 128 KB–1 MB depending on device

**Opportunity**: When the kernel brings in a 32-page cluster via swap readahead, MiniMem could dispatch all 32 decompression operations to a workqueue or thread pool, decompressing them in parallel across available CPUs. Estimated speedup: if single-page decompress takes 2–5 μs and we have 8+ CPUs available, a 32-page cluster could go from 64–160 μs (serial) to ~8–20 μs (8-way parallel), a **4–8× reduction in total stall latency**.

---

## 3 — Multi-Threaded Decompression Safety

### Locking for concurrent pool access

If multiple threads decompress different pages from the same compressed pool:

- **Read-side**: Decompression is a read-only operation on the compressed data. If compressed pages are stored independently (each page self-contained), **no read-side locking is needed** — multiple CPUs can read different compressed pages simultaneously without contention.
- **Write-side**: The decompressed page must be written to a fresh physical page. If each decompress targets a different page frame, **no write-side conflict**.
- **Pool metadata**: The xarray/rbtree mapping VA → compressed entry needs RCU protection for lookups (MiniMem already has this) and a spinlock for insertions/deletions. This is the same pattern already implemented in MiniMem's compression map.

**Conclusion**: If MiniMem stores each page as an independently compressed unit (which it does — WKdm, LZ4, BDI are all per-page compressors), concurrent decompression of different pages is **inherently safe with minimal locking**.

### Data dependencies between compressed pages

Per-page compressors (WKdm, LZ4, BDI, same-page detection) produce **independent compressed blocks**. There are no cross-page references. This is by design — memory pages are 4 KB aligned in the page table and must be individually addressable.

**Exception**: Dictionary-based compression (Zstd with a shared dictionary) creates cross-page dependencies. If MiniMem uses Zstd-dict, the shared dictionary is read-only during decompress, so concurrent access is safe — but the dictionary must be pinned in memory. This is already handled by Zstd's thread-safe decompression API.

### TLB shootdowns

When multiple pages are remapped simultaneously after parallel decompression:

- Each page gets its PTE updated. On x86, this requires a `TLB flush` (INVLPG for single entries, or full TLB flush for many).
- If multiple CPUs are remapping pages for the same mm simultaneously, **TLB shootdown IPIs** may be needed to invalidate TLB entries on other CPUs that have the old mappings cached.
- **Risk**: If 32 pages are remapped in parallel by different threads, and each thread does `flush_tlb_page()`, this could generate up to 32 × N_cpus IPIs — a "TLB shootdown storm."
- **Mitigation**: Batch TLB flushes. Use `tlb_gather_mmu()` / `tlb_finish_mmu()` pattern (Linux already uses this for `mprotect`, `munmap`). Gather all pages that need PTE updates, then do a single TLB flush at the end. This reduces IPIs from O(pages × CPUs) to O(CPUs).

**Recommendation**: MiniMem's parallel decompress path should use `struct mmu_gather` to batch PTE updates and issue a single TLB flush after all pages in the cluster are decompressed.

### Thundering herd

When many threads wait for the same compressed page to be decompressed:

- In Linux, a page fault on a page being read in will block on the page's `PG_locked` flag. Multiple threads faulting on the same page will all sleep until the I/O (or decompress) completes, then all wake up.
- This is the existing kernel behavior for swap-in — `wait_on_page_bit_common()` handles this correctly with wait queues.
- **No new thundering herd risk** for MiniMem beyond what already exists in the swap path. The kernel's page lock mechanism already serializes concurrent access to the same page.

---

## 4 — GPU-Parallel Decompression

### nvCOMP (NVIDIA)

NVIDIA nvCOMP is a GPU-accelerated compression/decompression library supporting LZ4, Snappy, Zstd, GDeflate, Deflate, Cascaded, and Bitcomp formats. Key architecture:

- **Batch API**: `nvcompBatchedLZ4DecompressAsync()` processes N independent chunks in a single GPU kernel launch. Each chunk is decompressed by one or more GPU thread blocks. Throughput scales with batch size.
- **GDeflate**: A bit-swizzled DEFLATE format that extracts 32-way parallelism. Each compressed block is split into 32 independent sub-streams that can be decoded in parallel by GPU threads. Same compression ratio as standard DEFLATE.
- **Blackwell Decompression Engine (DE)**: Hardware-fixed-function block on B200/B300/GB200 GPUs. Supports LZ4, Snappy, Deflate decompression at up to **600 GB/s** throughput. Integrated with the copy engine — compressed data can be decompressed in-transit during PCIe or C2C transfers. Maximum chunk size: 4 MiB on B200.
- **Throughput data** (from NVIDIA benchmarks, A100 GPU, Silesia dataset):
  - LZ4: ~30–50 GB/s decompression (batch, large chunks)
  - GDeflate: ~20–40 GB/s decompression
  - Snappy: ~15–25 GB/s decompression
  - With DE on Blackwell: up to 600 GB/s for hardware-decoded formats
- **Low-level batch API** vs high-level API: Low-level batch API is more efficient for many small buffers (avoids per-buffer kernel launch overhead). Critical for 4 KB page-sized chunks.

### GPU decompression latency vs CPU

- **GPU kernel launch latency**: 5–20 μs per launch (driver overhead). This dominates for small payloads.
- **GPU decompression time for a single 4 KB page**: ~1–5 μs (compute time), but total latency including launch is **10–25 μs**.
- **CPU decompression for a single 4 KB page**: 1–5 μs (LZ4/WKdm), no launch overhead.
- **Break-even**: GPU decompression wins when **batching 32+ pages** in a single kernel launch, amortizing the launch overhead. A 32-page batch: GPU ~15–25 μs total vs CPU serial ~64–160 μs vs CPU 8-way parallel ~8–20 μs.

**Critical constraint**: GPU decompression requires:
1. Data must be on the GPU (or copied there via `cudaMalloc` / `cudaMallocFromPoolAsync`).
2. Result must be copied back to CPU.
3. PCIe round-trip latency: ~5–10 μs for 4 KB.
4. Total GPU round-trip for a 4 KB page: **15–35 μs** — **3–10× slower than CPU for a single page**.

**Conclusion**: GPU decompression is **not viable for single-page CPU memory faults** due to PCIe latency. It IS viable for:
- VRAM compression/decompression (data already on GPU)
- Large batch operations where the launch overhead is amortized
- CPU-side prefetching where latency is less critical

### cuDecompress / AMD equivalent

- **cuDecompress**: Does not exist as a standalone CUDA library. nvCOMP is NVIDIA's unified GPU compression/decompression library.
- **AMD**: No direct equivalent to nvCOMP exists. AMD's `HIP` ecosystem relies on porting CUDA code via HIPify. AMD has published research on GPU-accelerated compression but no production library comparable to nvCOMP. The `rocThrust` and `rocPRIM` libraries provide parallel primitives that could be used to build custom GPU decompression kernels.

---

## 5 — Hardware Parallel Decompression

### Intel QAT (QuickAssist Technology)

Intel QAT provides hardware-accelerated compression and decompression (Deflate, LZ4, LZ4s, Snappy) on Xeon Scalable processors. Key properties:

- **Multi-stream parallelism**: QAT supports multiple concurrent compression/decompression sessions. The hardware has multiple compression engines that can operate in parallel. Each session is independent.
- **Throughput**: Up to 100 Gbps (12.5 GB/s) per QAT instance. Multiple instances can be used in parallel.
- **Latency**: Per-request latency is ~5–20 μs depending on data size and queue depth.
- **API**: The kernel QAT driver exposes compression via the `crypto` API framework. User-space access via Intel's QAT Zlib plugin.
- **Relevance to MiniMem**: QAT could decompress batches of pages in hardware while CPU continues other work. However, the per-request latency (~10–20 μs for 4 KB) is comparable to or slower than CPU decompression (~2–5 μs for LZ4). QAT wins on **throughput** for sustained batch workloads, not on **latency** for single-page faults.

### CXL inline compression

- No CXL compression standard exists as of 2026. The CXL 3.1 specification defines memory expansion but not inline compression.
- Research proposals (e.g., CXL-Compressed from academic papers) suggest compressing data in the CXL memory expansion device, but no shipping hardware implements this.
- The CXL Type-3 memory model IS visible to the Linux mm/ subsystem as regular memory — MiniMem's kernel module can compress/decompress CXL-attached memory using the same Stage 1 mechanisms as regular DRAM.

### FPGA-based parallel decompression engines

- Xilinx/AMD Alveo and Intel FPGA cards can implement parallel LZ4/DEFLATE decompression pipelines.
- Typical throughput: 10–40 Gbps per pipeline, with multiple pipelines per FPGA.
- Latency: Pipeline latency is typically 0.5–2 μs once the pipeline is saturated.
- **Problem**: FPGA decompression requires data to be copied to/from FPGA memory over PCIe — same round-trip latency problem as GPU (~5–10 μs for 4 KB).
- **Viable for**: Data already on FPGA (network packets, storage I/O). Not viable for CPU page-fault-path decompression.

---

## 6 — Prefetching + Parallel Decompress

### Software prefetch instructions

- `_mm_prefetch()` / `PREFETCHT0/T1/T2/NTA`: x86 instructions to warm L1/L2/LLC caches.
- **Application**: During decompression of page N, prefetch compressed data for page N+1 into L2/LLC. This can hide memory latency for the next decompress operation.
- **Impact**: If compressed pages are scattered in memory (likely — different xarray nodes), prefetching can reduce the ~50–100 ns memory access latency for the next page's compressed data.
- **Not a parallelism technique** — this is a latency hiding technique for serial decompression. But combined with parallel dispatch, it can reduce per-thread decompress time.

### Async decompression APIs

- **No standard async decompression API exists in the Linux kernel**. The kernel's `crypto acomp` (async compression) API exists but is designed for full buffers, not page-fault-path integration.
- **User-space**: `libdeflate` is synchronous. `ISA-L` is synchronous. `Zstd` has a streaming API but not async. `zlib` has no async mode.
- **Opportunity**: MiniMem could define an internal async decompression interface:
  ```
  int minimem_async_decompress(struct page *page, void *compressed, size_t comp_size,
                               void (*callback)(struct page *, int status));
  ```
  Dispatch to a workqueue, callback fires when decompress completes. This integrates with the page fault handler's wait-queue mechanism.

### Double-buffering

- Decompress the next batch of pages while the current batch is being faulted in by the process.
- **Implementation**: Maintain two decompression queues. While the faulting thread consumes pages from queue A, worker threads fill queue B. Swap when queue A is exhausted.
- **Best for**: Sequential access patterns (swap readahead hit, mmap scanning). Not useful for random access.
- **Expected benefit**: Eliminates almost all stall latency for sequential workloads by keeping decompressed pages ready before the process faults on them.

---

## 7 — Memory Access Safety Models

### RCU (Read-Copy-Update)

- **Already used by MiniMem** for xarray lookups. RCU allows lock-free reads of the compression map while writers (compress/decompress) use spinlocks.
- **For parallel decompression**: RCU is the right model for lookup. Each decompress thread does `rcu_read_lock()`, looks up the compressed entry, reads the compressed data, decompresses — all under RCU. No locking needed between concurrent decompressors of different pages.
- **Limitation**: RCU doesn't protect the compressed data buffer from being freed while a decompress is in progress. Need a reference count or hazard pointer for that.

### Seqlocks

- Good for read-mostly data where occasional writes occur and readers can retry.
- **Application**: Page metadata (compressed size, algorithm ID, status flags) could use a seqlock. Readers (decompress path) read metadata with `read_seqbegin()`/`read_seqretry()`. Writers (compress path, reclaim) update with `write_seqlock()`.
- **Advantage over RCU**: Seqlock readers don't need to enter/exit RCU, slightly less overhead. Disadvantage: readers may retry, which adds variance.

### Hazard pointers

- Each thread publishes pointers to objects it's currently accessing. Reclaimers check hazard pointers before freeing.
- **Application**: Before a decompress thread reads a compressed page buffer, it publishes a hazard pointer to that buffer. When the buffer is being freed (e.g., page is being reclaimed or recompressed), the reclaimer waits until no hazard pointers reference it.
- **Advantage**: Lightweight, no atomic reference count needed. Disadvantage: scan all hazard pointers to determine if an object is safe to free — O(threads) cost.

### Epoch-based reclamation

- Used by lock-free data structures (e.g., crossbeam in Rust, jemalloc's ralloc).
- **Application**: Threads register in an epoch. Objects freed in epoch N are only actually reclaimed when no thread remains in epoch N or earlier.
- **Advantage**: No per-object reference counting. Batch reclamation. Disadvantage: memory is freed lazily — can delay reclamation by several epochs.

**Recommendation for MiniMem**:

| Use case | Recommended model | Reason |
|---|---|---|
| Compression map lookup | RCU (already implemented) | Lock-free reads, kernel standard |
| Page metadata | Seqlock | Read-mostly, tiny data, fast retry |
| Compressed buffer lifetime | Reference count (`atomic_t`) | Simple, kernel-idiomatic, no scan needed |
| Decompressed page handoff | Page lock (`PG_locked`) | Kernel standard for page fault path |

---

## 8 — Real-World Implementations

### IBM Active Memory Expansion (AME)

- Available on AIX on Power Systems (POWER7+ and later).
- Compresses memory pages to expand effective memory capacity.
- Uses hardware-accelerated compression on Power processors (NX-842 engine).
- **Decompression**: Single-page, inline on page fault. No parallel decompression documented.
- **Compression ratio**: 1.5–2.5× typical. Configurable per LPAR.
- **Key insight**: AME proves that kernel-transparent memory compression is production-viable at enterprise scale, but it doesn't parallelize decompression.

### VMware ESXi memory compression

- ESXi compresses idle VM memory pages using a zlib-based compressor.
- Compressed pages stored in a per-VM compression cache.
- **Decompression**: Single-page, inline on VM exit (when VM needs the page). No parallel decompression.
- **Compression ratio**: ~2:1 typical.
- **Key insight**: VMware uses a per-VM LRU for compressed pages, evicting to swap when the compression cache is full. This tiered approach (compress → swap if cache full) is a good model for MiniMem.

### Windows 10/11 memory compression (RSS / Memory Compression)

- Windows 10+ compresses modified pages in a "compression store" managed by the System process.
- Uses a variant of LZ77/Xpress (Microsoft's fast compressor).
- **Decompression**: Single-page, inline on page fault. The Windows memory manager decompresses one page at a time in the fault path.
- **No parallel decompression** — Windows uses working-set trimming and prefetch, but not parallel decompression.
- **Key insight**: Windows proves that per-page compression in the OS memory manager works at scale. The System process (formerly "Registry / compressed memory") holds compressed pages and their metadata.

### Oracle TME (Transparent Memory Extension)

- Oracle TME is a feature of Oracle Database that transparently compresses in-memory data.
- Uses proprietary compression (likely dictionary-based for columnar data).
- **Not OS-level**: TME is an application-level feature within Oracle Database. It compresses database buffers, not arbitrary process memory.
- **Not relevant** to MiniMem's kernel-level approach.

---

## Feasibility Assessment

### Parallel cluster decompression in Linux kernel: **FEASIBLE**

The technical path is clear:
1. When a page fault triggers swap readahead (32 pages), MiniMem intercepts the cluster.
2. Dispatch all 32 decompression operations to per-CPU workqueues or a kernel thread pool.
3. Each worker decompresses one page into a pre-allocated page frame.
4. Use `struct mmu_gather` to batch PTE updates.
5. Single TLB flush after all pages are decompressed.
6. Wake the faulting process.

**Estimated speedup for 32-page cluster**:
- Serial (current Linux): 32 × 3 μs = 96 μs (LZ4), or 32 × 5 μs = 160 μs (WKdm)
- 8-way parallel: 4 × 3 μs + 2 μs overhead = 14 μs (LZ4), or 4 × 5 μs + 2 μs = 22 μs (WKdm)
- **4.5–7× latency reduction**

**Overhead sources**:
- Workqueue dispatch: ~1–2 μs per operation
- TLB batch flush: ~2–5 μs (single IPI per remote CPU)
- Synchronization: ~1 μs for completion

### Safety model: **WELL-UNDERSTOOD**

RCU for map lookups, atomic refcounts for buffer lifetime, `PG_locked` for page handoff, `mmu_gather` for batched PTE updates. All kernel-standard primitives. No novel safety concerns.

### GPU-parallel decompression viability: **VIABLE FOR VRAM ONLY**

- CPU page faults: GPU round-trip latency (15–35 μs) exceeds CPU decompress time (2–5 μs). **Not viable**.
- VRAM compression: Data is already on GPU. nvCOMP batch decompression at 30–50 GB/s (A100) or up to 600 GB/s (Blackwell DE) is compelling. **Highly viable**.
- CPU prefetch: If MiniMem prefetches compressed pages to GPU memory for future use, the GPU can decompress while CPU does other work. But the result still needs to come back to CPU — only useful if the decompressed data is needed on GPU later.

### Recommended Architecture for MiniMem's Parallel Wake-Up

```
┌─────────────────────────────────────────────────────────┐
│                   Page Fault Path                         │
│                                                          │
│  1. Fault on compressed page                             │
│  2. Look up swap readahead window (32 pages)             │
│  3. Identify cluster of compressed pages                  │
│  4. Dispatch N decompress workers to per-CPU workqueues   │
│  5. Each worker:                                         │
│     - rcu_read_lock()                                    │
│     - Look up compressed entry in xarray                  │
│     - atomic_inc(&entry->refcount)                        │
│     - Decompress to pre-allocated page frame              │
│     - atomic_dec(&entry->refcount)                        │
│     - Mark page PG_uptodate                              │
│     - Signal completion                                  │
│  6. Wait for all workers (completion or wait queue)       │
│  7. Batch PTE update via mmu_gather                       │
│  8. Single TLB flush                                     │
│  9. Wake faulting process                                │
└─────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────┐
│              VRAM Parallel Wake-Up                        │
│                                                          │
│  1. GPU command stream needs compressed VRAM buffer      │
│  2. Identify cluster of compressed buffers                │
│  3. nvcompBatched*DecompressAsync() on GPU stream        │
│  4. GPU decompresses all buffers in parallel              │
│  5. Command stream continues after decompress completes  │
│  (Zero CPU involvement — all on GPU)                     │
└─────────────────────────────────────────────────────────┘
```

### Specific Kernel Integration Points

| Integration point | File / function | Purpose |
|---|---|---|
| Swap readahead | `mm/memory.c: do_swap_page()` → `swap_ra_info()` | Intercept cluster of pages for parallel decompress |
| Page fault handler | `mm/memory.c: handle_pte_fault()` | Entry point for compressed-page detection |
| Workqueue | `kernel/workqueue.c` | Dispatch parallel decompress workers |
| `mmu_gather` | `arch/x86/mm/tlb.c: arch_tlb_gather_mmu()` | Batch PTE updates and TLB flushes |
| Page lock | `include/linux/pagemap.h: lock_page()` | Serialize access to same page across faults |
| Per-CPU buffers | `mm/` per-CPU infrastructure | Already used for compression; extend for decompress |
| sysfs stats | `/sys/kernel/minimem/` | Track parallel_decompress_count, parallel_avg_cluster_size |

### Performance Estimates for Cluster Decompression

| Scenario | Cluster size | Serial time | Parallel time (8 CPUs) | Speedup |
|---|---|---|---|---|
| LZ4, pointer-heavy pages | 8 pages | 16 μs | 4 μs | 4.0× |
| LZ4, pointer-heavy pages | 32 pages | 64 μs | 10 μs | 6.4× |
| WKdm, mixed pages | 8 pages | 40 μs | 7 μs | 5.7× |
| WKdm, mixed pages | 32 pages | 160 μs | 22 μs | 7.3× |
| BDI, cache-line pages | 8 pages | 4 μs | 2 μs | 2.0× |
| BDI, cache-line pages | 32 pages | 16 μs | 4 μs | 4.0× |

*Note: Parallel time includes 2 μs overhead for dispatch + synchronization. BDI is already so fast that parallelization has diminishing returns.*

---

## Open Questions

1. **Workqueue vs kthread pool**: Per-CPU workqueues are kernel-standard, but a dedicated kthread pool might have lower dispatch latency for this specific use case. Needs benchmarking.
2. **Optimal cluster size**: 32 pages (swap readahead default) or dynamic based on access pattern? Should MiniMem adapt cluster size based on readahead hit rate?
3. **Interaction with MGLRU**: Multi-gen LRU changes when pages are evicted. How does this affect the compressed page population and cluster availability?
4. **NUMA locality**: If compressed pages are stored on a different NUMA node than the decompressing CPU, memory access latency increases. Should decompress workers be NUMA-aware?
5. **Zstd dictionary**: If MiniMem uses Zstd with a shared dictionary for homogeneous workloads, does the dictionary become a contention point for parallel decompression? (Likely no — it's read-only, but needs verification.)
6. **Huge page interaction**: If the faulting page is part of a transparent huge page (2 MB), the cluster could be 512 pages. Is this the right granularity for parallel decompression?
7. **VRAM integration with TTM**: How does parallel GPU decompression interact with TTM's buffer migration and eviction? Need to understand TTM command-stream barriers.
8. **Blackwell DE for VRAM**: The hardware DE supports LZ4/Snappy/Deflate but not GDeflate or custom formats. If MiniMem uses GDeflate or custom AI weight compression, it falls back to SM-based decompression. Performance impact?

---

## References

- Linux kernel source: `mm/memory.c` (do_swap_page, handle_pte_fault), `mm/swap_state.c` (swap readahead), `mm/vmscan.c` (kswapd)
- NVIDIA nvCOMP documentation: https://docs.nvidia.com/cuda/nvcomp/index.html
- NVIDIA Blackwell Decompression Engine FAQ: https://docs.nvidia.com/cuda/nvcomp/decompression_engine_faq.html
- NVIDIA blog: "Speeding Up Data Decompression with nvCOMP and the NVIDIA Blackwell Decompression Engine" (Oct 2025)
- NVIDIA blog: "Accelerating Lossless GPU Compression with New Flexible Interfaces in NVIDIA nvCOMP" (Mar 2022)
- GDeflate format: https://docs.nvidia.com/cuda/nvcomp/gdeflate.html
- Intel QAT: https://www.intel.com/content/www/us/en/products/details/quickassist-technology.html
- Linux swap readahead: `mm/swap_state.c`, `swap_ra_info()`
- RCU in Linux: `Documentation/RCU/whatisRCU.rst`
- `mmu_gather` batched TLB flush: `arch/x86/mm/tlb.c`
- IBM AME: AIX documentation on Active Memory Expansion
- VMware ESXi memory compression: VMware KB 2010224
- Windows 10 memory compression: Microsoft documentation on memory compression in Windows 10+
- Dave Hansen, "Migrate Pages in lieu of discard" RFC (2020): https://lwn.net/Articles/824830/
- Johannes Weiner, "Reconsidering swapping" (LWN, 2016): https://lwn.net/Articles/690079/