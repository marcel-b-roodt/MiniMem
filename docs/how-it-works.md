# MiniMem — How It Works

A plain-language guide to what MiniMem does, why it matters, and how the pieces fit together.

---

## The Problem

Computers run out of memory. When they do, they either crash, swap to disk (slow), or kill your process (worse). Meanwhile, most of the data sitting in RAM is wasteful:

- **Zero pages** — entire 4KB pages of nothing but zeroes. They're everywhere.
- **Pointer pages** — 64-bit addresses where the top half is almost always zero.
- **AI weights** — billions of numbers that follow predictable patterns.
- **Repeated values** — the same number, over and over, page after page.

The data is compressible. Nobody's compressing it. That's the gap MiniMem fills.

---

## What MiniMem Does

MiniMem compresses memory **transparently**. Applications don't know it's happening. They don't need to change a single line of code. The kernel (or GPU driver) decides what to compress, picks the best algorithm, and decompresses on-demand when the application needs the data back.

Three rules that cannot be broken:

1. **Transparent** — zero application changes required
2. **Lossless** — decompressed data must be byte-for-byte identical to the original
3. **Fast** — decompression must be faster than reading from disk

---

## How It Works — RAM Path

### Step 1: Find Cold Pages

Not all memory is equal. A program's "hot" pages (accessed frequently) should stay uncompressed. Only "cold" pages — ones that haven't been touched in a while — are compression candidates.

MiniMem includes an **idle page scanner** — a kernel thread that periodically walks through memory and identifies pages that haven't been accessed recently (using the kernel's `PAGE_IDLE_FLAG`).

### Step 2: Classify the Page

When the scanner finds a cold page, it doesn't just blindly compress it. It first **analyzes the page** to figure out what kind of data is inside:

- Is it all zeros? → Same-page detector
- Is it all the same byte repeated? → Same-page detector
- Are most values small integers or pointers with zero upper halves? → WKdm (a dictionary-based algorithm designed for this)
- Are there cache-line patterns with small deltas? → BDI (Base-Delta-Immediate)
- Are they AI weights (FP16, BF16, INT8)? → Specialized AI compressors
- Something else? → LZ4 (general-purpose, fast)

This classification takes **under 0.5 microseconds** per page. The classifier is called the **advisor**, and it's a simple decision tree that looks at byte-level statistics.

### Step 3: Compress and Store

Once the advisor picks the best algorithm, MiniMem compresses the page and stores the compressed data in a **zsmalloc pool** — a kernel memory allocator designed for compressed objects. The original page is freed back to the system.

MiniMem only compresses pages that save at least **12.5%** of their original size (configurable via `min_savings_pct`). If compressing a 4KB page only saves 200 bytes, it's not worth the overhead.

### Step 4: Mark the Page as "Compressed"

Here's the clever part. MiniMem replaces the page's entry in the page table (the PTE) with a special marker that says "this page is compressed, here's where to find it." This marker fits in a single PTE entry — no extra page tables needed.

When an application tries to access that page, the CPU triggers a **page fault** because the page isn't present in memory anymore. MiniMem intercepts this fault, decompresses the page, puts it back where it was, and the application continues — none the wiser.

### Step 5: Decompress on Access

Decompression is the critical path. The whole point of the system is that accessing compressed memory should feel almost as fast as accessing uncompressed memory. Here are the decompression speeds for each algorithm:

| Algorithm | Best at | Decompress time (4KB) | Ratio |
|---|---|---|---|
| Same-page | Zero/repeated pages | 0.09 μs | 819:1 |
| BDI | Small-delta cache lines | 0.13 μs | 7–60:1 |
| WKdm-64 | Pointer-heavy pages | 1.5–2.0 μs | 1.5–29:1 |
| AI INT8 | Uniform INT8 weights | 0.32 μs | 44:1 |
| AI FP16/BF16 | FP16/BF16 weights | 0.7–5.1 μs | 1.4–2.8:1 |
| Block classifier | Structured integer data | 0.2–0.5 μs | 1.4–146:1 |
| LZ4 | General purpose | 0.7–4.9 μs | 1.2–157:1 |
| LZSSE8 | x86-64 with SSE4.1 | 7–15 μs | 1.1–3.2:1 |
| Zstd (dict) | Cold recompression | 0.9–11.6 μs | 1.3–216:1 |

For comparison: a disk swap-in costs **50–500 μs**. Even the slowest MiniMem decompression (Zstd at ~12 μs) is **4× faster than the fastest disk read**. Most pages decompress in under 5 μs.

### Step 6: Parallel Decompression (Bonus)

When the kernel reads in a cluster of swap pages (it typically reads 32 pages at once during "swap readahead"), MiniMem decompresses them all in parallel using kernel workqueues. On a 4-vCPU machine, this gives a **3.76× speedup** over serial decompression.

---

## How It Works — VRAM Path

GPU memory (VRAM) is even more expensive and scarce than RAM. An LLM like Mixtral 8×7B has ~47 billion parameters but only activates ~13 billion per token (it's a Mixture-of-Experts model). That means **70–90% of the weights are cold at any given time.**

MiniMem's VRAM path (currently in design) provides **tiered compression**:

```
Hot VRAM  → uncompressed, instant access
Warm VRAM → compressed in VRAM, ~5 μs to decompress
Cold RAM  → compressed in system RAM, ~15–35 μs
Frozen    → compressed on NVMe, ~100+ μs
```

The key insight: MoE models have a **router** that decides which experts to activate. MiniMem can use that same router to predict which weights will be needed next and pre-decompress them. This turns a "stall waiting for decompression" into a "pipeline while the GPU is busy with other work."

---

## The Algorithms — What Makes MiniMem Different

Most memory compression systems (zram, zswap, macOS VM compressor) use **one algorithm** for everything. That's like using a hammer for every job — it works for nails, but it's terrible for screws.

MiniMem uses **12 different algorithms** and picks the right one for each page:

### The Heavy Lifters

| Algorithm | What it's good at | Why it matters |
|---|---|---|
| **Same-page** | Pages where every byte is identical | Zero memory allocation. 819:1 ratio. Detects the most common waste pattern instantly. |
| **BDI** | Pages with small numeric differences between values | Near-instant decompression (0.13 μs). Designed for cache lines but works on whole pages too. |
| **WKdm-64** | Pages with 64-bit pointers/integers | Our novel 64-bit variant. 6.2:1 on pointer-heavy pages (vs 1.6:1 for LZ4). This is the biggest win for server workloads. |
| **LZ4** | Everything else | Fast, reliable general-purpose. The fallback that still beats swap. |

### The AI Specialists

| Algorithm | What it's good at | Why it matters |
|---|---|---|
| **AI FP16/BF16 (BYTE_STREAM_SPLIT)** | Half-precision floating point weights | 1.96:1 on data where LZ4 gets 1.0:1 (no compression at all). Uses byte interleaving to expose patterns. |
| **AI INT8 (row-delta XOR)** | Quantized integer weights | 44.5:1 on uniform INT8 data. Row-wise XOR deltas make repeated values compress extremely well. |

### The General-Purpose Options

| Algorithm | Role | Tradeoff |
|---|---|---|
| **LZSSE8** | x86-64 fast path | 4.7 GB/s decompress but needs SSE4.1. Currently underperforms LZ4 on 4KB pages — better for larger buffers. |
| **Zstd (dict)** | Cold recompression | Best ratios (4.6:1 on PTE pages) but slow decompression. Used for pages that are cold enough to re-compress in the background. |
| **Delta XOR** | Similar page pairs | Near-instant XOR + LZ4 on the delta. Useful for fork-COW pages that are nearly identical. |
| **Block classifier** | Heterogeneous pages | Per-64-byte-block classification with type-specific encoding. Good for mixed pages that other algorithms can't handle. |

---

## Current Status

| Component | Status | What's done |
|---|---|---|
| Algorithm library (libminimem) | ✅ Complete | 12 algorithms, 75+ tests, advisor, packaged as .so/.a |
| Kernel module | 🔧 In progress | Compress, store, scan, shrink, parallel decompress, sysfs stats — all working. PTE marking and fault interception need kernel patches. |
| VRAM compression | 📋 Planned | Architecture designed, CUDA prototype next |
| Hardware acceleration | 📋 Planned | CXL, QAT, FPGA paths identified |
| Specialized compressors | 🔧 Partial | AI weight compressors done. Page-table-aware, DSC-Lite, delta-streaming planned. |

---

## Performance Summary

### Best Results Per Page Type

| Page type | Best algorithm | Ratio | Decompress time |
|---|---|---|---|
| Zero pages | Same-page | 819:1 | 0.09 μs |
| Repeated value | Same-page | 819:1 | 0.09 μs |
| Pointer-heavy (64-bit) | WKdm-64 | 6.2:1 | 2.0 μs |
| Integer-heavy | Zstd dict | 2.8:1 | 11.6 μs |
| Page table entries | Zstd dict | 4.6:1 | 10.2 μs |
| AI FP16 weights | AI FP16 | 1.96:1 | 5.1 μs |
| AI INT8 weights | AI INT8 | 44.5:1 | 0.38 μs |
| Sparse data | Block classifier | 1.4:1 | 0.48 μs |
| Mixed / general | Zstd dict | 1.35:1 | 4.6 μs |

### How This Compares to Swap

| Access type | Latency | Notes |
|---|---|---|
| Uncompressed RAM | 0.1 μs | Baseline |
| MiniMem decompress (fast path) | 0.1–2.0 μs | Same-page, BDI, WKdm-64 |
| MiniMem decompress (general) | 2–5 μs | LZ4, AI compressors |
| MiniMem decompress (cold) | 5–12 μs | Zstd, worst case |
| SSD swap read | 50–500 μs | 10–100× slower than MiniMem |
| HDD swap read | 1,000–10,000 μs | Not even comparable |

**Bottom line: MiniMem decompression is 10–100× faster than swapping to SSD, and 100–1000× faster than HDD.**

---

## What Makes MiniMem Unique

1. **Multi-algorithm adaptive compression.** No other system picks the best algorithm per page. zram uses LZ4 or zstd for everything. macOS uses WKdm for everything. MiniMem classifies and selects.

2. **WKdm-64.** A novel 64-bit extension of WKdm that exploits zero upper halves of 64-bit pointers. 6.2:1 on pointer-heavy pages where LZ4 gets 1.6:1. This alone could double effective memory on server workloads.

3. **AI-specific compressors.** BYTE_STREAM_SPLIT and row-delta XOR are purpose-built for ML weight tensors. They compress FP16 data that LZ4 can't compress at all (1.96:1 vs 1.00:1).

4. **In-memory compression of mapped pages.** zram and zswap only compress pages that are being swapped out. MiniMem compresses pages that are still in the process address space, using PTE markers and fault interception. This is what macOS does; Linux currently doesn't.

5. **Parallel cluster decompression.** 3.76× speedup on 4 vCPUs by decompressing 32-page clusters in parallel using kernel workqueues.

6. **Savings threshold enforcement.** Only compresses pages that save at least 12.5% (configurable). Never wastes memory on incompressible pages.

7. **Zero-allocation fast paths.** Same-page detection stores 5 bytes instead of 4KB. BDI decompresses in under 0.2 μs. These paths don't allocate memory in the hot decompression path.

---

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────────┐
│                      Application Space                          │
│   (Sees normal memory. Never knows compression is happening.)   │
└──────────────────────────┬──────────────────────────────────────┘
                           │
                    Page fault (compressed PTE)
                           │
┌──────────────────────────▼──────────────────────────────────────┐
│                     MiniMem Kernel Module                        │
│                                                                 │
│  ┌──────────────┐   ┌─────────────┐   ┌──────────────────┐   │
│  │  Idle Page   │   │  Advisor     │   │  Compress/        │   │
│  │  Scanner     │──▶│  (classify   │──▶│  Decompress       │   │
│  │  (find cold) │   │  + select)   │   │  (12 algorithms) │   │
│  └──────────────┘   └─────────────┘   └────────┬─────────┘   │
│                                                  │              │
│  ┌──────────────┐   ┌─────────────┐             │              │
│  │  Shrinker    │   │  PTE Marker │◀────────────┘              │
│  │  (evict under │   │  (swap entry│                            │
│  │   pressure)  │   │   encoding) │                            │
│  └──────────────┘   └─────────────┘                            │
│                           │                                     │
│                  ┌────────▼─────────┐                            │
│                  │  zsmalloc Pool   │                            │
│                  │  (compressed     │                            │
│                  │   page storage)  │                            │
│                  └──────────────────┘                            │
│                                                                 │
│  ┌──────────────┐   ┌─────────────┐                             │
│  │  Parallel     │   │  Sysfs      │                            │
│  │  Decompress   │   │  (25 stats) │                            │
│  │  (workqueues) │   └─────────────┘                            │
│  └──────────────┘                                               │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│                     libminimem (Userspace)                       │
│                                                                 │
│  Same algorithm library, used standalone or for VRAM/GPU work.  │
│  Packaged as libminimem.a / libminimem.so.                      │
│  Includes advisor, all compressors, and benchmark harness.       │
└─────────────────────────────────────────────────────────────────┘
```

---

## Quick Start

```bash
# Build the library
meson setup build && ninja -C build

# Run LZSSE8 tests
cc -o test_lzsse8 tests/lib/test_lzsse8.c \
   -I src -DMINIMEM_BUILD -L build -lminimem -lzstd -lstdc++ -lm \
   -Wl,-rpath,build
LD_LIBRARY_PATH=build ./test_lzsse8

# Build the kernel module (requires kernel headers)
bash build-kmod.sh

# Run VM tests (requires QEMU)
bash vm-test-minimem.sh
```

---

## What's Next

The biggest unlocked value is **end-to-end transparent compression on a real kernel**. We have:

- All algorithms working and benchmarked
- A kernel module that compresses, stores, and decompresses pages
- A scanner that finds idle pages and compresses them
- Parallel decompression for clustered reads

What we need:

- A custom kernel with our patches applied (PTE marker handling + fault handler registration)
- CONFIG_KALLSYMS_ALL enabled so the kprobe hook can resolve kernel symbols
- End-to-end test: allocate memory → scanner compresses it → access triggers decompress → verify data

After that: VRAM compression for AI workloads, which is where the AI-specific compressors really shine.