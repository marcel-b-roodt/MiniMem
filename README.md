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
- **Parallel decompression**: Workqueue-based cluster decompression with auto-detect (3.76× speedup on 4 vCPUs)
- **Per-process stats**: Optional per-PID compression statistics via sysfs/debugfs
- **Runtime patch detection**: Detects kernel patches at load; falls back to kprobe-only mode on unpatched kernels
- **DKMS packaging**: Auto-rebuilds per kernel update; includes kernel patches for full scanner functionality
- **42 kselftest + 15 E2E + 3 stress tests** all passing in QEMU VM

### Stage 2 — VRAM Compression 🔧 In Progress

Transparent GPU memory compression for AI workloads and games. Phase 1 (C core) is passing 9 standalone tests. See [docs/vram-compression.md](docs/vram-compression.md).

### Stage 3 — Hardware Acceleration 📋 Planned

CXL Type-3, Intel QAT, FPGA offload. CXL memory works today with the Stage 1 kernel module (fully visible to `mm/`). See [docs/hardware-acceleration.md](docs/hardware-acceleration.md).

---

## Installation

### Arch Linux (local build)

The easiest way to install on Arch — builds the DKMS module, installs systemd units, and the userspace library:

```bash
# Full install (module + systemd + library)
sudo ./scripts/local-install.sh

# Module only (no systemd, no library)
sudo ./scripts/local-install.sh --module-only

# Check status
sudo ./scripts/local-install.sh --status

# Full uninstall (stops services, unloads module, removes everything)
sudo ./scripts/local-install.sh --uninstall
```

### AUR (when registration reopens)

```bash
yay -S minimem minimem-dkms minimem-dkms-systemd
```

Three packages:
- **`minimem`** — userspace library (`libminimem.so`, headers, pkg-config) + CLI tool
- **`minimem-dkms`** — kernel module (DKMS, auto-rebuilds on kernel update)
- **`minimem-dkms-systemd`** — systemd units for auto-load and auto-enable

### Fedora / Debian / Ubuntu (OBS)

Pre-built packages are published on Open Build Service. See [packaging/](packaging/) for RPM and Debian package definitions.

### Manual build

```bash
# Library (userspace)
meson setup build
meson compile -C build
meson test -C build --print-errorlogs   # requires Criterion
sudo meson install -C build

# Kernel module
./build-kmod.sh build      # builds minimem.ko
./build-kmod.sh load        # loads module with sudo

# DKMS install (includes kernel patches)
sudo ./scripts/dkms-install.sh
sudo ./scripts/dkms-install.sh --patches   # also apply kernel patches
```

### Verify it's working

```bash
minimem status              # show module status and global stats
minimem config              # show all configuration settings
```

---

## Monitoring and Statistics

The `minimem` CLI is installed to `/usr/bin/minimem` (or `/usr/local/bin/minimem` with local-install):

```bash
minimem status              # global stats (default command)
minimem config              # show all configuration settings
minimem config scanner_enabled 1   # enable scanner (root)
minimem config parallel_mode 2     # auto-detect parallel (root)
minimem summary             # anonymized UID-level summary
minimem per-process         # detailed per-PID view (root)
minimem watch               # live 2s dashboard
minimem load                # load kernel module (root)
minimem unload              # unload kernel module (root)
minimem reset               # reset counters (root)
minimem version             # show version
minimem help                # full usage
```

### Global stats (always available)

```bash
minimem status

# Raw sysfs attributes
cat /sys/kernel/minimem/pages_compressed
cat /sys/kernel/minimem/bytes_saved
cat /sys/kernel/minimem/decompress_avg_ns
cat /sys/kernel/minimem/pool_pages
cat /sys/kernel/minimem/scanner_enabled
cat /sys/kernel/minimem/parallel_mode
```

### Per-process stats (opt-in)

Per-process statistics are **off by default** for zero overhead. Enable them to see which processes benefit from compression:

```bash
# Enable per-process tracking
sudo minimem config per_process_stats 1

# Anonymized summary (world-readable, UID-level only, no PIDs or process names)
minimem summary

# Detailed per-PID view (root only)
minimem per-process

# Live dashboard
minimem watch

# Configure how many UIDs appear in summary (1-20, default 5)
sudo minimem config per_process_top_n 10

# Disable (clears all per-process data, restores zero overhead)
sudo minimem config per_process_stats 0
```

Per-process stats expose:
- **`per_process_stats`** (sysfs, 0644) — enable/disable toggle, writing 0 clears all entries
- **`per_process_top_n`** (sysfs, 0644) — how many UIDs in summary (1-20, default 5)
- **`stats_summary`** (sysfs, 0444) — anonymized UID-level aggregates, safe for non-root
- **`per_process`** (debugfs, 0400) — root-only detailed PID/command/bytes/latency table

Memory overhead: ~128 bytes per tracked process, capped at 1024 processes (~128 KB max). When disabled: zero overhead (single branch check).

### Sysfs attributes reference

All attributes are under `/sys/kernel/minimem/`:

| Attribute | Mode | Description |
|---|---|---|
| `pages_compressed` | 0444 | Total pages compressed since load |
| `pages_decompressed` | 0444 | Total pages decompressed since load |
| `bytes_saved` | 0444 | Total bytes saved (original − compressed) |
| `compress_count` | 0444 | Number of compress operations |
| `decompress_count` | 0444 | Number of decompress operations |
| `compress_ns_total` | 0444 | Total time spent compressing (ns) |
| `decompress_ns_total` | 0444 | Total time spent decompressing (ns) |
| `compress_avg_ns` | 0444 | Average compress latency (ns) |
| `decompress_avg_ns` | 0444 | Average decompress latency (ns) |
| `zswap_pages` | 0444 | Pages currently in compressed pool |
| `zswap_bytes` | 0444 | Bytes currently in compressed pool |
| `zswap_saved` | 0444 | Bytes saved by current compressed pages |
| `pool_pages` | 0444 | Pool memory pages allocated |
| `parallel_clusters` | 0444 | Pages decompressed in parallel |
| `parallel_pages` | 0444 | Parallel decompression batches |
| `scanner_enabled` | 0644 | Scanner on/off (0/1) |
| `scanner_interval_ms` | 0644 | Scanner sweep interval (ms, 100–60000) |
| `min_savings_pct` | 0644 | Minimum savings % to compress (0–90) |
| `scanner_pages_scanned` | 0444 | Pages scanned by scanner |
| `scanner_pages_idle` | 0444 | Idle pages found by scanner |
| `scanner_pages_compressed` | 0444 | Pages compressed by scanner |
| `scanner_pages_skipped` | 0444 | Pages skipped (too small savings) |
| `hook_faults` | 0444 | Faults intercepted by hook |
| `kernel_patches` | 0444 | 0=kprobe-only, 1=patched kernel |
| `max_pool_pages` | 0644 | Max pool pages (0=unlimited) |
| `parallel_mode` | 0644 | 0=off, 1=on, 2=auto (default) |
| `per_process_stats` | 0644 | Per-process tracking on/off (default: 0) |
| `per_process_top_n` | 0644 | Top-N UIDs in summary (1–20, default: 5) |
| `stats_summary` | 0444 | Anonymized UID-level compression stats |

---

## Configuration

### Systemd auto-load and auto-enable

When installed with systemd support (`minimem-dkms-systemd` package or `local-install.sh`):

- **`minimem-load.service`** — loads the module on boot
- **`minimem.service`** — enables the scanner on boot
- **`modules-load.d/minimem.conf`** — tells systemd-modules-load to load minimem

```bash
# Enable auto-load and scanner on boot
sudo systemctl enable --now minimem-load.service
sudo systemctl enable --now minimem.service

# Check status
systemctl status minimem-load.service minimem.service

# Disable auto-load
sudo systemctl disable minimem.service minimem-load.service
```

### Runtime configuration

```bash
# Enable the scanner (start compressing idle pages)
sudo minimem config scanner_enabled 1

# Set scanner interval (milliseconds, 100–60000)
sudo minimem config scanner_interval_ms 2000

# Set minimum savings threshold (percent, 0–90)
sudo minimem config min_savings_pct 20

# Limit pool size (0 = unlimited)
sudo minimem config max_pool_pages 10000

# Parallel decompression: 0=off, 1=on, 2=auto (default)
sudo minimem config parallel_mode 2
```

---

## Recovery and Disabling

MiniMem is designed so that **no data is ever at risk**:

- **Module unload decompresses everything** — `rmmod minimem` restores all compressed pages to normal memory
- **Incompressible pages are never touched** — the advisor skips them entirely
- **Compression is always lossless** — verified by `memcmp` round-trip in every test

### Quick disable

```bash
# Stop the scanner (module stays loaded, no new compression)
sudo minimem config scanner_enabled 0

# Unload the module entirely
sudo minimem unload
```

### Disable auto-load on boot

```bash
sudo systemctl disable minimem.service minimem-load.service
sudo rm /usr/lib/modules-load.d/minimem.conf
```

### Full uninstall

```bash
sudo ./scripts/local-install.sh --uninstall
```

### Emergency recovery (kernel panic on boot)

If the module causes a kernel panic during boot:

1. **Blacklist via GRUB** — press `e` at the GRUB menu, find the `linux` line, append `minimem.blacklist=1`, press `Ctrl+X` to boot
2. **Once booted**, make it permanent:
   ```bash
   echo "blacklist minimem" | sudo tee /etc/modprobe.d/minimem.conf
   sudo ./scripts/local-install.sh --uninstall
   ```
3. **If GRUB is inaccessible** — boot from a live USB, chroot into the system, and add the blacklist or remove the module

See [docs/recovery.md](docs/recovery.md) for full recovery documentation.

---

## How compression works for large applications

Linux manages memory in **4 KB pages**. Each page is independent — MiniMem compresses individual idle pages, not entire processes.

A database with 16 GB of allocated memory might have 4 GB of hot pages being actively queried and 12 GB of cached pages sitting idle. MiniMem's scanner identifies the idle pages and compresses each one independently. You get 50–70% savings on the idle pages without touching the hot ones.

Even actively-queried data benefits: sequential scans touch pages briefly then move on. The scanner's two-pass mark-sweep (mark idle → sweep compress) naturally captures recently-cooled pages. MiniMem and zram are complementary — they target different page populations.

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
| [Recovery guide](docs/recovery.md) | Disabling, uninstalling, emergency recovery |
| [Research index](docs/research/README.md) | Numbered research documents |

---

## Project status

| Stage | Status | What's working |
|---|---|---|
| Stage 0 — Algorithm Library | ✅ Complete | 12 algorithms, 75 tests, benchmarks published |
| Stage 1 — Kernel Module | ✅ Complete | Transparent page compression, PTE markers, scanner, shrinker, per-process stats, DKMS, full VM test suite |
| Stage 2 — VRAM Compression | 🔧 In Progress | C core passing 9 tests; PyTorch allocator next |
| Stage 3 — Hardware Acceleration | 📋 Planned | CXL works today; QAT/FPGA evaluation pending |

---

## Packaging

| Channel | Package | Status |
|---|---|---|
| AUR | `minimem` (library + CLI), `minimem-dkms` (kernel module), `minimem-dkms-systemd` (systemd) | ✅ PKGBUILDs ready |
| OBS | `minimem` (Fedora, Debian, Ubuntu, openSUSE) | 🔧 Packaging ready, fixing openSUSE build |
| DKMS | `minimem-dkms` | ✅ Auto-rebuild per kernel update |
| Local | `scripts/local-install.sh` | ✅ Build + install + systemd on Arch |

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