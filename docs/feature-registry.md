# MiniMem — Feature Registry

**Single source of truth for all features.** Update this file whenever a feature is added, changed, or planned.

Status legend: ✅ Complete · 🔧 In Progress · 📋 Planned · ❌ Removed / Deferred

---

## Stage 0 — Algorithm Library & Benchmarks

| Feature | Status | Notes |
|---|---|---|
| Benchmark framework (Criterion + page/tensor harness) | ✅ Complete | Criterion integrated; 11 synthetic page types; JSON/CSV output |
| LZ4 integration | ✅ Complete | Reference C v1.10.0 vendored; roundtrip tests passing; benchmarks published |
| LZSSE8 integration | ✅ Complete | SSE4.1 SIMD LZ77; ~4.7 GB/s decompress; runtime feature detection; BSD-3-Clause vendored; falls back to LZ4 on non-x86 |
| WKdm implementation | ✅ Complete | Scalar 32-bit implementation; scratch buffer API; zero-allocation compress/decompress; 2.08:1 on pointer-heavy |
| WKdm-64 (8-byte word) | ✅ Complete | Scalar 64-bit implementation; 29:1 on zero pages; 2x faster decompress than WKdm32; 1.5:1 on pointer-heavy |
| BDI implementation | ✅ Complete | Cache-line compressor; zero/uniform/small-delta modes; tests passing |
| Zstd (dictionary) | ✅ Complete | System zstd 1.5.7 linked; compression level 1; roundtrip tests passing |
| Delta encoding primitives | ✅ Complete | XOR delta + recovery implemented; roundtrip tests passing |
| Same-page detection | ✅ Complete | Zero-fill and repeated-value pages; zero allocation; 819:1 on zero pages |
| Block classifier | ✅ Complete | 64-byte block classification; ZERO/SPARSE/UNIFORM/SMALL_RANGE/DENSE encoding; 146:1 on zero pages; 2.42:1 on integer-heavy |
| Compression advisor | ✅ Complete | Page content classifier → algorithm selector; heuristic-based decision tree |
| AI weight compressor (FP16/BF16) | ✅ Complete | BYTE_STREAM_SPLIT: separates high/low bytes then block-classifies each stream; 1.96:1 on FP16 weights, 5.2μs decompress |
| AI weight compressor (INT8) | ✅ Complete | Row-delta XOR + block classification; 44.5:1 on uniform INT8, 1.25:1 on integer-heavy, 1.57μs decompress |
| Algorithm comparison benchmark suite | ✅ Complete | 10 algorithms x 11 page types; throughput, latency, ratio measured |
| Benchmark report format | ✅ Complete | CSV/JSON output to reports/; Stage 0 results published with 7 algorithms |
| Userspace library (libminimem) | ✅ Complete | Shared and static library; pkg-config; CMake find module; public headers (minimem.h, advisor.h, test_data.h); two example programs; soname versioning |
| kselftest (kernel module) | ✅ Complete | 41 tests: module load/unload, sysfs attributes, debugfs, PTE marker roundtrip, compression roundtrip, benchmark, sysfs knobs (max_pool_pages, min_savings_pct, scanner), runs in QEMU VM |

---

## Stage 1 — Linux Kernel In-Memory Compression Module

| Feature | Status | Notes |
|---|---|---|
| Kernel module scaffold | ✅ Complete | Loadable module, sysfs stats (17 attributes), debugfs benchmark interface |
| Zsmalloc storage backend | ✅ Complete | zsmalloc pool for compressed pages; zs_obj_write/read API (6.x kernel) |
| Compression map | ✅ Complete | xarray-based VA → entry map; kmem_cache-backed entries; RCU-safe lookups |
| Compression dispatch | ✅ Complete | Per-CPU buffer compression dispatch; same-page, BDI, WKdm, WKdm-64, block_class, LZ4, delta |
| Debugfs test interface | ✅ Complete | /sys/kernel/debug/minimem/{bench,compress,stats}; baseline/serial/parallel benchmark modes |
| Parallel cluster decompression | ✅ Complete | Workqueue-based (minimem_dec); atomic completion; 32-page clusters; **3.76× speedup on 4 vCPUs** |
| QEMU VM test harness | ✅ Complete | vm-test-minimem.sh; Alpine rootfs; safe bare-metal isolation; 7-phase progressive testing |
| Multi-algorithm dispatch | ✅ Complete | 7 algorithms wired into kernel dispatch; same_page, BDI, WKdm, WKdm-64, block_class, LZ4, delta |
| Sysfs stats interface | ✅ Complete | 28 attributes: compress/decompress counts, latencies, zswap pages/bytes/saved, pool_pages, max_pool_pages, benchmark timings, scanner knobs, hook_faults, kernel_patches, parallel stats, scanner_pages_compressed/skipped |
| Memory overhead tracking | ✅ Complete | pool_pages sysfs attribute; zswap_pages/bytes/saved for compression efficiency |
| Concurrency (RCU-safe, per-CPU) | ✅ Complete | RCU-safe xarray lookups; spinlock-protected writes; per-CPU compress/decompress buffers |
| PTE marking for compressed pages | ✅ Complete | PTE_MARKER_MINIMEM=BIT(3) in SWP_PTE_MARKER offset; 54-bit index space; debugfs roundtrip test; compress_and_replace_pte implemented via resolved kallsyms symbols; MM_ANONPAGES RSS counter maintained |
| Page fault handler (decompress path) | ✅ Complete | kprobe on do_swap_page; __pte_offset_map_lock and pte_mkwrite resolved via kallsyms; set_pte_at implemented as WRITE_ONCE (x86-64); installs present PTE after decompression; MM_ANONPAGES RSS counter incremented; **4/4 pages decompressed correctly in E2E VM test** |
| PTE replacement (compress path) | ✅ Complete | minimem_compress_and_replace_pte() walks pgd→p4d→pud→pmd→pte; compresses page; stores in zsmalloc; replaces PTE with MiniMem swap entry; enforces min_savings_pct; MM_ANONPAGES RSS counter decremented; **compress_vaddr debugfs interface for E2E testing** |
| E2E test suite | ✅ Complete | minimem_e2e_test.sh: hook resolution, compression roundtrip, min_savings_pct/max_pool_pages enforcement, scanner; test_transparent_e2e: 4 pages compressed + PTE replaced + fault-decompressed with byte-exact verification in QEMU VM |
| Idle page tracking | ✅ Complete | minimem_scanner daemon; CONFIG_PAGE_IDLE_FLAG; PFN batch scanning; VMA-based mark-sweep scanning; mark pass sets idle+clears young on anonymous pages; sweep pass compresses cold pages; **sweep pass enabled on patched kernels (via minimem_hook_marker_ready), disabled on unpatched kernels** |
| Memory pressure shrinker | ✅ Complete | shrinker_alloc/register/free API; drains compressed entries via minimem_map_drain(); frees zsmalloc memory under pressure |
| Pool size limit | ✅ Complete | max_pool_pages sysfs knob (0=unlimited); compress_and_store returns -ENOSPC when limit reached |
| Sysfs knobs (scanner + policy) | ✅ Complete | scanner_enabled, scanner_interval_ms, min_savings_pct, max_pool_pages, scanner_pages_scanned/idle/compressed/skipped |
| Userspace test driver | ✅ Complete | test_minimem_kernel: same_page, advisor, roundtrip, map, latency budget |
| kselftest suite | ✅ Complete | 42 tests: module load/unload, sysfs attributes (incl. kernel_patches), debugfs, PTE marker roundtrip, compression roundtrip, benchmark, sysfs knobs |
| Stress tests (VM) | ✅ Complete | Concurrent fault stress, memory pressure + shrinker, module unload safety — all pass in QEMU VM |
| Kernel compatibility | ✅ Complete | Built for 6.18.33-1-MANJARO; zsmalloc 6.x API |
| DKMS packaging | ✅ Complete | dkms.conf, DKMS Makefile, install/uninstall scripts, convenience scripts; auto-rebuild per kernel update; kernel patch application support |
| Kernel patch detection | ✅ Complete | Runtime detection of minimem_register_fault_handler symbol; registers VM_FAULT_NOPAGE handler when patches present; falls back to kprobe on unpatched kernels; kernel_patches sysfs attribute |
| Scanner sweep (patched kernels) | ✅ Complete | minimem_hook_marker_ready() returns true when kernel patches detected; sweep pass compresses cold pages; handle_pte_marker returns VM_FAULT_NOPAGE via registered handler |
| CI/CD (GitHub Actions) | ✅ Complete | 3 workflows: library build+test (gcc 12/14, Criterion), kernel module build (Ubuntu 22.04/24.04), packaging verification (DKMS+Debian+Fedora+AUR); Dependabot for Actions |

---

## Stage 2 — VRAM Transparent Compression

| Feature | Status | Notes |
|---|---|---|
| VRAM buffer tracking | 📋 Planned | Classify by type: weights, activations, textures, framebuffers |
| VRAM compression map | 📋 Planned | GPU-side buffer ID → compressed region |
| Decompression on GPU access | 📋 Planned | GPU page faults or command-stream barriers |
| AI workload compressor | 🔧 In Progress | FP16/BF16 BYTE_STREAM_SPLIT and INT8 row-delta implemented in lib; needs GPU dispatch integration |
| nvCOMP integration | 📋 Planned | Evaluate GPU-parallel bulk compression |
| Driver integration points | 🔧 In Progress | TTM `amdgpu_vram_mgr` is primary insertion point; mm/ has zero VRAM visibility; CXL Type-3 works today; see research/014 |
| Userspace API | 📋 Planned | ioctl/sysfs for compression policy advice |
| VRAM test harness | 📋 Planned | Mock buffers for correctness, real GPU for throughput |
| Sparse activation tiering | 📋 Planned | MoE expert routing → tiered VRAM/RAM/NVMe with compression; no existing system uses compression for offloading; see research/017 |
| DSC-Lite lossless predictor | 📋 Planned | MMAP + ICH for 32/64-bit words, no quantization; estimated 1.5-2.5:1; see research/015 |
| Parallel cluster decompression | 📋 Planned | 4.5-7× latency reduction on 32-page clusters; swap readahead provides clustering; see research/016 |

---

## Stage 3 — Hardware Acceleration

| Feature | Status | Notes |
|---|---|---|
| Intel QAT LZ4 backend | 📋 Planned | Offload to QAT on Xeon platforms |
| CXL inline compression | 📋 Planned | CXL Type-3 memory IS visible to mm/ — can use Stage 1 kernel module directly; no new infrastructure needed |
| IBM NX-842 backend | 📋 Planned | PowerPC hardware compression |
| SIMD runtime dispatch | 📋 Planned | CPUID-based AVX2/AVX-512/NEON selection |
| FPGA evaluation | 📋 Planned | Xilinx Vitis LZ4/DEFLATE assessment |
| Hardware availability detection | 📋 Planned | Graceful fallback to software |
| HW vs SW benchmark | 📋 Planned | Throughput and latency per backend |

---

## Stage 4 — Specialized Compressors

| Feature | Status | Notes |
|---|---|---|
| AI weight compressor | ✅ Complete | BYTE_STREAM_SPLIT (FP16/BF16) + row-delta XOR (INT8); 1.96:1 on FP16, 44.5:1 on uniform INT8; all under 10μs decompress |
| Page-table-aware compressor | 📋 Planned | Exploit pointer alignment, upper-byte zeros |
| Delta-streaming compressor | 📋 Planned | XOR deltas for COW/incremental pages |
| Dictionary compressor (pre-trained) | 📋 Planned | Homogeneous workload dictionaries |
| Compression advisor (auto-selector) | ✅ Complete | Heuristic-based decision tree; picks best algorithm per page type |
| DSC-Lite lossless predictor | 📋 Planned | MMAP prediction + ICH dictionary for 32/64-bit words; borrowable from DSC; see research/015 |
| Domain-specific benchmarks | 📋 Planned | LLM weights, page tables, fork-COW pages |