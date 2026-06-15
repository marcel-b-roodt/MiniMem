# MiniMem — RAM Compression Candidates

Evaluation of compression algorithms and system approaches for transparent in-memory page compression in the Linux kernel.

See [research/README.md](research/README.md) for detailed research backing these assessments.

---

## System-level Approaches

### zram (Compressed RAM Block Device)

**Category:** Existing Linux mechanism (swap compression)
**Status:** In mainline since 3.14. Production-ready.

**What it does:** Creates RAM-based block devices. Pages written to zram are compressed and stored in a zsmalloc pool. Used as swap.

**Pros:**
- Fully transparent to applications when used as swap
- Battle-tested: default on Fedora 33+, Android, Samsung/Xiaomi devices
- Multi-algorithm recompression (CONFIG_ZRAM_MULTI_COMP, up to 4 algorithms)
- Same-page detection (zero allocation for zero-fill/repeated-value pages)
- Dictionary-trained zstd for homogeneous content
- Writeback to disk for idle/incompressible pages

**Cons:**
- **Only compresses pages being evicted to swap** — does not compress pages still mapped in process address space
- Requires swap configuration (not applicable to swapless systems)
- Fixed-size device (pre-allocated, wastes memory if underutilized)
- Adds a block device layer (unnecessary indirection for in-memory compression)

**Verdict:** ✅ Use as reference implementation and benchmark target. MiniMem's kernel module should reuse zsmalloc and multi-algorithm patterns, but operate on still-mapped pages — not swap.

**Implementation notes:** MiniMem's compression map and zsmalloc usage should follow zram's proven patterns. The same-page detection and multi-algorithm recompression code may be directly reusable.

**Open questions:** Can we share a zsmalloc pool between zram and MiniMem, or do we need separate pools?

---

### zswap (Compressed Swap Cache)

**Category:** Existing Linux mechanism (swap cache compression)
**Status:** In mainline since 3.11. Uses zsmalloc since 6.18.

**What it does:** Compressed cache layer in front of a swap device. Intercepts pages being swapped out, compresses them into RAM. Evicts to swap on memory pressure.

**Pros:**
- Completely transparent to applications
- LRU eviction to swap (graceful under pressure)
- Per-cgroup control (disable writeback for latency-sensitive workloads)
- Proactive shrinker for cold pages (since 6.x)

**Cons:**
- Same fundamental limitation as zram: only touches pages being swapped out
- Requires a backing swap device
- Compression map (xarray indexed by swap offset) is tied to swap infrastructure

**Verdict:** ✅ Use as architectural reference. The compression map design (xarray lookup, LRU eviction) is directly relevant to MiniMem's compression map, but keyed by virtual address instead of swap offset.

**Open questions:** Could MiniMem's compression map reuse zswap's xarray infrastructure with a different key space?

---

### macOS Memory Compression (WKdm)

**Category:** Existing production system (in-memory compression)
**Status:** Shipping since OS X Mavericks (2013).

**What it does:** Compresses cold pages **in-place** (while still logically in the process address space). Uses WKdm algorithm. Decompresses on access via page fault.

**Pros:**
- **This is exactly what MiniMem aims to do for Linux**
- WKdm exploits pointer/integer structure in memory pages (aligned values, similar pointers, small integers)
- Works even better on 64-bit: upper bytes of 64-bit pointers/integers are often zero, achieving >4:1 compression
- Transparent to applications
- Proven at scale on millions of Macs

**Cons:**
- Closed source — no implementation to study
- macOS has a simpler memory model (no cgroups, no complex swap layers)
- WKdm is old (1999) — modern SIMD-optimized algorithms may outperform it

**Verdict:** ✅ Adopt as the primary proof that this approach works. Implement WKdm as a baseline, then benchmark against modern SIMD alternatives (LZ4, LZSSE8). See [research/001-wkdm-memory-compression.md](research/001-wkdm-memory-compression.md).

---

### KSM (Kernel Same-page Merging)

**Category:** Existing Linux mechanism (deduplication, not compression)
**Status:** In mainline. Used by KVM for VM memory sharing.

**What it does:** Scans physical pages across processes. Merges identical pages into a single COW-backed copy. Zero decompression cost.

**Pros:**
- Zero decompression overhead (pages are identical, just COW'd)
- Significant savings for VM workloads (many identical guest pages)
- Already in mainline

**Cons:**
- Only helps with **identical** pages — not similar pages
- Scanning overhead (ksmd daemon continuously compares pages)
- COW write fault overhead for pages that diverge after merging
- Not compression — doesn't help with pages that are merely similar

**Verdict:** ✅ Complementary. MiniMem compresses pages KSM cannot merge (similar but not identical). A combined approach could: (1) KSM merges identical pages, (2) MiniMem compresses similar cold pages, (3) delta encoding handles pages that diverged slightly from a KSM-merged base.

**Open questions:** Can we use KSM's page comparison infrastructure to find "similar" pages for delta encoding?

---

## Algorithm Candidates

### LZ4

**Category:** General-purpose fast LZ77
**Decompression speed:** ~3,700 MB/s
**Compression ratio:** ~48% (2.1:1)
**Decompression latency:** ~1.1 μs per 4KB page
**Implementation complexity:** Low (reference C implementation is ~7K LoC)

**Pros:**
- Fastest general-purpose decompressor widely available
- Battle-tested in zram (production since 2014)
- SIMD-optimized paths (SSE2, AVX2)
- Simple implementation, easy to audit for kernel use
- Hardware acceleration available (Intel QAT 2.0, Marvell Structera)

**Cons:**
- Moderate compression ratio (~2:1 on general data)
- Not designed for memory page structure — treats pages as byte streams
- No dictionary mode (unlike zstd)

**Verdict:** ✅ Adopt as the **primary algorithm** for the hot decompression path. Every page that gets compressed should first be tried with LZ4. Cold pages can be recompressed with denser algorithms.

---

### LZSSE8

**Category:** SIMD-optimized LZ77
**Decompression speed:** ~4,700 MB/s
**Compression ratio:** ~36% (2.8:1)
**Decompression latency:** ~0.9 μs per 4KB page
**Implementation complexity:** Medium (SSE4.1 required, format designed around SIMD)

**Pros:**
- Fastest software decompression of any LZ77 variant
- Better compression ratio than LZ4 (2.8:1 vs 2.1:1)
- Branchless decompression eliminates misprediction penalties

**Cons:**
- x86-64 only (requires SSE4.1) — no ARM/NEON port exists
- Smaller community, less battle-tested than LZ4
- Creating a NEON port would be a significant contribution

**Verdict:** ✅ Adopt as the **performance-optimized path** on x86-64 with SSE4.1. Runtime feature detection selects LZSSE8 when available, falls back to LZ4 otherwise.

**Implementation notes:** A NEON port for AArch64 would make this usable on ARM servers (Graviton, Apple Silicon). This is a potential novel contribution.

**Open questions:** Is the 27% decompression speed advantage (4.7 vs 3.7 GB/s) worth the complexity and x86-only limitation?

---

### WKdm

**Category:** Memory-page-specific (word-oriented)
**Decompression speed:** Very fast (word-at-a-time dictionary lookup; no exact benchmarks in GB/s, estimated ~2-3 GB/s)
**Compression ratio:** 2-4:1 on memory pages (better on 64-bit due to zero upper bytes)
**Decompression latency:** ~1.5-2 μs per 4KB page (estimated)
**Implementation complexity:** Low (~1K LoC)

**Pros:**
- Designed specifically for memory page content, not general data
- Exploits structure: aligned values, similar pointers, small integers
- Apple uses this in production (macOS memory compression since 2013)
- Works better on 64-bit than 32-bit (more zero upper bytes)
- Very small code footprint

**Cons:**
- Not SIMD-optimized (1999 algorithm, predates widespread SIMD)
- No recent benchmarks against modern algorithms on page data
- Compression ratio depends heavily on page content (poor on random data)

**Verdict:** ✅ Adopt as a **specialized algorithm for pointer-heavy and integer-heavy pages**. Use alongside LZ4: WKdm for structured pages, LZ4 for general pages. The compression advisor (Stage 4) can classify pages and select the best algorithm.

**Open questions:** Could a SIMD-optimized WKdm outperform LZ4 on pointer-heavy pages? This is a potential novel contribution.

---

### BDI (Base-Delta-Immediate)

**Category:** Cache-line-specific (base + small deltas)
**Decompression speed:** Near-instant (1-2 cycles in hardware; trivial in software)
**Compression ratio:** 2-4:1 on cache lines with small deltas (values near a common base)
**Decompression latency:** <0.1 μs per 64B cache line
**Implementation complexity:** Low (~500 LoC)

**Pros:**
- Trivially fast decompression (just add base to each delta)
- Extremely effective on pages with small value ranges (page tables, sparse arrays, timestamps)
- Hardware-friendly (could be implemented in memory controller)
- Simple to understand and verify

**Cons:**
- Only works on cache-line granularity (64B), not full pages
- Poor compression on data with large value ranges
- Not a standalone algorithm — best as a pre-filter or specialized mode

**Verdict:** ✅ Adopt as a **specialized algorithm for page-table pages and small-delta pages**. Use as a pre-filter: if BDI compresses a cache line well, store it as BDI; otherwise, fall back to LZ4 for the full page.

**Open questions:** What fraction of typical memory pages have BDI-compressible cache lines? Need benchmarks on real page dumps.

---

### Zstd (Dictionary-trained)

**Category:** General-purpose with dictionary support
**Decompression speed:** ~1,550-2,050 MB/s (fast levels)
**Compression ratio:** ~34% (2.9:1) at level -1; better with dictionaries
**Decompression latency:** ~2-2.6 μs per 4KB page
**Implementation complexity:** Medium (~10K LoC for decoder)

**Pros:**
- Best compression ratio among fast algorithms
- Dictionary mode dramatically improves ratio on small, homogeneous data blocks
- zram already supports dictionary-trained zstd (algorithm_params sysfs)
- Multiple compression levels (speed vs ratio tradeoff)
- Good decompression speed preserved across all compression levels

**Cons:**
- Slower decompression than LZ4 (1.5-2x)
- Larger code footprint than LZ4
- Dictionary training requires representative data samples
- Not suitable for the hot decompression path (page faults)

**Verdict:** ✅ Adopt as the **cold-page recompression algorithm**. When a page has been idle for a long time, re-compress it with zstd+dictionary for better density. Decompression latency is acceptable for pages that are cold enough to have been re-compressed.

---

### Delta Encoding (XOR Delta)

**Category:** Page-differential (not standalone compression)
**Decompression speed:** Near memcpy (single XOR per word)
**Compression ratio:** Variable (4:1 to 100:1 for nearly-identical pages; useless for unrelated pages)
**Decompression latency:** <0.5 μs per 4KB page (AVX2 XOR is ~64 GB/s)
**Implementation complexity:** Low

**Pros:**
- Near-zero decompression cost (XOR is the cheapest possible operation)
- Extremely effective for: fork COW pages, incremental data, consecutive weight matrix rows
- Can be combined with LZ4: delta-encode first, then LZ4-compress the delta
- AVX2 XOR throughput is ~64 GB/s (faster than memcpy)

**Cons:**
- Requires a "base page" to delta against — extra metadata and storage
- Useless for unrelated pages
- Base page must be kept uncompressed (or at least easily decompressible)
- Finding similar pages efficiently is an unsolved problem at scale

**Verdict:** ✅ Adopt as a **specialized mode for similar-page pairs**. Most effective for fork-COW workloads and AI weight tensors (consecutive rows are often similar). Combine with LZ4: delta → LZ4 compress the delta → store small delta + base page reference.

**Open questions:** How to efficiently find "similar enough" pages for delta encoding? Bloom filter on page hashes? Locality-sensitive hashing?

---

## Assessment Summary

| Algorithm | Decompress speed | Ratio (memory pages) | Role in MiniMem |
|---|---|---|---|
| LZ4 | ~3.7 GB/s | ~2.1:1 | Primary hot-path algorithm |
| LZSSE8 | ~4.7 GB/s | ~2.8:1 | Performance-optimized path (x86-64) |
| WKdm | ~2-3 GB/s | 2-4:1 | Specialized: pointer/integer pages |
| BDI | Near-instant | 2-4:1 | Specialized: small-delta cache lines |
| Zstd (dict) | ~1.5-2 GB/s | 2.9:1+ | Cold-page recompression |
| Delta (XOR) | ~64 GB/s | Variable | Specialized: similar-page pairs |
| Same-page | 0 (flag) | Infinite | Zero-allocation fast path |

**Recommended default pipeline:**
1. Check same-page (zero-fill, repeated value) → zero allocation
2. Check BDI-compressible → store as BDI if ratio > threshold
3. Check pointer/integer structure → WKdm if likely
4. Default → LZ4 (or LZSSE8 on x86-64 with SSE4.1)
5. Cold page recompression → zstd with dictionary
6. Similar page pair → delta encoding + LZ4