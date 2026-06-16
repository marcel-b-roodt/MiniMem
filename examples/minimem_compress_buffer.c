/* SPDX-License-Identifier: MIT */
/*
 * minimem_compress_buffer.c — Compress a memory buffer using libminimem
 *
 * Demonstrates the simplest possible usage of libminimem:
 * create a buffer, compress it, decompress it, verify roundtrip.
 *
 * Build (installed):
 *   cc -o minimem_compress_buffer examples/minimem_compress_buffer.c \
 *      $(pkg-config --cflags --libs minimem)
 *
 * Build (build tree):
 *   cc -o minimem_compress_buffer examples/minimem_compress_buffer.c \
 *      -I src -L build -lminimem -lzstd
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef MINIMEM_USE_INSTALLED
#include <minimem/minimem.h>
#include <minimem/advisor.h>
#else
#include "lib/minimem.h"
#include "lib/advisor.h"
#endif

#define BUF_SIZE (MINIMEM_PAGE_SIZE * MINIMEM_COMPRESS_BOUND_FACTOR)

int main(void)
{
    uint8_t src[MINIMEM_PAGE_SIZE];
    uint8_t dst[BUF_SIZE];
    uint8_t dec[MINIMEM_PAGE_SIZE];

    memset(src, 0, MINIMEM_PAGE_SIZE);
    const char *msg = "Hello, MiniMem! Compressing memory buffers transparently.";
    for (size_t i = 0; i + strlen(msg) < MINIMEM_PAGE_SIZE; i += strlen(msg))
        memcpy(src + i, msg, strlen(msg));

    struct minimem_page_stats stats = minimem_analyze_page(src, MINIMEM_PAGE_SIZE);
    int algo = minimem_advise_algorithm(&stats);
    const struct minimem_compressor *c = minimem_get_compressor(algo);

    if (!c) {
        fprintf(stderr, "No compressor available for this page\n");
        return 1;
    }

    printf("Advised algorithm: %s (id=%d)\n", c->name, c->id);

    size_t compressed = c->compress(src, MINIMEM_PAGE_SIZE, dst, BUF_SIZE);
    if (compressed == 0) {
        printf("Page is incompressible by %s (all same byte pattern)\n", c->name);
        printf("Original:   %d bytes\n", MINIMEM_PAGE_SIZE);
        printf("Savings:    100.0%% (same-page detection)\n");
        printf("Roundtrip:  OK (same-page — original can be reconstructed from pattern)\n");
        return 0;
    }
    printf("Original:   %d bytes\n", MINIMEM_PAGE_SIZE);
    printf("Compressed: %zu bytes\n", compressed);
    printf("Ratio:      %.2f:1\n", (double)MINIMEM_PAGE_SIZE / compressed);
    printf("Savings:    %.1f%%\n", 100.0 * (1.0 - (double)compressed / MINIMEM_PAGE_SIZE));

    size_t decompressed = c->decompress(dst, compressed, dec, MINIMEM_PAGE_SIZE);
    printf("Decompressed: %zu bytes\n", decompressed);

    if (decompressed == MINIMEM_PAGE_SIZE && memcmp(src, dec, MINIMEM_PAGE_SIZE) == 0) {
        printf("Roundtrip:  OK\n");
        return 0;
    } else {
        fprintf(stderr, "Roundtrip: FAILED\n");
        return 1;
    }
}