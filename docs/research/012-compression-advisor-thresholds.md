# 012 — Compression Advisor Threshold Calibration

## Summary

The compression advisor is MiniMem's page content classifier that selects the best algorithm per page. This document describes how to calibrate the advisor's decision thresholds using benchmark data. The advisor must be deterministic, fast (<0.5 μs per page), and make optimal or near-optimal algorithm selections.

## Key Findings

### Advisor Design: Decision Tree

The advisor is a multi-level decision tree, not a machine learning model. It must be:
- **Deterministic:** Same input always produces same output
- **Fast:** <0.5 μs per 4KB page (budget is <5% of decompression latency)
- **Simple:** Implementable in kernel code (no floating point, no dynamic allocation)

```
Page (4KB)
  |
  v
[1] All same value? ──yes──> SAME_PAGE (zero allocation)
  |
  no
  |
  v
[2] Zero-byte fraction > 50%? ──yes──> WKdm (exploits zero bytes)
  |
  no
  |
  v
[3] 8-byte aligned, most upper 4 bytes zero? ──yes──> WKdm (pointer-heavy)
  |
  no
  |
  v
[4] Cache-line delta variance < threshold? ──yes──> BDI (small deltas)
  |
  no
  |
  v
[5] Known page type (PTE, slab metadata)? ──yes──> SPECIALIZED
  |
  no
  |
  v
[6] Default ──> LZ4 (or LZSSE8 if available)
```

### Classification Metrics (Per Page)

| Metric | Computation | Purpose | Cost |
|---|---|---|---|
| Zero-byte fraction | Count 0x00 bytes / total | Same-page, WKdm suitability | ~0.01 μs (SIMD) |
| Most-common byte | Mode of byte histogram | Same-page detection | ~0.05 μs (SIMD histogram) |
| 8-byte alignment score | Fraction of 8-byte values with zero upper 4 bytes | WKdm suitability | ~0.02 μs (SIMD compare) |
| Cache-line delta variance | Max - Min per 64B cache line, averaged | BDI suitability | ~0.1 μs (per cache line) |
| Byte entropy | Shannon entropy of byte distribution | General compressibility | ~0.05 μs (histogram + log) |
| Repeated value count | Number of 8-byte values that repeat | Dictionary encoding | ~0.02 μs (SIMD compare) |

Total classification time: ~0.25 μs per page. Well within the <0.5 μs budget.

### Threshold Calibration Method

1. **Generate test data:** All 11 synthetic page types × all 7 algorithms
2. **Benchmark:** Measure compression ratio and decompression latency for each (page_type, algorithm) pair
3. **Build decision matrix:** For each page type, find the algorithm that achieves the best ratio × latency product (or best ratio subject to latency constraint)
4. **Fit thresholds:** For each decision node in the tree, find the threshold that best separates the optimal algorithm groups
5. **Validate:** On held-out data (different seeds, real page dumps), verify that the advisor selects the optimal algorithm ≥95% of the time

### Expected Decision Matrix (Hypothesized)

| Page Type | Best Algorithm | Expected Ratio | Key Metric |
|---|---|---|---|
| Zero | SAME_PAGE | ∞ | All bytes = 0x00 |
| Repeated value | SAME_PAGE | >100:1 | All bytes equal |
| Pointer-heavy | WKdm | 3-4:1 | >50% upper bytes zero |
| Integer-heavy | WKdm | 2-3:1 | >30% zero bytes |
| PTE | BDI or page-table-aware | 4-10:1 | Small delta variance |
| AI FP16 | Block-classification + LZ4 | 2-4:1 | Depends on distribution |
| AI INT8 | Block-classification | 3-5:1 | Sparse + small-range blocks |
| AI sparse | Block-classification (RLE) | 10-50:1 | >25% zero blocks |
| Delta pair | Delta (XOR) + LZ4 | 10-100:1 | <5% differing bytes |
| Mixed | LZ4 | 1.5-2:1 | General baseline |
| Random | LZ4 (or skip) | 1.0-1.1:1 | High entropy |

### Fallback Policy

When compression ratio is <1.2:1, store the page uncompressed. The overhead of compression metadata and decompression latency is not justified for marginal savings. The `can_compress()` function in each compressor can make this determination quickly.

## Relevance to MiniMem

- The compression advisor is the **glue** that makes multi-algorithm compression practical
- Without it, MiniMem would need to pick one algorithm for all pages (suboptimal)
- With it, every page gets the best algorithm for its content type
- The advisor must be implemented in both the kernel module (for RAM compression) and potentially in GPU code (for VRAM compression)
- Kernel implementation constraints: no floating point, no dynamic allocation, must be RCU-safe

## Open Questions

- What are the actual optimal thresholds? Need real benchmark data across all algorithms × all page types.
- Should the advisor consider decompression latency as well as ratio? (A page that compresses well with zstd but decompresses slowly might be better served by LZ4.)
- How often does the advisor need to re-classify a page? Once at compression time, or re-evaluate on re-compression?
- For the kernel module: can we use the page's `PG_young` / `PG_idle` flags as a hint? Cold pages → zstd (better ratio), hot pages → LZ4 (faster decompress).
- Can we use kprobes or ftrace to identify page types by allocation context (slab, anonymous, file-backed)?

## References

- C-Store column encoding selection: Stonebraker et al. "C-Store: A Column-oriented DBMS." VLDB 2005.
- zram CONFIG_ZRAM_MULTI_COMP: Linux kernel source, drivers/block/zram/
- H.264 mode decision: Richardson. "H.264 and MPEG-4 Video Compression." Wiley 2003.