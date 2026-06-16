# 020 — Kernel Patch for do_swap_page() MiniMem PTE Marker Handling

## Summary

This document describes the required kernel patch to `mm/memory.c:do_swap_page()` to handle MiniMem PTE markers transparently. Without this patch, MiniMem markers fall through to the "unknown pte marker" case in `handle_pte_marker()` and return `VM_FAULT_SIGBUS` (SIGBUS to userspace). The patch adds a check for `PTE_MARKER_MINIMEM` before the unknown marker fallback, decompresses the page, installs a present PTE, and returns `VM_FAULT_NOPAGE`.

## Key Findings

### do_swap_page() flow (Linux 6.18)

```
do_swap_page(vmf)
  entry = softleaf_from_pte(vmf->orig_pte)
  if (!softleaf_is_swap(entry)):
    if softleaf_is_migration(entry):     → migration_entry_wait()
    else if softleaf_is_device_exclusive: → remove_device_exclusive_entry()
    else if softleaf_is_device_private:   → device private handling
    else if softleaf_is_hwpoison:         → VM_FAULT_HWPOISON
    else if softleaf_is_marker(entry):    → handle_pte_marker(vmf)
    else:                                  → VM_FAULT_SIGBUS
    goto out;  // skip all swap-in code
```

### handle_pte_marker() flow (Linux 6.18)

```
handle_pte_marker(vmf)
  marker = softleaf_to_marker(entry)
  if (marker & PTE_MARKER_POISONED):  → VM_FAULT_HWPOISON
  if (marker & PTE_MARKER_GUARD):     → VM_FAULT_SIGSEGV
  if (softleaf_is_uffd_wp_marker):    → pte_marker_handle_uffd_wp()
  // Unknown marker:
  return VM_FAULT_SIGBUS
```

### Current PTE marker bits (x86-64)

```
PTE_MARKER_UFFD_WP  = BIT(0)  — userfaultfd write-protect
PTE_MARKER_POISONED  = BIT(1)  — HWPOISON
PTE_MARKER_GUARD     = BIT(2)  — guard page
PTE_MARKER_MINIMEM    = BIT(3)  — MiniMem compressed page (our addition)
```

### MiniMem PTE entry layout (x86-64)

```
SWP_PTE_MARKER type = 31 (5-bit type field)
Offset field:
  Bits 0-2: PTE marker flags (UFFD_WP, POISONED, GUARD)
  Bit 3:    PTE_MARKER_MINIMEM
  Bits 4+:  compression map index (xarray key)
```

## The Patch

### Approach: Add MiniMem check in handle_pte_marker()

The cleanest approach is to add a check for `PTE_MARKER_MINIMEM` in `handle_pte_marker()` before the unknown marker fallback. This requires:

1. A kernel-visible `minimem_handle_fault()` function that:
   - Extracts the map index from the PTE marker offset field
   - Looks up the compressed page in the zsmalloc-backed xarray map
   - Decompresses the page into a newly allocated page
   - Installs a present PTE (using `set_pte_at()`, `mk_pte()`, `pte_mkwrite()`)
   - Frees the zsmalloc entry and removes the map entry
   - Returns `VM_FAULT_NOPAGE` on success

2. The function must be callable from `handle_pte_marker()` — either:
   a. **Direct call** (MiniMem compiled into the kernel): `#ifdef CONFIG_MINIMEM` guard
   b. **Function pointer** (MiniMem as module): register a callback via `minimem_register_fault_handler()`
   c. **Notifier chain** (most flexible): `atomic_notifier_call_chain(&minimem_fault_chain, ...)`

### Recommended: Function pointer approach (option b)

This allows MiniMem to remain a loadable module while providing the cleanest integration:

```c
// In include/linux/minimem.h
#ifdef CONFIG_MINIMEM
extern vm_fault_t (*minimem_fault_handler)(struct vm_fault *vmf);
#else
#define minimem_fault_handler NULL
#endif

// In mm/memory.c handle_pte_marker():
if (marker & PTE_MARKER_MINIMEM) {
    if (minimem_fault_handler)
        return minimem_fault_handler(vmf);
    pr_warn("minimem: fault on MINIMEM marker but handler not registered\n");
    return VM_FAULT_SIGBUS;
}

// In minimem module (init):
minimem_fault_handler = minimem_handle_swap_fault;

// In minimem module (exit):
minimem_fault_handler = NULL;
```

### Alternative: Notifier chain

More complex but supports multiple subscribers:

```c
// In include/linux/minimem.h
extern struct atomic_notifier_head minimem_fault_chain;

// In mm/memory.c handle_pte_marker():
if (marker & PTE_MARKER_MINIMEM) {
    ret = atomic_notifier_call_chain(&minimem_fault_chain, 0, vmf);
    if (ret == NOTIFY_OK)
        return VM_FAULT_NOPAGE;
    return VM_FAULT_SIGBUS;
}
```

### Current kprobe approach (out-of-tree)

Without a kernel patch, MiniMem uses a kprobe on `do_swap_page()`:

- Registers kprobe on `do_swap_page` symbol
- In the pre_handler: checks `vmf->orig_pte` for MiniMem marker
- If found: decompresses page, allocates new page, installs present PTE
- Uses `kallsyms_lookup_name` (resolved via kprobe trick) for unexported PTE functions

**Limitations:**
- kprobe overhead on every page fault (even non-MiniMem faults)
- Relies on unexported kernel symbols (`pte_offset_map_lock`, `set_pte_at`, `mk_pte`, `pte_mkwrite`, `pte_unmap_unlock`)
- `kallsyms_lookup_name` may not be available on hardened kernels
- Race conditions: PTE may change between kprobe pre_handler and do_swap_page's own PTE check
- Cannot safely return from kprobe pre_handler to skip do_swap_page processing

### Patch file structure

```
patches/
  minimem-6.18-01-handle-minimem-marker.patch    — mm/memory.c: add MINIMEM check
  minimem-6.18-02-pte-marker-bit.patch           — include/linux/swapops.h: add PTE_MARKER_MINIMEM
  minimem-6.18-03-fault-handler-registration.patch — kernel/minimem_fault.c: registration API
```

## Relevance to MiniMem

- **Without this patch**: MiniMem PTE markers cause SIGBUS on page fault (the "unknown marker" path)
- **With this patch**: MiniMem markers are intercepted transparently, pages are decompressed on access, and applications see no difference
- **The patch is the critical path to production readiness** — the kprobe approach is a development/proof-of-concept mechanism

## Open Questions

1. **Function pointer vs notifier chain?** Function pointer is simpler and sufficient for a single consumer. Notifier chain is more extensible but adds complexity.
2. **Should the handler run in interrupt context or process context?** Page faults run in process context (same as do_swap_page), so the handler can sleep (needed for zsmalloc reads and page allocation).
3. **TLB flushing**: After installing the present PTE, we need `flush_tlb_page(vma, address)`. This function is not exported to modules but IS available in-kernel.
4. **Memory accounting**: The newly allocated page should be charged to the process's mm (same as swap-in).
5. **Race with concurrent faults**: Two processes faulting on the same MiniMem PTE marker could both try to decompress. Need to check `pte_same()` after re-acquiring PTE lock (same pattern as do_swap_page).

## References

- `mm/memory.c` do_swap_page() — lines 4795-4866 (Linux 6.18)
- `mm/memory.c` handle_pte_marker() — lines 4585-4610 (Linux 6.18)
- `include/linux/swapops.h` — softleaf_t API, PTE marker definitions
- `include/linux/pgtable.h` — PTE manipulation inline functions
- MiniMem research/019 — PTE marking strategy and swap entry layout
- MiniMem src/kernel/minimem_hook.c — kprobe-based fault handler implementation