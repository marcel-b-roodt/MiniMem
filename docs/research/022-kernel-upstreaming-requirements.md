# 022 — Kernel Upstreaming Requirements

## Summary

MiniMem needs to be bulletproof before it can be submitted to the Linux kernel mainline. This document tracks the requirements, open questions, and action items for kernel upstreaming.

## Key Findings

### What MiniMem needs for mainline acceptance

1. **PTE marker bit allocation**: MiniMem uses `BIT(3)` in `SWP_PTE_MARKER` offset (type 31). This needs formal allocation from the mm subsystem maintainers. Current `PTE_MARKER_UFFD_WP`, `PTE_MARKER_POISONED`, and `PTE_MARKER_GUARD` use bits 0-2. BIT(3) is unused but must be officially reserved.

2. **Small, reviewable patch set**: The kernel patches are only ~42 lines. This is small enough for review but must be clean, documented, and follow `Documentation/process/submitting-patches.rst`.

3. **Function pointer registration**: MiniMem uses a function pointer (`minimem_register_fault_handler`) for loadable module compatibility. This is the clean upstreamable approach — no `#ifdef CONFIG_MINIMEM` in `mm/memory.c`.

4. **Performance data**: Reviewers will want:
   - Latency measurements: decompression overhead vs page fault cost
   - Memory savings: real workload data (database, AI inference, desktop)
   - No regressions: no measurable overhead when scanner is disabled

5. **Safety guarantees**:
   - OOM behavior: what happens under extreme memory pressure?
   - Module unload: clean `rmmod` with no resource leaks (already verified)
   - Concurrency: RCU-safe lookups, spinlock-protected writes (already implemented)
   - Huge pages: what happens with THP? (needs testing)
   - KSM interaction: does MiniMem conflict with KSM same-page merging?

### Reviewer concerns to anticipate

| Concern | MiniMem's answer |
|---|---|
| "Why not extend zram/zswap?" | MiniMem compresses *still-mapped* pages (before eviction). zram/zswap only compress *after* eviction. Different page populations. Complementary, not competing. |
| "PTE marker collision" | BIT(3) is currently unused in mainline. Needs formal allocation. |
| "kprobe overhead" | kprobe on `do_swap_page` adds ~1-2μs per swap-in fault. Only affects unpatched kernels. With kernel patches, zero overhead. |
| "Double compression" | If VM reclaims a MiniMem-compressed page, it decompresses then zswap recompresses. Wasteful but correct. Future optimization: hand off compressed data directly. |
| "Module unload safety" | Already verified: shrinker drains, RCU grace periods, clean `rmmod`. |
| "NUMA awareness" | Per-CPU compression buffers are NUMA-local. zsmalloc pool is node-aware. |

### Submission checklist

- [ ] Formally allocate PTE_MARKER_MINIMEM bit (BIT(3)) with mm maintainers
- [ ] Clean up kernel patches for `checkpatch.pl --strict`
- [ ] Benchmark data: decompression latency vs page fault, real workloads
- [ ] Test with THP (transparent huge pages) enabled
- [ ] Test with KSM enabled
- [ ] Test under OOM conditions
- [ ] Test with zram enabled simultaneously
- [ ] Document `Documentation/vm/minimem.rst` for kernel tree
- [ ] Submit to linux-mm mailing list with cc: to relevant maintainers
- [ ] Respond to review feedback iteratively

### Timeline estimate

Realistic: **6-18 months** from first submission to mainline merge, assuming active engagement with reviewers. The patch set is small (~42 lines) which helps, but mm subsystem is conservative.

## Relevance to MiniMem

This drives the quality bar for the kernel module. Every item in the checklist above makes the module more robust even before mainline submission.

## Open Questions

- What kernel version should we target for first submission? (6.12 LTS? Latest stable?)
- Should we submit the full module or just the PTE marker registration API?
- How do we handle the `zs_obj_write/read` API dependency (zsmalloc is stable API but not designed for external consumers)?

## References

- `docs/research/019-pte-marking-and-fault-interception.md` — PTE marker design
- `docs/research/020-kernel-patch-do-swap-page.md` — kernel patch specification
- `docs/research/004-zram-zswap-architecture.md` — zram/zswap coexistence analysis
- `docs/research/021-zram-baseline-comparison.md` — MiniMem vs zram benchmark comparison
- Linux kernel `Documentation/process/submitting-patches.rst`