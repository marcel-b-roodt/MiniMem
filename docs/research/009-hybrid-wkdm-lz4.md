# 009 — Hybrid WKdm+LZ4 Pipeline

## Summary

Investigation into whether a two-pass compression pipeline — WKdm first to exploit word-level structure (pointers, zeros, small integers), then LZ4 on the WKdm output to exploit byte-level redundancy — can achieve better compression ratios than either algorithm alone on mixed-structure memory pages.

## Key Findings

- WKdm produces three output streams per page: tags (2-bit classification per word), dictionary indices, and exceptions (literal values)
- The tag stream is highly compressible: most pages are dominated by 1-2 tag types (zeros and repeated values)
- The exception stream from WKdm often contains residual byte-level patterns (consecutive small values, partial zero runs) that LZ77 can compress
- A hybrid pipeline could work in two modes:
  1. **Sequential:** WKdm compresses the page → LZ4 compresses the WKdm output → store both compressed streams
  2. **Selective:** WKdm compresses word-aligned portions → LZ4 compresses byte-aligned portions → advisor decides which portions go to which algorithm
- The sequential approach adds decompression overhead (two passes) but may improve ratio significantly on mixed pages
- The selective approach requires a page content classifier but avoids double-decompression overhead

## Relevance to MiniMem

- Mixed-structure pages (50% pointer data, 50% random data) are the worst case for any single algorithm
- WKdm alone achieves ~2:1 on pointer-heavy pages but ~1:1 on random data
- LZ4 alone achieves ~2:1 on general data but doesn't exploit word-level structure
- A hybrid could achieve ~2.5-3:1 on mixed pages by letting each algorithm handle what it's best at
- The compression advisor (Stage 4) could decide between: pure WKdm, pure LZ4, or hybrid based on page classification

## Open Questions

- Does LZ4 actually improve on WKdm output? WKdm's output is more uniform than the input — LZ4 may find less to compress.
- What is the decompression latency of the two-pass pipeline? Target is <10 μs per 4KB page.
- Can the WKdm output format be designed to be more LZ4-friendly (e.g., separating tag stream from data stream)?
- Would selective classification (advisor deciding per-cache-line which algorithm to use) be better than sequential two-pass?
- How does SIMD-WKdm + LZ4 compare to SIMD-WKdm alone? If SIMD-WKdm is fast enough, the LZ4 pass may not be worth the latency.

## References

- Wilson, Kaplan, Smaragdakis. "The Case for Compressed Caching in Virtual Memory Systems." USENIX 1999.
- LZ4 block format specification: https://github.com/lz4/lz4/blob/dev/doc/lz4_Block_format.md
- Video codec intra+inter prediction patterns: P-frames encode delta from reference, then apply lossless compression on the residual