# MiniMem — Research Index

Numbered research documents capturing findings from papers, experiments, and investigations. Each doc is referenced from design docs (candidates, roadmap, architecture) — not duplicated.

---

## Naming convention

`NNNN-short-topic.md` — four-digit zero-padded number, hyphen, lowercase topic. Numbers are sequential.

---

## Index

| # | Topic | Key takeaway |
|---|---|---|
| [001](001-wkdm-memory-compression.md) | WKdm Memory Compression | Word-oriented algorithm for memory pages; exploits pointer/integer structure; used by macOS since 2013 |
| [002](002-bdi-cache-line-compression.md) | BDI Cache-Line Compression | Base-Delta-Immediate encoding; trivial decode; 1-2 cycles in hardware |
| [003](003-lz4-lzsse-benchmarks.md) | LZ4 & LZSSE8 Benchmarks | Fastest software decompressors; LZSSE8 at 4.7 GB/s via SIMD; comparison data |
| [004](004-zram-zswap-architecture.md) | zram & zswap Architecture | Linux kernel's existing compressed memory; only compresses swap pages, not active pages |
| [005](005-macos-memory-compression.md) | macOS Memory Compression | The system that proves in-memory compression works; WKdm since Mavericks 2013 |
| [006](006-nvcomp-gpu-compression.md) | nvCOMP GPU Compression | NVIDIA's GPU-parallel compression; >50 GB/s aggregate but high per-op latency |
| [007](007-delta-encoding-streaming.md) | Delta Encoding & Streaming | XOR deltas, predictive coding, sliding dictionaries from streaming and signal processing |
| [008](008-ai-workload-compression.md) | AI Workload Compression | Weight tensor properties, quantization patterns, sparse structure; no existing lossless VRAM compressor |
| [009](009-hybrid-wkdm-lz4.md) | Hybrid WKdm+LZ4 Pipeline | Two-pass compression: WKdm for word structure, LZ4 for byte residuals; potential 2.5-3:1 on mixed pages |
| [010](010-simd-wkdm-feasibility.md) | SIMD-WKdm Feasibility | No known SIMD WKdm exists; AVX2 gather could enable 4-6x speedup; potential novel contribution |
| [011](011-block-classification-weights.md) | Block Classification for AI Weights | Per-block classification (zero/sparse/uniform/small-range/dense) achieves 3-8x on quantized weights; highest-impact novel contribution |
| [012](012-compression-advisor-thresholds.md) | Compression Advisor Thresholds | Decision tree classifier for per-page algorithm selection; <0.5 μs budget; calibrated from benchmark data |
| [013](013-specialized-algorithm-inspiration.md) | Specialized Algorithm Inspiration | Parquet BYTE_STREAM_SPLIT, nvCOMP Cascaded, SIMD-BP128, row-delta prediction for AI weights |
| [014](014-linux-kernel-vram-architecture.md) | Linux Kernel–VRAM Boundary | mm/ has zero VRAM visibility; TTM manages VRAM entirely; CXL Type-3 IS visible to mm/; no existing VRAM compression patches |
| [015](015-display-stream-compression-dsc.md) | Display Stream Compression (DSC) | DSC is lossy (visually lossless only); hardware is display-path only; MMAP prediction + ICH techniques borrowable for lossless memory compression; no CXL/PCIe compression standard exists |
| [016](016-parallel-decompression-swarming.md) | Parallel Decompression Swarming | 4.5–7× latency reduction via parallel cluster decompression; swap readahead provides clustering; RCU + atomic refs for safety; GPU viable for VRAM only (not CPU faults); mmu_gather for batched TLB flush |
| [017](017-sparse-activation-brain-inspired-recall.md) | Sparse Activation & Brain-Inspired Recall | MoE models have 70-90% cold weights; layer-sequential access enables perfect prefetching; tiered VRAM/RAM/NVMe strategy; no existing offloading system uses compression |
| [018](018-memory-overhead-analysis.md) | Memory Overhead Analysis | ~88 bytes metadata per compressed page; 12.5% minimum savings threshold; shrinker recommended |
| [019](019-pte-marking-and-fault-interception.md) | PTE Marking & Fault Interception | SWP_PTE_MARKER type 31 + BIT(3) custom marker; 54-bit index space; page fault → decompress → remap; module-only via kprobes or kernel patch for do_swap_page |
| [020](020-kernel-patch-do-swap-page.md) | Kernel Patch for do_swap_page() | Add PTE_MARKER_MINIMEM check in handle_pte_marker(); function pointer registration for module callback; kprobe approach works but has overhead and reliability limitations |
| [021](021-zram-baseline-comparison.md) | zram Baseline Comparison | zram only compresses swapped pages; MiniMem compresses still-mapped idle pages; MiniMem 2.8× better ratio on pointer-heavy pages, 27× on AI INT8; complementary to zram, not a replacement |
| [022](022-kernel-upstreaming-requirements.md) | Kernel Upstreaming Requirements | PTE marker bit allocation, small patch set (~42 lines), function pointer registration, performance data needed, safety guarantees (OOM, THP, KSM), 6-18 month timeline estimate |

---

## How to use this folder

1. **Reference from design docs.** Candidates and architecture docs should link to research docs, not repeat their content.
2. **Add before investigating.** When starting a research session, create the doc first. Fill it in as you go.
3. **Update when revisited.** If new information comes to light, update the existing doc rather than creating a new one.
4. **Every doc must have:** Summary, Key Findings, Relevance to MiniMem, Open Questions, References.