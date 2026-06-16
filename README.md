# MiniMem — Transparent Lossless Memory Compression

MiniMem compresses memory that applications aren't actively using — transparently, losslessly, and fast enough to sit in the page fault path. No application changes required.

[![CI — Library](https://github.com/marcel-b-roodt/MiniMem/actions/workflows/ci.yml/badge.svg)](https://github.com/marcel-b-roodt/MiniMem/actions/workflows/ci.yml)
[![CI — Kernel Module](https://github.com/marcel-b-roodt/MiniMem/actions/workflows/kernel-module-ci.yml/badge.svg)](https://github.com/marcel-b-roodt/MiniMem/actions/workflows/kernel-module-ci.yml)
[![CI — Packaging](https://github.com/marcel-b-roodt/MiniMem/actions/workflows/packaging.yml/badge.svg)](https://github.com/marcel-b-roodt/MiniMem/actions/workflows/packaging.yml)

---

## What it does

MiniMem has three layers, each at a different stage of development:

### Stage 0 — Algorithm Library ✅ Complete

The userspace compression library (`libminimem`) provides 12 algorithms optimised for memory pages and structured data:

| Algorithm | Best ratio | Decompress speed | Use case |
|---|---|---|---|
| Same-page | 819:1 (zero) | 0.07 μs/page | Zero-fill and repeated-value pages |
| BDI | 60:1 (zero), 7:1 (uniform) | 0.17 μs/page | Cache-line-friendly integer pages |
| WKdm-32 | 15:1 (zero), 2.08:1 (pointer) | 2.3 μs/page | Pointer-heavy pages (macOS-style) |
| WKdm-64 | 29:1 (zero) | 1.7 μs/page | 64-bit word pages |
| LZ4 | 3:1 typical | 0.5 μs/page | General-purpose fast path |
| LZSSE8 | 4.7 GB/s decompress | — | SIMD-accelerated fast decompress (x86) |
| Zstd | 4.64:1 (PTE) | 10.6 μs/page | Best ratio, higher latency |
| Delta XOR | Round-trip verified | — | XOR delta primitive |
| Block classifier | 146:1 (zero) | — | 5-type page classifier |
| AI-FP16/BF16 | 1.96:1 (FP16) | 5.2 μs/page | BYTE_STREAM_SPLIT for weights |
| AI-INT8 | 44.5:1 (uniform) | 1.6 μs/page | Row-delta XOR for quantised weights |
| Advisor | — | <0.5 μs | Heuristic page → algorithm selector |

75 Criterion unit tests passing. Benchmark report in [docs/benchmarks.md](docs/benchmarks.md).

### Stage 1 — Linux Kernel Module ✅ Complete

A loadable kernel module (`minimem.ko`) that transparently compresses idle anonymous pages in-place:

- **Page fault path**: kprobe on `do_swap_page` resolves compressed pages, installs present PTE — 4/4 pages verified in QEMU E2E test
- **Scanner**: VMA-based mark-sweep identifies idle pages, compresses them with advisor-selected algorithms
- **PTE markers**: Custom `SWP_PTE_MARKER` type with 54-bit index space
- **Shrinker**: Memory pressure triggers decompression via `shrinker_alloc`/`register` API
- **Runtime patch detection**: Detects kernel patches at load; falls back to kprobe-only mode on unpatched kernels
- **DKMS packaging**: Auto-rebuilds per kernel update; includes 42-line kernel patches for full scanner functionality
- **42 kselftest + 15 E2E + 3 stress tests** all passing in QEMU VM

### Stage 2 — VRAM Compression 📋 Planned

Transparent GPU memory compression for AI workloads and games. Phase 1 will be a userspace PyTorch custom allocator using nvCOMP for batch compression of idle weight tensors. See [docs/vram-compression.md](docs/vram-compression.md).

### Stage 3 — Hardware Acceleration 📋 Planned

CXL Type-3, Intel QAT, FPGA offload. CXL memory works today with the Stage 1 kernel module (fully visible to `mm/`). See [docs/hardware-acceleration.md](docs/hardware-acceleration.md).

---

## Quick start

### Build the library (userspace)

```bash
meson setup build
meson compile -C build
meson test -C build --print-errorlogs   # requires Criterion
```

### Build the kernel module

```bash
./build-kmod.sh build      # builds minimem.ko
./build-kmod.sh tests      # builds static VM test binaries
./vm-test-minimem.sh        # full test suite in QEMU VM
```

### Install the library

```bash
meson install -C build      # installs libminimem.so, headers, pkg-config
```

### DKMS install (kernel module)

```bash
sudo ./scripts/dkms-install.sh    # installs minimem-dkms, applies kernel patches
```

See [CONTRIBUTING.md](CONTRIBUTING.md) for the full development workflow.

---

## Documentation

| Document | Description |
|---|---|
| [Project goal](docs/goal.md) | What MiniMem is and isn't |
| [Roadmap](docs/roadmap.md) | Stage-by-stage feature plan |
| [Feature registry](docs/feature-registry.md) | Source of truth for every feature's status |
| [Architecture](docs/architecture.md) | Codebase architecture for contributors |
| [Compression candidates](docs/candidates.md) | Algorithm assessment format |
| [VRAM compression](docs/vram-compression.md) | GPU memory compression deep dive |
| [Hardware acceleration](docs/hardware-acceleration.md) | CXL, QAT, FPGA, SIMD paths |
| [Specialized compression](docs/specialized-compression.md) | AI weights, page tables, streaming |
| [Benchmarks](docs/benchmarks.md) | Performance summary with external references |
| [Research index](docs/research/README.md) | 21 numbered research documents |

---

## Project status

| Stage | Status | What's working |
|---|---|---|
| Stage 0 — Algorithm Library | ✅ Complete | 12 algorithms, 75 tests, benchmarks published |
| Stage 1 — Kernel Module | ✅ Complete | Transparent page compression, PTE markers, scanner, shrinker, DKMS, full VM test suite |
| Stage 2 — VRAM Compression | 📋 Planned | Userspace PyTorch allocator design in progress |
| Stage 3 — Hardware Acceleration | 📋 Planned | CXL works today; QAT/FPGA evaluation pending |

---

## Packaging

| Channel | Package | Status |
|---|---|---|
| AUR | `minimem` (library), `minimem-dkms` (kernel module) | ✅ PKGBUILDs ready |
| OBS | `minimem` (Fedora, Debian, Ubuntu, openSUSE) | ✅ Packaging ready |
| DKMS | `minimem-dkms` | ✅ Auto-rebuild per kernel update |

---

## AI Tooling Transparency Disclosure

Most code in this repository was generated with AI assistance and reviewed by the maintainer. Specifically:

- **Tooling:** [OpenCode](https://opencode.ai) running GLM-5.1 (an open-weight model from [Zhipu AI / THUDM](https://github.com/THUDM/GLM-4), [glm-4 license](https://huggingface.co/THUDM/glm-4-9b/blob/main/LICENSE)) via [Ollama](https://ollama.com). Other models, provided they are open-source and appropriately licensed, may also be used.
- **Human role:** Project architecture, feature design, code structure, acceptance testing, and all release decisions are made by the maintainer. Every AI-generated change is read, evaluated, and tested before merging. No code ships without human approval.
- **CI:** All code passes meson build, Criterion tests, kernel module build, and DKMS packaging verification on every push via GitHub Actions.
- **Not used:** Autonomous agents, unsupervised commits, or unsupervised merges. No outside contributors.

If you have questions about how any piece of code was written or verified, please open an issue.

---

## Contributing

MiniMem is **open to suggestions but closed to pull requests.** Bug reports, feature requests, and design feedback are welcome via issues. See [CONTRIBUTING.md](CONTRIBUTING.md) for the development workflow.

---

## License

MiniMem uses a multi-license structure:

| Component | License | Why |
|---|---|---|
| Kernel module (`src/kernel/`) | **GPL-2.0-only** | Required by Linux kernel module loading |
| Userspace library (`src/lib/`) | **MIT** | Permissive, library-friendly |
| LZ4 vendor code (`src/lib/vendor/lz4/`) | **BSD-2-Clause** | Upstream license |
| LZSSE8 vendor code (`src/lib/vendor/lzsse8/`) | **BSD-3-Clause** | Upstream license |
| Documentation, tests, packaging | **MIT** | Permissive |

See [LICENSE](LICENSE) for the full license text.

---

*MiniMem — compress what you aren't using, decompress when you need it back.*