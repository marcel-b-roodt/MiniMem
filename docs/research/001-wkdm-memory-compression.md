# 001 — WKdm Memory Compression

## Summary

WKdm is a word-oriented dictionary-based compression algorithm designed specifically for memory pages, not general data. Created by Paul R. Wilson, Scott F. Kaplan, and Yannis Smaragdakis in 1999, it exploits the structure of 64-bit pointers and integers (aligned values, similar pointers, small integers, zero upper bytes) to achieve 2-4:1 compression on memory pages. Apple adopted it for macOS memory compression in Mavericks (2013), proving the approach at scale.

## Key Findings

- WKdm processes 4KB pages word-at-a-time (4-byte or 8-byte words), not byte-at-a-time
- Each word is classified into one of several categories:
  - **Zero word:** encoded as a single bit
  - **Repeated value:** encoded as a dictionary index (if seen before in this page)
  - **Small value:** low N bits stored, high bits implied
  - **Other:** stored literally
- The dictionary is built per-page from the first occurrence of each unique word pattern
- On 64-bit systems, compression is even better than on 32-bit because:
  - Pointers have many zero upper bytes (canonical high-half addressing)
  - Small integers have zero upper bytes
  - A 64-bit integer with only the low 10 bits set compresses to ~14 bits (>4:1 ratio)
- Similar nearby pointers (common in data structures, linked lists, heap objects) achieve >3:1
- Decompression is fast: dictionary lookup + category decode, no back-reference chasing
- The algorithm is small (~1K lines of C), making it suitable for kernel embedding

## Relevance to MiniMem

- **Primary reference** for the in-memory compression approach — macOS uses WKdm to compress cold pages in-place, which is exactly what MiniMem's kernel module aims to do
- WKdm should be implemented as one of MiniMem's specialized algorithms, targeting pointer-heavy and integer-heavy memory pages
- The per-page dictionary approach is a good fit for the kernel module's compression map (no cross-page state needed)
- WKdm's weakness is on random/unstructured data — MiniMem should fall back to LZ4 for those pages
- **Novel contribution opportunity:** A SIMD-optimized WKdm using AVX2 gather/scatter instructions could significantly outperform the 1999 scalar implementation. No SIMD WKdm implementation is known to exist.

## Open Questions

- What is WKdm's actual decompression throughput in GB/s on modern hardware? (No recent benchmarks exist)
- How does WKdm compare to LZ4 on real memory page dumps? (The original 1999 paper predates LZ4)
- Could a hybrid WKdm+LZ4 approach work? (WKdm for word-level patterns, LZ4 for residual byte-level redundancy)
- What is the optimal word size for WKdm on 64-bit? The original used 32-bit words; 64-bit words might be better on modern hardware
- Apple's implementation may have diverged significantly from the 1999 paper. Can we reproduce their results?

## References

- Wilson, Kaplan, Smaragdakis. "The Case for Compressed Caching in Virtual Memory Systems." USENIX 1999.
- Kaplan. "Compressed Caching and Modern Virtual Memory Simulation." PhD thesis, UT Austin, 1999.
- Apple OS X Mavericks memory compression (2013) — no published implementation details, but confirmed by Apple engineers on LWN and in WWDC sessions
- WK4x4 variant: processes 4 sub-words per word, designed for 64-bit pages with 32-bit sub-word patterns