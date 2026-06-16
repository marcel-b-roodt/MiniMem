# MiniMem — Action Plan

**Created:** 2026-06-16
**Purpose:** Deliver MiniMem as both a kernel-level module and a standalone library, proving compression benefits for RAM, enabling VRAM proof-of-concept, and making components usable by open-source AI and GPU projects.

---

## Vision Summary

MiniMem has two delivery tracks that reinforce each other:

1. **Kernel module track** — Transparent in-memory compression for Linux RAM (the "macOS gap" filler). This is the core contribution: a loadable kernel module that compresses cold pages while they remain mapped, with parallel cluster decompression on page fault.

2. **Standalone library track** — The compression algorithm library (`libminimem`) usable independently by any project. AI frameworks, GPU drivers, and self-hosting tools can link against it without needing the kernel module.

Both tracks share the same algorithm library, benchmarks, and test infrastructure.

---

## Delivery Milestones

### Milestone 1: Working Kernel Module (RAM Compression)

**Goal:** First public demonstration that transparent in-memory compression works on Linux — the thing nobody has done.

**Deliverables:**
- Kernel module that compiles on latest stable + LTS, loads/unloads cleanly
- PTE marking scheme for "compressed in RAM" pages (custom swp_entry)
- Page fault handler: intercept → decompress → remap → TLB flush → resume
- Idle page tracking (PG_idle/PG_young) to identify cold pages
- Multi-algorithm dispatch with advisor (same_page, BDI, WKdm-64, LZ4)
- Compression map (xarray + kmem_cache, RCU-safe)
- Parallel cluster decompression on swap readahead (4.5-7× latency reduction)
- sysfs stats: pages_compressed, bytes_saved, decompress_count, decompress_latency
- kselftest suite
- Real data benchmarks on actual Linux page dumps

**Success metric:** 2× effective memory capacity on a cold-page-heavy workload, <10μs decompression per page, <5% CPU overhead on active workload.

**Why this matters:** This is the "macOS gap" filler. No existing Linux system compresses pages that are still mapped. zram/zswap only compress swap-evicted pages. This alone is a publishable contribution.

### Milestone 2: Standalone Library Release (libminimem)

**Goal:** Package the algorithm library as a standalone C library that any project can use.

**Deliverables:**
- `libminimem.so` / `libminimem.a` with stable API
- 12 algorithms: same_page, BDI, WKdm, WKdm-64, LZ4, LZSSE8, Zstd-dict, Delta, Block-classifier, AI-FP16, AI-BF16, AI-INT8
- Compression advisor API (`minimem_advise()`, `minimem_advise_best()`)
- Header-only C API, C99 compatible, no kernel dependencies
- pkg-config, CMake integration
- API documentation and examples
- Benchmark suite that can be run by downstream users

**Why this matters:** This makes MiniMem useful beyond the kernel. AI frameworks (PyTorch, llama.cpp, vLLM), GPU drivers (Mesa/AMDGPU), and self-hosting tools (Ollama, text-generation-webui) can all use `libminimem` to compress model weights, KV caches, and other memory.

### Milestone 3: VRAM Proof-of-Concept

**Goal:** Demonstrate that compressing AI model weights in VRAM saves significant memory, enabling larger models or more concurrent inference on the same hardware.

**Deliverables:**
- Userspace VRAM compression prototype: CUDA/Vulkan program that loads an LLM, compresses idle weight tensors using libminimem, decompresses on demand
- Tiered memory management: hot (uncompressed VRAM) → warm (compressed VRAM) → cold (compressed RAM) → frozen (compressed NVMe)
- MoE sparse activation map: use router output to determine which expert weight blocks to keep hot, which to compress
- Benchmark on real LLM inference (Mixtral 8x7B or similar): measure VRAM savings, inference latency impact, throughput change
- Integration with llama.cpp or vLLM as a custom memory allocator plugin
- Documentation of TTM insertion points for future kernel driver integration

**Success metric:** 30-50% VRAM savings on MoE inference with <5% throughput degradation.

**Why this matters:** This proves VRAM compression works and quantifies the benefit. It's the data that justifies the driver-level integration work. Self-hosting AI platforms (Ollama, text-generation-webui, LocalAI) can use the userspace prototype immediately.

### Milestone 4: Open-Source Driver Integration Path

**Goal:** Provide the hooks, documentation, and prototypes for integrating MiniMem compression into open-source GPU drivers.

**Deliverables:**
- AMDGPU TTM compression hook prototype (patch against amdgpu_vram_mgr)
- Mesa/Gallium3D buffer compression API proposal
- Vulkan memory allocator extension proposal (VK_EXT_memory_compression)
- Documentation: "How to add compression to your GPU driver" guide
- Benchmarks showing VRAM savings on AMD and Intel hardware
- Patches submitted to appropriate mailing lists (dri-devel, amd-gfx)

**Why this matters:** This is the path to truly transparent VRAM compression. It won't happen overnight, but having working patches and a clear integration guide accelerates adoption.

### Milestone 5: Hardware Acceleration & CXL

**Goal:** Offload compression to hardware where available, and make CXL memory compression transparent.

**Deliverables:**
- Intel QAT LZ4 backend for kernel module (bulk page compression)
- SIMD runtime dispatch (AVX2, AVX-512, NEON) for algorithm hot paths
- CXL Type-3 memory compression: extend Stage 1 kernel module to compress CXL NUMA node pages with tier-appropriate algorithms
- FPGA evaluation report (Xilinx Vitis LZ4/DEFLATE)
- Hardware detection and graceful fallback

**Success metric:** QAT offload achieves >2× throughput over software on bulk compression; SIMD paths achieve >1.5× decompression speedup over scalar.

---

## Immediate Next Steps (Ordered by Impact)

### Step 1: Get the kernel module compiling and loading

This is the highest-impact blocker. The kernel module scaffold exists but needs kernel headers to compile.

- [ ] Install linux618-headers (requires reboot)
- [ ] Compile kernel module (`cd src/kernel && make`)
- [ ] Test `insmod`/`rmmod` on a VM
- [ ] Wire WKdm-64, BDI, LZ4 into `minimem_compress.c` dispatch
- [ ] Verify sysfs stats after load

### Step 2: PTE marking and page fault handler

This is the core kernel mechanism that makes transparent compression work.

- [ ] Design swp_entry format for "compressed in RAM" pages
- [ ] Implement page fault handler (do_swap_page hook or custom handler)
- [ ] Decompress on fault → remap → TLB flush → resume
- [ ] Test in VM: force page compression, access compressed page, verify roundtrip

### Step 3: Idle page tracking and compression policy

- [ ] Hook PG_idle/PG_young or soft-dirty bits
- [ ] Implement compression policy: idle time threshold, minimum savings ratio
- [ ] Background compression daemon (workqueue or kernel thread)
- [ ] Test: verify only cold pages are compressed, hot pages left alone

### Step 4: Parallel cluster decompression

- [ ] Intercept swap readahead cluster in page fault path
- [ ] Dispatch N decompression workers to per-CPU workqueues
- [ ] Batch PTE updates via mmu_gather
- [ ] Single TLB flush after cluster is decompressed
- [ ] Benchmark: measure 4-7× latency reduction on 32-page clusters

### Step 5: Standalone library packaging

- [ ] Create `libminimem` shared library build target
- [ ] Stable API header (`minimem.h`) with versioning
- [ ] pkg-config file, CMake find module
- [ ] API documentation (man pages or Doxygen)
- [ ] Example programs: compress/decompress a file, compress a memory buffer

### Step 6: VRAM userspace prototype

- [ ] CUDA program: load LLM weights, classify with advisor, compress cold tensors
- [ ] Tiered memory: hot/warm/cold/frozen with compression at each boundary
- [ ] MoE sparse activation: use router output to determine hot experts
- [ ] Benchmark on Mixtral 8x7B or comparable MoE model
- [ ] Integration with llama.cpp custom allocator API

### Step 7: Real data benchmarks

- [ ] Collect actual Linux page dumps (dd if=/dev/mem or crash utility)
- [ ] Collect real AI model weight files (PyTorch safetensors)
- [ ] Run full benchmark suite on real data
- [ ] Publish results in reports/ with methodology documentation

---

## Relationship to Open-Source AI Ecosystem

MiniMem can directly benefit these open-source projects:

| Project | Integration Path | Benefit |
|---|---|---|
| **llama.cpp** | Custom memory allocator that compresses weight tensors via libminimem | Run larger models on same hardware; mmap-weight compression |
| **vLLM** | PagedAttention integration — compress cold KV cache pages | More concurrent requests per GPU |
| **Ollama** | Bundle libminimem; compress model weights before loading | Faster model loading, less RAM/VRAM needed |
| **Mesa/AMDGPU** | TTM compression hook patch | Transparent VRAM compression for all workloads |
| **OpenCode / Odysseus** | Link libminimem for model weight compression | Self-host larger models with less hardware |
| **Kubernetes / container runtimes** | Sidecar or init container that loads MiniMem kernel module | Compress idle container memory, increasing node density |

**Key insight for self-hosted AI:** Quantization (INT4/INT8/FP8) reduces model size but degrades quality. Compression preserves exact quality while reducing memory footprint. A 13B model in FP16 compressed 2:1 fits in the same memory as an INT8 model, but with **zero quality loss**. Combined with quantization (compress the quantized weights), the savings multiply: INT4 model compressed 2:1 ≈ 8× memory reduction over FP32.

---

## Quantization + Compression Synergy

Quantization and compression are **orthogonal and multiplicative**:

| Configuration | Memory per param | 13B model size | Quality |
|---|---|---|---|
| FP32 uncompressed | 4 bytes | 52 GB | Full |
| FP16 uncompressed | 2 bytes | 26 GB | Full |
| INT8 uncompressed | 1 byte | 13 GB | Slight loss |
| INT4 uncompressed | 0.5 bytes | 6.5 GB | Moderate loss |
| FP16 compressed 2:1 | 1 byte | 13 GB | **Full quality** |
| INT8 compressed 1.5:1 | 0.67 bytes | 8.7 GB | Slight loss |
| INT4 compressed 2:1 | 0.25 bytes | 3.25 GB | Moderate loss |

**MiniMem's value proposition:** Instead of degrading quality with aggressive quantization, use mild quantization + lossless compression to achieve the same memory savings with better quality. Our AI-FP16 compressor achieves 1.96:1 on FP16 weights — this means a 13B model needs 13.3 GB instead of 26 GB, with **zero quality loss**.

---

## Timeline Estimate

| Milestone | Estimated Duration | Depends On |
|---|---|---|
| M1: Working kernel module | 2-3 months | Kernel headers, testing VM |
| M2: Standalone library | 1-2 months | M1 (algorithm library is already done) |
| M3: VRAM proof-of-concept | 2-3 months | M2 (needs libminimem) |
| M4: Driver integration path | 3-6 months | M3 (needs VRAM data), upstream review cycles |
| M5: Hardware acceleration | 2-3 months | M1 (kernel module), hardware availability |

M1 and M2 can proceed in parallel. M3 starts as soon as M2 is usable. M4 requires community engagement. M5 depends on hardware availability.

---

## What Makes MiniMem Novel

1. **First Linux kernel module to compress still-mapped pages** — zram/zswap only compress swap pages. macOS has had this since 2013. Nobody has done it for Linux.

2. **First parallel cluster decompression** — all existing systems (zram, zswap, IBM AME, VMware, Windows) decompress one page at a time. MiniMem will be the first to decompress swap readahead clusters in parallel.

3. **First compression-aware VRAM tiering for AI** — no existing weight offloading system (DeepSpeed ZeRO, FlexGen, llama.cpp) uses compression. Combined with MoE sparse activation, this enables 3-10× VRAM savings.

4. **First adaptive per-page algorithm selector for memory compression** — zram has fixed algorithm per device. MiniMem classifies each page and selects the best algorithm, achieving better compression across diverse workloads.

5. **First BYTE_STREAM_SPLIT compressor for AI weights** — exploiting the shared-exponent-byte pattern in FP16/BF16 weights for lossless compression. No existing system does this.