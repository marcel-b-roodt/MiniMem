# 013 — Specialized Compression Algorithm Inspiration

## Summary

Survey of specialized compression techniques from columnar storage (Parquet), GPU compression (nvCOMP Cascaded), vectorized integer compression (SIMD-BP128), and byte-stream splitting — all applicable to MiniMem's AI weight, page-table, and VRAM compressors.

## Key Findings

### 1. Parquet Column Encodings

Parquet's encoding suite is the closest analog to what MiniMem needs for per-block compression:

| Encoding | How it works | MiniMem analog |
|---|---|---|
| PLAIN | Raw values, no compression | DENSE block type |
| RLE/bit-packing hybrid | Run-length + bit-pack groups of 8 | SMALL_RANGE + SPARSE block types |
| DELTA_BINARY_PACKED | Delta + min-delta frame + bit-pack per miniblock | Row-delta for weight matrices |
| DELTA_BYTE_ARRAY | Prefix-length delta for strings | Delta-streaming for similar pages |
| BYTE_STREAM_SPLIT | Scatter bytes by position (byte 0, byte 1, ...) then compress | **Key insight for FP16/BF16/INT8 weights** |
| Dictionary | Build dictionary, store indices | WKdm (already have this) |

**Critical insight: BYTE_STREAM_SPLIT.** This separates the bytes of multi-byte values into independent streams. For FP16 weights, this means separating the low byte and high byte into two streams, then compressing each. The high byte stream is highly compressible (mostly exponents near 0 or 0x3F/0x40 for small floats). The low byte stream has more entropy but still benefits from LZ4. This is exactly what our byte-level block classifier was missing — it treats each byte independently instead of understanding that the high byte of a 16-bit float carries different information than the low byte.

### 2. nvCOMP Cascaded Codec

NVIDIA's Cascaded codec applies a 3-stage pipeline on the GPU:
1. **RLE:** Replace runs of identical values with (value, count) pairs
2. **Delta encoding:** Store differences between consecutive values
3. **Bit-packing:** Pack deltas using minimum bits per value

Achieves >100 GB/s on A100 for integer data. This is essentially our block classifier's SMALL_RANGE encoding but with delta prediction added. The key difference: Cascaded applies delta across consecutive values (temporal prediction), while our block classifier uses min-value as a base (spatial prediction). For AI weight rows, delta across columns (temporal) may be better than min-base (spatial).

### 3. SIMD-BP128 (Lemire)

Vectorized integer decompression at >4 billion integers/second. Uses SIMD to unpack 128 integers at a time from bit-packed format. Key technique: transposes 128×bitwidth matrix into 4×128-bit registers, then shuffles to extract values.

**Relevance:** If we bit-pack deltas for SMALL_RANGE blocks, we can use the same SIMD transpose technique for decompression at >16 GB/s — far exceeding our throughput needs.

### 4. Byte-Stream Split for FP16/BF16

For 16-bit floating point weights, BYTE_STREAM_SPLIT followed by LZ4 achieves 2-4x better compression than LZ4 alone. The reason:
- Low bytes (mantissa low 8 bits): moderate entropy, LZ4 finds some matches
- High bytes (sign + exponent + mantissa high bits): very low entropy, most values cluster around 0x00, 0x3C, 0x3D, 0x3E — RLE or bit-packing can compress this to <4 bits/byte

**This is the missing piece in our block classifier.** Our current implementation works at the byte level, which misses the multi-byte value structure of FP16/BF16 data.

### 5. Row-Delta Prediction for Weight Matrices

LLM weight matrices are stored row-major. Consecutive rows in a projection matrix often have similar patterns. Row-delta prediction:
1. XOR each row with the previous row
2. The resulting deltas are mostly zero (same pattern across rows) or small (slowly varying weights)
3. Compress the delta matrix instead of the original

This is analogous to P-frame prediction in video codecs. Our delta.c already has XOR primitives. Applied per-row (not per-page), this could turn a 2:1 compression into 4:1+ on weight matrices.

### 6. Live Memory Compression Strategies (Production Systems)

| System | Algorithm | Compression tier | Still-mapped pages? |
|---|---|---|---|
| macOS (Mavericks+) | WKdm | RAM → compressed RAM → swap | **Yes** |
| Linux zswap | LZ4/zstd | Swap front-end | No (swap pages only) |
| Linux zram | LZO/LZ4/zstd | RAM block device | No (swap pages only) |
| Windows 10+ | WofCompressedData | NTFS compression | No (file pages) |
| IBM AIX | AIX WLM | Entropy compression | Unknown |
| tcmalloc (TCMalloc) | — | — | No (no compression) |

**Only macOS compresses still-mapped pages.** Linux deliberately does not — zram/zswap only compress pages that have already been selected for eviction. MiniMem's opportunity: be the Linux equivalent of macOS's compressed memory tier.

### 7. Compressed Caching (Research, 1999-2010)

Wilson, Kaplan, Smaragdakis's original 1999 paper proposed exactly what MiniMem implements: compressed caching as a tier between RAM and disk. Their findings:
- WKdm achieves 2-4:1 on typical process memory pages
- Decompression cost is ~5μs per 4KB page (1999 hardware)
- Compressed caching improves performance when the decompression cost is less than the disk access cost
- **On modern hardware, decompression is ~0.5-2μs, and SSD access is ~50-100μs. The gap has widened enormously.** Compressed caching is even more viable now than in 1999.

## Relevance to MiniMem

### Immediate: Value-level AI weight compressor

Our current block classifier works at the byte level. For AI weights (FP16/BF16/INT8), we need value-level encoding:
1. **FP16/BF16:** BYTE_STREAM_SPLIT → separate high/low bytes → RLE+bitpack highs, LZ4 lows
2. **INT8:** Delta prediction across rows → bit-pack deltas → RLE zeros
3. **INT4:** Already bit-packed; delta + RLE on nibbles

### Short-term: Row-delta compressor for weight matrices

Add a row-delta mode to the block classifier:
1. XOR each row with the previous row
2. Feed the delta block into the same classifier (ZERO/SPARSE/SMALL_RANGE/DENSE)
3. Expected: many more ZERO and SPARSE blocks after delta prediction

### Medium-term: SIMD-BP128 decompression for SMALL_RANGE blocks

When bit-packing deltas, use the SIMD-BP128 transpose technique for decompression. Expected throughput: >16 GB/s, well above our needs.

### Architecture: VRAM compression path

For VRAM (Stage 2), the compression pipeline should be:
1. **Classify buffer type** (weights, activations, textures)
2. **For weights:** value-level split + delta + block-classify → compress
3. **For textures:** LZ4 (general purpose, GPU-friendly)
4. **For activations:** keep uncompressed (hot, short-lived)
5. **Batch decompression:** use nvCOMP Cascaded for GPU-parallel decompression when available
6. **Pre-decompression:** schedule decompression of layer N+1 while computing layer N

## Open Questions

- What is the actual compression ratio of BYTE_STREAM_SPLIT + LZ4 on real LLM BF16 weights?
- Does row-delta prediction help on quantized (INT8/INT4) weights, or only on FP16?
- Can we implement BYTE_STREAM_SPLIT as a pre-processing step before our existing block classifier?
- Should the value-level compressor be a separate algorithm or a mode of the block classifier?
- For VRAM: can we do compression/decompression entirely on GPU (compute shader), avoiding CPU-GPU round-trips?

## References

- Parquet encodings: https://parquet.apache.org/docs/file-format/data-pages/encodings/
- nvCOMP Cascaded: https://docs.nvidia.com/cuda/nvcomp/
- Lemire, Boytsov. "Decoding billions of integers per second through vectorization." VLDB 2012. https://arxiv.org/pdf/1209.2137
- Wilson, Kaplan, Smaragdakis. "The Case for Compressed Caching in Virtual Memory Systems." USENIX 1999.
- Parquet BYTE_STREAM_SPLIT: https://github.com/apache/parquet-format/blob/master/Encodings.md