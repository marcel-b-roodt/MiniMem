# MiniMem — VRAM Compression

Deep dive into transparent GPU memory compression. See [research/006](research/006-nvcomp-gpu-compression.md), [research/008](research/008-ai-workload-compression.md), [research/014](research/014-linux-kernel-vram-architecture.md), [research/015](research/015-display-stream-compression-dsc.md), [research/016](research/016-parallel-decompression-swarming.md), and [research/017](research/017-sparse-activation-brain-inspired-recall.md) for supporting research.

---

## The Problem

Modern AI workloads (LLM inference, diffusion models) and game engines consume massive amounts of VRAM. A single LLM layer can occupy hundreds of MB. GPU memory is expensive and limited. Unlike system RAM, there is no "swap" for VRAM — when you run out, the workload fails.

Current GPU drivers do **no transparent compression** of VRAM contents. Textures use GPU-native texture compression (BCn, ASTC, ETC2) but that is lossy and applied at authoring time, not at runtime. General VRAM buffers (weights, activations, framebuffers) are stored uncompressed.

---

## VRAM Content Types

| Buffer type | Compressibility | Access pattern | Best algorithm |
|---|---|---|---|
| AI weights (FP32) | High — repeated patterns, near-zero values | Read-heavy (inference), read-write (training) | AI-specific + Zstd |
| AI weights (INT8/INT4 quantized) | High — sparse blocks, quantization patterns | Read-heavy | AI-specific + LZ4 |
| Activations | Medium — varies by layer | Write-once, read-once | LZ4 (fast decompress) |
| Framebuffers | Low-Medium — already post-compute | Write-heavy, read-once | Delta encoding (frame-to-frame) |
| Textures | Medium — spatial coherence | Read-heavy | Already handled by GPU texture compression |
| Index/vertex buffers | Low — already compact | Read-heavy | LZ4 (marginal savings) |
| Staging buffers | Variable — transient data | Write-once, read-once | LZ4 (if large enough) |

---

## Candidate Approaches

### 1. Driver-level buffer compression (transparent)

**Architecture:** Hook into the GPU driver's memory allocator. When a buffer is allocated, mark it as compressible. When VRAM pressure occurs, compress idle buffers. Decompress on GPU access.

**Pros:**
- Fully transparent to applications
- Works with any workload
- Can target the highest-value buffers first (idle weight tensors)

**Cons:**
- Requires driver modifications (AMDGPU, Mesa, NVIDIA proprietary)
- GPU page faults are expensive (microseconds) — only viable for truly idle buffers
- Decompression on GPU requires compute shader or copy engine
- Compressed data still lives in VRAM (savings offset by metadata overhead)

**Verdict:** Investigate. This is the ideal architecture but depends on driver cooperation. Start with a userspace prototype using CUDA/Vulkan, then evaluate kernel driver integration.

**Open questions:** Which driver hooks are available? AMDGPU and Mesa are open-source — can we add compression hooks there? NVIDIA requires closed-source driver cooperation.

**Update (from research 014):** The Linux kernel's mm/ subsystem has **zero visibility** into discrete GPU VRAM. All VRAM management is inside the DRM/TTM driver stack. The primary insertion point is `amdgpu_vram_mgr` (TTM resource manager for VRAM). CXL Type-3 memory, by contrast, IS fully visible to mm/ and can be compressed by MiniMem's existing kernel module. See [research/014](research/014-linux-kernel-vram-architecture.md) for full analysis.

---

### 2. Application-advised compression (semi-transparent)

**Architecture:** Provide a userspace API (ioctl, CUDA extension, Vulkan extension) that allows applications to advise which buffers are compressible and when. The driver acts on hints but is not required for correctness.

**Pros:**
- Applications can provide semantic hints (this is a weight tensor, this is an activation)
- Driver doesn't need to guess buffer types
- Can integrate with existing ML frameworks (PyTorch, TensorFlow) as a custom allocator
- Works without full driver modifications

**Cons:**
- Not fully transparent — requires application/framework changes
- Adoption barrier: frameworks need to add support
- Incorrect hints can cause performance degradation

**Verdict:** ✅ Adopt as the **initial approach**. Build a CUDA-based custom allocator that marks weight buffers as compressible. This is deployable without driver changes and provides real-world data for evaluating the transparent approach.

**Implementation notes:** Start with a PyTorch custom allocator plugin that uses nvCOMP for background compression of idle weight tensors.

---

### 3. nvCOMP bulk compression (GPU-parallel)

**Category:** Existing library (NVIDIA)
**Throughput:** >50 GB/s aggregate on A100 (batched decompression)
**Latency:** 5-20 μs kernel launch overhead per operation

**What it does:** NVIDIA's GPU compression library. Supports LZ4, Snappy, DEFLATE, zstd, Bitcomp, Cascaded, ANS, GDeflate. All operations run on GPU.

**Pros:**
- Extremely high aggregate throughput (massively parallel)
- Multiple algorithm choices
- Production-quality, maintained by NVIDIA
- Supports batched operations (compress/decompress many buffers at once)

**Cons:**
- High per-operation latency (5-20 μs kernel launch) — not suitable for single-page random access
- Only suitable for bulk operations (batch compress/decompress)
- NVIDIA GPU only
- Not transparent — must be explicitly called

**Verdict:** ✅ Use for **bulk VRAM compression**. When VRAM pressure occurs, batch-compress idle weight buffers using nvCOMP. Batch-decompress when buffers are needed. Not suitable for per-page on-demand decompression.

**Open questions:** Can we batch decompressions to hide kernel launch overhead? If we know which layers are needed next (inference scheduling), we can pre-decompress.

---

### 4. Compute-shader decompression (inline)

**Architecture:** Instead of batch decompression, insert a compute shader barrier before the kernel that needs a compressed buffer. The decompress shader runs inline in the GPU command stream.

**Pros:**
- Lower latency than host-initiated nvCOMP call (no CPU round-trip)
- Can be inserted automatically by a compiler pass or driver hook
- Works for per-buffer on-demand decompression

**Cons:**
- Requires custom compute shaders for each compression algorithm
- Adds GPU compute overhead to the critical path
- Barrier stalls the pipeline until decompression completes

**Verdict:** Investigate for latency-sensitive workloads. Could complement nvCOMP batch compression: batch compress, inline decompress.

---

### 5. Framebuffer delta compression (frame-to-frame)

**Architecture:** For framebuffers that change incrementally (UI rendering, video), store only the XOR delta from the previous frame.

**Pros:**
- Deltas are highly compressible (most pixels unchanged between frames)
- XOR delta is trivially fast on GPU
- Natural for game rendering (most of the framebuffer is unchanged)

**Cons:**
- Requires keeping the previous framebuffer as a base
- First frame must be stored uncompressed
- Not useful for AI workloads (no frame-to-frame correlation)
- Modern GPUs already do lossy display compression (DSC)

**Verdict:** 📋 Lower priority. Games already have DSC (Display Stream Compression). The bigger win is in AI weight and activation buffers. Could revisit for game-specific VRAM savings.

---

## AI Workload Compression — Specialized Approach

AI model weights have specific properties that general-purpose algorithms don't fully exploit:

### Properties of LLM weight tensors

1. **Quantization patterns:** INT8, INT4, FP16, BF16 weights have limited value ranges with frequent near-zero values
2. **Sparse structure:** Pruned models have explicit zero blocks that compress trivially
3. **Repeated blocks:** Similar patterns repeat across attention heads and layers
4. **Column/row locality:** Consecutive rows in a weight matrix are often similar (delta-encodable)
5. **Power-law value distribution:** Many small values, few large values — entropy coding opportunities

### Proposed AI weight compressor pipeline

```
Weight tensor
    |
    v
1. Block classification
   (sparse block / dense block / uniform block)
    |
    +-- Sparse block --> Run-length encode + LZ4
    +-- Uniform block --> Store base value + count
    +-- Dense block --> Delta encode rows + LZ4
    |
    v
2. Compress per-block with selected strategy
    |
    v
3. Store compressed blocks + block metadata
    |
    v
4. GPU decompression:
   - Batch decompress via nvCOMP (preload)
   - Or inline compute shader (on-demand)
```

### Expected savings

| Weight type | Expected compression | Algorithm |
|---|---|---|
| FP32 unquantized | 2-3:1 | Zstd dictionary |
| INT8 quantized | 3-5:1 | AI-specific (sparse + delta) |
| INT4 quantized | 4-8:1 | AI-specific (sparse + RLE) |
| Sparse (pruned) | 10-50:1 | RLE on zero blocks |

---

## Implementation Roadmap for VRAM

1. **Prototype** (userspace): CUDA custom allocator + nvCOMP batch compression of PyTorch weight tensors
2. **Benchmark**: Measure compression ratio, decompression latency, and VRAM savings on real LLM inference workloads
3. **AI-specific compressor**: Implement block classification + sparse/delta/RLE pipeline
4. **Driver integration**: Evaluate AMDGPU/Mesa hooks for transparent buffer compression
5. **Production**: Ship as a PyTorch plugin, then evaluate kernel-level integration

---

## VRAM Access Architecture (from research/014)

The Linux kernel's `mm/` subsystem has **zero visibility** into discrete GPU VRAM. All VRAM management is inside the DRM/TTM driver stack:

| Access Path | mm/ Visibility | Compression Feasibility |
|---|---|---|
| Discrete GPU VRAM | None | Requires TTM/driver hooks |
| AMD APU UMA carveout | None (BIOS reserved) | Requires amdgpu_vram_mgr hooks |
| AMD APU GTT domain | Full (normal struct page) | **Works today** with MiniMem Stage 1 |
| Intel i915 buffers | Full (system RAM only) | **Works today** with MiniMem Stage 1 |
| CXL Type-3 memory | Full (NUMA node) | **Works today** with MiniMem Stage 1 |

**Primary insertion point for discrete VRAM:** `amdgpu_vram_mgr` (TTM resource manager). Add compression as an alternative to eviction-to-system-RAM.

**CXL Type-3:** Fully visible to `mm/` via `dax_kmem`. MiniMem's kernel module compresses CXL pages with zero changes.

---

## Display Stream Compression — What We Can Borrow (from research/015)

DSC is **lossy** (visually lossless only) — cannot be used directly. DSC hardware is display-path only. However, DSC's *prediction and entropy coding* techniques are borrowable:

| DSC Technique | MiniMem Adaptation |
|---|---|
| MMAP prediction (JPEG-LS median) | Lossless predictor for 32/64-bit words on structured pages |
| ICH (Indexed Color History, 32 entries) | Dictionary of recent word values (expand to 64-256 entries for memory pages) |
| DSU-VLC entropy coding | Lossless residual encoding |
| Slice-based parallel encoding | Sub-page blocks for parallel decompression |
| Rate control / quantization | **Remove entirely** (this is the lossy part) |

**Novel idea:** "DSC-Lite" — MMAP + ICH for 32/64-bit words, no quantization, no rate control. Estimated 1.5-2.5:1 on structured pages. Needs implementation and benchmarking.

---

## Sparse Activation & Tiered VRAM Strategy (from research/017)

MoE models have 70-90% of weights cold at any given time. Layer-sequential access in dense models enables perfect prefetching. No existing offloading system uses compression.

**Tiered VRAM strategy:**

| Tier | Location | Content | Latency |
|---|---|---|---|
| Hot | VRAM, uncompressed | Active expert(s) + current layer + KV cache | 0 (immediate) |
| Warm | VRAM, compressed | Recently-used experts, prefetched layers | ~5μs (GPU decompress) |
| Cold | RAM, compressed | Inactive experts, previous layers | ~15-35μs (PCIe + decompress) |
| Frozen | NVMe, compressed | Rarely-used experts, model backup | ~100μs+ (SSD I/O + decompress) |

**Transition rules:** Expert not used for K tokens → demote one tier. Expert needed by router → promote to hot (decompress). K should be tuned per model and workload.

**Compression algorithm per tier:**

| Tier | Best Algorithm | Why |
|---|---|---|
| Hot | None (uncompressed) | Zero-latency access |
| Warm | ai_fp16 (BYTE_STREAM_SPLIT) or LZ4 | Fast decompress (~5μs), decent ratio |
| Cold | Zstd dict | Best ratio (2.12:1 on FP16), acceptable decompress (~7.75μs) |
| Frozen | Zstd max | Best ratio, latency irrelevant (I/O dominates) |

---

## Parallel Decompression (from research/016)

When swap readahead brings in a 32-page cluster, MiniMem can decompress all pages in parallel:

| Method | 32-page cluster latency | Speedup vs serial |
|---|---|---|
| Serial (current Linux) | 64-160 μs | 1× |
| 8-way parallel (CPU workqueue) | 10-22 μs | 4.5-7× |
| GPU nvCOMP batch | 15-25 μs | 4-8× (VRAM only) |

**Safety model:** RCU for map lookups, atomic refcounts for buffer lifetime, `PG_locked` for page handoff, `mmu_gather` for batched PTE updates. All kernel-standard primitives.

**VRAM parallel decompression:** Use nvCOMP `BatchedDecompressAsync()` for VRAM-resident compressed buffers. Zero CPU involvement — all on GPU.

**GPU decompression is NOT viable for CPU page faults** (PCIe round-trip 15-35μs exceeds CPU decompress time 2-5μs). Only viable for data already on GPU.