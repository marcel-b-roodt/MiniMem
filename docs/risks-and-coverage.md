# MiniMem DKMS Install — Risks, Coverage, and Limitations

## What Happens When You Install Locally

### Module-Only Install (no patches, no reboot)

1. `sudo modprobe minimem` — module loads immediately
2. Module registers kprobe on `do_swap_page`
3. Scanner mark pass works (sets idle flags on anonymous pages)
4. **Scanner sweep pass is disabled** — `minimem_hook_marker_ready()` returns false
5. `cat /sys/kernel/minimem/kernel_patches` → `0`
6. You can compress pages via debugfs: `echo <vaddr> > /sys/kernel/debug/minimem/compress`
7. Fault decompression works for pages compressed this way

**Risk level: Low.** Module operates in degraded mode. No automatic page compression.

### Full Install (patches applied, kernel rebuilt, rebooted)

1. Module registers fault handler via `minimem_register_fault_handler()`
2. Scanner sweep pass is enabled
3. Mark-sweep cycle runs automatically
4. Full transparent compression lifecycle: mark → sweep → fault → decompress
5. `cat /sys/kernel/minimem/kernel_patches` → `1`

**Risk level: Medium.** You modified your kernel. See below.

## Risks

### Low-Risk (module-only)

| Risk | Severity | Likelihood | Mitigation |
|---|---|---|---|
| Module load failure | Low | Low | Module fails to load gracefully; `dmesg` shows error |
| kprobe conflict | Low | Low | Another module already probing `do_swap_page`; our probe fails, module still loads (hook init is non-fatal) |
| Memory overhead | Low | Certain | ~88 bytes metadata per compressed page + per-CPU buffers (8KB each). Negligible unless millions of pages compressed |
| Decompression latency | Low | Certain | 0.09-10 μs per page. Always faster than SSD swap-in |
| Module unload issues | Low | Low | `rmmod` should work cleanly. If compressed pages exist, they must be decompressed first via shrinker |

### Medium-Risk (with kernel patches)

| Risk | Severity | Likelihood | Mitigation |
|---|---|---|---|
| Kernel build failure | Medium | Low | Patches are 42 lines; if they don't apply cleanly, `--dry-run` catches it before modifying source |
| Patch conflicts on kernel update | Medium | Medium | New kernel version may change `mm/memory.c` context; patches must be re-adapted per kernel release |
| Modified kernel instability | Medium | Low | Patches are minimal (add a case to `handle_pte_marker` + a function pointer); no control flow changes |
| SIGBUS on compressed pages after module unload | High | Low | If module unloads while pages are compressed, any access to those pages gets `VM_FAULT_SIGBUS`. **Always decompress all pages before unloading** (shrinker does this) |
| Double compression | Low | Low | If MiniMem and zswap both try to compress the same page; MiniMem only compresses still-mapped pages, zswap only compresses swapped pages — they shouldn't overlap |
| PTE corruption | High | Very Low | If our PTE manipulation has a bug, a process gets SIGSEGV. We check `pte_same()` before modifying, hold PTL throughout |

### High-Risk (unlikely but possible)

| Risk | Severity | Likelihood | Mitigation |
|---|---|---|---|
| Data loss | Critical | Very Low | If decompression produces wrong bytes, process reads corrupted data. **All algorithms are lossless with round-trip tests.** The risk is implementation bugs, not algorithm design |
| Deadlock | Critical | Very Low | If kprobe handler allocates memory while holding a lock, and that allocation triggers another fault. We use `GFP_ATOMIC` and per-CPU buffers to avoid this |
| Kernel panic | Critical | Very Low | If `do_swap_page` kprobe pre_handler corrupts registers or VM state. Our handler is read-only on registers and only modifies PTEs under PTL |

## Coverage Gaps — What We're NOT Testing

### 1. Real Hardware (not QEMU)

| Gap | Impact |
|---|---|
| Different CPU microarchitectures | Kprobe behavior may differ; timing assumptions may not hold |
| NUMA systems | Per-CPU buffer assumptions; zsmalloc node allocation not tested on multi-node |
| Large memory systems (>128 GB) | xarray scaling; zsmalloc pool fragmentation; scanner performance |
| Real workloads | We test synthetic patterns, not actual process memory |

### 2. Concurrency and Stress

| Gap | Impact |
|---|---|
| Concurrent faults on same compressed page | Two CPUs fault → two decompressions → who wins? We check `pte_same()` but race between alloc and install is not stress-tested |
| High fault rate under memory pressure | Shrinker decompresses while scanner compresses — live lock risk |
| Module load/unload under load | `rmmod` while faults are in flight — use-after-free if timing is wrong |
| OOM with compressed pages | If system is out of memory and MiniMem can't allocate decompression buffers → process gets SIGBUS |

### 3. Kernel Version Compatibility

| Gap | Impact |
|---|---|
| Kernels other than 6.18 | Patches only tested on 6.18; `do_swap_page` internals change across versions |
| LTS kernels (6.6, 6.12) | Different kprobe behavior; different `swap_entry` layout; `CONFIG_PAGE_IDLE_FLAG` may not exist |
| Future kernels (6.19+) | Patches may not apply; `handle_pte_marker` signature may change |

### 4. Scanner E2E

| Gap | Impact |
|---|---|
| Full mark → sweep → fault cycle | Only tested for 4 pages via debugfs; never tested with real scanner sweep on patched kernel |
| Scanner vs process lifecycle | What if process exits while scanner is compressing its pages? |
| Scanner vs fork/COW | Compressing a page that's shared via fork → corrupt both processes? We check `PageAnon` but COW pages are also anon before write |
| Scanner vs mremap/munmap | Page moves or disappears while scanner holds reference |

### 5. Failure Modes

| Gap | Impact |
|---|---|
| zsmalloc allocation failure | `zs_malloc` returns 0 → compression fails gracefully? Not tested under real pressure |
| kallsyms resolution failure | If kernel doesn't export `kallsyms_lookup_name` via kprobe — module loads but hook doesn't work |
| Decompression failure | If compressed data is corrupted → process gets SIGBUS. No recovery path tested |

## What You Should Test Before Production

1. **Module load/unload cycle** — `insmod` → verify sysfs → `rmmod` → verify clean unload
2. **Compress a page via debugfs** — access it → verify data matches
3. **Unload module with compressed pages** — does shrinker clean up? Or SIGBUS?
4. **Memory pressure** — fill RAM, enable scanner, verify no crashes
5. **Kernel update** — DKMS auto-rebuild: `pacman -Syu` → new kernel → module still works?

## Summary for Users

**If you install the DKMS module without kernel patches:**
- Safe to try. Module loads, provides debugfs compression, scanner mark pass.
- No automatic page compression (sweep disabled).
- `rmmod` cleans up. No persistent state after unload.

**If you apply kernel patches and rebuild:**
- You modified your kernel. Keep a backup kernel to boot into if something goes wrong.
- Scanner sweep will automatically compress idle pages.
- **Do not unload the module while compressed pages exist** — use shrinker first or reboot.

**In either case:**
- No data corruption risk from correct decompression (all algorithms are lossless, tested).
- The main risk is availability (SIGBUS/SIGSEGV if something goes wrong during decompression), not data integrity.