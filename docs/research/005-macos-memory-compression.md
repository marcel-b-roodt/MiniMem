# 005 — macOS Memory Compression

## Summary

Since OS X Mavericks (2013), macOS implements transparent in-memory compression of cold pages using the WKdm algorithm. This is the only production operating system that compresses still-mapped pages in-place, and it is exactly the capability that MiniMem aims to bring to Linux. macOS's implementation proves the approach works at scale on millions of devices.

## Key Findings

- macOS compresses **cold anonymous pages** while they remain in the process's virtual address space
- On access, a page fault triggers decompression, the page is remapped, and execution resumes
- The algorithm is WKdm (Wilson, Kaplan, Smaragdakis 1999) — word-oriented dictionary compression designed for memory pages
- Apple engineers (including Phil, one of WKdm's original developers) confirmed on LWN that WKdm works even better on 64-bit systems:
  - 64-bit pointers have many zero upper bytes (canonical addressing)
  - A 64-bit integer with only 10 significant bits compresses to ~14 bits (>4:1)
  - Similar nearby pointers (heap objects, linked lists) achieve >3:1
- macOS's memory pressure handling uses a tiered approach:
  1. First: drop clean file-backed pages (cheapest, no I/O to reclaim)
  2. Second: compress cold anonymous pages (WKdm, saves memory at cost of decompression on fault)
  3. Third: swap compressed pages to disk (decompress + write, most expensive)
- The "memory compression" layer sits between normal memory and swap, exactly where MiniMem's kernel module would sit
- macOS exposes compression stats via Activity Monitor: "Memory Used", "Cached Files", "Swap Used", and "Compressed"
- On typical workloads, macOS achieves 2:1 effective memory expansion via compression

## What macOS Does That Linux Doesn't

| Feature | macOS | Linux (current) |
|---|---|---|
| Compress still-mapped cold pages | Yes (WKdm) | No |
| Compress swap pages | Yes (also WKdm) | Yes (zram/zswap, LZ4/LZO/zstd) |
| Page fault → decompress → remap | Yes | No (only page fault → swap-in from disk) |
| Compression tier between RAM and swap | Yes | Partially (zswap is between swap and RAM, but only for swap pages) |
| Per-page algorithm selection | Unknown (likely single algorithm) | Yes (zram multi-algorithm) |

## Relevance to MiniMem

- **Proof of concept.** macOS demonstrates that in-memory compression of cold pages works at scale. The approach is sound.
- **WKdm is validated.** If Apple uses it on millions of Macs, it works for memory pages. MiniMem should implement WKdm as a baseline.
- **The opportunity is real.** Linux has no equivalent. MiniMem fills this gap.
- **We can do better.** macOS likely uses a single algorithm (WKdm). MiniMem can use multi-algorithm dispatch (LZ4 for general, WKdm for pointers, BDI for small deltas, zstd for cold recompression).
- **Modern hardware helps.** WKdm was designed in 1999. Modern CPUs with SIMD and out-of-order execution may make other algorithms competitive or superior.

## Open Questions

- Does macOS still use WKdm, or has Apple developed a successor algorithm?
- What is macOS's decompression latency? (Not published, but page fault latency should include decompression time.)
- How does macOS handle write-back to compressed pages? (Decompress → modify → re-compress? Or copy-on-write?)
- What compression ratio does macOS achieve on typical workloads? (Activity Monitor shows "Compressed" memory but not the compression ratio.)
- Could MiniMem achieve better results than macOS by using algorithm diversity?

## References

- Wilson, Kaplan, Smaragdakis. "The Case for Compressed Caching in Virtual Memory Systems." USENIX 1999.
- Apple OS X Mavericks release (2013): https://www.apple.com/newsroom/2013/10/22Apple-Unveils-OS-X-Mavericks/
- LWN discussion thread with WKdm developer Phil: https://lwn.net/Articles/564199/
- macOS memory management overview: https://developer.apple.com/library/archive/documentation/Performance/Conceptual/ManagingMemory/Articles/AboutMemory.html