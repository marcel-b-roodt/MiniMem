# MiniMem — Feature Registry

**Single source of truth for all features.** Update this file whenever a feature is added, changed, or planned.

Status legend: ✅ Complete · 🔧 In Progress · 📋 Planned · ❌ Removed / Deferred

---

## Stage 0 — Algorithm Library & Benchmarks

| Feature | Status | Notes |
|---|---|---|
| Benchmark framework (Criterion + page/tensor harness) | ✅ Complete | Criterion integrated; 11 synthetic page types; JSON/CSV output |
| LZ4 integration | ✅ Complete | Reference C v1.10.0 vendored; roundtrip tests passing; benchmarks published |
| LZSSE8 integration | 📋 Planned | SSE4.1 SIMD; fastest software decompression; x86-64 only |
| WKdm implementation | ✅ Complete | Scalar 32-bit implementation; scratch buffer API; zero-allocation compress/decompress; 2.08:1 on pointer-heavy |
| WKdm-64 (8-byte word) | ✅ Complete | Scalar 64-bit implementation; 29:1 on zero pages; 2x faster decompress than WKdm32; 1.5:1 on pointer-heavy |
| BDI implementation | ✅ Complete | Cache-line compressor; zero/uniform/small-delta modes; tests passing |
| Zstd (dictionary) | ✅ Complete | System zstd 1.5.7 linked; compression level 1; roundtrip tests passing |
| Delta encoding primitives | ✅ Complete | XOR delta + recovery implemented; roundtrip tests passing |
| Same-page detection | ✅ Complete | Zero-fill and repeated-value pages; zero allocation; 819:1 on zero pages |
| Block classifier | ✅ Complete | 64-byte block classification; ZERO/SPARSE/UNIFORM/SMALL_RANGE/DENSE encoding; 146:1 on zero pages; 2.42:1 on integer-heavy |
| Compression advisor | ✅ Complete | Page content classifier → algorithm selector; heuristic-based decision tree |
| Algorithm comparison benchmark suite | ✅ Complete | 7 algorithms x 11 page types; throughput, latency, ratio measured |
| Benchmark report format | ✅ Complete | CSV/JSON output to reports/; Stage 0 results published with 7 algorithms |

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
| AI weight compressor | 🔧 In Progress | Block classifier handles INT8/FP16; needs value-level encoding for AI-specific data shapes |
| Page-table-aware compressor | 📋 Planned | Exploit pointer alignment, upper-byte zeros |
| Delta-streaming compressor | 📋 Planned | XOR deltas for COW/incremental pages |
| Dictionary compressor (pre-trained) | 📋 Planned | Homogeneous workload dictionaries |
| Compression advisor (auto-selector) | ✅ Complete | Heuristic-based decision tree; picks best algorithm per page type |
| Domain-specific benchmarks | 📋 Planned | LLM weights, page tables, fork-COW pages |