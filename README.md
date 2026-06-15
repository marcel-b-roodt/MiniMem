# MiniMem — Transparent Memory Compression

**MiniMem** is an open-source research and implementation project for transparent, lossless memory compression at the Linux kernel and GPU driver level.

---

## What it does

- **RAM compression** — transparently compress cold memory pages in-place, reducing physical memory usage without application changes
- **VRAM compression** — compress GPU memory buffers in the driver layer, reducing VRAM usage for AI workloads and games
- **Hardware acceleration** — offload compression to CXL memory controllers, Intel QAT, or FPGA when available
- **Specialized compressors** — domain-optimized algorithms for AI model weights, page-table pages, and streaming data

---

## Quick links

| Resource | Link |
|---|---|
| Project goal | [docs/goal.md](docs/goal.md) |
| Roadmap | [docs/roadmap.md](docs/roadmap.md) |
| Feature status | [docs/feature-registry.md](docs/feature-registry.md) |
| Architecture | [docs/architecture.md](docs/architecture.md) |
| Compression candidates | [docs/candidates.md](docs/candidates.md) |
| VRAM compression | [docs/vram-compression.md](docs/vram-compression.md) |
| Hardware acceleration | [docs/hardware-acceleration.md](docs/hardware-acceleration.md) |
| Specialized compression | [docs/specialized-compression.md](docs/specialized-compression.md) |
| Research index | [docs/research/README.md](docs/research/README.md) |

---

## Status

Early research and design phase. No production code yet. See [docs/roadmap.md](docs/roadmap.md) for the development plan.

---

## License

MIT