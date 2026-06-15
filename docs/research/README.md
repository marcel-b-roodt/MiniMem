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

---

## How to use this folder

1. **Reference from design docs.** Candidates and architecture docs should link to research docs, not repeat their content.
2. **Add before investigating.** When starting a research session, create the doc first. Fill it in as you go.
3. **Update when revisited.** If new information comes to light, update the existing doc rather than creating a new one.
4. **Every doc must have:** Summary, Key Findings, Relevance to MiniMem, Open Questions, References.