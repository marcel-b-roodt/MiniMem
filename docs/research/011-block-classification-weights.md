# 011 — Block Classification for AI Weight Compression

## Summary

Investigation into the block classification approach for lossless AI weight tensor compression. The key hypothesis: classifying each 64-byte block of a weight tensor by type (zero, sparse, uniform, small-range, dense) and applying the optimal encoding per block type achieves significantly better compression than general-purpose algorithms (LZ4, zstd) on quantized weight data. This is the core technique behind MiniMem's proposed AI weight compressor.

## Key Findings

### Block Type Distribution (Estimated)

Based on analysis of published LLM weight statistics (LLaMA-7B, LLaMA-70B, Mixtral-8x7B):

| Weight format | Zero blocks | Sparse blocks | Uniform blocks | Small-range blocks | Dense blocks |
|---|---|---|---|---|---|
| FP32 (unquantized) | ~5% | ~10% | ~5% | ~15% | ~65% |
| FP16/BF16 | ~5% | ~15% | ~8% | ~20% | ~52% |
| INT8 (quantized) | ~8% | ~20% | ~12% | ~25% | ~35% |
| INT4 (quantized) | ~15% | ~25% | ~15% | ~20% | ~25% |
| Sparse (pruned) | ~40-70% | ~10-20% | ~5% | ~5-10% | ~10-20% |

Key observation: quantized weights have MORE structure than FP32 weights. INT8/INT4 weights are MORE compressible with specialized algorithms, not less — despite having higher byte-level entropy.

### Per-Block Encoding Strategies

**Zero block** (all values are zero):
- Encoding: 1-byte flag (type = ZERO)
- Ratio: 64:1 (1 byte instead of 64)
- Decompression: memset to zero — nearly free

**Sparse block** (<25% non-zero values):
- Encoding: 16-byte bitmap (which bytes are non-zero) + packed non-zero values
- Example: 8 non-zero INT8 values → 16 bytes bitmap + 8 bytes values = 24 bytes → 2.67:1
- Decompression: scatter non-zero values according to bitmap — simple and fast

**Uniform block** (single repeated value):
- Encoding: 1-byte flag + value (1-8 bytes) + count (1-2 bytes) → 3-11 bytes
- Ratio: 6-21:1
- Decompression: memset to value — nearly free

**Small-range block** (all values near a base value, fitting in ≤N bits):
- Encoding: 1-byte flag + base value (1-8 bytes) + bit-packed deltas (N bits per value)
- Example: INT8 values in range [-16, +15] → 4 bits per value → base (1 byte) + 32 bytes of 4-bit deltas = 33 bytes → 1.94:1
- Better on INT4: values in range [-2, +1] → 2 bits per value → base (1 byte) + 16 bytes = 17 bytes → 3.76:1
- Decompression: add base to each unpacked value — simple shift-and-add

**Dense block** (general, no exploitable structure):
- Encoding: 1-byte flag + LZ4 compression of raw block
- Fallback to general-purpose compression
- Ratio: ~1.2-1.5:1 on INT8 (high byte entropy)
- Decompression: standard LZ4 decompression

### Why LZ4 Underperforms on Quantized Weights

INT8 quantized weights have these properties that hurt LZ77 compression:
1. Each byte is in range [0, 255] with near-uniform distribution — no repeated byte sequences
2. Adjacent bytes (weights) are semantically similar but numerically different — XOR delta is small but byte-level similarity is low
3. The structure is at the value level (INT8 values), not the byte level

A block classifier that works at the value level (INT8, INT4, BF16) can exploit this structure, while LZ4 operates at the byte level and cannot.

### Block Classification Algorithm

Fast classification per 64-byte block:
```
1. Check if all zeros → ZERO (cost: 1 SIMD compare)
2. Check if single repeated value → UNIFORM (cost: 1 SIMD compare + 1 SIMD compare)
3. Compute non-zero count → if <25% → SPARSE
4. Compute value range (max - min) → if range fits in ≤8 bits → SMALL-RANGE
5. Otherwise → DENSE (use LZ4)
```

Target classification time: <0.01 μs per block (10 SIMD instructions). Total for a 4KB page (64 blocks): <0.64 μs.

## Relevance to MiniMem

- **Highest-impact novel contribution.** No existing system does lossless VRAM compression with block classification.
- The AI weight compressor is the primary algorithm for MiniMem's VRAM compression path.
- Block classification naturally integrates with the compression advisor: classify blocks at compression time, select best encoding.
- Expected compression ratios (3-8x on quantized weights, 10-50x on sparse models) are dramatically better than LZ4 (1.2-2x).

## Open Questions

- What is the actual block type distribution on real LLM weights? Need benchmarks on LLaMA, Mistral, Mixtral model files.
- How does the overhead of block classification (0.5-1 μs per page) compare to the savings from better compression?
- Can block classification be done on GPU (compute shader) for VRAM compression?
- What is the optimal block size? 64 bytes (cache line) vs 128 bytes vs 256 bytes. Larger blocks give more data for LZ4 on dense blocks but reduce classification granularity.
- For the decompression path: can we decompress block-by-block in parallel on GPU? If each block is independently encoded, GPU decompression is trivially parallelizable.

## References

- GPTQ: Frantar et al. "GPTQ: Accurate Post-Training Quantization for Generative Pre-trained Transformers." ICLR 2023.
- AWQ: Lin et al. "AWQ: Activation-Aware Weight Quantization for LLM Compression and Acceleration." 2023.
- Parquet column encoding: https://parquet.apache.org/documentation/latest/
- nvCOMP Cascaded codec: https://docs.nvidia.com/cuda/nvcomp/ (block-based compression with run-length, delta, and LZ4 encoding)