# MiniMem — Feature Registry

**Single source of truth for all features.** Update this file whenever a feature is added, changed, or planned.

Status legend: ✅ Complete · 🔧 In Progress · 📋 Planned · ❌ Removed / Deferred

---

## Stage 0 — Algorithm Library & Benchmarks

| Feature | Status | Notes |
|---|---|---|
| Benchmark framework (Criterion + page/tensor harness) | 📋 Planned | |
| LZ4 integration | 📋 Planned | Reference C + SIMD; industry standard for speed |
| LZSSE8 integration | 📋 Planned | SSE4.1 SIMD; fastest software decompression; x86-64 only |
| WKdm implementation | 📋 Planned | Word-oriented memory-page compressor; exploits pointer/integer structure |
| BDI implementation | 📋 Planned | Base-Delta-Immediate; cache-line granularity; trivial decode |
| Zstd (dictionary-trained) | 📋 Planned | Best ratio among fast algorithms; dictionary mode for homogeneous pages |
| Delta encoding primitives | 📋 Planned | XOR delta between similar pages; near-zero decode for COW/incremental |
| Same-page detection | 📋 Planned | Zero-fill and repeated-value pages; zero allocation |
| Algorithm comparison benchmark suite | 📋 Planned | Throughput, latency, ratio on page/tensor data |
| Benchmark report format | 📋 Planned | CSV/JSON output to reports/ |

---

## Stage 1 — Linux Kernel In-Memory Compression Module

| Feature | Status | Notes |
|---|---|---|
| Kernel module scaffold | 📋 Planned | Loadable module, sysfs stats, clean rmmod |
| Page idle tracking | 📋 Planned | PG_idle/PG_young or soft-dirty bit hook |
| Compression map | 📋 Planned | VA → compressed handle (zsmalloc-backed) |
| PTE marking for compressed pages | 📋 Planned | Custom swp_entry; analogous to swap entries |
| Page fault handler (decompress path) | 📋 Planned | Intercept → decompress → remap → TLB flush |
| Compression policy engine | 📋 Planned | Idle time, access frequency, min savings threshold |
| Multi-algorithm dispatch | 📋 Planned | LZ4 (fast) for hot path, zstd (dense) for cold recompression |
| Sysfs stats interface | 📋 Planned | pages_compressed, bytes_saved, decompress_count, latency histogram |
| Concurrency (RCU-safe, per-CPU) | 📋 Planned | No global locks on hot paths |
| kselftest suite | 📋 Planned | Load/unload, round-trip, stress, stats |
| Kernel compatibility | 📋 Planned | Latest stable + latest LTS |

---

## Stage 2 — VRAM Transparent Compression

| Feature | Status | Notes |
|---|---|---|
| VRAM buffer tracking | 📋 Planned | Classify by type: weights, activations, textures, framebuffers |
| VRAM compression map | 📋 Planned | GPU-side buffer ID → compressed region |
| Decompression on GPU access | 📋 Planned | GPU page faults or command-stream barriers |
| AI workload compressor | 📋 Planned | Specialized for quantized weight tensors |
| nvCOMP integration | 📋 Planned | Evaluate GPU-parallel bulk compression |
| Driver integration points | 📋 Planned | AMDGPU, Mesa, Vulkan memory allocator |
| Userspace API | 📋 Planned | ioctl/sysfs for compression policy advice |
| VRAM test harness | 📋 Planned | Mock buffers for correctness, real GPU for throughput |

---

## Stage 3 — Hardware Acceleration

| Feature | Status | Notes |
|---|---|---|
| Intel QAT LZ4 backend | 📋 Planned | Offload to QAT on Xeon platforms |
| CXL inline compression | 📋 Planned | Marvell Structera-style path |
| IBM NX-842 backend | 📋 Planned | PowerPC hardware compression |
| SIMD runtime dispatch | 📋 Planned | CPUID-based AVX2/AVX-512/NEON selection |
| FPGA evaluation | 📋 Planned | Xilinx Vitis LZ4/DEFLATE assessment |
| Hardware availability detection | 📋 Planned | Graceful fallback to software |
| HW vs SW benchmark | 📋 Planned | Throughput and latency per backend |

---

## Stage 4 — Specialized Compressors

| Feature | Status | Notes |
|---|---|---|
| AI weight compressor | 📋 Planned | Exploit quantization patterns, sparse structure |
| Page-table-aware compressor | 📋 Planned | Exploit pointer alignment, upper-byte zeros |
| Delta-streaming compressor | 📋 Planned | XOR deltas for COW/incremental pages |
| Dictionary compressor (pre-trained) | 📋 Planned | Homogeneous workload dictionaries |
| Compression advisor (auto-selector) | 📋 Planned | Runtime page classifier → algorithm selection |
| Domain-specific benchmarks | 📋 Planned | LLM weights, page tables, fork-COW pages |