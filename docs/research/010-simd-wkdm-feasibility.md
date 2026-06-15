# 010 — SIMD-WKdm Feasibility

## Summary

Investigation into whether WKdm's word-oriented dictionary compression can be accelerated using SIMD instructions (AVX2, AVX-512, NEON). No known SIMD implementation of WKdm exists. The algorithm's word-at-a-time classification and dictionary lookup may be parallelizable at 8-word or 16-word granularity, potentially making it the fastest decompressor for pointer-heavy memory pages.

## Key Findings

### WKdm Algorithm Structure (Decomposition for SIMD)

WKdm processes a 4KB page as an array of 32-bit (or 64-bit) words. For each word:
1. **Classification:** tag each word as ZERO, EXACT (dictionary match), PARTIAL (low bits match), or MISS (no pattern)
2. **Dictionary update:** on EXACT and PARTIAL, add to per-page dictionary
3. **Output streams:** tag bits, dictionary indices, partial masks, exception values

Steps 1 and 2 are the bottleneck. Step 3 is packing, which is inherently sequential but cheap.

### SIMD Parallelization Opportunities

**Classification (Step 1):** Embarrassingly parallel. Process 8×32-bit words per AVX2 instruction:
- `_mm256_cmpgt_epi32` — classify zero words
- `_mm256_cmpeq_epi32` — dictionary lookup via compare against dictionary entries
- `_mm256_and_si256` — extract low bits for PARTIAL classification
- Estimated: 4-6 AVX2 instructions per 8-word group → ~200-300 cycles for 1024 words

**Dictionary Lookup (Step 2):** This is the challenge. WKdm's dictionary is a hash table with 8-bit tags. Each lookup requires:
- Compute tag from word
- Probe dictionary at tag position
- Handle collisions (linear probing or rehash)

SIMD dictionary lookup approaches:
- **Broadcast + compare:** Load 8 words, broadcast each to all dictionary positions, compare. O(N×D) where N=words, D=dictionary size. D=256 for WKdm.
- **Gather + compare:** AVX2 `_mm256_i32gather_epi32` can load 8 dictionary entries in one instruction, then compare all 8 against the input word.
- **Perfect hashing:** If dictionary tags are designed as a perfect hash (no collisions for the expected word distribution), lookup reduces to a single gather instruction.

**Estimated speedup:** Scalar WKdm processes ~1 word per cycle (classification + dictionary + packing). AVX2 could process ~4-8 words per cycle with gather-based dictionary lookup. Expected 4-6x speedup.

### Proposed SIMD-WKdm Implementation

```
Phase 1: Classification (AVX2)
  - Load 8 words at a time
  - Classify all 8 as ZERO/EXACT/PARTIAL/MISS using SIMD comparisons
  - Store tag bits to tag stream

Phase 2: Dictionary Lookup (AVX2 gather)
  - For EXACT: gather dictionary entries, compare with input words
  - For PARTIAL: gather dictionary entries, compare low bits
  - For MISS: add to dictionary, emit as exception

Phase 3: Packing (scalar)
  - Pack tag bits, dictionary indices, partial masks, and exceptions
  - This phase is sequential but fast (simple memcpy-like operations)
```

### 64-bit WKdm (WK8x8 variant)

On 64-bit systems, processing 64-bit words instead of 32-bit words doubles the potential compression (more zero upper bytes) but changes the dictionary structure:
- 64-bit words mean fewer words per page (512 instead of 1024)
- Dictionary tags become 16-bit or use a different hash
- AVX2 can process 4×64-bit words per instruction (`_mm256_cmpgt_epi64`)

A 64-bit WKdm variant could be designed around 8-byte words with 8 sub-words per word (WK8x8 from the original paper). This variant has not been benchmarked on modern hardware.

## Relevance to MiniMem

- If SIMD-WKdm achieves >3 GB/s decompression on pointer-heavy pages, it becomes the preferred algorithm for that page type, beating LZ4
- The combination of better ratio (2-4:1) AND better speed than LZ4 on structured pages would be a significant result
- This is a potential novel contribution: no SIMD WKdm implementation is known to exist
- The compression advisor could select SIMD-WKdm for pointer-heavy pages and LZ4 for general pages

## Open Questions

- What is the actual speedup of SIMD classification vs scalar? Need microbenchmarks.
- Can the dictionary lookup be made collision-free for the expected word distributions in memory pages?
- Is a 64-bit WKdm variant (processing 8-byte words) better than a 32-bit variant on 64-bit systems?
- How does SIMD-WKdm compare to LZSSE8 on the same data? LZSSE8 achieves 4.7 GB/s decompression on general data.
- What is the power/energy cost of AVX2 gather instructions compared to simple arithmetic?

## References

- Wilson, Kaplan, Smaragdakis. "The Case for Compressed Caching in Virtual Memory Systems." USENIX 1999.
- Intel Intrinsics Guide: https://www.intel.com/content/www/us/en/docs/intrinsics-guide/
- WK4x4 variant description in Kaplan's PhD thesis (UT Austin, 1999)
- LZSSE8: https://github.com/rygorous/lzsse