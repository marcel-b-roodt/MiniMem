# MiniMem — Development Roadmap

Target: **Transparent, lossless memory compression for Linux RAM, GPU VRAM, and hardware-accelerated paths.**

Stages are ordered by dependency. Each stage produces components consumed by later stages. The algorithm library (Stage 0) is the foundation — everything else depends on it.

---

## Stage 0 — Algorithm Library & Benchmarks

The compression algorithms and benchmarking infrastructure that all later stages depend on.

| Item | Description |
|---|---|
| Benchmark framework | Criterion-based harness with page-dump and tensor-dump data loaders |
| LZ4 | Reference C implementation + SIMD paths. Industry standard for speed. |
| LZSSE8 | SSE4.1 SIMD-optimized LZ77. Fastest software decompression (~4.7 GB/s). x86-64 only. |
| WKdm | Word-oriented memory-page compressor (Wilson et al. 1999). Exploits pointer/integer structure in memory pages. |
| BDI | Base-Delta-Immediate cache-line compressor (Pekhimenko et al. 2012). 1-2 cycle decode in hardware, trivial in software. |
| Zstd (dictionary) | Dictionary-trained mode for homogeneous page content (kernel pages, weight blocks). Best ratio among fast algorithms. |
| Delta encoding | XOR delta between similar pages. Near-zero decode cost for fork-COW and incremental pages. |
| Same-page detection | Zero-fill and repeated-value pages identified with zero memory allocation (just a flag). |
| Comparison suite | All algorithms benchmarked on: random pages, zero-heavy, pointer-heavy, AI weight tensors |

---

## Stage 1 — Linux Kernel In-Memory Compression Module

The core deliverable: transparent compression of cold pages in a running Linux system.

| Item | Description |
|---|---|
| Module scaffold | Loadable kernel module, sysfs stats, clean rmmod |
| Page idle tracking | Hook into PG_idle/PG_young or soft-dirty bits to identify cold pages |
| Compression map | Virtual-address → compressed-handle lookup structure (zsmalloc-backed) |
| PTE marking | Custom swp_entry for "compressed in RAM" pages (analogous to swap entries) |
| Page fault handler | Intercept compressed-page faults → decompress → remap → TLB flush → resume |
| Compression policy | Thresholds: idle time, access frequency, minimum savings ratio |
| Multi-algorithm dispatch | Hot-path LZ4 (fast), cold-page recompression with zstd (dense) |
| Concurrency | RCU-safe lookups, per-CPU buffers, no global locks on hot paths |
| kselftest | Load/unload, round-trip, stress, stats validation |
| Compatibility | Compiles against latest stable + latest LTS |

**Feeds into:** Stage 2 (compression map pattern reused for VRAM), Stage 3 (HW accel offload for compression/decompression), Stage 4 (specialized algorithms plugged into multi-algorithm dispatch)

---

## Stage 2 — VRAM Transparent Compression

Apply compression to GPU memory, reducing VRAM usage for AI and games.

| Item | Description |
|---|---|
| VRAM buffer tracking | Track allocated GPU buffers, classify by type (weights, activations, textures, framebuffers) |
| VRAM compression map | GPU-side structure mapping buffer ID → compressed region |
| Decompression on access | Hook into GPU page faults or command-stream barriers |
| AI workload compressor | Specialized algorithm for quantized weight tensors (from Stage 4) |
| nvCOMP integration | Evaluate NVIDIA's GPU-parallel LZ4/zstd for bulk compression |
| Driver integration points | AMDGPU, Mesa, Vulkan memory allocator hooks |
| Userspace API | ioctl/sysfs for compression policy advice from applications |
| VRAM test harness | Mock VRAM buffers for correctness, real GPU for throughput |

**Depends on:** Stage 0 (algorithms), Stage 1 (compression map pattern, policy framework)
**Feeds into:** Stage 4 (AI workload data shapes VRAM compressor design)

---

## Stage 3 — Hardware Acceleration

Offload compression to specialized hardware when available.

| Item | Description |
|---|---|
| Intel QAT LZ4 | Offload bulk page compression to QAT hardware on Xeon platforms |
| CXL inline compression | Evaluate Marvell Structera-style inline compression for CXL memory |
| IBM NX-842 | PowerPC hardware compression where available |
| SIMD dispatch | Runtime CPUID-based selection of AVX2/AVX-512/NEON decompression paths |
| FPGA evaluation | Assess Xilinx Vitis LZ4/DEFLATE for line-rate compression |
| HW detection | Graceful detection and fallback to software when HW absent |
| Benchmark: HW vs SW | Throughput and latency comparison for each backend |

**Depends on:** Stage 0 (algorithms with HW-accelerated variants), Stage 1 (kernel module for integration points)

---

## Stage 4 — Specialized Compressors

Domain-optimized algorithms that achieve higher compression or speed than general-purpose approaches on specific memory content types.

| Item | Description |
|---|---|
| AI weight compressor | Exploit quantization patterns, sparse structure, repeated blocks in model weights |
| Page-table-aware compressor | Exploit pointer alignment, upper-byte zeros in 64-bit entries |
| Delta-streaming compressor | XOR deltas between similar pages (fork COW, consecutive weight rows) |
| Dictionary compressor | Pre-trained dictionaries for homogeneous workloads (kernel pages, AI weight blocks) |
| Compression advisor | Runtime page content classifier → automatic algorithm selection |
| Domain benchmarks | AI model weights (LLM layers), page tables, fork-COW pages |

**Depends on:** Stage 0 (all algorithms available), Stage 1 (kernel module for deployment), Stage 2 (VRAM layer for AI compressor deployment)
**Feeds back into:** Stage 1 (better algorithms for kernel module), Stage 2 (AI compressor for VRAM)

---

## Cross-stage dependencies

```
Stage 0 (Algorithms)
  ├──→ Stage 1 (Kernel module) ──→ Stage 2 (VRAM)
  │         │                           │
  │         ├──→ Stage 3 (HW accel)     │
  │         │                           │
  └─────────┴──→ Stage 4 (Specialized) ─┘
                    ↑ feeds back into 1 and 2
```

Stage 0 is the foundation. Stages 1-3 can progress in parallel once Stage 0 is solid. Stage 4 feeds back into Stages 1 and 2, improving their algorithms over time.