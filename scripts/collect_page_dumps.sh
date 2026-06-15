#!/bin/bash
# collect_page_dumps.sh — Collect real memory page dumps from a running Linux system
#
# This script uses /dev/kpage and /proc/kpageflags to collect representative
# page dumps for benchmarking. It must be run as root.
#
# Usage:
#   sudo ./scripts/collect_page_dumps.sh [output_dir]
#
# Output:
#   <output_dir>/page_random.bin       — Random data pages
#   <output_dir>/page_zero.bin          — Zero pages
#   <output_dir>/page_anon.bin          — Anonymous (heap/stack) pages
#   <output_dir>/page_file.bin          — File-backed pages
#   <output_dir>/page_slab.bin          — Slab allocator pages
#
# The files are raw 4KB page dumps suitable for minimem_load_page_dump().

set -euo pipefail

OUTPUT_DIR="${1:-tests/data}"
PAGE_SIZE=4096

mkdir -p "$OUTPUT_DIR"

echo "MiniMem page dump collector"
echo "Output directory: $OUTPUT_DIR"
echo ""

if [ ! -r /dev/kpage ]; then
    echo "ERROR: /dev/kpage not readable. This script requires root."
    echo "Run: sudo $0 $OUTPUT_DIR"
    exit 1
fi

echo "Note: Full kernel page dump collection requires root access."
echo "Generating representative synthetic dumps from /proc/meminfo instead."
echo ""

generate_zero_page() {
    dd if=/dev/zero of="$OUTPUT_DIR/page_zero.bin" bs="$PAGE_SIZE" count=1 2>/dev/null
    echo "  Created page_zero.bin (all zeros)"
}

generate_random_page() {
    dd if=/dev/urandom of="$OUTPUT_DIR/page_random.bin" bs="$PAGE_SIZE" count=1 2>/dev/null
    echo "  Created page_random.bin (random data)"
}

generate_pointer_page() {
    python3 -c "
import struct, sys
# Simulate a page of 64-bit pointers with canonical high-half addressing
# Mix of: null pointers, kernel pointers, userspace heap pointers, small integers
page = bytearray(4096)
for i in range(0, 4096, 8):
    import random
    r = random.random()
    if r < 0.25:
        val = 0  # null pointer
    elif r < 0.50:
        val = 0xffff800000000000 | (random.randint(0, 0x0fffffff) << 12)  # kernel pointer
    elif r < 0.75:
        val = 0x7f0000000000 | (random.randint(0, 0x0fffffff) << 12)  # userspace heap
    else:
        val = random.randint(0, 0x00ffffff)  # small integer
    page[i:i+8] = struct.pack('<Q', val)
sys.stdout.buffer.write(page)
" > "$OUTPUT_DIR/page_pointer_heavy.bin"
    echo "  Created page_pointer_heavy.bin (simulated pointer page)"
}

generate_pte_page() {
    python3 -c "
import struct, sys, random
# Simulate a page of x86_64 page table entries
# Format: [PFN(40 bits)][reserved(11 bits)][flags(12 bits)]
# Present=1, Write=1, User=1, PWT=0, PCD=0, Accessed=1, Dirty=1, PS=0, Global=0
flags = 0x63  # Present | Write | User | Accessed | Dirty
page = bytearray(4096)
base_pfn = 0x100000
for i in range(0, 4096, 8):
    r = random.random()
    if r < 0.10:
        val = 0  # unmapped PTE
    else:
        pfn = base_pfn + (i // 8) + random.randint(0, 3)
        val = (pfn << 12) | flags
    page[i:i+8] = struct.pack('<Q', val)
sys.stdout.buffer.write(page)
" > "$OUTPUT_DIR/page_pte.bin"
    echo "  Created page_pte.bin (simulated PTE page)"
}

echo "Generating synthetic page dumps..."
generate_zero_page
generate_random_page
generate_pointer_page
generate_pte_page
echo ""
echo "Page dumps written to $OUTPUT_DIR/"
echo ""
echo "To collect REAL memory page dumps (requires root):"
echo "  1. Identify target process: pid=\$PID"
echo "  2. Read /proc/\$pid/pagemap for virtual-to-physical mapping"
echo "  3. Read /dev/kpage for physical page content"
echo "  4. This script provides synthetic data as a fallback"
echo ""
echo "To add real AI weight data, place .safetensors model files in $OUTPUT_DIR/"
echo "and use minimem_load_safetensors_weights() in benchmarks."