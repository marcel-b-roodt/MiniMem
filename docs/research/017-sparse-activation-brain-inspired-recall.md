# 017 — Sparse Activation & Brain-Inspired Memory Recall for AI Weights

## Summary

Investigation of techniques for keeping only actively-needed AI model weight blocks in fast memory, with a compact index ("sparse activation map") that enables on-demand recall and decompression of cold blocks — analogous to how biological neural networks only activate relevant regions. Covers Mixture-of-Experts routing, activation sparsity, weight offloading systems (DeepSpeed ZeRO, FlexGen, llama.cpp), block-level access patterns, and compression-aware model architectures.

---

## Key Findings

### 1. Mixture of Experts (MoE) — Natural Sparsity

MoE models are the most mature form of sparse activation in production LLMs:

| Model | Params | Active per Token | Sparsity |
|---|---|---|---|
| Mixtral 8x7B | 46.7B | 12.9B (2/8 experts) | 72% inactive |
| DeepSeek-V2 | 236B | 21B (6/160 experts) | 91% inactive |
| Switch Transformer | 1.6T | ~100B (1/128 experts) | 92%+ inactive |
| DBRX | 132B | 36B (2/16 experts) | 73% inactive |

**Key property:** The router determines which expert(s) each token activates. Only those experts' weight blocks need to be in fast memory. The remaining experts are cold and can be compressed/offloaded.

**Access pattern:** Expert selection is input-dependent — the router produces a small integer (expert ID) per token. This is exactly a "sparse activation map": given an input, the map tells you which weight blocks to load.

**Implication for MiniMem:** MoE models have 70-90% of weights cold at any given time. If we compress cold experts and keep only the activation map (router weights + expert indices) in fast memory, we can serve MoE models with 3-10× less VRAM.

### 2. Activation Sparsity in Dense Models

Even in dense (non-MoE) models, significant sparsity exists:

**ReLU and variants (ReLU, GeLU, SiLU/Swish):**
- ReLU zeros out ~50-90% of activations depending on layer depth and input
- After ReLU, most FFN weight columns don't contribute to the output
- The "active" weight columns are input-dependent — different per token
- This creates implicit sparsity in the weight access pattern

**Attention head sparsity:**
- Not all attention heads are equally important per input
- Research shows 20-40% of heads can be pruned per-input with <1% quality loss
- "Attention head pruning" and "adaptive attention" literature
- No standard runtime mechanism to skip unused heads

**Sparse attention patterns:**
- Long-context models use sparse attention (sliding window, global tokens, dilated patterns)
- The KV cache for sparse attention is much smaller than full attention
- But the weight matrices (Q/K/V/O projections) are still fully loaded

**Implication for MiniMem:** Dense models have less structured sparsity than MoE, but the FFN layers (which are 2/3 of transformer params) have high activation sparsity. If we can predict which weight columns are needed (based on activation pattern), we could compress+offload the inactive columns.

### 3. Weight Offloading Systems — Prior Art

| System | Strategy | Memory Tiers | Latency Tolerance | Compression |
|---|---|---|---|---|
| **DeepSpeed ZeRO-3** | Partition params across GPUs; gather on forward pass | GPU VRAM ↔ NVLink | Synchronous (stall) | None |
| **DeepSpeed ZeRO-Infinity** | Offload to CPU RAM + NVMe | GPU ↔ CPU ↔ NVMe | Async prefetch, pipelined | None |
| **FlexGen** | GPU → CPU → SSD hierarchy with scheduling | GPU ↔ CPU ↔ SSD | Batched, scheduled | None |
| **llama.cpp** | mmap() model weights; OS handles paging | VRAM ↔ RAM ↔ SSD | OS page fault | None |
| **Hugging Face Accelerate** | Device map for model sharding | GPU/CPU/Disk | Synchronous | None |
| **OffloadTransformer** | Layer-wise offload with prefetching | GPU ↔ CPU | Prefetch next layer | None |

**Key observation:** None of these systems use compression. They all rely on raw data transfer between memory tiers. This is a clear gap — compressing cold weights before offloading would reduce transfer time and memory footprint simultaneously.

**llama.cpp approach is closest to MiniMem's philosophy:**
- Uses `mmap()` to memory-map the model file
- The OS handles paging: weights loaded on demand via page faults
- Works well on systems with fast SSD (NVMe) and enough RAM
- **Limitation:** No compression — entire model file must fit in RAM+swap
- **MiniMem advantage:** If we compress cold pages in RAM, the effective capacity increases, reducing SSD paging

### 4. Block-Level Weight Access Patterns

AI model weights have highly structured access patterns:

**Transformer layer ordering:**
```
Input → Layer 0 → Layer 1 → ... → Layer N → Output
```
- Layers are accessed sequentially during inference
- Each layer is used exactly once per forward pass
- After layer N is computed, layer N's weights are not needed until the next token
- This creates a natural "activation window" — only 1-2 layers are hot at a time

**Within a layer:**
```
Layer = [Attention(Q,K,V,O), FFN(up, gate, down), Norms]
```
- Attention weights: accessed first, produce Q/K/V projections
- FFN weights: accessed after attention, 2-3x larger than attention
- After FFN completes, attention weights for this layer are cold

**Weight block sizes:**
- Typical weight matrix: [hidden_dim, intermediate_dim] e.g., [4096, 11008] for LLaMA-7B
- At FP16: 4096 × 11008 × 2 bytes = 88 MB per FFN sub-matrix
- At 64-byte block granularity: 88 MB / 64 = ~1.4M blocks per matrix
- At 4KB page granularity: 88 MB / 4096 = ~22K pages per matrix

**Temporal locality:**
- During batched inference, the same layer weights are reused for all tokens in the batch
- Prefill phase: sequential across layers (one pass)
- Decode phase: same layer weights reused every token (high temporal locality)
- KV cache grows during decode but weights are stable

**Implication for MiniMem:** Layer-sequential access means we can aggressively compress and offload completed layers while the current layer is being computed. With 2-layer buffering (compressing layer N-1 while computing layer N), we can overlap compression with computation.

### 5. Brain-Inspired Architectures

**Sparse Distributed Memory (Kanerva, 1988):**
- Memory is addressed by content similarity, not by location
- Each stored pattern activates a distributed set of "hard locations"
- Only activated locations are read/written
- Directly analogous to MoE: experts = hard locations, router = similarity function
- No modern hardware implementation — purely theoretical

**Content-Addressable Memory (CAM) for weights:**
- Associative memory that returns stored data matching a query key
- TCAM (Ternary CAM) in networking hardware does this at line rate
- Could be used for expert/weight-block lookup: "given this activation pattern, which weights are needed?"
- Not practical for large weight matrices (CAM is very area-expensive)
- Software approximation: hash-based index (MiniMem's xarray map)

**Predictive prefetching based on model structure:**
- Transformer layer access is deterministic: Layer N is always followed by Layer N+1
- This enables perfect prefetching: while computing Layer N, prefetch Layer N+1
- Combined with compression: decompress Layer N+1 while computing Layer N
- This is well-understood and implemented in OffloadTransformer, PipeTransformer

**Biological analogy — sparse coding:**
- Biological neural networks use sparse coding: only ~1-5% of neurons fire at any time
- AI models with ReLU activations achieve similar sparsity (~5-50% depending on layer)
- The "brain-inspired" insight: don't keep inactive neurons/weights in fast memory
- MiniMem's approach: compress the inactive weights, decompress on demand

### 6. Compression-Aware Model Architectures

**Structured pruning → compressible blocks:**
- Magnitude pruning: zero out small weights → many zero blocks (SAME_PAGE detectable)
- N:M structured sparsity (2:4, 4:8): NVIDIA Ampere+ hardware supports 2:4 sparsity with 2× throughput
- Wanda pruning: prune based on weight × activation magnitude
- After pruning, model has many zero blocks that compress to near-zero size

**Quantization-aware compression:**
- INT4/INT8 quantized weights have small byte ranges → ideal for block classification
- GPTQ/AWQ/SmoothQuant produce quantized weights with known scale factors
- Scale factors are uniform within groups → high-byte uniformity in block representation
- Our BYTE_STREAM_SPLIT compressor (ai_weights) directly benefits from this

**Models designed for compression:**
- No major work on "compression-friendly" architectures
- Closest: "architectures for efficient inference" (MoE, sparse attention, early exit)
- Opportunity: design weight initialization and regularization to create more compressible patterns (e.g., encouraging weight clustering)

### 7. The "Sparse Activation Map" Concept

**Definition:** A compact data structure that, given an input or activation pattern, returns the set of weight blocks that need to be in fast memory.

**For MoE models:** The router IS the activation map. It's a small matrix (typically [hidden_dim, num_experts]) that produces expert IDs per token. Size: negligible compared to expert weights (e.g., 4096 × 8 × 2 bytes = 64 KB vs 46.7B params).

**For dense models:** There is no natural activation map. However:
- After ReLU/GeLU, the activation mask (which neurons fired) determines which weight columns are needed
- The activation mask is a binary vector of size [intermediate_dim] (e.g., 11008 bits = 1.4 KB per layer per token)
- This mask IS an activation map — but it's computed AFTER the first matrix multiply, not before
- **Chicken-and-egg problem:** You need the weights to compute which weights you need

**Approaches to break the circularity:**
1. **Predictor network:** Small auxiliary network that predicts activation patterns from input embeddings. Trained jointly with the main model. If prediction accuracy is >90%, we can prefetch the predicted-active weights.
2. **Chunked computation:** Split weight matrix into chunks. Compute first chunk, use result to predict which remaining chunks are needed. Overlaps computation with decompression.
3. **Static analysis:** For many inputs, certain weight blocks are always active (e.g., bias terms, common features). Pre-load these; compress the rest.
4. **Speculative decompression:** Predict likely-needed blocks, start decompression speculatively. If wrong, stall and decompress the correct blocks. Amortized cost depends on prediction accuracy.

**Practical recommendation:** For MoE, use the router directly. For dense models, use layer-sequential prefetching (no prediction needed — layer N+1 is always next).

---

## Relevance to MiniMem

### Architecture: Sparse Activation Map + Compression

```
┌──────────────────────────────────────────────────────────────┐
│                  VRAM Compression System                     │
│                                                              │
│  ┌─────────────┐    ┌──────────────┐    ┌─────────────────┐  │
│  │ Activation  │───>│  Compression │───>│  Compressed     │  │
│  │ Map         │    │  Advisor      │    │  Weight Store   │  │
│  │ (router /   │    │ (classify +   │    │ (VRAM / RAM /   │  │
│  │  predictor) │    │  select algo) │    │  NVMe tiers)    │  │
│  └─────────────┘    └──────────────┘    └─────────────────┘  │
│         │                    │                    │          │
│         v                    v                    v          │
│  ┌─────────────────────────────────────────────────────┐    │
│  │            Parallel Decompression Engine             │    │
│  │  (CPU workqueue cluster OR GPU nvCOMP batch)         │    │
│  └─────────────────────────────────────────────────────┘    │
└──────────────────────────────────────────────────────────────┘
```

### Application-Specific Compression Profiles

| Application Type | Access Pattern | Compression Strategy | Expected Ratio |
|---|---|---|---|
| MoE LLM inference | Expert routing → sparse expert load | Compress inactive experts (70-90%); BYTE_STREAM_SPLIT for FP16/BF16 weights | 3-10× on cold experts |
| Dense LLM inference | Layer-sequential; high temporal locality | 2-layer buffering: compress layer N-1 while computing N; prefetch N+1 | 1.5-3× (half the model cold at any time) |
| LLM fine-tuning | All weights accessed per step; gradients add memory | Compress optimizer states (Adam: 2× model size); compress gradients of frozen layers | 2-4× on optimizer states |
| Vision model inference | Layer-sequential (same as LLM) | Same strategy as dense LLM | 1.5-3× |
| Diffusion model inference | UNet: same weights used N times (denoising steps) | High temporal locality — keep weights hot; compress noise scheduler | Low benefit (weights always hot) |
| Reinforcement learning | Policy + value networks, replay buffer | Compress replay buffer (mostly cold old transitions); compress target networks (updated slowly) | 2-5× on replay buffer |

### Compression Ratios on AI Weights (from MiniMem benchmarks)

| Algorithm | FP16 Weights | INT8 Weights | Zero Pages |
|---|---|---|---|
| ai_fp16 (BYTE_STREAM_SPLIT) | 1.96:1 | N/A | 40.96:1 |
| ai_int8 (row-delta XOR) | N/A | 1.25:1 (varied) / 44.5:1 (uniform) | 146.29:1 |
| Zstd dict | 2.12:1 | 1.61:1 | 215.58:1 |
| LZ4 | 1.46:1 | N/A | 157.54:1 |
| Block classifier | 1.06:1 | N/A | 146.29:1 |

**Key insight:** Zstd gives the best ratio on FP16 weights (2.12:1) but at 7.75μs decompress — still under budget. For cold experts that won't be accessed for many tokens, Zstd is the right choice. For warm experts that might be needed soon, LZ4 or ai_fp16 is better (faster decompress).

### Recommended Tiered Strategy for VRAM

```
Hot tier  (VRAM, uncompressed):  Active expert(s) + current layer + KV cache
Warm tier (VRAM, compressed):    Recently-used experts, prefetched layers
Cold tier (RAM, compressed):     Inactive experts, previous layers
Frozen tier (NVMe, compressed):  Rarely-used experts, model backup
```

**Transition rules:**
- Expert used by router → promote from cold/warm to hot (decompress if needed)
- Expert not used for K tokens → demote from hot to warm (compress in VRAM)
- Expert not used for K×10 tokens → demote from warm to cold (compress to RAM)
- Expert not used for K×100 tokens → demote to frozen (compress to NVMe)

**K should be tuned per model and workload.** For Mixtral 8x7B with batch inference, K=16 (tokens) is reasonable — if an expert hasn't been needed for 16 tokens, it's likely cold.

---

## Open Questions

1. **Can we build a practical activation predictor for dense models?** The chicken-and-egg problem (need weights to know which weights are needed) is fundamental. Chunked computation with overlap is promising but unproven. Need to prototype and measure prediction accuracy vs decompression latency tradeoff.

2. **What is the optimal tiering strategy for MoE models?** How many experts to keep hot vs warm vs cold? Depends on batch size, sequence length, and expert routing distribution. Need simulation with real MoE routing traces.

3. **Can we exploit structured pruning for compression?** If we prune a model to 2:4 sparsity (NVIDIA hardware support), does the sparse representation (indices + non-zero values) compress further with our block classifier? Or is the sparse format already optimal?

4. **How does batch size affect the activation map?** With batch=1, only 1-2 experts are active per token. With batch=64, potentially all experts are active (different tokens route to different experts). The compression benefit decreases with batch size.

5. **Can we overlap GPU computation with GPU-side decompression?** If we use nvCOMP or a custom compute shader for VRAM decompression, can it run concurrently with the inference compute? This depends on GPU SM availability and memory bandwidth contention.

6. **What about quantized weight formats (INT4, NF4, FP8)?** Our current ai_weights compressor handles FP16/BF16/INT8. INT4 (GPTQ, AWQ) and FP8 need new byte-split strategies — the high/low byte split doesn't apply the same way to sub-byte formats.

---

## References

1. Fedus et al., "Switch Transformers: Scaling to Trillion Parameter Models" (JMLR 2022)
2. Jiang et al., "Mixtral of Experts" (arXiv 2401.04088, 2024)
3. DeepSeek-AI, "DeepSeek-V2: A Strong, Economical, and Efficient MoE Language Model" (2024)
4. Rajbhandari et al., "ZeRO: Memory Optimizations Toward Training Trillion Parameter Models" (SC 2020)
5. Sheng et al., "FlexGen: High-Throughput GPU Inference with Offloading" (arXiv 2303.06865, 2023)
6. Gerganov, "llama.cpp — Port of Facebook LLaMA model in C/C++" (GitHub, 2023+)
7. Kanerva, "Sparse Distributed Memory" (MIT Press, 1988)
8. Frantar & Alistarh, "GPTQ: Accurate Post-Training Quantization for Generative Pre-trained Transformers" (ICLR 2023)
9. Lin et al., "AWQ: Activation-aware Weight Quantization for LLM Compression and Acceleration" (MLSys 2024)
10. NVIDIA, "Structured Sparsity in Ampere Architecture" (2020)
11. MiniMem research/008 (AI workload compression), /013 (specialized algorithm inspiration), /014 (VRAM architecture)