# 014 — Linux Kernel–VRAM Architectural Boundary & Compression Insertion Points

## Summary

Investigation of how the Linux kernel interacts with GPU VRAM, whether the core mm/ subsystem has any visibility into VRAM pages, what driver-level interfaces exist for VRAM management, and where compression could realistically be inserted. Also covers CXL Type-3 memory, AMD APU unified memory, and any existing VRAM compression or overcommit work.

---

## Key Findings

### 1. The core kernel mm/ subsystem has ZERO visibility into discrete GPU VRAM

The Linux kernel's memory management subsystem (`mm/`) manages system RAM pages via `struct page`, page tables, the buddy allocator, and the LRU. **Discrete GPU VRAM is entirely outside this world.** The kernel cannot see, page out, compress, or otherwise manage VRAM pages. VRAM is accessed by the CPU via PCI BAR memory-mapped I/O (MMIO) apertures, not through normal page-table-managed memory.

- VRAM is physically on the GPU card, connected via PCIe
- The CPU accesses VRAM through a limited-size MMIO aperture (the PCI BAR window)
- There is no `struct page` backing for VRAM — the kernel's `mm/` code has no data structures tracking VRAM pages
- The kernel cannot fault on VRAM pages, cannot swap them, cannot run kswapd on them
- This is fundamentally different from system RAM, where every page has a `struct page` and participates in the LRU

### 2. VRAM management lives entirely inside the DRM/TTM/GEM driver stack

The DRM (Direct Rendering Manager) subsystem manages GPU memory through two frameworks:

| Framework | Scope | VRAM support |
|---|---|---|
| **TTM** (Translation Table Manager) | Discrete GPUs with dedicated VRAM | Yes — full VRAM management |
| **GEM** (Graphics Execution Manager) | UMA/integrated GPUs (system RAM only) | No — no VRAM management |

**TTM** is the VRAM manager. It provides:

- `ttm_resource_manager` — per-memory-type resource allocator (VRAM, GTT, system)
- `ttm_buffer_object` — the fundamental VRAM buffer abstraction
- `ttm_place` / `ttm_placement` — buffer placement policies
- `ttm_device` — device-level memory management
- LRU-based eviction: when VRAM is full, TTM evicts BOs to system RAM (GTT domain) or swap
- `ttm_tt` — system-RAM backing for evicted buffers (uses `struct page` once moved to system RAM)

**GEM** is simpler — it only manages system-RAM-backed buffer objects via shmem. Intel's i915 uses GEM because Intel integrated GPUs share system RAM.

### 3. AMDGPU VRAM management specifics

AMDGPU is the most relevant open-source driver for VRAM compression research. Key structures:

- **`amdgpu_vram_mgr`** — TTM resource manager for VRAM domain. Allocates VRAM space via a DRM MM range allocator (`drm_mm`). Tracks usage via `ttm_resource_manager.usage`.
- **`AMDGPU_GEM_DOMAIN_VRAM`** — the VRAM memory domain. On discrete GPUs, this is the physical VRAM on the card. On APUs, this is BIOS-carved-out system memory.
- **`AMDGPU_GEM_DOMAIN_GTT`** — GPU-accessible system memory, mapped through the GART (Graphics Address Remapping Table).
- **Buffer object lifecycle:** Created in VRAM → if pressure, evicted to GTT (system RAM) → if still pressure, swapped to shmem/disk.

**VRAM eviction path (existing):**
```
VRAM (hot) → GTT/system RAM (evicted) → shmem/swap (backed up)
```

This is the **only existing "compression" of VRAM** — eviction to system RAM. TTM already handles this via its LRU and `ttm_bo_evict()` path.

**Key AMDGPU source files:**
- `drivers/gpu/drm/amd/amdgpu/amdgpu_vram_mgr.c` — VRAM allocation
- `drivers/gpu/drm/amd/amdgpu/amdgpu_ttm.c` — TTM integration, VRAM/GTT managers
- `drivers/gpu/drm/amd/amdgpu/amdgpu_object.c` — Buffer object operations
- `drivers/gpu/drm/amd/amdgpu/amdgpu_vm.c` — GPUVM page table management

### 4. The GPUVM layer — a second page table system

AMDGPU manages its own page tables (GPUVM), entirely separate from the CPU's page tables. Each process can have its own GPUVM address space with its own page tables. The GPU hardware walks these page tables to translate GPU virtual addresses to VRAM/system-RAM physical addresses.

- GPUVM supports mapping both VRAM pages and system-RAM pages
- GPU page faults are generated when a GPU accesses an unmapped or evicted page
- The driver handles GPU page faults by moving the buffer back to VRAM

This is architecturally analogous to the CPU's page fault → swap-in path, but it's entirely within the GPU driver.

### 5. DRM GPUVM — a new shared abstraction (2023–2025)

Danilo Krummrich's DRM GPUVM work (merged 2024) provides a **driver-agnostic** GPU virtual memory management framework. It offers:

- Common GPU VA space management
- Shared dma-resv for per-VM buffers
- External GEM object tracking
- Evicted object tracking and validation

This is significant for MiniMem because it provides a **cross-driver abstraction point** where compression could be inserted. If we add compression at the GPUVM layer, it works for any driver using DRM GPUVM (nouveau, future drivers, potentially AMDGPU if it adopts GPUVM).

### 6. Nouveau VRAM management

Nouveau (NVIDIA open-source driver) also uses TTM for VRAM management. It has:
- `nouveau_vmm` — GPU virtual memory manager
- TTM-based buffer object management (similar pattern to AMDGPU)
- The new UVMM (user VM) code uses DRM GPUVM

Nouveau is less mature than AMDGPU but follows the same TTM architecture.

### 7. i915 / Intel — no VRAM to manage

Intel integrated GPUs (i915, xe) use system RAM exclusively. GEM manages buffer objects backed by shmem. There is no VRAM domain. Compression of GPU buffers on Intel is therefore a system-RAM compression problem (covered by MiniMem's Stage 1 kernel module).

### 8. CXL Type-3 devices — memory that IS visible to mm/

CXL Type-3 memory expanders are a **fundamentally different** architecture from discrete GPU VRAM:

| Property | Discrete GPU VRAM | CXL Type-3 Memory |
|---|---|---|
| CPU access | Via MMIO BAR only | Via coherent load/store (CXL.mem) |
| Kernel mm/ visibility | None — no `struct page` | Full — added via memory hotplug |
| NUMA placement | Not a NUMA node | Appears as a NUMA node |
| Swap/compress by kernel | Impossible | Fully possible — same as RAM |
| Latency | High (PCIe round-trip) | Lower (coherent CXL.cache/mem) |
| Management | GPU driver (TTM) | Linux mm/ + CXL subsystem |

**CXL Type-3 volatile memory is added to the system via the `dax_kmem` driver**, which converts CXL memory regions into normal NUMA memory that the page allocator manages. This means:

- CXL memory participates in the normal LRU
- CXL memory can be swapped
- CXL memory **can be compressed by MiniMem's kernel module** just like regular RAM
- The memory tier subsystem (`drivers/dax/kmem.c`, `mm/memory-tiers.c`) places CXL memory in a lower tier (higher NUMA distance) than DRAM

**CXL is the best path for MiniMem's kernel-level compression to reach "VRAM-like" memory.** CXL memory is slower than DRAM but faster than disk; compressing it extends effective capacity just like compressing regular RAM.

**CXL-specific compression opportunity:** CXL memory is a natural candidate for demotion-based compression. When DRAM is under pressure, pages are demoted to CXL. If CXL is also under pressure, compressing CXL pages before going to disk would be a novel contribution.

### 9. AMD APU unified memory architecture

AMD APUs (Ryzen with integrated Radeon, Ryzen AI "Strix Point/Halo") use a unified memory architecture:

- The GPU and CPU share the same physical DRAM
- VRAM domain on APUs = BIOS-carved-out region of system RAM (the "UMA carveout")
- The carveout is typically 512 MB – 2 GB, reserved by BIOS at boot
- This memory is NOT managed by the Linux page allocator — it's allocated by TTM/amdgpu_vram_mgr
- The GTT domain maps normal system RAM into the GPU's address space via GART

**Implications for compression on APUs:**
- The UMA carveout is effectively reserved system RAM that the kernel mm/ cannot touch
- If we could convince the kernel mm/ to manage this region (or a portion of it), compression would work exactly like system RAM compression
- However, the carveout exists because the GPU needs physically contiguous, low-latency memory — compressing it would add decompression latency on GPU access
- More promising: APUs can use system RAM (GTT domain) for GPU buffers, and those GTT pages ARE normal `struct page`-backed pages that the kernel can compress via MiniMem's Stage 1 module

**The AMD APU situation is nuanced:** For GTT-backed GPU buffers, kernel-level compression works today. For the UMA carveout, compression would require cooperation from amdgpu's VRAM manager.

### 10. Existing kernel patches/proposals for VRAM compression

**There are no mainstream kernel patches for VRAM compression.** The existing VRAM "overcommit" mechanism is simply TTM eviction to system RAM. Specific findings:

- **TTM eviction = the existing overcommit mechanism.** TTM already overcommits VRAM by evicting cold buffers to system RAM. This is not compression — it's migration to a slower tier.
- **No VRAM compression patches found** in linux-next, LKML archives (2020–2026), or dri-devel discussions.
- **AMDgpu has a swap/backup mechanism** (`ttm_tt` with `TTM_TT_FLAG_SWAPPED` and the newer `ttm_tt_setup_backup()` / `TTM_TT_FLAG_BACKED_UP` flags). This writes evicted buffer contents to shmem/swap — effectively "compressing" by moving to disk, but without actual in-memory compression.
- **DRM GPUVM's evicted object tracking** (2024) is the closest to a framework for managing "compressed-out" GPU objects, but it tracks eviction to system RAM, not in-place compression.
- **TTM pool shrinking** (`ttm_pool_shrink`) — the TTM page pool participates in the kernel's memory shrinker infrastructure. When system RAM is under pressure, the shrinker can free pages from the TTM pool. This is system-RAM pressure relief, not VRAM compression.
- **No proposals for in-VRAM compression** (compressing VRAM buffers in-place to save VRAM space) have been found.

---

## Compression Insertion Points

Based on the above findings, here are the realistic insertion points for VRAM compression, ordered by feasibility:

### Layer 1: TTM Resource Manager (highest feasibility for kernel-level work)

**Where:** `ttm_resource_manager` callbacks in each driver's VRAM manager.

**How:** Add a compression layer between `ttm_resource_manager.alloc()` and the actual VRAM allocation. When VRAM pressure occurs, instead of evicting a buffer to system RAM, compress it in-place in VRAM.

**Pros:**
- Works within existing TTM infrastructure
- Can be done per-driver (AMDGPU first)
- Compressed buffers still in VRAM — no PCIe round-trip on decompress
- Decompression can use GPU DMA copy engine (SDMA on AMD)

**Cons:**
- Requires modifying each driver's TTM implementation
- Compressed data must still fit in VRAM (metadata overhead)
- Decompression triggers GPU page fault → driver must handle recovery
- Not cross-driver portable without DRM GPUVM-level abstraction

**Key files to modify:**
- `drivers/gpu/drm/amd/amdgpu/amdgpu_vram_mgr.c` — add compress/decompress hooks
- `drivers/gpu/drm/amd/amdgpu/amdgpu_ttm.c` — modify eviction path
- `drivers/gpu/drm/ttm/ttm_resource.c` — add compression resource type

### Layer 2: DRM GPUVM (cross-driver, but newer and less mature)

**Where:** `drm_gpuvm` evicted object tracking and validation.

**How:** Add a "compressed" state between "in-VRAM" and "evicted-to-system-RAM". When a BO is compressed, it remains in VRAM but in compressed form. The GPUVM tracks it as a new state.

**Pros:**
- Cross-driver abstraction
- Aligns with the GPUVM state machine (idle → evicted → validated)
- Could work for nouveau, future drivers

**Cons:**
- GPUVM is still maturing; adding compression state is a significant extension
- AMDGPU hasn't adopted GPUVM yet
- Requires GPU-side decompression mechanism

### Layer 3: Userspace / Application-Advised (most deployable today)

**Where:** Vulkan memory allocator, CUDA custom allocator, or Mesa Gallium3D driver.

**How:** Application or runtime marks certain buffers as compressible. On VRAM pressure, a userspace daemon or runtime compresses idle buffers using GPU compute (nvCOMP or custom shader). Decompress on next use.

**Pros:**
- No kernel changes required
- Can deploy today with CUDA/Vulkan
- PyTorch custom allocator integration exists as a proven path

**Cons:**
- Not transparent — requires application/runtime cooperation
- Userspace cannot hook into TTM eviction decisions
- No GPU page fault mechanism for on-demand decompress (must predict need)

### Layer 4: CXL Memory (for CXL-attached memory, not discrete VRAM)

**Where:** MiniMem's existing kernel module, applied to CXL NUMA nodes.

**How:** The CXL memory added via `dax_kmem` is normal kernel-managed memory. MiniMem's kernel module can compress pages in CXL nodes exactly as it does for regular DRAM nodes.

**Pros:**
- Works today with MiniMem's existing Stage 1 infrastructure
- CXL memory is a natural compression target (slower tier, more capacity-constrained)
- No GPU driver changes needed
- Kernel mm/ already manages CXL pages

**Cons:**
- Only applies to CXL-attached memory, not discrete GPU VRAM
- CXL memory is used differently from VRAM (compute vs. graphics)

---

## Relevance to MiniMem

### Immediate (Stage 1)

- **CXL memory compression** is a direct fit for the kernel module. CXL pages are normal `struct page`-backed memory that the kernel manages. No new infrastructure needed.
- System-RAM-backed GPU buffers (GTT domain on AMDGPU, all buffers on Intel i915) are normal pages that can be compressed by MiniMem's existing module.

### Medium-term (Stage 2)

- **TTM-layer VRAM compression** is the most feasible kernel-level insertion point for discrete GPU VRAM. This requires modifying AMDGPU's `amdgpu_vram_mgr` to add an in-place compression path as an alternative to eviction-to-system-RAM.
- The **key design decision** is whether compression happens on the CPU (read VRAM via MMIO BAR → compress → write back) or on the GPU (dispatch a compute shader to compress). CPU-side compression is simpler but slower (PCIe round-trip). GPU-side compression is faster but requires a GPU compute context.
- **Decompression must happen on the GPU** for latency reasons — a GPU page fault on a compressed buffer triggers SDMA-based decompression.

### Long-term (Stage 3+)

- **DRM GPUVM-level compression** would be the cross-driver abstraction, but this requires GPUVM adoption by major drivers and a compression extension to the GPUVM state machine.
- **CXL Type-3 with GPU direct access** — future CXL devices may allow GPUs to directly access CXL-attached memory. If CXL memory becomes GPU-visible, compression at the CXL layer benefits both CPU and GPU consumers.

---

## Open Questions

1. **Can we intercept GPU page faults for compressed VRAM buffers?** AMDGPU's GPUVM page fault handler (`amdgpu_vm_fault`) is the natural decompression trigger, but modifying it to handle compressed buffers is non-trivial. Need to investigate whether a "compressed PTE" flag can be added to GPUVM page tables.

2. **What is the latency budget for VRAM decompression?** GPU page faults are already expensive (~10 μs). If decompression adds <10 μs on top, total ~20 μs may be acceptable for truly idle buffers. Need benchmarking.

3. **Can SDMA copy engines be used for decompression?** AMDGPU's SDMA engines can do memory-to-memory copies. If we store compressed data in VRAM and dispatch an SDMA-based decompress, we avoid GPU compute overhead. But SDMA engines don't support general computation — only copies and fills. We may need a lightweight compute shader instead.

4. **Is there a way to add `struct page` backing for VRAM?** Some experimental work (e.g., `ZONE_DEVICE` for device memory) exists for persistent memory and HMM (Heterogeneous Memory Management). Could VRAM be mapped as `ZONE_DEVICE` memory with `struct page`? This would give mm/ visibility, but the latency and coherence implications are severe.

5. **What about HMM (Heterogeneous Memory Management)?** Linux has HMM (`mm/hmm.c`) which provides mirror-like tracking of CPU page tables for device drivers. HMM is used for migrating pages between system RAM and device memory. Could HMM be extended to support compression of device-local pages?

6. **NVIDIA's forthcoming open-source kernel modules** — NVIDIA is gradually opening their kernel driver. If they adopt TTM or provide similar hooks, could we add compression there too?

---

## References

- [DRM Memory Management — Kernel Docs](https://docs.kernel.org/gpu/drm-mm.html) — TTM and GEM reference
- [AMDGPU Driver Core — Kernel Docs](https://docs.kernel.org/gpu/amdgpu/driver-core.html) — Memory domains, buffer objects, GPUVM
- [AMDGPU Memory Stats — Kernel Docs](https://docs.kernel.org/gpu/amdgpu/driver-misc.html) — VRAM/GTT sysfs stats
- [Compute Express Link — Kernel Docs](https://docs.kernel.org/driver-api/cxl/index.html) — CXL subsystem overview
- [CXL Theory of Operation — Kernel Docs](https://docs.kernel.org/driver-api/cxl/theory-of-operation.html) — CXL memory device architecture
- [DRM GPUVM RFC — LWN/dri-devel](https://lwn.net/Articles/949845/) — Danilo Krummrich's GPUVM feature series
- [TTM source](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/gpu/drm/ttm/) — TTM buffer object framework
- [AMDGPU VRAM manager source](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/gpu/drm/amd/amdgpu/amdgpu_vram_mgr.c) — VRAM allocation implementation
- [CXL DAX/kmem driver](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/dax/kmem.c) — CXL memory hotplug to kernel page allocator
- [HMM — Heterogeneous Memory Management](https://docs.kernel.org/core-api/mm/hmm.html) — Device memory management via CPU page table mirroring