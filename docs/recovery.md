# MiniMem Recovery and Disabling Guide

## Quick Disable (if something seems wrong)

### Stop the scanner (module stays loaded, no new compression)
```bash
echo 0 | sudo tee /sys/kernel/minimem/scanner_enabled
```

### Unload the module
```bash
# Stop scanner first, then unload
echo 0 | sudo tee /sys/kernel/minimem/scanner_enabled
sudo rmmod minimem
```
All compressed pages are decompressed back to normal memory on unload. **No data is lost.**

### Disable auto-load on boot
```bash
sudo systemctl disable minimem.service minimem-load.service
sudo rm /usr/lib/modules-load.d/minimem.conf
```

### Full uninstall
```bash
sudo ./scripts/local-install.sh --uninstall
```

## Emergency Recovery (kernel panic on boot)

If MiniMem causes a kernel panic during boot:

1. **Blacklist via GRUB** — edit the boot entry at the GRUB menu:
   - Press `e` at the GRUB menu
   - Find the `linux` line
   - Append: `minimem.blacklist=1`
   - Press `Ctrl+X` to boot
   - The module will be completely ignored by the kernel

2. **Permanent blacklist** — once booted:
   ```bash
   echo "blacklist minimem" | sudo tee /etc/modprobe.d/minimem.conf
   sudo ./scripts/local-install.sh --uninstall
   ```

3. **If GRUB is inaccessible** — use a live USB:
   - Boot from USB
   - Chroot into the system
   - Run `echo "blacklist minimem" > /etc/modprobe.d/minimem.conf`
   - Or remove the module: `rm /lib/modules/$(uname -r)/updates/minimem.ko*`

## Decompression Safety

MiniMem is designed so that **no data is ever at risk**:

- **Module unload**: All compressed pages are decompressed back to normal memory before the module releases its last reference. The `rmmod` path is safe.

- **OOM condition**: If decompression fails due to insufficient memory, the kernel will kill processes via the normal OOM killer — MiniMem does not introduce new failure modes.

- **PTE markers**: On an unpatched kernel, MiniMem uses kprobes on `do_swap_page`. If the kprobe fails to register, the module simply refuses to load. On a patched kernel, the PTE marker is a swap-type entry — if the module is absent, the kernel treats it as a swap-in and faults normally.

- **No data corruption path**: Compression is always lossless (verified by `memcmp` round-trip). The advisor only compresses pages that meet the savings threshold. Incompressible pages are left untouched.

## Sysfs Stats Reference

All stats are in `/sys/kernel/minimem/`:

| Attribute | Description |
|---|---|
| `pages_compressed` | Total pages compressed since load |
| `pages_decompressed` | Total pages decompressed since load |
| `bytes_saved` | Total bytes saved (original - compressed) |
| `compress_count` | Number of compress operations |
| `decompress_count` | Number of decompress operations |
| `compress_ns_total` | Total time spent compressing (ns) |
| `decompress_ns_total` | Total time spent decompressing (ns) |
| `compress_avg_ns` | Average compress latency (ns) |
| `decompress_avg_ns` | Average decompress latency (ns) |
| `zswap_pages` | Pages currently in compressed pool |
| `zswap_bytes` | Bytes currently in compressed pool |
| `zswap_saved` | Bytes saved by current compressed pages |
| `pool_pages` | Pool memory pages allocated |
| `parallel_clusters` | Pages decompressed in parallel |
| `parallel_pages` | Parallel decompression batches |
| `scanner_enabled` | Scanner on/off (0/1) |
| `scanner_interval_ms` | Scanner sweep interval (ms) |
| `min_savings_pct` | Minimum savings % to compress |
| `scanner_pages_scanned` | Pages scanned by scanner |
| `scanner_pages_idle` | Idle pages found by scanner |
| `scanner_pages_compressed` | Pages compressed by scanner |
| `scanner_pages_skipped` | Pages skipped (too small savings) |
| `hook_faults` | Faults intercepted by hook |
| `kernel_patches` | 0=kprobe-only, 1=patched kernel |
| `max_pool_pages` | Max pool pages (0=unlimited) |
| `parallel_mode` | 0=off, 1=on, 2=auto (default) |

Quick overview:
```bash
cat /sys/kernel/minimem/*
```

Formatted summary:
```bash
echo "=== MiniMem Status ==="
echo "Pages compressed:    $(cat /sys/kernel/minimem/pages_compressed)"
echo "Bytes saved:         $(cat /sys/kernel/minimem/bytes_saved)"
echo "Avg compress time:   $(cat /sys/kernel/minimem/compress_avg_ns) ns"
echo "Avg decompress time: $(cat /sys/kernel/minimem/decompress_avg_ns) ns"
echo "Pool pages:          $(cat /sys/kernel/minimem/pool_pages)"
echo "Scanner:             $(cat /sys/kernel/minimem/scanner_enabled)"
echo "Patches:             $(cat /sys/kernel/minimem/kernel_patches)"
```