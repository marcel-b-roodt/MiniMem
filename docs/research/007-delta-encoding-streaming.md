# 007 — Delta Encoding & Streaming Compression

## Summary

Delta encoding and streaming compression techniques from data streaming, signal processing, and columnar databases offer patterns that can be adapted for memory page and AI workload compression. The key insight: **data that changes incrementally can be compressed by encoding only the differences**, and **recent data is the best predictor for upcoming data**.

## Key Findings

### XOR Delta Encoding

- The simplest form of delta encoding: XOR each word with a base word (previous page, parent page, reference value)
- AVX2 XOR throughput: ~64 GB/s (faster than memcpy on modern CPUs)
- Where two pages differ by <10% of bytes, XOR delta + RLE on zeros achieves 10-100:1 compression
- Where pages differ by >50%, XOR delta offers little benefit over independent compression
- XOR delta is trivially parallelizable and requires no complex data structures

### Predictive Coding (from signal processing)

- DPCM (Differential Pulse-Code Modulation): encode each sample as the difference from a predicted value
- FLAC uses linear prediction (coefficients fitted to recent samples); residuals are near-zero
- JPEG-LS uses median-of-three neighborhood prediction (up, left, up-left)
- Application to weight matrices: predict each row as a linear function of the previous row. Residuals are small and compress well.
- This generalizes simple XOR delta to weighted delta (where the predictor is not just "previous value" but a function of several previous values)

### Sliding Dictionary (from streaming compression)

- Zstd streaming API maintains a sliding window dictionary that captures recent patterns
- Apache Kafka uses zstd with per-topic dictionaries; gRPC uses per-connection streaming compression
- Key insight: **consecutive pages in a process's address space often share structure**
  - Heap pages from the same allocator have similar metadata headers
  - Stack pages from the same thread have similar frame layouts
  - mmap'd file pages have the same file format structure
- A sliding dictionary of recently-compressed pages improves compression of the next page from the same source
- Zstd's `--train` mode can produce static dictionaries for specific page content profiles

### Columnar Compression Patterns (from databases)

- C-Store and Parquet encode each column independently using the best algorithm for its data type
- Dictionary encoding for low-cardinality columns (few distinct values)
- Delta encoding for sorted/monotonic columns
- Bit-packing for small-range columns (store N bits instead of 32/64)
- RLE for sparse columns (many repeated values)
- Application: memory pages can be classified by "column type" (pointer page, integer page, zero-sparse page, general page) and compressed with the best algorithm for that type

### Incremental Page Compression (from video compression)

- Video P-frames encode each frame as a delta from a reference frame
- Application: when a compressed page is decompressed for a write, keep the old compressed version as a reference. After the write, delta-encode the new page against the old version.
- If the change is small (common for incremental updates to data structures), the delta will be much smaller than recompressing from scratch
- This avoids the "write amplification" problem: currently, any write to a compressed page requires full decompression + modification + full recompression

## Relevance to MiniMem

### Kernel Module (RAM)

- **Sliding dictionary per-process:** Maintain a small (64KB-256KB) per-process dictionary of recently-seen page patterns. When compressing a new page from process X, use process X's dictionary as a zstd warm start.
- **Incremental writes:** When a compressed page is written to, delta-encode the new version against the old instead of full recompression. This reduces write amplification for pages that are modified incrementally.
- **Fork-COW optimization:** Fork creates COW pages that are identical to the parent. When a COW page diverges, delta-encode against the parent page instead of compressing from scratch. The delta will be tiny for pages that only changed slightly.

### VRAM (AI Workloads)

- **Row-delta prediction for weight matrices:** Consecutive rows in a weight matrix are often similar. Predictive coding (linear predictor on previous rows) produces small residuals that compress much better than the original values.
- **Layer-to-layer dictionaries:** Train a zstd dictionary on a sample of weight blocks from the model. All layers can use this dictionary for better compression of their weight blocks.

### Specialized Compressor Design

- The columnar database pattern (classify → select best algorithm → compress) directly informs MiniMem's compression advisor design
- Predictive coding from signal processing can improve delta encoding from simple XOR to weighted prediction
- Sliding dictionaries from streaming compression improve per-process compression ratios

## Open Questions

- What is the optimal sliding dictionary size for memory page compression? (Too small: misses patterns. Too large: overhead.)
- How to find "similar enough" pages for delta encoding efficiently? Locality-sensitive hashing? Bloom filters on page hashes?
- Can predictive coding (linear prediction) improve compression of AI weight tensors compared to simple XOR delta? By how much?
- What is the overhead of maintaining per-process sliding dictionaries in the kernel? Memory cost? CPU cost of dictionary lookup?
- For incremental page writes: how often does a written page differ from its pre-write version by <10%? (This determines how often delta encoding helps.)

## References

- Zstd streaming API: https://facebook.github.io/zstd/
- FLAC linear prediction: https://xiph.org/flac/format.html#prediction
- C-Store column compression: Stonebraker et al. "C-Store: A Column-oriented DBMS." VLDB 2005.
- Apache Parquet encoding: https://parquet.apache.org/documentation/latest/
- Video P-frame encoding: Richardson. "H.264 and MPEG-4 Video Compression." Wiley 2003.