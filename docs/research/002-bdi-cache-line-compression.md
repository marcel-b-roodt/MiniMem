# 002 — BDI: Base-Delta-Immediate Cache-Line Compression

## Summary

BDI (Base-Delta-Immediate) is a cache-line compression algorithm proposed by Pekhimenko et al. at ISCA 2012. It represents values in a cache line as a base value plus small deltas, or as immediate values that fit in a small number of bits. BDI achieves 2-4:1 compression on many cache lines with trivially fast decompression (1-2 cycles in hardware).

## Key Findings

- BDI operates on 64-byte cache lines (standard cache line size on x86 and ARM)
- Three encoding modes:
  - **Zero:** entire cache line is zero → single bit flag
  - **Base+Delta:** store a base value + per-word signed deltas (e.g., 4-byte base + 8 × 1-byte deltas for a line of 8×4-byte values)
  - **Immediate:** each value fits in a smaller fixed width (e.g., 4 values each fitting in 2 bytes → 8 bytes total)
- Compression ratio depends on value distribution:
  - Incrementing sequences (1, 2, 3, 4...) → very high compression (1 byte delta each)
  - Pointer arrays (similar base addresses with small offsets) → high compression
  - Random values → no compression (stored uncompressed)
- Decompression is trivially parallelizable in hardware: add base to each delta independently
- BDI can decode in 1-2 cycles in a hardware implementation (simple adder tree)
- The paper showed that ~80% of cache lines in SPEC CPU2006 benchmarks are BDI-compressible
- BDI inspired C-PACT and other pattern-aware cache compression schemes

## Relevance to MiniMem

- BDI is ideal for **page-table pages** and **small-delta pages** — exactly the kind of structured memory where general LZ77 algorithms underperform
- For MiniMem's kernel module, BDI can be used as a **pre-filter**: check each cache line in a page for BDI-compressibility, encode those that qualify, and fall back to LZ4 for the rest
- The zero-line detection is already done by zram (same-page optimization) — BDI extends this to near-zero (small-delta) lines
- BDI's per-cache-line granularity means partial pages can be compressed efficiently (some lines BDI, some lines literal)
- **Novel contribution opportunity:** Combine BDI with LZ4 in a two-level page compressor: BDI-encode compressible cache lines, then LZ4 the resulting byte stream. This could outperform either algorithm alone on structured pages.

## Open Questions

- What fraction of memory pages have >50% BDI-compressible cache lines? Need benchmarks on real page dumps.
- Can BDI be extended to work with 8-byte deltas (for 64-bit values with larger ranges)?
- What is the software decompression throughput of BDI on modern CPUs? (The paper only evaluates hardware decode latency.)
- Is there synergy between BDI and WKdm? Both exploit value structure, but at different granularities (cache line vs page).

## References

- Pekhimenko et al. "Base-Delta-Immediate Compression: Practical Data Compression for On-Chip Caches." ISCA 2012.
- Pekhimenko et al. "Linearly Compressed Pages: A Low-Complexity, Low-Latency Compressed Cache." PACT 2013.
- Alameldeen & Wood. "Frequent Pattern Compression: A Significance-Based Compression Scheme for L2 Caches." ISCA 2004. (Precursor to BDI; encodes frequent patterns like zeros, repeated values, small values.)