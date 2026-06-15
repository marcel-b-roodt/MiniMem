# MiniMem — VRAM Compression

Deep dive into transparent GPU memory compression. See [research/006-nvcomp-gpu-compression.md](research/006-nvcomp-gpu-compression.md) and [research/008-ai-workload-compression.md](research/008-ai-workload-compression.md) for supporting research.

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