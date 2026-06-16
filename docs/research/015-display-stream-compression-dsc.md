# 015 — Display Stream Compression (DSC) & In-Flight Compression Standards

## Summary

Investigation of VESA Display Stream Compression (DSC) — a low-latency, visually-lossless compression codec used in DisplayPort, HDMI, and MIPI DSI display links — and related in-flight compression standards (CXL compression, PCIe data compression). The goal is to determine whether DSC's algorithms, hardware IP, or design patterns can be borrowed or adapted for MiniMem's lossless in-memory compression use case (RAM and VRAM).

---

## Key Findings

### DSC Technical Architecture

**Algorithm family:** Delta PCM + predictive coding + entropy coding. DSC is *not* a transform-based codec (no DCT). It operates on pixel groups in raster scan order.

**Encoding pipeline (per slice):**
1. **Color space conversion:** RGB → reversible YCgCo (lossless, integer-reversible transform)
2. **Pixel grouping:** 3 consecutive pixels for 4:4:4; 6 pixels for native 4:2:2/4:2:0
3. **Component splitting:** Each component (Y, Cg, Co) coded in independent substreams
4. **Prediction (3 modes, per-group selection):**
   - **MMAP (Modified Median Adaptive Prediction):** JPEG-LS-like median predictor — picks median of left/above-left/above neighbors
   - **Block Prediction (BP):** More complex — references a previous pixel group via offset; optional for decoders, negotiated at link handshake
   - **Midpoint Prediction:** Fallback, uses midpoint of component range
5. **Residual encoding:**
   - If recent pixels repeat → **Indexed Color History (ICH):** 32-entry table of recent pixel values; group references an entry directly. Excellent for computer-generated/text content.
   - Otherwise → **DSU-VLC (Delta Size Unit-Variable Length Coding):** Entropy coding of prediction residuals. Residual size signalled per group.
6. **Rate control:** Tracks flatness (low variance → fewer bits) and buffer fullness → adjusts quantization bit depth per pixel group. Operates in **constant bitrate (CBR)** or **variable bitrate (VBR)** modes. Minimum BPP = 6 bit/px; typical = 8 bit/px. VBR simply adds 0 BPP option (disables link temporarily).
7. **Slice structure:** Encoded groups combined into slices. Common: 100% or 25% picture width × 8/32/108 lines.

**Compression ratio:** Up to 3:1 (e.g., 24 bit/px → 8 bit/px). Practical range 1:1 to 3:1. VDC-M (mobile variant) achieves up to 5:1 with higher complexity.

**Latency:** Decoder adds ≤ 1 raster scan line of latency. For 4K@60Hz, that's ~8 μs. Decoder is ~100k gates.

### Lossy vs. Lossless — The Critical Distinction

**DSC is NOT mathematically lossless.** It is "visually lossless" per ISO/IEC 29170:
- An algorithm is visually lossless when "observers fail to correctly identify the reference image more than 75% of the trials" (interleaved A/B protocol)
- DSC satisfies this criterion for most images, but **some trials showed detectable compression on certain images** (research by Sudhama et al. 2018, Mohona et al. 2020)
- ISO 29170 explicitly allows excluding "particularly strong artifact" images from testing

**Why DSC cannot be used for MiniMem:**
- MiniMem requires **bit-exact** decompression. DSC introduces quantization noise in rate control.
- The rate controller adjusts quantization bit depth per pixel group, which discards least-significant bits of prediction residuals.
- No "lossless mode" exists in the DSC standard.

**What CAN be borrowed:** The *prediction and entropy coding* techniques (MMAP prediction, ICH, DSU-VLC) are algorithmically sound and could be adapted for lossless use if the quantization/rate-control step is removed.

### DSC Version History

| Version | Date | Changes |
|---|---|---|
| 1.0 | 2014-03 | Initial release (quickly deprecated) |
| 1.1 | 2014-08 | Replaced 1.0 |
| 1.2 | 2016-01 | Native 4:2:2/4:2:0 encoding, 14/16 bpc |
| 1.2a | 2017-01 | Minor algorithm tweaks; **current reference version** |
| 1.2b | 2021-08 | Editorial corrections only; implementations still use 1.2a |
| 1.3 | — | **Not released as of June 2026.** No public information on a DSC 1.3 spec. |

**DSC 1.2a** remains the active standard. DSC 1.2b is editorial-only. VESA has not announced a DSC 1.3.

**VDC-M** (VESA Display Compression - Mobile) is a separate, more aggressive variant: up to 5:1 compression, higher complexity, designed for MIPI DSI-2 in smartphones.

### Hardware Implementation

**Fixed-function hardware in GPUs:**
- **NVIDIA:** DSC encoder/decoder in display engine since Turing (GTX 16xx / RTX 20xx, 2018). Present in Ampere, Ada Lovelace, Blackwell.
- **AMD:** DSC encoder/decoder in Display Core Next (DCN) since RDNA2 (RX 6000, 2020). Part of the display controller, not the 3D/compute pipeline.
- **Intel:** DSC in display engine since Gen12 (Xe, 2020).
- **ARM/Mobile:** DSC in Mali display processors; VDC-M in mobile SoCs (Qualcomm, MediaTek).
- **IP vendors:** Synopsys, ARM, and others offer DSC encoder/decoder IP cores for SoC integration.

**Key constraint:** DSC hardware sits in the **display pipe**, not in the memory path or compute path. It compresses pixels *on their way out of the framebuffer to the display link*. It does NOT compress framebuffer data in VRAM.

### AMD Unified Compression Engine (UCE)

AMD's RDNA2 architecture introduced the **Unified Compression Engine (UCE)** as part of the display controller. UCE is AMD's marketing name for their integrated DSC implementation within the DCN (Display Core Next) block.

**What UCE actually does:**
- Implements DSC 1.2a encoding/decoding for DisplayPort/HDMI output
- Part of the display controller, NOT the memory controller or compute pipeline
- The "Unified" refers to sharing the DSC codec between multiple display outputs (unlike having separate encoders per pipe)

**UCE is NOT a general-purpose memory compression engine.** It is a DSC implementation scoped to the display output path. It cannot be repurposed for in-memory compression because:
1. It operates on display-formatted pixel data, not arbitrary memory pages
2. It sits between the framebuffer and the PHY, not between memory and the compute units
3. It introduces lossy quantization (DSC's rate control)
4. There is no software API or hardware path to submit arbitrary data to the UCE for compression

### Software Implementations

**VESA provides reference C source code** for DSC 1.2a/1.2b as a free download from their standards page. This is a bit-exact software encoder/decoder implementation used for:
- Compliance testing
- Hardware verification
- FPGA prototyping

The reference code is available at: https://fs16.formsite.com/VESA/form714826558/secure_index.html

**No open-source DSC implementation exists in mainline Linux.** The AMDGPU and i915 drivers contain hardware-specific DSC register programming but not a software codec. Mesa's AMD display code (DC) programs DSC parameters for display output.

### In-Flight Compression Standards (Beyond DSC)

| Standard | Scope | Lossless? | Compression Ratio | Status |
|---|---|---|---|---|
| **DSC** | Display link (DP, HDMI, DSI) | No (visually lossless) | Up to 3:1 | Deployed since 2014 |
| **VDC-M** | Mobile display (MIPI DSI-2) | No (visually lossless) | Up to 5:1 | Deployed in mobile SoCs |
| **CXL Compression** | CXL Type-3 devices (memory expansion) | Not standardized | N/A | CXL 3.0 spec mentions compression as optional; no standard algorithm defined |
| **PCIe Data Compression** | PCIe link | Not standardized | N/A | No compression in PCIe base spec. Some proprietary implementations (e.g., IBM Power) |
| **USB4/TB4** | USB4/TB tunnels | No native compression | N/A | Tunnels DisplayPort (which can use DSC) but no native data compression |

**CXL Compression:**
- CXL 3.0 (2022) mentions compression as an optional feature for CXL.mem but **does not define a compression algorithm or format**
- No shipping CXL devices implement compression as of 2026
- CXL Type-3 memory expanders could theoretically benefit from compression but the standard leaves it undefined — this is an opportunity for MiniMem

**PCIe Data Compression:**
- PCIe base specification has never included data compression
- IBM Power systems have proprietary link-level compression (not standard)
- Some RDMA/RoCE implementations use header compression but not payload compression
- PCIe 6.0/7.0 use PAM4 signaling and FLIT-based framing but no data compression

### GPU Command Stream Compression

**GPU command streams are NOT compressed** in current architectures:
- AMD: Command buffers (IB chains) sent via ring buffer to CP (Command Processor) — no compression
- NVIDIA: Push buffers — no compression
- Intel: Batch buffers — no compression

**Why command streams are hard to compress:**
- They are already compact (encoded GPU instructions, memory addresses, state values)
- Low entropy: lots of random-looking addresses and state register writes
- Extremely latency-sensitive (compression overhead would stall the GPU front-end)
- Variable-length commands make random access impossible without full decompression

**One partial exception:** Display command streams in some architectures use delta encoding for repeated state changes, but this is not general compression.

---

## Relevance to MiniMem

### What We Can Borrow from DSC

| DSC Technique | MiniMem Applicability | Adaptation Required |
|---|---|---|
| **MMAP prediction** (JPEG-LS median predictor) | Directly applicable to memory pages containing structured data (pointers, integers) | Remove quantization; use prediction residuals as compressed output |
| **Indexed Color History (ICH)** | Applicable to pages with repeated values (zero pages, sparse data, uniform blocks) | Adapt for 32/64-bit words instead of 24-bit pixels; increase table size for memory page diversity |
| **DSU-VLC entropy coding** | Applicable to residual encoding after prediction | Could replace or complement LZ4 for structured-data pages |
| **Slice-based parallel encoding** | Applicable: independent encoding of page segments | Map DSC slices to sub-page blocks (e.g., 128-byte sectors within a 4KB page) |
| **Rate control / quantization** | **NOT applicable** — this is the lossy part | Must be completely removed for MiniMem |
| **YCoCg color space** | **NOT applicable** — pixel-specific transform | No analog for arbitrary memory data |

### What We Cannot Use

1. **DSC hardware (UCE, DCN blocks):** Fixed-function display pipeline hardware. No path to submit arbitrary memory pages. Cannot be repurposed.
2. **DSC codec as-is:** Fundamentally lossy. The quantization step in rate control discards information.
3. **VDC-M:** Even more aggressively lossy; not suitable.
4. **DSC reference code:** Useful as an algorithm reference for prediction and entropy coding patterns, but cannot be used as a compressor for MiniMem without removing the lossy rate control.

### Novel Idea: "DSC-Lite" Lossless Predictor for Memory Pages

A DSC-inspired lossless compressor for structured memory pages could work as follows:

1. **Prediction:** Use MMAP-like median predictor on 32/64-bit words (left neighbor, above-left, above)
2. **Dictionary:** ICH-like table of 32-64 recent word values for repeated patterns
3. **Residual coding:** DSU-VLC or simple variable-length encoding of prediction residuals
4. **No quantization, no rate control** — pure lossless
5. **Per-sector independent decoding** for parallel decompression

This would be most effective on:
- Stack/heap pages (pointers, integers with sequential structure)
- AI weight tensors (low-variance blocks, quantized values)
- Page tables (repeated patterns, hierarchical structure)

It would be LEAST effective on:
- Encrypted data (random)
- Compressed data (already high entropy)
- Code pages (mixed patterns)

**Estimated compression ratio:** 1.5-2.5:1 on structured pages (similar to WKdm's performance profile, potentially better on word-aligned data due to ICH).

### Hardware Leverage Opportunities

1. **CXL Type-3 with compression:** MiniMem's kernel module could compress pages before sending them to CXL memory expanders. No hardware exists today, but the CXL spec leaves room for it.
2. **GPU compute shaders for VRAM compression:** Instead of repurposing DSC hardware, use GPU compute shaders to compress/decompress VRAM buffers (as explored in research/006-nvcomp-gpu-compression.md).
3. **Intel QAT / FPGA:** General-purpose compression accelerators (already in MiniMem's hardware acceleration plan). Not DSC-specific.

---

## Limitations and Constraints

1. **DSC is lossy — full stop.** No lossless mode. Cannot be used directly for MiniMem.
2. **DSC hardware is display-path only.** No mechanism to repurpose UCE/DCN for general memory compression.
3. **DSC is pixel-format aware.** It assumes 4:4:4, 4:2:2, or 4:2:0 color data. Arbitrary byte streams don't map cleanly.
4. **DSC's ICH table is small (32 entries).** Memory pages have much higher diversity than pixel colors. A memory-adapted ICH would need 64-256 entries.
5. **DSC processes in raster scan order.** Memory page access patterns may not benefit from raster-scan locality.
6. **CXL compression is undefined.** The spec mentions it as optional but defines no algorithm or interchange format.
7. **No in-flight compression standard exists for PCIe or GPU command streams.** This is a gap in the industry.
8. **VESA DSC 1.3 has not been announced.** No indication of a lossless mode being considered.

---

## Open Questions

1. **Can we build a lossless "DSC-inspired" predictor that outperforms WKdm on structured pages?** Requires implementation and benchmarking. The ICH component is novel compared to WKdm.
2. **What is the decompression latency of a lossless DSC-like predictor on a 4KB page?** Need to estimate: MMAP prediction + ICH lookup + DSU-VLC decode on ~1024 32-bit words. Target: < 1 μs.
3. **Would SIMD acceleration of a DSC-like predictor be feasible?** The prediction step (median of 3 neighbors) and ICH lookup are challenging to vectorize due to data dependencies. May need a modified predictor designed for SIMD from the start.
4. **Is there any GPU hardware (present or planned) that could be used for general in-VRAM compression?** As of 2026, none. DSC hardware is display-only. The only GPU-side compression is texture decompression (BCn/ASTC) in the texture samplers.
5. **Will CXL 4.0 or later standardize compression?** Unknown. If CXL standardizes a compression algorithm and interchange format, MiniMem should implement it.
6. **Are there patent/licensing concerns with using DSC prediction algorithms?** DSC is under VESA's RAND licensing. The prediction and entropy coding techniques (MMAP, DSU-VLC) have prior art in JPEG-LS. Need to review VESA's IP disclosures.

---

## References

1. VESA DSC Standard v1.2a (2017-01): https://glenwing.github.io/docs/VESA-DSC-1.2a.pdf
2. VESA DSC Standard v1.2b (2021-08): Available for free download from VESA
3. VESA Display Compression Codecs overview: https://vesa.org/vesa-display-compression-codecs/
4. DisplayPort FAQ (DSC section): https://www.displayport.org/faq/
5. Wikipedia — Display Stream Compression: https://en.wikipedia.org/wiki/Display_Stream_Compression
6. Walls & MacInnis, "VESA Display Stream Compression" (VESA ETP200, 2014-03): http://www.vesa.org/wp-content/uploads/2014/04/VESA_DSC-ETP200.pdf
7. Walls & MacInnis, "VESA Display Stream Compression: An Overview" SID Symposium Digest 45(1):360-363, 2014-06
8. ISO/IEC 29170-2:2015 — Evaluation procedure for nearly lossless coding
9. Sudhama et al., "Visually Lossless Compression of HDR Images" SID Symposium 49(1):1151-1154, 2018
10. Mohona et al., "Subjective Assessment of Stereoscopic Image Quality: The Impact of Visually Lossless Compression" QoMEX 2020
11. AMD RDNA2 architecture overview (UCE description): AMD developer documentation
12. CXL 3.0 Specification (2022): https://www.computeexpresslink.org/
13. MiniMem research/001 (WKdm), /006 (nvCOMP), /008 (AI weights), /014 (Linux-VRAM boundary)