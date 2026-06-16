# Contributing to MiniMem

Bug reports, feature requests, and design feedback are welcome via [GitHub Issues](https://github.com/marcel-b-roodt/MiniMem/issues).

**MiniMem is currently closed to pull requests.** The project is developed by a single maintainer with AI assistance (see AI disclosure in [README.md](README.md)). Suggestions and issue discussions are welcome; code contributions are not being accepted at this time.

---

## Quick Start

```bash
git clone https://github.com/marcel-b-roodt/MiniMem.git
cd MiniMem
meson setup build
meson compile -C build          # userspace library
./build-kmod.sh build           # kernel module
```

## Contribution Layers

### 1. Algorithm Library (`src/lib/`)

**What goes here:** All compression algorithms — general-purpose, AI-specific, page-table-aware, delta-streaming. This is the most accessible layer.

**Requirements:**
- C11, no allocations in compress/decompress hot paths
- Must pass round-trip test: `memcmp(src, decompressed, len) == 0`
- Must have Criterion benchmark (throughput MB/s, latency μs, ratio)
- Compressed size must never exceed original (incompressible → return 0 or error)
- Decompression latency target: <10μs per 4KB page
- Register algorithm in `src/lib/registry.c`
- Add test in `tests/lib/`
- Add entry in `docs/candidates.md` with assessment format

**No kernel knowledge required.** Algorithms are pure C and testable from userspace.

### 2. Kernel Module (`src/kernel/`)

**What goes here:** Page fault handling, PTE manipulation, scanner, sysfs, zsmalloc integration. Transparent RAM compression plumbing.

**Requirements:**
- Linux coding style (`indent -linux`)
- Must compile as loadable module (no kernel rebuild required)
- Must not depend on out-of-tree kernel patches (patches are optional enhancements)
- All PTE manipulation via kallsyms-resolved symbols
- Must expose stats via `/sys/kernel/minimem/`
- Clean `rmmod` — no resource leaks
- Test in QEMU VM (`vm-test-minimem.sh`)
- kselftest compliance (42+ tests)

**Kernel patches** (`patches/`) enhance the module but are not required. The module gracefully degrades to kprobe-only mode without them.

### 3. VRAM Layer (`src/vram/`)

**What goes here:** GPU memory compression — driver integration, tensor-tiered management, GPU-side dispatch.

**Requirements:**
- Rust or C depending on target driver (Mesa → Rust, amdgpu → C)
- Must work with actual GPU hardware (CI can skip without GPU)
- Userspace prototype first, then driver integration
- See `docs/vram-compression.md` and `docs/research/` for design constraints

**VRAM compressors live in `src/lib/`** — the algorithm is library-level. `src/vram/` is only the dispatch/integration layer.

## Adding a New Compression Algorithm

1. Implement in `src/lib/compressors/your_algo.c` and `.h`
2. Add `MINIMEM_ALGO_YOUR_ALGO` ID in `src/lib/minimem.h`
3. Register in `src/lib/registry.c`
4. Write round-trip + benchmark tests in `tests/lib/test_your_algo.c`
5. Add candidate assessment in `docs/candidates.md`
6. Wire into kernel dispatch in `src/kernel/minimem_compress.c` (if appropriate for RAM pages)
7. Wire into advisor in `src/lib/advisor.c`
8. Update `docs/feature-registry.md`

## Kernel Patches

MiniMem ships two small kernel patches (42 lines total) that enable the scanner sweep pass:

| Patch | What it does |
|---|---|
| `minimem-6.18-fault-handler-registration.patch` | Adds `include/linux/minimem.h` + registration API |
| `minimem-6.18-handle-minimem-marker.patch` | Adds `PTE_MARKER_MINIMEM` case to `handle_pte_marker()` |

These are applied by the DKMS install script. **The module works without them** — it falls back to kprobe-based fault handling (scanner sweep disabled).

Patches must track the kernel version. When updating for a new kernel version:
1. Test against new kernel in QEMU VM
2. Adjust patch context lines if kernel source changed
3. Create new patch file with kernel version in name: `minimem-6.XX-*.patch`
4. Update `patches/series`
5. Update `dkms/dkms.conf` PATCHES array

## Security

### Commits
- Sign commits with GPG: `git config commit.gpgsign true`
- Conventional commit format (see AGENTS.md)

### Branch Protection
- `main` branch requires signed commits + passing CI
- No direct push to `main`

### Package Security
- AUR packages: PGP-sign PKGBUILDs
- DKMS builds from source on user's machine — no binary trust issue
- Kernel patches are verifiable (42 lines, fully readable)

## Reporting Issues

- Algorithm bugs (wrong decompression, corruption): **critical** — include test case with specific data
- Kernel module crashes: include `dmesg` output, kernel version, `CONFIG_` options
- Performance regressions: include Criterion benchmark before/after
- Feature requests: describe the use case, not just the solution

## License

By contributing, you agree that your code is licensed under GPL-2.0-only (kernel module) or the license stated in each file (library: see individual files).