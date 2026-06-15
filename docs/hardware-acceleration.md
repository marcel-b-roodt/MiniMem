# MiniMem — Hardware Acceleration

Evaluation of hardware-accelerated compression paths for transparent memory compression.

---

## The Case for Hardware

Software LZ4 decompression at ~3.7 GB/s is fast, but memory bandwidth on modern systems is 45-90 GB/s (DDR5) to 819 GB/s (HBM3). Software decompression is a bottleneck when the compressed data must be read and decompressed at memory speeds.

Hardware acceleration targets:
- **CXL memory:** Compress data on the CXL link, reducing bandwidth consumption and increasing effective capacity
- **Memory controller:** Compress in the memory controller (cycle-level latency)
- **Coprocessor:** Offload bulk compression to dedicated hardware (QAT, FPGA)

---

## Candidate Hardware Paths

### 1. Intel QAT (QuickAssist Technology)

**Category:** Coprocessor (offload)
**Algorithms:** DEFLATE, LZ4 (QAT 2.0+)
**Throughput:** 10+ Gbps per instance; multiple instances on Xeon platforms
**Latency:** Microsecond-scale per operation (unsuitable for single-page access)

**Pros:**
- Hardware LZ4 acceleration on Xeon 4xxx+ platforms
- Multiple instances available (scale with cores)
- Software fallback via QATzip library
- Supports both compression and decompression

**Cons:**
- Microsecond latency per operation — too slow for page-fault-path decompression
- Only available on Intel Xeon (not consumer CPUs)
- Driver complexity (kernel QAT driver, user-space library)
- Best for bulk operations, not per-page

**Verdict:** ✅ Adopt for **bulk compression offload** in the kernel module. When the idle page scanner identifies many cold pages, batch-compress them via QAT instead of consuming CPU cycles. Decompression stays in software (LZ4 at 3.7 GB/s is fast enough for page faults).

**Implementation notes:** Use the kernel QAT driver's async compression API. Submit a batch of pages, get callbacks when compression completes. No impact on the hot path.

---

### 2. CXL Inline Compression (Marvell Structera)

**Category:** Memory controller (inline)
**Algorithm:** Hardware LZ4 (in CXL path)
**Throughput:** Line-rate (CXL bandwidth)
**Latency:** Single-digit microseconds or lower

**What it does:** CXL-attached memory device with inline compression. Data is compressed as it passes through the CXL link, transparent to software. Effectively increases CXL memory capacity by the compression ratio.

**Pros:**
- Fully transparent to software (hardware handles everything)
- Compresses on the memory bus, not via CPU
- Increases effective CXL memory capacity
- No CPU cycles consumed for compression/decompression

**Cons:**
- Hardware-only (Marvell Structera product, not universally available)
- Proprietary — cannot study or modify the implementation
- CXL ecosystem is still maturing (limited deployment)
- Compression ratio depends on hardware implementation (likely LZ4-level)

**Verdict:** ✅ Target as a **hardware-accelerated backend**. MiniMem should detect Structera devices and delegate CXL memory compression to them. When unavailable, fall back to software compression before writing to CXL memory.

**Open questions:** Can we achieve similar results with software LZ4 on the CXL path? If software compression is fast enough, hardware acceleration is unnecessary. Need benchmarks comparing software LZ4 vs hardware inline on CXL.

---

### 3. IBM NX-842 / NX-GZIP

**Category:** Coprocessor (hardware accelerator on PowerPC / Z)
**Algorithm:** 842 (custom), GZIP
**Throughput:** Hardware-dependent
**Latency:** Low (cycle-level for memory controller on Z-series)

**What it does:** IBM PowerPC and Z-series mainframes have hardware compression in the memory controller or as a coprocessor (NX = Nest Accelerator). The 842 algorithm is designed for memory compression. Z-series has transparent memory compression in the memory controller.

**Pros:**
- Proven hardware memory compression (IBM Z has done this for decades)
- Memory controller integration on Z-series (cycle-level latency)
- Linux kernel already has NX-842 and NX-GZIP drivers (in mainline)

**Cons:**
- Only available on IBM PowerPC and Z-series hardware
- 842 is a proprietary IBM algorithm (limited ecosystem)
- Not relevant for x86 or ARM deployments (MiniMem's primary targets)

**Verdict:** 📋 Support as a **platform-specific backend**. MiniMem should use NX-842 when running on PowerPC/Z, but this is not a primary target. The lesson from IBM is that hardware memory compression works and is worth doing.

---

### 4. SIMD-Accelerated Software (AVX2, AVX-512, NEON)

**Category:** CPU instruction set extensions (not separate hardware)
**Algorithms:** LZSSE8 (SSE4.1), LZ4 (AVX2), libdeflate (AVX2/NEON)
**Throughput:** 4-5 GB/s decompression (LZSSE8); 3-4 GB/s (LZ4 AVX2)
**Latency:** Sub-microsecond per 4KB page

**Pros:**
- Available on virtually all modern x86-64 CPUs (SSE4.1 since 2008)
- No additional hardware required
- Runtime feature detection (CPUID) selects best available path
- Near-hardware throughput for decompression
- Applicable to the page-fault hot path (lowest possible latency)

**Cons:**
- Still software — cannot match hardware memory controller compression
- SIMD paths are architecture-specific (x86 vs ARM)
- AVX-512 can cause frequency throttling on some CPUs
- NEON port of LZSSE8 does not exist

**Verdict:** ✅ Adopt as the **primary acceleration path** for the page-fault decompression hot path. SIMD-accelerated software is the best option for per-page decompression because:
1. No hardware dependency
2. Sub-microsecond latency
3. Runtime detection provides automatic optimization
4. Covers the most common deployment scenario (x86-64 servers + ARM cloud)

**Implementation notes:**

| CPU Feature | Algorithm | Path |
|---|---|---|
| AVX-512 VL + BMI2 | LZ4 AVX-512 | Fastest x86 |
| AVX2 | LZ4 AVX2 | Second fastest x86 |
| SSE4.1 | LZSSE8 | Best ratio on x86 |
| SSE2 | LZ4 SSE2 | Baseline x86 |
| NEON | LZ4 NEON | ARM (Graviton, Apple Silicon) |
| Scalar | LZ4 scalar | Fallback |

A NEON port of LZSSE8 would be a novel contribution, bringing the fastest decompression to ARM servers.

---

### 5. FPGA Compression (Xilinx Vitis)

**Category:** FPGA acceleration
**Algorithms:** LZ4, DEFLATE (Xilinx Vitis libraries)
**Throughput:** 100+ Gbps (line-rate)
**Latency:** Microsecond-scale per operation

**Pros:**
- Extremely high throughput (line-rate compression)
- Programmable — can implement custom algorithms
- Available on cloud FPGAs (AWS F1, Azure FPGA)

**Cons:**
- High latency per operation (not suitable for page faults)
- FPGA programming complexity (HDL or Vitis HLS)
- Not available on most servers
- Cost and power consumption

**Verdict:** 📋 Investigate for **bulk CXL memory compression**. If MiniMem compresses data before sending it over CXL, an FPGA in the CXL path could compress at line rate. This is a niche scenario but could be valuable for CXL memory pooling. Low priority.

---

### 6. Hardware rANS (Range Asymmetric Numeral System)

**Category:** Research / experimental hardware
**Algorithm:** rANS (asymmetric numeral system)
**Throughput:** 121.2x encode, 70.9x decode speedup over Python baseline (from arXiv:2511.04684)

**Pros:**
- Higher compression ratios than classical LZ77 codecs
- Hardware implementation achieving high throughput
- Entropy coding can complement LZ77 (LZ + ANS pipeline)

**Cons:**
- Research-stage hardware (not commercially available)
- ANS is more complex than LZ77 for hardware implementation
- Not directly applicable to the page-fault path

**Verdict:** 📋 Monitor. rANS hardware could be a future acceleration path for cold-page recompression (where compression ratio matters more than decompression speed). Not actionable now.

---

## Hardware vs Software Decision Matrix

| Path | Latency | Throughput | Availability | MiniMem Role |
|---|---|---|---|---|
| SIMD (LZSSE8/LZ4) | <1 μs | 4-5 GB/s | Ubiquitous | **Primary decompression** |
| QAT (bulk) | ~10 μs | 10+ Gbps | Intel Xeon only | Bulk compression offload |
| CXL inline | ~1-5 μs | CXL rate | Structera only | CXL memory compression |
| IBM NX-842 | Low | HW-dependent | PowerPC/Z only | Platform backend |
| FPGA | ~10 μs | 100+ Gbps | Cloud FPGAs | CXL path (niche) |
| rANS HW | TBD | High | Research | Future (monitor) |

**Key insight:** For the page-fault decompression path, SIMD-accelerated software is the best option because it is universally available and sub-microsecond. Hardware acceleration is valuable for bulk compression (offloading CPU work) and CXL-path compression (reducing bandwidth), but not for per-page random access.

---

## Recommended Architecture

```
Hot path (page fault decompression):
  SIMD LZ4 / LZSSE8  →  sub-μs latency, universally available

Cold path (idle page compression):
  CPU LZ4  →  immediate compression when page goes cold
  QAT LZ4  →  batch offload when many pages to compress
  Zstd     →  recompression for long-idle pages (better ratio)

CXL path (memory tiering):
  Structera HW  →  inline compression if device present
  Software LZ4  →  compress before CXL write if no Structera

VRAM path (GPU buffers):
  nvCOMP GPU  →  batch compress/decompress on GPU
  Compute shader  →  inline decompress on demand
```