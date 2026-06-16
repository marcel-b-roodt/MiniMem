# 021 — zram Baseline Comparison

## Summary

zram (compressed RAM swap) is the natural baseline for MiniMem. This document compares MiniMem against zram configured with various compression algorithms, quantifying the gap MiniMem fills.

## Key Findings

### How zram Works

zram creates a compressed block device in RAM. Pages are compressed **only when swapped out** — the page must already be unmapped from the process. Decompression happens on swap-in.

```
Process allocates memory → page goes cold → VM reclaims page → page swapped out
  → zram compresses it → stored in zsmalloc → later: page fault → swap-in
  → zram decompresses → page remapped
```

### How MiniMem Works (Different)

MiniMem compresses pages **while they remain in the process address space**. A PTE marker replaces the present PTE. Decompression happens on the next access.

```
Process allocates memory → page goes cold → MiniMem scanner compresses it
  → PTE marker replaces present PTE → original page freed → later: page fault
  → MiniMem decompresses → present PTE installed → process continues
```

### Critical Difference: When Compression Happens

| Event | zram | MiniMem |
|---|---|---|
| Page becomes idle | Nothing happens | Compressed immediately |
| VM reclaims page | Swapped to zram | Already compressed |
| Page accessed again | Swap-in (decompress) | Page fault (decompress) |
| Decompression latency | 5–15 μs (LZ4/zstd) | 0.1–5 μs (adaptive) |

**zram only compresses pages the VM has already decided to evict.** MiniMem compresses pages earlier, before the VM needs to reclaim them. On a memory-stressed system, zram and MiniMem both kick in. On a system with plenty of free RAM, zram does nothing while MiniMem can still save 30–50% of physical memory by compressing idle pages.

### Algorithm Comparison: zram vs MiniMem

zram uses a single algorithm per block device. MiniMem uses 12 algorithms with per-page selection.

| Algorithm | zram available? | MiniMem has? | Ratio on best page type | MiniMem advantage |
|---|---|---|---|---|
| Same-page detection | Yes (zram has this) | Yes | 819:1 | None — both detect zero/repeated pages |
| LZ4 | Yes (default option) | Yes | 157:1 on zero, 2:1 on structured | None on LZ4-optimized pages |
| LZO-RLE | Yes (zram default) | No | ~1.5:1 on general | zram's default is weaker than LZ4 |
| zstd | Yes (zram selectable) | Yes (dict mode) | 4.6:1 on PTE | MiniMem uses zstd for cold recompression |
| WKdm | No | Yes | 3.2:1 on pointer-heavy | MiniMem specific — zram has no dictionary codec |
| WKdm-64 | No | Yes | 6.2:1 on pointer-heavy | Novel 64-bit variant — zram has nothing like this |
| BDI | No | Yes | 60:1 on zero, 7:1 on uniform | Cache-line granularity — zram doesn't have this |
| Block classifier | No | Yes | 146:1 on zero, 2.4:1 on integer | Per-block typing — zram doesn't have this |
| AI FP16/BF16 | No | Yes | 2.0:1 on FP16 weights | BYTE_STREAM_SPLIT — zram can't touch this |
| AI INT8 | No | Yes | 44:1 on uniform INT8 | Row-delta XOR — zram can't touch this |
| Delta XOR | No | Yes | Varies | Paired-page compression — zram doesn't do this |
| LZSSE8 | No | Yes | 3.2:1 on repeated | SIMD path — zram doesn't use this |

### Ratio Comparison on Key Page Types (4KB pages)

| Page type | zram (LZ4) | zram (zstd) | MiniMem (best algo) | MiniMem advantage |
|---|---|---|---|---|
| Zero pages | 157:1 | 216:1 | 819:1 (same-page) | 3.8× vs LZ4 |
| Pointer-heavy (64-bit) | 1.6:1 | 2.2:1 | 6.2:1 (WKdm-64) | 2.8× vs zstd |
| Page table entries | 2.0:1 | 4.6:1 | 4.6:1 (zstd) | 1.0× (same algo) |
| Integer-heavy | 1.8:1 | 2.8:1 | 2.8:1 (zstd) | 1.0× (same algo) |
| AI FP16 weights | 1.0:1 | 1.3:1 | 2.0:1 (AI FP16) | 1.5× vs zstd |
| AI INT8 uniform | 1.0:1 | 1.6:1 | 44:1 (AI INT8) | 27× vs zstd |
| General mixed | 1.2:1 | 1.4:1 | 1.4:1 (zstd) | 1.0× (same algo) |

### Latency Comparison

| Scenario | zram swap-in | MiniMem decompress | Notes |
|---|---|---|---|
| Best case (same-page) | ~2 μs | 0.09 μs | MiniMem skips decompression entirely |
| Fast general (LZ4/WKdm) | ~5 μs | 0.7–3.5 μs | Similar order; MiniMem faster on structured pages |
| Slow general (zstd) | ~12 μs | 10–12 μs | Same algorithm, same latency |
| AI weights (FP16) | ~5 μs (LZ4, no compression) | 5 μs (AI FP16, 2:1 ratio) | MiniMem compresses what zram can't |

### The Real-World Gap

On a typical server workload:
- **zram saves** memory for pages the VM has already decided to swap out
- **MiniMem saves additional** memory by compressing idle pages that are still mapped
- **zram + MiniMem together** would save more than either alone

On a desktop with 16 GB RAM:
- Typical idle desktop: 40–60% of pages are idle/cold
- zram: compresses only pages the VM swaps out (maybe 10–20% of RAM)
- MiniMem: compresses any idle page (40–60% of RAM)
- **MiniMem addresses 2–4× more pages than zram**

### When zram is Sufficient

zram is the right tool when:
- The system is under memory pressure and needs swap
- You want simple, single-algorithm compression
- You don't want to modify kernel page tables

MiniMem is the right tool when:
- You want to compress idle pages **before** the VM reclaims them
- You have domain-specific data (AI weights, page tables, pointer-heavy pages)
- You want the best algorithm per page, not a single global choice

## Relevance to MiniMem

MiniMem should be positioned as **complementary** to zram, not a replacement. The comparison table above shows:

1. **zram's strength**: well-tested, mainline, simple
2. **MiniMem's strength**: compresses earlier, picks better algorithms, covers page types zram can't
3. **Together**: MiniMem compresses idle pages in-place; zram compresses swap pages; both use zsmalloc

The **baseline** for any MiniMem benchmark should be zram with LZ4 (default) and zstd (best ratio), measuring:
- Memory savings (MB freed per GB of RAM)
- Decompression latency (μs per page)
- Effective memory capacity (GB usable per GB physical)

## Open Questions

- Should MiniMem hand off compressed pages to zram when the VM reclaims them? This avoids double-compression.
- Can MiniMem share the idle-page detection with the VM's reclaim path, or does it need its own scanner?
- What is the real-world idle-page percentage on AI inference workloads?

## References

- Linux kernel: `drivers/block/zram/`, `mm/zswap.c`
- zram multi-compression: CONFIG_ZRAM_MULTI_COMP (kernel 6.x+)
- MiniMem research/004: zram/zswap architecture analysis
- MiniMem docs/how-it-works.md: plain-language guide