# MiniMem — TODO

## Stage 0 — Algorithm Library & Benchmarks

- [x] Benchmark framework setup (Criterion + page/tensor data harness)
- [x] LZ4 integration — reference C v1.10.0 vendored; roundtrip + benchmark tests passing
- [ ] LZSSE8 integration — SSE4.1 SIMD decompressor, x86-64 only
- [ ] WKdm implementation — scalar implementation needs ratio improvement; decompress verified
- [x] BDI (Base-Delta-Immediate) implementation — zero/uniform/small-delta modes; tests passing
- [x] Zstd wrapper — system zstd 1.5.7 linked; level-1 compression; roundtrip tests passing
- [x] Delta encoding primitives — XOR delta + recovery; roundtrip tests passing
- [x] Same-page detection — zero-fill and repeated-value pages; 819:1 on zero; tests passing
- [x] Algorithm comparison benchmark suite — 5 algorithms x 11 page types; first results published
- [x] Benchmark report format — CSV/JSON output to reports/

## Stage 1 — Linux Kernel In-Memory Compression Module

- [ ] Kernel module scaffold — loadable module, sysfs stats, clean rmmod
- [ ] Page idle tracking — hook into `PG_idle`/`PG_young` or soft-dirty bit tracking
- [ ] Compression map — data structure mapping virtual address → compressed page handle (zsmalloc pool)
- [ ] PTE marking — custom swp_entry for "compressed in RAM" pages (analogous to swap entries)
- [ ] Page fault handler — intercept compressed-page faults, decompress, remap, TLB flush
- [ ] Compression policy — threshold for when to compress (idle time, access frequency)
- [ ] Decompression path — per-CPU buffers, direct map into faulting page
- [ ] Multi-algorithm recompression — hot pages use LZ4 (fast), cold pages recompress with zstd (dense)
- [ ] `/sys/kernel/minimem/` stats — pages_compressed, bytes_saved, decompress_count, latency histogram
- [ ] Concurrency — RCU-safe lookups, per-CPU compression buffers, no global locks on hot paths
- [ ] kselftest suite — load/unload, compress/decompress round-trip, stress test, stats validation
- [ ] Compatibility — compiles against latest stable + latest LTS kernel

## Stage 2 — VRAM Transparent Compression

- [ ] VRAM buffer tracking — track allocated GPU buffers, classify by type (weights, activations, textures, framebuffers)
- [ ] VRAM compression map — GPU-side data structure mapping buffer ID → compressed region
- [ ] Decompression on access — hook into GPU page fault handlers or command-stream barriers
- [ ] AI workload compressor — specialized algorithm for quantized weight tensors (highly compressible sparse patterns)
- [ ] nvCOMP integration — evaluate NVIDIA's GPU-parallel LZ4/zstd for bulk VRAM compression
- [ ] Driver integration points — AMDGPU, Mesa, Vulkan memory allocator hooks
- [ ] VRAM compression stats — exposed via debugfs or sysfs
- [ ] Userspace API — ioctl or sysfs interface for applications to request/advise compression policy
- [ ] VRAM test harness — mock VRAM buffers for correctness, real GPU for throughput

## Stage 3 — Hardware Acceleration

- [ ] Intel QAT LZ4 backend — offload bulk page compression to QAT hardware
- [ ] CXL inline compression — evaluate Marvell Structera-style inline compression path
- [ ] IBM NX-842 backend — PowerPC hardware compression where available
- [ ] SIMD dispatch — runtime CPUID-based selection of AVX2/AVX-512/NEON decompression paths
- [ ] FPGA evaluation — assess Xilinx Vitis LZ4/DEFLATE for line-rate compression
- [ ] Hardware availability detection — graceful fallback to software when HW not present
- [ ] Benchmark: HW vs SW — throughput and latency comparison for each backend

## Stage 4 — Specialized Compressors

- [ ] AI weight compressor — exploit quantization patterns, sparse structure, repeated blocks in model weights
- [ ] Page-table-aware compressor — exploit pointer alignment, upper-byte zeros in 64-bit page table entries
- [ ] Delta-streaming compressor — compress deltas between similar pages (e.g., process fork COW pages, consecutive weight matrix rows)
- [ ] Dictionary compressor for homogeneous workloads — pre-trained dictionaries for specific page content profiles (kernel page tables, AI weight blocks)
- [ ] Compression advisor — runtime classifier that selects the best algorithm per page based on content analysis
- [ ] Domain-specific benchmarks — AI model weights (LLM layers, attention matrices), page tables, fork-COW pages

---

## Bugs

*(none yet)*

---

## Notes

- All kernel code follows Linux coding style. No exceptions.
- Decompression must be fast enough to sit in the page fault path. Target: <10 μs per 4KB page on modern hardware.
- Lossless is non-negotiable. Every algorithm must pass round-trip verification.
- The algorithm library (Stage 0) is shared by all later stages. No stage should vendor its own copy of an algorithm.
- Hardware acceleration is always optional. Software fallback must exist for every HW path.