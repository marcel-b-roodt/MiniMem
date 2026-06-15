# MiniMem — Architecture

> **Who this is for:** contributors and anyone curious about how the project is structured internally.

---

## Layer map

| Layer | Path | Description |
|---|---|---|
| Algorithm library | `src/lib/` | LZ4, LZSSE8, WKdm, BDI, Zstd, delta encoding. Pure C with SIMD paths. Shared by all other layers. |
| Kernel module | `src/kernel/` | Linux kernel module for transparent in-memory page compression. |
| VRAM compression | `src/vram/` | GPU memory compression layer. Driver integration + GPU-side structures. |
| Hardware acceleration | `src/hw/` | CXL, QAT, FPGA, SIMD dispatch backends. |
| Specialized compressors | `src/specialized/` | Domain-optimized: AI weights, page tables, delta streaming. |
| Tests | `tests/` | Criterion unit + benchmark tests. Mirror `src/` structure. |
| Reports | `reports/` | Benchmark output (CSV/JSON). Performance regression tracking. |

---

## Key data flow: RAM compression path

```
Process address space
    |
    v
Page tables (PTEs)
    |
    +-- Present page ------------> Direct access (normal)
    |
    +-- "Compressed" PTE ------> Page fault handler
                                    |
                                    v
                              Look up compression map
                              (VA -> zsmalloc handle)
                                    |
                                    v
                              Decompress from pool
                              (per-CPU buffer)
                                    |
                                    v
                              Remap page, flush TLB
                                    |
                                    v
                              Resume process
```

### Compression map

The core data structure. Maps a virtual address (or physical page) to a compressed page handle.

- **Backing store:** zsmalloc pool (already in mainline kernel; allows non-contiguous page-spanning objects)
- **Lookup:** RCU-safe hash table keyed by (mm_struct, virtual address)
- **Handle:** opaque zsmalloc handle + compressed length + algorithm ID + flags
- **Per-entry overhead:** ~32 bytes (handle + length + algorithm + padding)

### PTE marking

Compressed pages use a custom swap entry type (analogous to `swp_entry_t` for swap). The entry stores:
- Type bits: "compressed in RAM" marker
- Compression map index (for lookup)

On access, the page fault handler recognizes the type, looks up the compression map, decompresses, and remaps.

---

## Key data flow: VRAM compression path

```
GPU application (CUDA/Vulkan)
    |
    v
VRAM allocation (driver layer)
    |
    +-- Active buffer -----------> Direct GPU access (normal)
    |
    +-- Idle buffer -----------> Compression triggered
                                    |
                                    v
                              Classify buffer type
                              (weights / activations / textures)
                                    |
                                    v
                              Select algorithm
                              (AI-compressor / LZ4 / zstd)
                                    |
                                    v
                              Compress to spare VRAM region
                                    |
                                    v
                              Update VRAM compression map
                              (buffer ID -> compressed region)
                                    
    On GPU access:
                                    |
                                    v
                              GPU page fault / barrier
                                    |
                                    v
                              Decompress to original VRAM region
                                    |
                                    v
                              Resume GPU command stream
```

### VRAM compression map

GPU-accessible data structure mapping buffer ID to compressed region.

- **Storage:** GPU-visible memory (allocated from VRAM itself)
- **Lookup:** hash table or radix tree in GPU memory
- **Integration point:** Vulkan memory allocator or driver-internal buffer tracker

---

## Algorithm library design

All algorithms share a common interface:

```c
struct minimem_compressor {
    const char *name;
    size_t (*compress)(const uint8_t *src, size_t src_len,
                       uint8_t *dst, size_t dst_cap);
    size_t (*decompress)(const uint8_t *src, size_t src_len,
                         uint8_t *dst, size_t dst_cap);
    size_t (*compress_bound)(size_t src_len);
    bool (*can_compress)(const uint8_t *src, size_t src_len); // quick check
};
```

### Algorithm properties

| Algorithm | Granularity | Target data | SIMD | HW accel |
|---|---|---|---|---|
| LZ4 | 4KB page | General | SSE2/AVX2 | QAT |
| LZSSE8 | 4KB page | General | SSE4.1 | No |
| WKdm | 4KB page | Pointer/integer pages | No | No |
| BDI | 64B cache line | Small-delta pages | No | HW-native |
| Zstd (dict) | 4KB page | Homogeneous pages | SSE2/AVX2 | No |
| Delta | 4KB page | Similar to base page | AVX2 XOR | No |
| Same-page | 4KB page | Zero/repeated value | No | No |

---

## Kernel module design

### Module lifecycle

1. `insmod minimem.ko` — register page fault hook, create zsmalloc pool, init sysfs
2. Idle page scanner identifies cold pages (via PG_idle/PG_young)
3. Cold pages are compressed, unmapped, PTEs updated, compression map populated
4. On access: page fault → decompress → remap → resume
5. `rmmod minimem` — decompress all remaining pages, free pool, unregister hook, clean sysfs

### Sysfs interface

```
/sys/kernel/minimem/
  pages_compressed      (atomic64_t: total pages currently compressed)
  bytes_saved           (atomic64_t: original - compressed bytes)
  decompress_count      (atomic64_t: total page faults resolved by decompression)
  decompress_ns_total   (atomic64_t: cumulative decompression time in ns)
  compress_count        (atomic64_t: total compressions performed)
  compress_ns_total     (atomic64_t: cumulative compression time in ns)
  algorithm_stats/      (per-algorithm: count, avg_ratio, avg_latency)
```

---

## Testing architecture

### Unit tests (Criterion)

Mirror `src/` structure under `tests/`:

```
tests/
  lib/
    test_lz4.c
    test_lzsse8.c
    test_wkdm.c
    test_bdi.c
    test_zstd_dict.c
    test_delta.c
    test_same_page.c
  kernel/
    test_compress_page.c      (user-space simulation)
    test_compression_map.c
  vram/
    test_vram_compress.c       (mock VRAM)
  specialized/
    test_ai_weights.c
    test_page_table.c
```

### Benchmarks

Every algorithm test includes Criterion benchmarks measuring:
- Throughput: MB/s (compress and decompress)
- Latency: μs per 4KB page
- Ratio: compressed / original size
- Data types: random, zero-heavy, pointer-heavy, AI weights, page tables

### Kernel module tests

- kselftest for in-kernel correctness
- User-space driver for API-level testing (load module, trigger compression via memory pressure, verify stats, unload)

---

## Build system

- **Algorithm library + tests:** Meson (fast, correct dependency tracking, C-native)
- **Kernel module:** kernel build system (Kbuild)
- **Benchmarks:** integrated into Meson test harness, output to `reports/`