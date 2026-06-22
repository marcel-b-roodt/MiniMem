# 024 — QEMU VM Test Infrastructure and CPU Overhead Guarantees

## Summary

MiniMem's transparent compression pipeline (scanner → compress → PTE replace → fault → decompress) has never been fully tested in an isolated QEMU VM with the kprobe fault handler. The existing E2E tests cover module load/unload, sysfs knobs, and debugfs compression, but do not verify the complete transparent roundtrip where the scanner compresses a page, replaces its PTE, and the kprobe fault handler decompresses it on access. Additionally, there is no measurement of scanner CPU overhead or decompression latency guarantees. This research doc defines the test infrastructure needed and the overhead targets.

## Key Findings

### 1. Kprobe Fault Handler Cannot Handle PTE Markers on x86-64

**Critical discovery**: On kernel 6.18 (x86-64), the kprobe on `do_swap_page()` approach does NOT work for transparent PTE marker decompression because:

1. **PTE markers go through `do_swap_page()`** — On 6.18, `handle_pte_fault()` dispatches PTE markers (SWP_PTE_MARKER type 31) to `do_swap_page()`, which then calls `handle_pte_marker()` internally.

2. **`pte_unmap_same()` bypasses PTE lock check on x86-64** — On x86-64, `sizeof(pte_t) == sizeof(unsigned long)` (8 bytes), so the SMP lock-protected comparison in `pte_unmap_same()` is skipped. It always returns 1 ("same"), meaning `do_swap_page()` continues with the original `vmf->orig_pte`.

3. **`handle_pte_marker()` returns `VM_FAULT_SIGBUS`** for unknown markers — MiniMem's PTE_MARKER_MINIMEM (BIT(3)) is not recognized by the kernel's `handle_pte_marker()`, which falls through to `return VM_FAULT_SIGBUS`. This kills the process with SIGBUS/SIGSEGV.

4. **Modifying `vmf->orig_pte` doesn't help** — Even if our kprobe handler installs a present PTE and modifies `vmf->orig_pte`, `pte_unmap_same()` bypasses the lock check, and `do_swap_page()` continues to process the original marker via `handle_pte_marker()`.

### Fix: Scanner Sweep Requires Kernel Patches

The scanner's sweep pass (PTE replacement) now requires `minimem_hook_marker_ready()` (kernel patches detected), not just `minimem_hook_fault_handler_ready()` (kprobe fallback). On unpatched kernels:

- **Mark pass**: Works normally (marks pages idle, clears young flag)
- **Sweep pass**: Skipped (no PTE replacement, no transparent decompression)
- **Debugfs compress_vaddr**: Still works (explicit compression via debugfs)
- **Kprobe fault handler**: Still registered but only useful for debugfs-triggered compression

This means transparent compression (scanner → compress → PTE replace → fault → decompress) only works on **patched kernels** where `handle_pte_marker()` has been modified to call MiniMem's fault handler.

### 2. Current Test Coverage

| Test Area | Current Coverage | Gap |
|---|---|---|
| Module load/unload | kselftest ✅ | No drain-and-restore verification |
| Sysfs knobs | kselftest ✅ | No adaptive interval verification |
| Debugfs compress | E2E test ✅ | Not transparent — uses debugfs, not scanner |
| PTE marker roundtrip | debugfs ✅ | Not via scanner + fault handler |
| Scanner compression | E2E test (basic) ✅ | No data integrity verification after decompress |
| Kprobe fault handler | Not tested ❌ | The most common path on unpatched kernels |
| Drain-and-restore | Not tested ❌ | Critical data safety path |
| Skip filters | Not tested ❌ | VM_LOCKED, mlocked, shared pages |
| Adaptive interval | Not tested ❌ | Back-off behavior |
| CPU overhead | Not measured ❌ | No quantitative guarantee |
| Decompression latency | Not measured ❌ | Target: <10μs per page |
| Large memory | Not tested ❌ | 4GB+ VM |

### 2. Why the Kprobe Path is Critical

On stock kernels (no MiniMem patches applied), the fault interception path is:
1. Process accesses compressed page → page fault → `do_swap_page()` called
2. Our kprobe pre-handler fires on entry to `do_swap_page()`
3. Pre-handler checks `is_minimem_pte(orig_pte)` → true
4. Calls `minimem_handle_swap_fault()` which:
   - Looks up vaddr in compression map
   - Allocates new page
   - Reads compressed data from zsmalloc
   - Decompresses into new page
   - Installs present PTE
   - Returns 1 (telling kprobe "handled, skip do_swap_page")
5. `do_swap_page()` continues but the PTE is now present

This path MUST be tested end-to-end because:
- The kprobe fires on **every** swap entry, not just MiniMem ones
- If the kprobe handler returns 0 for non-MiniMem entries, it must not interfere
- If the kprobe handler has a bug, processes get SIGBUS or kernel panic
- The kprobe pre-handler receives `struct pt_regs *regs` and extracts `vmf` from `regs->di` — this is ABI-dependent

### 3. Transparent Compression Test Design

The test needs a static binary that:
1. `mmap`s anonymous pages
2. Writes a known pattern (e.g., 0xAA repeated)
3. `madvise(MADV_DONTNEED)` or simply sleeps to let pages go idle
4. Enables the scanner
5. Waits for the scanner to compress the pages (1-2 sweep cycles)
6. Verifies the PTE has been replaced (read `/proc/self/pagemap`)
7. Re-reads the pages — triggers kprobe fault handler → decompression
8. Verifies data integrity: `memcmp(buf, expected_pattern, PAGE_SIZE) == 0`
9. Disables scanner
10. Unloads module, checks drain-and-restore

### 4. CPU Overhead Targets

Based on the design goals (decompression overhead below page fault cost ~2-10μs):

| Metric | Target | Measurement Method |
|---|---|---|
| Scanner CPU (active) | <5% single core | `/proc/[pid]/stat` utime+stime over wall time |
| Scanner CPU (idle, backed off) | <0.5% single core | Same, after 5+ empty cycles |
| Decompression latency | <10μs per page | `decompress_avg_ns` sysfs |
| Compression latency | <50μs per page | `compress_avg_ns` sysfs |
| Fault handler overhead | <15μs total | `hook_faults_ns / hook_faults` sysfs |
| Mark pass throughput | >100k pages/s | `scanner_pages_scanned` delta over time |
| Sweep pass throughput | >50k pages/s | `scanner_pages_compressed` delta over time |

### 5. Adaptive Interval Behavior

The scanner interval adapts:
- Base: `scanner_interval_ms` (default 1000ms)
- Each empty cycle (0 pages compressed): +2000ms, max 30000ms
- Each productive cycle: reset to base
- Skip-list decays every 8 empty cycles

Expected behavior in QEMU VM (768MB, 4 vCPUs):
- First 2-3 cycles: mark pass identifies idle pages (1-2s each)
- Cycles 3-5: sweep compresses pages (1-2s each)
- After most pages compressed: backs off to 3s, then 5s, then max 30s
- CPU during backed-off cycles: near zero (just sleep + quick check)

## Relevance to MiniMem

This is the critical verification step before any real-world deployment. The kprobe path is what most users will experience (unpatched kernels), and it has never been E2E tested. CPU overhead guarantees are needed to justify the "transparent" and "fast" claims in the project goal.

## Open Questions

1. Should we add a `/proc/self/pagemap` reader to verify PTE replacement, or is data integrity sufficient? → Data integrity is sufficient for now; pagemap verification would require custom kernel patches.
2. What's the minimum VM size needed to trigger meaningful scanner activity? → 768MB is sufficient for basic testing; 4GB+ needed for adaptive backoff and large-memory verification.
3. Should the CPU overhead test run in the VM or on bare metal? → Both; VM for quick verification, bare metal for accurate latency numbers.
4. Do we need to test with different kernel versions in the VM? → Yes; 6.x is our target, but testing on 6.2+ (shrinker API) and latest LTS is important.
5. **Can we make the kprobe approach work on x86-64?** → No, not for PTE marker faults. The `pte_unmap_same()` fast path on x86-64 bypasses the PTE lock check, so our handler's PTE modification is not detected by `do_swap_page()`. The only reliable approach is kernel patches to `handle_pte_marker()`.
6. **Could we use ftrace to skip `do_swap_page()` entirely?** → Possible but fragile; ftrace's `FTRACE_OPS_FL_IPMODIFY` could modify the return address, but this is architecture-specific and may conflict with other tracers. The kernel patch approach is more reliable.
7. **What about using a kretprobe to override the return value?** → A kretprobe fires after `do_swap_page()` returns, which is too late — the SIGBUS has already been delivered.

## References

- `tests/kernel/test_transparent_e2e.c` — existing static binary for PTE roundtrip
- `tests/kernel/minimem_e2e_test.sh` — existing E2E test script
- `tests/kernel/test_cpu_overhead.c` — CPU overhead measurement static binary
- `tests/kernel/test_drain_restore.c` — drain-and-restore verification static binary
- `tests/kernel/test_scanner_roundtrip.sh` — full scanner+fault handler roundtrip shell test
- `vm-test-minimem.sh` — VM test harness
- `src/kernel/minimem_scanner.c` — adaptive interval implementation
- `src/kernel/minimem_hook.c` — kprobe fault handler (now sweep-gated by minimem_hook_marker_ready)
- `docs/research/020-kernel-patch-do-swap-page.md` — kernel patch for handle_pte_marker()
- Linux kernel `mm/memory.c` — `do_swap_page()`, `pte_unmap_same()`, `handle_pte_marker()`