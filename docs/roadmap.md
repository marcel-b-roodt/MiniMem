# MiniMem — Development Roadmap

Target: **Transparent, lossless memory compression for Linux RAM, GPU VRAM, and hardware-accelerated paths.**

See [action-plan.md](action-plan.md) for the detailed delivery plan with milestones and timeline.

Stages are ordered by dependency. Each stage produces components consumed by later stages. The algorithm library (Stage 0) is the foundation — everything else depends on it.

---

## Stage 0 — Algorithm Library & Benchmarks ✅ Complete

| Feature | Status | Notes |
|---|---|---|
| Benchmark framework | ✅ Complete | Criterion harness; 11 synthetic page types; JSON/CSV output |
| LZ4 | ✅ Complete | v1.10.0 vendored; roundtrip tests; 2.5μs decompress (O2) |
| LZSSE8 | ✅ Complete | SSE4.1 SIMD LZ77; ~4.7 GB/s decompress; runtime detection; BSD-3-Clause vendored |
| WKdm | ✅ Complete | 32-bit word-oriented; 2.08:1 on pointer-heavy |
| WKdm-64 | ✅ Complete | 64-bit variant; 29:1 on zero; 2× faster decompress |
| BDI | ✅ Complete | Base-Delta-Immediate; 60:1 on zero; 0.17μs decompress |
| Zstd (dict) | ✅ Complete | Level 1; 4.64:1 on PTE; best ratio, slowest decompress |
| Delta XOR | ✅ Complete | XOR delta/recovery primitives; roundtrip verified |
| Same-page | ✅ Complete | 819:1 on zero; 0.07μs decompress |
| Block classifier | ✅ Complete | 5-type classification; 3-bit packed headers; 146:1 on zero |
| AI-FP16/BF16 | ✅ Complete | BYTE_STREAM_SPLIT; 1.96:1 on FP16; 5.2μs decompress |
| AI-INT8 | ✅ Complete | Row-delta XOR; 44.5:1 on uniform INT8; 1.6μs decompress |
| Advisor | ✅ Complete | Heuristic decision tree; best-algorithm selector |
| Real data benchmarks | 📋 Planned | Need actual page dumps and safetensor weights |

---

## Stage 1 — Linux Kernel In-Memory Compression Module ✅ Complete

The core deliverable: transparent compression of cold pages in a running Linux system.

| Feature | Status | Notes |
|---|---|---|
| Module scaffold | ✅ Complete | Loadable module, sysfs stats (28 attributes), debugfs test interface |
| Compression map | ✅ Complete | xarray + kmem_cache; RCU-safe lookups |
| Zsmalloc storage | ✅ Complete | zsmalloc pool for compressed page data; 6.x kernel API (zs_obj_write/read) |
| Compression dispatch | ✅ Complete | Per-CPU buffers; 7 algorithms (same_page, BDI, WKdm, WKdm-64, block_class, LZ4, delta) |
| Parallel cluster decompression | ✅ Complete | Workqueue (minimem_dec); atomic completion; 32-page cluster support; 3.76× speedup |
| Debugfs benchmark | ✅ Complete | baseline/serial/parallel modes via /sys/kernel/debug/minimem/bench |
| Compile and load module | ✅ Complete | minimem.ko v0.6.0 for 6.18.33-1-MANJARO; depends on lz4_compress |
| PTE marking | ✅ Complete | PTE_MARKER_MINIMEM=BIT(3) in SWP_PTE_MARKER; 54-bit index; debugfs roundtrip; compress_and_replace_pte via kallsyms |
| Page fault handler | ✅ Complete | kprobe on do_swap_page OR registered VM_FAULT_NOPAGE handler (patched kernels); 4/4 E2E pages verified |
| Idle page tracking | ✅ Complete | VMA-based mark-sweep scanner; sweep enabled on patched kernels |
| Memory pressure shrinker | ✅ Complete | shrinker_alloc/register/free; drains compressed entries |
| Compression policy sysfs | ✅ Complete | scanner_enabled, scanner_interval_ms, min_savings_pct, max_pool_pages |
| Pool size limit | ✅ Complete | max_pool_pages sysfs knob (0=unlimited) |
| DKMS packaging | ✅ Complete | dkms.conf, Makefile, install/uninstall scripts, auto-rebuild on kernel update |
| Kernel patch detection | ✅ Complete | Runtime kallsyms check; kernel_patches sysfs; automatic mode selection |
| AUR packaging | ✅ Complete | PKGBUILD + .SRCINFO for minimem and minimem-dkms |
| kselftest | ✅ Complete | 41 tests in QEMU VM |
| E2E test | ✅ Complete | 4/4 transparent compression + fault decompression verified |
| **Remaining** | | |
| Full scanner E2E on patched kernel | 📋 Planned | Needs custom kernel build; runtime detection ready |
| Shrinker verification under pressure | 📋 Planned | Needs memory stress test |
| Real data benchmarks | 📋 Planned | Actual page dumps + AI model weight tensors |

---

## Stage 2 — VRAM Transparent Compression 📋 Planned

Apply compression to GPU memory, reducing VRAM usage for AI inference and games.

| Feature | Status | Notes |
|---|---|---|
| Userspace VRAM prototype | 📋 Planned | CUDA/Vulkan program: compress idle weight tensors with libminimem |
| Tiered VRAM management | 📋 Planned | Hot (uncompressed) → Warm (compressed VRAM) → Cold (compressed RAM) → Frozen (compressed NVMe) |
| MoE sparse activation map | 📋 Planned | Use router output to determine which expert blocks to keep hot |
| AI weight compressor (VRAM) | 📋 Planned | BYTE_STREAM_SPLIT + Zstd for tiered compression; fast decompress for warm tier |
| nvCOMP integration | 📋 Planned | GPU-parallel batch decompression for VRAM-resident data |
| TTM driver hook | 📋 Planned | Prototype patch against amdgpu_vram_mgr |
| Vulkan memory allocator extension | 📋 Planned | VK_EXT_memory_compression proposal |
| llama.cpp integration | 📋 Planned | Custom allocator plugin using libminimem |
| VRAM benchmarks | 📋 Planned | Real LLM inference: VRAM savings, latency impact, throughput |

**Key research findings (research/014, /017):**
- mm/ has zero VRAM visibility; TTM manages VRAM entirely
- CXL Type-3 memory IS visible to mm/ — works today
- MoE models have 70-90% cold weights — massive compression opportunity
- No existing offloading system uses compression

---

## Stage 3 — Hardware Acceleration 📋 Planned

Offload compression to specialized hardware when available.

| Feature | Status | Notes |
|---|---|---|
| Intel QAT LZ4 | 📋 Planned | Bulk page compression offload; wins on throughput, not latency |
| CXL inline compression | 📋 Planned | CXL Type-3 works with Stage 1 module directly; no CXL compression standard exists yet |
| SIMD runtime dispatch | 📋 Planned | AVX2/AVX-512/NEON for LZ4, WKdm, BDI hot paths |
| FPGA evaluation | 📋 Planned | Xilinx Vitis LZ4/DEFLATE assessment |
| IBM NX-842 | 📋 Planned | PowerPC hardware compression |
| HW detection + fallback | 📋 Planned | Graceful fallback to software |

**Key finding (research/016):** QAT wins on throughput (~12.5 GB/s) but per-request latency (10-20μs) exceeds CPU decompress (2-5μs). Only suitable for bulk background compression, not page fault path.

---

## Stage 4 — Specialized Compressors 🔧 In Progress

Domain-optimized algorithms achieving higher compression or speed than general-purpose approaches.

| Feature | Status | Notes |
|---|---|---|
| AI weight compressor | ✅ Complete | BYTE_STREAM_SPLIT (FP16/BF16) + row-delta XOR (INT8); all <10μs decompress |
| Page-table-aware | 📋 Planned | Exploit PTE structure; delta-encode PFNs; expected 4-10:1 |
| Delta-streaming | 📋 Planned | XOR deltas for COW/incremental pages |
| Dictionary (pre-trained) | 📋 Planned | Zstd dictionary for homogeneous workloads |
| DSC-Lite predictor | 📋 Planned | MMAP + ICH for 32/64-bit words; estimated 1.5-2.5:1 on structured pages |
| Compression advisor | ✅ Complete | Heuristic decision tree; picks best algorithm per page |
| Domain benchmarks | 📋 Planned | Real LLM weights, page tables, fork-COW pages |

---

## Cross-stage dependencies

```
Stage 0 (Algorithms) ✅
  ├──→ Stage 1 (Kernel module) ✅ ──→ Stage 2 (VRAM) 📋
  │         │                                │
  │         ├──→ Stage 3 (HW accel) 📋       │
  │         │                                │
  └─────────┴──→ Stage 4 (Specialized) 🔧 ──┘
                      ↑ feeds back into 1 and 2
```

Stage 0 and Stage 1 are complete. Stage 2 can begin userspace prototyping using libminimem. Stage 4 feeds improved algorithms back into Stages 1 and 2.