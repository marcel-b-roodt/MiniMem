# 006 — nvCOMP GPU Compression

## Summary

nvCOMP is NVIDIA's GPU-accelerated compression library. It provides LZ4, Snappy, DEFLATE, zstd, Bitcomp, Cascaded, ANS, and GDeflate algorithms, all running on the GPU. Aggregate throughput exceeds 50 GB/s on A100 for batched operations. However, per-operation latency (5-20 μs kernel launch) makes it unsuitable for single-page random access.

## Key Findings

- All compression/decompression runs on GPU (CUDA kernels)
- Batched operations achieve massive throughput through parallelism:
  - LZ4 on A100: >50 GB/s aggregate decompression (batch of 4KB+ chunks)
  - Cascaded codec: >100 GB/s for integer data (RLE + delta + bit-packing pipeline)
- Per-operation overhead: 5-20 μs kernel launch latency (not suitable for single small accesses)
- Algorithms supported:
  - **LZ4:** Fast general-purpose, compatible with CPU LZ4 format
  - **Snappy:** Google's format, wide compatibility
  - **Bitcomp:** GPU-native, optimized for GPU data patterns
  - **Cascaded:** Multi-layer: RLE + delta encoding + bit-packing. Best for structured integer data.
  - **ANS/GDeflate:** Entropy coding, higher ratio but slower
  - **zstd:** Dictionary mode available
- Cascaded codec is particularly relevant: it applies RLE → delta → bit-packing in sequence, achieving high compression on structured data (similar to MiniMem's proposed AI weight compressor)
- nvCOMP is production-quality, maintained by NVIDIA, available on all CUDA-capable GPUs

## Relevance to MiniMem

- **Bulk VRAM compression:** nvCOMP is the best option for batch-compressing idle VRAM buffers. When many weight tensors are idle, submit a batch to nvCOMP for background compression.
- **Cascaded codec for AI weights:** The RLE + delta + bit-packing pipeline is well-suited for quantized weight tensors. Evaluate this before building a custom AI weight compressor.
- **Not suitable for on-demand decompression:** 5-20 μs kernel launch latency is too high for per-page fault handling. Use batch pre-decompression instead (predict which weights are needed next and decompress them in advance).
- **Pre-decompression scheduling:** LLM inference has predictable access patterns (layers accessed sequentially). MiniMem's VRAM layer can schedule batch nvCOMP decompression of layer N+1 while layer N is being computed.

## Open Questions

- What is the compression ratio of nvCOMP Cascaded on real LLM weight tensors (INT8, INT4)?
- Can we overlap nvCOMP decompression with GPU compute? (CUDA streams allow this.)
- What is the VRAM overhead of nvCOMP's internal buffers and metadata?
- Is there an AMD/ROCm equivalent to nvCOMP? (If not, MiniMem would need a custom GPU compression path for AMD GPUs.)
- Can we use compute shaders (Vulkan compute) for compression on non-NVIDIA GPUs?

## References

- NVIDIA nvCOMP documentation: https://docs.nvidia.com/cuda/nvcomp/
- NVIDIA nvCOMP GitHub: https://github.com/NVIDIA/nvcomp
- Cascaded codec paper: NVIDIA. "Efficient Compression for GPU-Accelerated Data Processing."