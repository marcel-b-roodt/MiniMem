# MiniMem — Project Goal

## North Star

> **Transparently compress memory at the kernel and driver level, so applications get more effective RAM and VRAM without any code changes.**

MiniMem targets the gap between existing Linux memory compression (zram/zswap, which only compress pages being evicted to swap) and what macOS has done since 2013 (in-memory compression of cold pages). It extends the same principle to GPU VRAM and hardware-accelerated compression paths.

---

## What MiniMem Is

- A **transparent memory compression system** — applications and userspace code require zero changes. Compression happens in the kernel or driver.
- A **lossless compression library** — algorithms designed for the memory access path, not file compression. Decompression must be fast enough to sit behind a page fault.
- A **Linux kernel module** — compresses cold anonymous pages in-place, decompresses on access. The "macOS gap" filler.
- A **VRAM compression layer** — reduces GPU memory footprint for AI workloads and games by compressing idle GPU buffers in the driver.
- A **hardware acceleration framework** — offloads compression to CXL, QAT, FPGA when available, with software fallback.
- A **specialized compressor toolkit** — domain-optimized algorithms for AI weights, page tables, streaming deltas, and other structured memory content.

## What MiniMem Is Not

- A file compression tool. MiniMem compresses live memory, not files on disk.
- A swap compression system. zram and zswap already handle that. MiniMem compresses pages that are *still mapped* in process address space.
- A replacement for KSM. KSM deduplicates identical pages (zero decompression cost). MiniMem compresses *similar* pages that are not identical.
- A quantization or lossy compression system. Lossless is a hard constraint. No approximations.
- A userspace library that applications link against. Compression is transparent — the application never knows it is happening.

---

## Success Criteria

| Target | Metric |
|---|---|
| RAM compression | 2x+ effective memory capacity for cold-page-heavy workloads (AI inference, idle server processes) |
| Decompression latency | <10 μs per 4KB page on modern x86 (software path) |
| VRAM compression | 30-50% VRAM savings for AI inference workloads |
| Transparency | Zero application code changes. Existing binaries run unmodified. |
| Lossless | Bit-exact round-trip on every page. No data corruption. |
| Overhead | <5% CPU overhead for active workloads (compression only on cold pages) |
| Kernel compatibility | Loadable module on latest stable + latest LTS kernel |

---

## Guiding Principles

1. **Transparent.** Applications must not need to know about compression. If they do, we have failed.
2. **Lossless.** Every byte must decompress to exactly the original value. No exceptions, no approximate modes.
3. **Fast enough.** Decompression must be faster than reading from disk (swap). If it isn't, we are making things worse.
4. **Measurable.** Every component is benchmarked. If it isn't measured, it isn't done.
5. **Graceful degradation.** Hardware acceleration is optional. Software fallback exists for every path. If compression doesn't help (incompressible pages), we skip them with minimal overhead.