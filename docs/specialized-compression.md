# MiniMem — Specialized Compression

Second-layer investigation: what can we learn from other domains (data streaming, columnar databases, signal processing, display compression, brain-inspired memory) to build compression systems optimized for our specific targets (memory pages, AI workloads, VRAM)?

See [research/015](research/015-display-stream-compression-dsc.md) for DSC analysis and [research/017](research/017-sparse-activation-brain-inspired-recall.md) for sparse activation research.

---

## Why Specialized Compression Matters

General-purpose algorithms (LZ4, zstd) treat data as opaque byte streams. They find repeated byte sequences and back-reference them. But memory pages and AI weights have **semantic structure** that general-purpose algorithms ignore:

- **Memory pages** contain aligned 8-byte values (pointers, integers) where the upper 4 bytes are often zero on 64-bit systems
- **AI weight tensors** have quantization patterns, sparse blocks, and row-local similarity
- **Page table pages** contain PTEs that differ only in a few bits (PFN, permissions)
- **Fork COW pages** are nearly identical to their parent

A specialized compressor that understands this structure can achieve better compression and/or faster decompression than a general-purpose algorithm treating the same data as a byte stream.

---

## Lessons from Other Domains

### 1. Columnar Database Compression

**Source:** C-Store, Vertica, Parquet column encoding techniques

Columnar databases compress each column independently using algorithms matched to the column's data type and distribution:

| Column type | Compression technique |
|---|---|
| Low-cardinality (few distinct values) | Dictionary encoding |
| Sorted, monotonic | Delta encoding |
| Small range | Bit-packing (store N bits instead of 32/64) |
| Sparse (many zeros) | Run-length encoding (RLE) |
| General | LZ4/zstd on column chunks |

**Lesson for MiniMem:** Memory pages can be classified by their "column type" — a page of pointers is like a low-cardinality sorted column (many similar values). A page of zeros is like a sparse column. The **compression advisor** (Stage 4) is analogous to a columnar database's per-column encoding selection.

**Application:** Classify 4KB memory pages into types, then apply the best algorithm per type. This is more efficient than running every page through LZ4.

---

### 2. Data Streaming Compression (Zstd streaming, Snappy framing)

**Source:** Zstd streaming API, Apache Kafka compression, gRPC wire compression

Streaming compression processes data in windows, maintaining a sliding dictionary that captures recent patterns. The key insight: **recent data is the best dictionary for upcoming data**.

**Lesson for MiniMem:** Consecutive memory pages in a process's address space often share structure (same heap region, same stack frame, same mmap'd file). A **sliding window dictionary** trained on recently-compressed pages would improve compression of the next page.

**Application:** For the kernel module, maintain a per-process sliding dictionary of recently-seen page content patterns. When compressing a new page from process X, use process X's recent dictionary as a warm start for LZ4/zstd. This is cheaper than training a full dictionary and captures process-local patterns.

---

### 3. Signal Processing (Predictive Coding, DPCM)

**Source:** Audio compression (FLAC, Opus), image compression (JPEG-LS, WebP lossless)

Differential Pulse-Code Modulation (DPCM) encodes each sample as the **difference from a predicted value**. If the predictor is good, the residuals are small and compress trivially. FLAC uses linear prediction; JPEG-LS uses median-of-three neighborhood prediction.

**Lesson for MiniMem:** Memory pages have spatial locality — adjacent cache lines, adjacent pages, and adjacent rows in a matrix often differ by small amounts. A **simple predictor** (previous value, or median of neighbors) would produce small residuals that compress much better than the original values.

**Application:** For AI weight matrices, predict each row as a linear function of the previous row (or use a running mean). The residuals (differences from prediction) are much smaller than the original values and compress better with LZ4. This generalizes delta encoding from simple XOR to predictive coding.

---

### 4. Integer Compression (Elias-Fano, Roaring Bitmaps, SIMDPacking)

**Source:** Search engine indexing (Elias-Fano), database bitmaps (Roaring), vector search (SIMDPacking)

Integer compression techniques exploit the fact that many integer sequences are monotonically increasing and have small gaps. Elias-Fano encoding stores a sorted list of N integers in N + N * log(max/N) bits — near-optimal for clustered data.

**Lesson for MiniMem:** Page table entries are monotonically increasing PFNs with small gaps. Memory allocation metadata (free lists, buddy bitmaps) are integer sequences. These can be compressed far more efficiently than with general LZ77.

**Application:** A page-table-specific compressor using delta encoding + bit-packing (like SIMDPacking) could compress PTE pages to <20% of original size. This is much better than LZ4's ~50% on the same data.

---

### 5. Neural Network Compression (Quantization-Aware, Sparse Attention)

**Source:** LLM quantization (GPTQ, AWQ, SqueezeLLM), sparse attention patterns

LLM compression research has produced techniques for reducing model size:
- **GPTQ/AWQ:** Post-training quantization to INT4/INT8 with minimal accuracy loss
- **Sparse attention:** Most attention weights are near-zero and can be pruned
- **Weight clustering:** Similar weight values can be grouped and represented by a centroid

**Lesson for MiniMem:** These techniques are **lossy** (they change the model's output). MiniMem requires **lossless** compression. But the patterns they exploit (quantization levels, sparse structure, value clustering) are also present in the raw weights and can be exploited by lossless compressors.

**Application:** An AI-weight compressor should:
1. Detect quantization level (FP32, FP16, INT8, INT4)
2. Group values by magnitude (separate near-zero values from large values)
3. Delta-encode within groups (similar values near each other)
4. Bit-pack small values (INT4 weights need only 4 bits, not 8)
5. LZ4-compress the residual after all pre-processing

This pipeline is lossless but exploits the same structure as lossy quantization.

---

### 6. Video/Frame Compression Techniques (Motion Vectors, P-frames)

**Source:** H.264/H.265 P-frames, AV1 reference frames

Video compression achieves extreme ratios by encoding each frame as a delta from a reference frame plus a motion vector. P-frames (predictive frames) can be 10-50x smaller than I-frames (intra-coded frames).

**Lesson for MiniMem:** Process memory pages exhibit temporal locality — a page that was just written is similar to what it contained before the write. Fork COW pages are identical to their parent until modified. Game framebuffers differ incrementally frame-to-frame.

**Application:** For the kernel module, when a compressed page is about to be **decompressed for a write**, don't throw away the old compressed version. After the write, delta-encode the new page against the old version. If the change is small (common for incremental updates), the delta will be much smaller than recompressing from scratch.

---

### 7. Run-Length Encoding for Sparse Data

**Source:** Scientific computing (sparse matrices), Genomics (run-length genomes)

Sparse matrices store non-zero values plus their indices, skipping zeros entirely. CSR/CSC formats achieve 10-100x compression for highly sparse data.

**Lesson for MiniMem:** Pruned AI models have sparse weight tensors (many zero blocks). Memory pages from calloc/mmap'd regions are all zeros. Page tables have large zero gaps between mapped regions.

**Application:** Same-page detection (zero pages) is the simplest form of RLE. For AI weights, block-sparse RLE (store non-zero 64B blocks, skip zero blocks) is more efficient than LZ4 on highly sparse tensors. This is already done in GPU sparse formats but not applied to general VRAM compression.

---

## Proposed Specialized Compressors for MiniMem

### A. Page-Table-Aware Compressor

**Target:** Pages containing page table entries (PTEs) and memory management metadata.
**Approach:**
1. Detect PTE page (all 8-byte aligned values with valid PTE bit patterns)
2. Delta-encode against base PTE (most bits are identical)
3. Bit-pack the differing bits (PFN is the main variable; often <32 bits even on 64-bit)
4. RLE on consecutive PTEs (contiguous PFNs compress to a base + count)

**Expected compression:** 4-10:1 on page table pages (vs ~2:1 with LZ4)

---

### B. AI Weight Compressor

**Target:** GPU weight tensors for LLM inference.
**Approach:**
1. Block classification: sparse (many zeros), dense-uniform (repeated values), dense-variable
2. Sparse blocks: RLE on zero runs + bit-packing on non-zero values
3. Dense-uniform blocks: store base value + count
4. Dense-variable blocks: predictive coding (row delta) + LZ4 on residuals
5. Cross-layer dictionary: train a zstd dictionary on a sample of weight blocks from the model

**Expected compression:**
- INT8 weights: 3-5:1 (vs ~2:1 with raw LZ4)
- INT4 weights: 4-8:1
- Sparse (pruned) models: 10-50:1 on zero blocks

---

### C. Delta-Streaming Compressor

**Target:** Pages that are similar to a known base page (fork COW, incremental updates, consecutive weight rows).
**Approach:**
1. Maintain a base page reference (recently decompressed, or a stable parent page)
2. XOR delta against base page
3. RLE on the delta (long runs of zero bytes where base and target match)
4. LZ4 on the non-zero portions of the delta

**Expected compression:** 4-100:1 for nearly-identical pages (fork COW with <1% changes); ~2:1 for pages with moderate differences.

---

### D. Compression Advisor (Auto-Selector)

**Target:** All pages — automatically select the best algorithm.
**Approach:**
1. Quick page scan (<0.1 μs): check for same-page, zero-heavy, pointer-aligned patterns
2. Fast classify (<0.5 μs): histogram of byte values, alignment check, delta variance
3. Select algorithm based on classification:
   - Same-page → zero allocation
   - Zero-heavy → same-page flag + small exception list
   - Pointer-aligned (8-byte aligned values, many zero upper bytes) → WKdm
   - Small-delta (cache lines near a common base) → BDI
   - Sparse (many zero runs) → RLE + LZ4
   - General → LZ4 (or LZSSE8)
4. After compression, check ratio. If ratio < 1.2:1, store uncompressed (not worth the metadata overhead)

**This is the key differentiator.** Instead of one algorithm for all pages, MiniMem would adaptively select the best algorithm per page. This is similar to what zram's CONFIG_ZRAM_MULTI_COMP does for recompression, but applied at initial compression time, not just for re-compression.

---

## Research Priorities

| Compressor | Novel contribution? | Impact | Priority |
|---|---|---|---|
| Page-table-aware | Medium (BDI exists, but page-table-specific is novel) | High (many PTE pages in kernel) | High |
| AI weight | High (no lossless AI-specific compressor exists) | Very high (VRAM savings for LLM inference) | Very high |
| Delta-streaming | Low (delta encoding is well-known) | Medium (fork COW) | Medium |
| Compression advisor | High (no adaptive per-page selector in Linux) | Very high (improves all workloads) | Very high |
| DSC-Lite predictor | High (novel lossless adaptation of DSC techniques) | High (structured pages, potentially better than WKdm) | Medium |
| Sparse activation map | High (novel compression-aware memory tiering) | Very high (3-10× VRAM savings for MoE) | High |

The **AI weight compressor** and **compression advisor** are the most novel and impactful contributions. The **sparse activation map** (tiered VRAM with compression) is the biggest architectural opportunity for VRAM compression. The **DSC-Lite predictor** is a promising research direction that needs implementation to validate.

---

## New: DSC-Inspired Lossless Predictor (from research/015)

DSC (Display Stream Compression) is lossy but its prediction techniques are borrowable for lossless use:

**Architecture:**
1. MMAP-like median predictor on 32/64-bit words (predict from left/above neighbors)
2. ICH-like dictionary of 32-64 recent word values (for repeated patterns)
3. DSU-VLC or simple variable-length encoding of prediction residuals
4. No quantization, no rate control — pure lossless
5. Per-sector independent decoding for parallel decompression

**Target pages:** Stack/heap (pointers, sequential integers), AI weights (low-variance blocks), page tables (hierarchical patterns). Estimated 1.5-2.5:1 on structured pages.

**Comparison with WKdm:** WKdm uses dictionary-based word classification (exact match, partial match, miss). DSC-Lite adds neighborhood prediction (median of neighbors) which may capture sequential patterns better than WKdm's dictionary approach. Needs benchmarking.

---

## New: Sparse Activation Architecture (from research/017)

MoE models provide a natural "sparse activation map" — the router determines which expert weight blocks are needed per token. This enables compression-aware VRAM tiering:

**Tiering pipeline:**
```
Input token → Router → Expert IDs → Check VRAM tier
                                         |
                                         +-- Hot (VRAM, uncompressed): use directly
                                         +-- Warm (VRAM, compressed): GPU decompress (~5μs)
                                         +-- Cold (RAM, compressed): PCIe transfer + CPU decompress (~15-35μs)
                                         +-- Frozen (NVMe, compressed): SSD I/O + decompress (~100μs+)
```

**Key insight:** No existing weight offloading system (DeepSpeed ZeRO, FlexGen, llama.cpp) uses compression. They all transfer raw data between tiers. Compressing cold weights reduces both transfer time and memory footprint.

**For dense models:** Use layer-sequential prefetching instead of expert routing. While computing layer N, decompress layer N+1 from warm tier. This eliminates the "chicken-and-egg" prediction problem.