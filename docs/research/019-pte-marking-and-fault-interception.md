# 019 — PTE Marking and Page Fault Interception for MiniMem

## Summary

Research into how MiniMem can mark pages as "compressed in RAM" so that the page fault handler can find and decompress them. All 32 x86-64 SWP_TYPE slots are consumed, so we cannot define a new swap type. The solution is to use `SWP_PTE_MARKER` (type 31) with a custom bit `PTE_MARKER_MINIMEM = BIT(3)` in the offset field, storing the compression map index in bits 4–57. This gives us 54 bits of index space (2^54 entries, vastly more than needed).

## Key Findings

### PTE Swap Entry Layout (x86-64)

| Field | Bits | Description |
|---|---|---|
| type | 5 bits (58–62) | Swap type selector |
| offset | 58 bits (0–57) | Swap offset / marker data |

All 32 type values are allocated (types 0–23 for real swap, 24–31 for special purposes including type 31 = SWP_PTE_MARKER).

### PTE Marker Encoding

Current PTE markers use only the low 3 bits of the offset field:
- `PTE_MARKER_UFFD_WP = BIT(0)`
- `PTE_MARKER_POISONED = BIT(1)`
- `PTE_MARKER_GUARD = BIT(2)`
- `PTE_MARKER_MASK = 0x7`

Bits 3–57 (55 bits) are unused. MiniMem can define:

```c
#define PTE_MARKER_MINIMEM  BIT(3)

static inline swp_entry_t make_minimem_entry(unsigned long map_index)
{
    return swp_entry(SWP_PTE_MARKER, PTE_MARKER_MINIMEM | (map_index << 4));
}

static inline bool is_minimem_entry(swp_entry_t entry)
{
    return is_pte_marker_entry(entry) && (swp_offset(entry) & PTE_MARKER_MINIMEM);
}

static inline unsigned long minimem_entry_index(swp_entry_t entry)
{
    return swp_offset(entry) >> 4;
}
```

**54 bits of index space**: bits 4–57 of the offset. This can address 2^54 compressed pages (2^54 × 4KB = 72 PB), which is more than sufficient.

### Why Not Other Approaches?

| Approach | Viable? | Reason |
|---|---|---|
| New SWP_TYPE | No | All 32 slots consumed; requires kernel patch |
| Zswap backend registration | No | Zswap is hardcoded; no registration API |
| Zram block device | Wrong model | Zram is a swap backend; MiniMem compresses in-RAM pages before swap |
| kprobes on do_swap_page | Fragile | Tying to internal kernel functions breaks across versions |
| PTE_MARKER_MINIMEM | **Yes** | Uses existing SWP_PTE_MARKER type; offset bits 3–57 are free; page fault handler already calls pte_marker_handler() |

### Page Fault Path

When a MiniMem-compressed page is accessed:

1. CPU raises page fault → `do_page_fault()` → `handle_mm_fault()` → `__handle_mm_fault()`
2. For non-present PTEs: `do_swap_page()` is called
3. `do_swap_page()` checks `is_pte_marker_entry(entry)` first
4. Currently calls `pte_marker_handler()` which checks UFFD_WP, POISONED, GUARD
5. **We need to hook into this path** to check for PTE_MARKER_MINIMEM

### Hook Strategy: Replace PTE After Compression

The transparent compression flow:

1. **Idle scanner** identifies cold pages (PG_idle set, PG_young clear)
2. **Compress**: Call `minimem_compress_and_store()` → stores in zsmalloc, gets map index
3. **Replace PTE**: Remove the present PTE and install a MiniMem swap entry:
   ```c
   old_pte = ptep_get_and_clear(mm, addr, ptep);
   entry = make_minimem_entry(map_index);
   new_pte = swp_entry_to_pte(entry);
   set_pte_at(mm, addr, ptep, new_pte);
   flush_tlb_page(vma, addr);
   ```
4. **Free the page**: The original page is freed back to the buddy allocator

On page fault (decompress path):

1. `do_swap_page()` encounters MiniMem PTE marker
2. MiniMem handler: extract `map_index = minimem_entry_index(entry)`
3. Allocate a new page
4. Decompress from zsmalloc into new page
5. Remove map entry, free zsmalloc object
6. Install present PTE mapping the new page
7. Flush TLB, resume process

### Kernel Patch vs. Module-Only

**Module-only approach** (recommended for v0.4.0):
- Use `register_die_notifier()` to catch `DIE_PAGE_FAULT` events
- Problem: die notifiers run before the page fault handler; cannot easily intercept and resume
- Alternative: Use **kprobes** on `do_swap_page` to hook the PTE marker check path
- **Drawback**: kprobes are fragile, version-dependent, and not upstreamable

**Kernel patch approach** (recommended for v1.0.0):
- Patch `mm/memory.c: do_swap_page()` to call a `minimem_handle_fault()` function when it encounters `PTE_MARKER_MINIMEM`
- Register MiniMem as a handler via a new `register_minimem_handler()` in mm/
- This is the clean, upstreamable path

**Decision**: For v0.4.0, implement the PTE marker encoding and the decompress-on-demand infrastructure, but use debugfs/sysfs for manual testing (write a vaddr to compress, read to fault). The actual page fault interception requires either kprobes (fragile) or a kernel patch (not yet submitted). We'll document both paths and implement the clean API so that a future kernel patch is a one-line call insertion.

### PG_idle / PG_young for Candidate Selection

On 64-bit kernels with `CONFIG_PAGE_IDLE_FLAG=y`:
- `PG_idle` and `PG_young` are direct bits in `page->flags` (no `page_ext` overhead)
- A page is "idle" when `PG_idle=1, PG_young=0`
- The kernel's `page_idle_clear_pte_refs()` can mark pages idle based on PTE access bits
- MiniMem can use this directly: scan pages, compress idle ones, clear PG_idle after compression

### Shrinker for Memory Pressure Decompression

The modern shrinker API (Linux 6.x):

```c
struct shrinker *shrinker_alloc(unsigned int flags, const char *fmt, ...);
void shrinker_register(struct shrinker *shrinker);
void shrinker_free(struct shrinker *shrinker);
```

MiniMem should register a shrinker that decompresses least-recently-used pages under memory pressure. The shrinker:
- `count_objects`: Returns number of compressed pages stored
- `scan_objects`: Decompresses pages back to RAM, freeing zsmalloc memory
- Priority: `DEFAULT_SEEKS` (cost of recreating = decompress from zsmalloc)

## Relevance to MiniMem

- **RAM compression**: The PTE marker approach is the key enabler for transparent in-RAM compression. Without it, MiniMem can only compress pages explicitly via debugfs.
- **Decompression**: The fault path must be fast (<10μs target). Decompression from zsmalloc + PTE restoration must stay within this budget.
- **Shrinker**: Essential for system stability — under memory pressure, MiniMem must release compressed memory by decompressing pages back.

## Open Questions

1. **How to handle shared pages (fork, COW)?** A compressed page's PTE entry must be handled correctly for shared/COW mappings. This requires careful refcounting in the compression map.
2. **Should we use `soft_dirty` to detect writes to decompressed pages?** This would allow re-compressing pages that are written to and then become idle again.
3. **What about transparent huge pages (THP)?** MiniMem currently works at 4KB granularity. THP pages (2MB) would need splitting before compression.
4. **How to coordinate with zswap?** If both zswap and MiniMem are active, pages that MiniMem compresses should be excluded from swap consideration.

## References

- Linux PTE markers: `include/linux/swapops.h`, `include/linux/swap.h`
- Page idle tracking: `include/linux/page_idle.h`, `fs/proc/page_idle.c`
- Shrinker API: `include/linux/shrinker.h`
- Memory hotplug notifier: `include/linux/memory.h`
- Zswap architecture: `mm/zswap.c` (see research/004)
- macOS memory compression: `docs/research/005-wkdm-memory-compression.md`