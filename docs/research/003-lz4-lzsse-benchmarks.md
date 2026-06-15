# 003 — LZ4 & LZSSE8 Benchmarks

## Summary

LZ4 and LZSSE8 are the two fastest software decompressors available for lossless compression. LZ4 is the industry standard (used in zram, Linux kernel, many databases). LZSSE8 is a SIMD-optimized variant that achieves ~27% faster decompression at the cost of requiring SSE4.1.

## Key Findings

### LZ4 1.10.0

- Decompression: ~3,716 MB/s (Silesia, EPYC 9554 @ 3.1 GHz)
- Compression: ~577 MB/s
- Ratio: 47.60% (2.1:1)
- Algorithm: LZ77 with 4-byte minimum match, 16-bit offset (64KB window), variable-length literals
- SIMD: SSE2 and AVX2 paths exist for compression; decompression SIMD benefit is modest (~1.1x)
- Used in production: zram default, zswap, Linux kernel crypto API, Apache Parquet, RocksDB
- Hardware acceleration: Intel QAT 2.0 (LZ4 offload), Marvell Structera CXL (inline LZ4)

### LZSSE8

- Decompression: ~4,752 MB/s (Silesia, same hardware)
- Compression: ~8.70 MB/s at -6 level
- Ratio: 35.61% (2.8:1)
- Algorithm: LZ77 variant redesigned for SIMD decompression
- Key innovation: token format designed around SSE4.1 shuffles and comparisons
  - Branchless literal copy and match copy using `_mm_shuffle_epi8` and `_mm_alignr_epi8`
  - Eliminates per-token branch misprediction that limits scalar LZ77
  - Processes multiple tokens in parallel within a 128-bit SSE register
- Requires: x86-64 with SSE4.1 (available since Intel Nehalem, 2008; AMD Bulldozer, 2011)
- No NEON port exists; x86-64 only
- Three variants: LZSSE2 (2-bit counts), LZSSE4 (4-bit counts), LZSSE8 (8-bit counts)
  - LZSSE8 has the best ratio; LZSSE2 has the simplest format

### LZ4fast (extreme acceleration)

- At -17 acceleration: ~4,166 MB/s decompression, 62.15% ratio
- Barely compresses (ratio 1.6:1) but very fast
- Useful when any compression is better than none and speed is paramount

### Comparison (Silesia corpus, EPYC 9554 @ 3.1 GHz)

| Algorithm | Decompress (MB/s) | Ratio (%) | Notes |
|---|---|---|---|
| LZSSE8 -6 | 4,752 | 35.61 | Fastest decompression, best ratio here |
| LZSSE4 -6 | 4,598 | 35.91 | Slightly simpler format |
| LZSSE2 -6 | 3,786 | 35.78 | Simplest SIMD format |
| LZ4fast -17 | 4,166 | 62.15 | Maximum speed, minimum ratio |
| LZ4 1.10 | 3,716 | 47.60 | Industry standard |
| memcpy (baseline) | 16,362 | 100.00 | No compression |

### Memory-bandwidth context

| Memory type | Bandwidth |
|---|---|
| DDR5-5600 dual-channel | ~90 GB/s |
| HBM3 (H100) | ~819 GB/s |
| GDDR6X (RTX 4090) | ~1,000 GB/s |

LZ4 decompression at 3.7 GB/s is far below memory bandwidth. However, for page-fault-path decompression, the relevant comparison is against disk I/O latency (swap-in at ~100 μs for SSD, ~10 ms for HDD), not memory bandwidth. A 1 μs LZ4 decompression is 100x faster than SSD swap-in.

## Relevance to MiniMem

- **LZ4 is the primary algorithm** for MiniMem's hot decompression path. It is fast enough for page faults, battle-tested in zram, and has the broadest hardware/software support.
- **LZSSE8 is the performance-optimized path** on x86-64 with SSE4.1. The 27% speed advantage (4.7 vs 3.7 GB/s) may not matter for page faults (both are <1 μs per page) but could matter for bulk decompression (VRAM pre-load).
- **A NEON port of LZSSE8** for ARM/AArch64 would be a novel contribution with significant value for cloud servers (AWS Graviton, Apple Silicon).
- **No pure software algorithm consistently exceeds 5 GB/s decompression** on general data. Hardware acceleration is needed for memory-bandwidth-speed decompression.

## Open Questions

- What is LZ4's decompression speed on real memory page data (not Silesia)? Memory pages may be more or less compressible than the Silesia corpus.
- Can LZ4 decompression be further optimized with AVX-512? (Some work exists but gains are modest.)
- What is the power consumption difference between LZ4 and LZSSE8 decompression? (Relevant for mobile/embedded.)
- Could a custom LZ77 variant designed specifically for 4KB pages (fixed 4KB window, no variable-length header) outperform LZ4?

## References

- LZ4 by Yann Collet: https://github.com/lz4/lz4
- LZSSE by Conor Stokes: https://github.com/conorstokes/LZSSE
- lzbench 2.0.1 benchmark suite: https://github.com/inikep/lzbench
- Intel QAT LZ4 hardware acceleration: https://www.intel.com/content/www/us/en/architecture-and-technology/intel-quickassist-technology.html