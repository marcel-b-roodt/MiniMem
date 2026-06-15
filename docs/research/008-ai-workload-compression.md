# 008 — AI Workload Compression

## Summary

AI model weights loaded into GPU memory (VRAM) are the most compelling compression target for MiniMem. LLM inference requires loading entire models into VRAM, and model sizes are growing faster than VRAM capacity. No existing system does transparent lossless compression of VRAM contents. The properties of AI weights (quantization patterns, sparse structure, row locality) make them highly compressible, but general-purpose algorithms like LZ4 don't fully exploit this structure.

## Key Findings

### Properties of LLM Weight Tensors

1. **Quantization levels:** Modern LLMs use FP16, BF16, INT8, and INT4 weights. Quantized weights have limited value ranges with frequent near-zero values and power-law distributions.

2. **Sparse structure:** Pruned models have explicit zero blocks. Even unpruned models have many near-zero values that could be treated as sparse. Block sparsity (contiguous 64B zero blocks) is common in structured pruning.

3. **Repeated blocks:** Similar patterns repeat across:
   - Attention heads within a layer (parallel heads learn similar patterns)
   - Layers at similar depth in the transformer stack
   - Up/down projection pairs in MLP layers

4. **Row/column locality:** Consecutive rows in a weight matrix are often similar. This is especially true for embedding layers and output projection layers.

5. **Power-law value distribution:** Most values are near zero; few are large. This makes entropy coding (Huffman, ANS) effective on the magnitude distribution.

### Compression Ratios Achievable

| Weight format | LZ4 ratio | Expected with specialized | Reason |
|---|---|---|---|
| FP32 unquantized | ~2:1 | 2-3:1 | High entropy in mantissa; zstd dictionary helps |
| FP16/BF16 | ~1.5:1 | 2-4:1 | Lower precision = more patterns |
| INT8 quantized | ~1.3:1 | 3-5:1 | Quantization levels + sparse blocks |
| INT4 quantized | ~1.2:1 | 4-8:1 | Very few distinct values, high sparsity |
| Sparse (pruned) | ~2:1 | 10-50:1 | Zero blocks compress trivially with RLE |

Note: LZ4 ratios on AI weights are often worse than on general data because quantized weights have high byte-level entropy (each byte is nearly random) even though the semantic structure is compressible. Specialized algorithms that understand the data layout can do much better.

### Why LZ4 Underperforms on AI Weights

LZ4 is a byte-level LZ77 compressor. It finds repeated byte sequences. But quantized AI weights have:
- **High byte-level entropy:** INT8 weights distributed across the full 0-255 range, no repeated byte patterns
- **Semantic structure at the value level:** Adjacent INT8 values may differ by 1-2, but their byte representations differ unpredictably
- **Block-level patterns:** Zero blocks are 64 bytes of zeros (LZ4 handles this well), but near-zero blocks (many small values) are just random bytes to LZ4

A specialized compressor should work at the **value level** (4-byte or 2-byte values for FP16/BF16, 1-byte for INT8, nibbles for INT4) rather than the byte level.

### Existing Approaches (All Lossy)

| System | What it does | Lossless? |
|---|---|---|
| GPTQ | Post-training INT4 quantization with calibration | No (changes values) |
| AWQ | Activation-aware INT4 quantization | No |
| SqueezeLLM | Sparse quantization with sensitivity analysis | No |
| SmoothQuant | Migration of quantization difficulty from activations to weights | No |
| SpQR | Two-level quantization (important weights in higher precision) | No |

**None of these are lossless.** They all change the model's output (with varying accuracy tradeoffs). MiniMem's constraint is lossless compression — the decompressed weights must be bit-exact to the original.

### Proposed Lossless AI Weight Compressor Pipeline

```
1. Block classification (per 64B block):
   - Zero block (all zeros) → RLE flag
   - Sparse block (<25% non-zero) → index list + packed non-zero values
   - Uniform block (single repeated value) → value + count
   - Small-range block (values in narrow range) → base + bit-packed deltas
   - Dense block (general) → delta-encode rows + LZ4

2. Per-block compression with selected strategy:
   - Zero → 1 byte flag
   - Sparse → 16-byte bitmap + 1-48 bytes of packed values
   - Uniform → 4-8 bytes (value + count)
   - Small-range → 8 bytes base + 4-16 bytes bit-packed deltas
   - Dense → row delta + LZ4 (standard compression)

3. Cross-block dictionary (optional, for cold storage):
   - Train zstd dictionary on sample of dense blocks
   - Re-compress dense blocks with zstd + dictionary for better ratio

4. Metadata:
   - Per-block: 4-byte header (type flag + compressed size)
   - Per-tensor: dimension info + block count + algorithm IDs
```

### Expected Savings on Real Models

| Model | VRAM (uncompressed) | LZ4 only | Specialized | VRAM saved (specialized) |
|---|---|---|---|---|
| LLaMA-7B (FP16) | 14 GB | ~10 GB (~1.4:1) | ~5 GB (2.8:1) | 9 GB |
| LLaMA-70B (INT8) | 70 GB | ~55 GB (~1.3:1) | ~18 GB (3.9:1) | 52 GB |
| LLaMA-70B (INT4) | 35 GB | ~30 GB (~1.2:1) | ~7 GB (5:1) | 28 GB |

These are estimates. Actual ratios depend on the model's weight distribution and require benchmarking on real model files.

## Relevance to MiniMem

- **The highest-impact target.** AI inference is VRAM-bound. Saving 50%+ of VRAM enables running larger models or more concurrent instances on the same hardware.
- **No existing lossless VRAM compressor.** This is a genuine gap. Lossy quantization exists (GPTQ, AWQ), but no one does transparent lossless compression of VRAM.
- **Feeds into VRAM layer (Stage 2).** The AI weight compressor is the primary algorithm for MiniMem's VRAM compression path.
- **Block classification is key.** The compression advisor concept (from specialized-compression.md) is essential here. Not all weight blocks benefit from the same algorithm.

## Open Questions

- What are the actual compression ratios on real model weights (LLaMA, Mistral, Mixtral)? Need benchmarks.
- Can we compress weight tensors on GPU (nvCOMP Cascaded codec) instead of CPU? What is the throughput?
- How does decompression latency affect inference throughput? If decompressing a layer adds 50 μs, and inference takes 10 ms per token, that's a 0.5% overhead.
- Can we overlap decompression with compute? (Decompress layer N+1 while computing layer N.)
- Does row-delta prediction actually help on real weight matrices? By how much compared to simple LZ4?
- For the kernel module: should AI weight pages in system RAM (before loading to GPU) also use the AI-weight-specific compressor?

## References

- GPTQ: Frantar et al. "GPTQ: Accurate Post-Training Quantization for Generative Pre-trained Transformers." ICLR 2023.
- AWQ: Lin et al. "AWQ: Activation-Aware Weight Quantization for LLM Compression and Acceleration." 2023.
- SqueezeLLM: Kim et al. "SqueezeLLM: Dense-and-Sparse Quantization." 2023.
- nvCOMP Cascaded codec: https://docs.nvidia.com/cuda/nvcomp/
- Hugging Face model storage formats: https://huggingface.co/docs/safetensors/