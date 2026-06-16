# MiniMem — Documentation

**MiniMem** is an open-source research and implementation project for transparent, lossless memory compression at the Linux kernel and GPU driver level.

---

## In this folder

| File | What it covers |
|---|---|
| [goal.md](goal.md) | Product vision — what MiniMem is and is not |
| [roadmap.md](roadmap.md) | Stage-by-stage feature roadmap |
| [feature-registry.md](feature-registry.md) | Live status of every feature |
| [architecture.md](architecture.md) | Codebase structure and key design decisions |
| [candidates.md](candidates.md) | RAM compression algorithm & approach candidates |
| [vram-compression.md](vram-compression.md) | VRAM-specific compression deep dive |
| [hardware-acceleration.md](hardware-acceleration.md) | Hardware acceleration paths: CXL, QAT, FPGA, SIMD |
| [specialized-compression.md](specialized-compression.md) | Second-layer investigation: novel specialized approaches |
| [how-it-works.md](how-it-works.md) | Plain-language guide: what MiniMem does, how it works, and why it matters |

---

## Research

| File | What it covers |
|---|---|
| [research/README.md](research/README.md) | Index of all research documents |

---

## Where to start

- **Understanding the vision?** → [goal.md](goal.md)
- **New to the project?** → [how-it-works.md](how-it-works.md)
- **Curious what's planned?** → [roadmap.md](roadmap.md) or [feature-registry.md](feature-registry.md)
- **Evaluating algorithms?** → [candidates.md](candidates.md)
- **VRAM compression?** → [vram-compression.md](vram-compression.md)
- **Contributing code?** → [architecture.md](architecture.md), then the test suite in `tests/`
- **Reading prior art?** → [research/README.md](research/README.md)