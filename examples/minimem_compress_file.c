/* SPDX-License-Identifier: MIT */
/*
 * minimem_compress_file.c — Compress a file using libminimem
 *
 * Usage: minimem_compress_file <input_file>
 *
 * Reads the input file, compresses it page by page using the advisor,
 * and prints compression statistics.
 *
 * Build (installed):
 *   cc -o minimem_compress_file examples/minimem_compress_file.c \
 *      $(pkg-config --cflags --libs minimem)
 *
 * Build (build tree):
 *   cc -o minimem_compress_file examples/minimem_compress_file.c \
 *      -I src -L build -lminimem -lzstd
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef MINIMEM_USE_INSTALLED
#include <minimem/minimem.h>
#include <minimem/advisor.h>
#else
#include "lib/minimem.h"
#include "lib/advisor.h"
#endif

#define BUF_SIZE (MINIMEM_PAGE_SIZE * MINIMEM_COMPRESS_BOUND_FACTOR)

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <input_file>\n", argv[0]);
        return 1;
    }

    FILE *fp = fopen(argv[1], "rb");
    if (!fp) {
        perror("fopen");
        return 1;
    }

    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (file_size <= 0) {
        fprintf(stderr, "Empty or invalid file\n");
        fclose(fp);
        return 1;
    }

    printf("File: %s (%ld bytes)\n", argv[1], file_size);
    printf("Page size: %d bytes\n\n", MINIMEM_PAGE_SIZE);

    uint8_t *src = malloc(MINIMEM_PAGE_SIZE);
    uint8_t *dst = malloc(BUF_SIZE);
    uint8_t *dec = malloc(MINIMEM_PAGE_SIZE);
    if (!src || !dst || !dec) {
        perror("malloc");
        return 1;
    }

    long total_original = 0;
    long total_compressed = 0;
    int pages = 0;
    int incompressible = 0;
    int algo_counts[MINIMEM_ALGO_COUNT] = {0};

    struct timespec ts_start, ts_end;
    double total_compress_ns = 0;
    double total_decompress_ns = 0;

    size_t n;
    while ((n = fread(src, 1, MINIMEM_PAGE_SIZE, fp)) > 0) {
        if (n < MINIMEM_PAGE_SIZE)
            memset(src + n, 0, MINIMEM_PAGE_SIZE - n);

        struct minimem_page_stats stats = minimem_analyze_page(src, MINIMEM_PAGE_SIZE);
        int algo = minimem_advise_algorithm(&stats);
        const struct minimem_compressor *c = minimem_get_compressor(algo);
        if (!c) {
            incompressible++;
            total_original += n;
            pages++;
            continue;
        }

        clock_gettime(CLOCK_MONOTONIC, &ts_start);
        size_t compressed_size = c->compress(src, MINIMEM_PAGE_SIZE, dst, BUF_SIZE);
        clock_gettime(CLOCK_MONOTONIC, &ts_end);
        double compress_ns = (ts_end.tv_sec - ts_start.tv_sec) * 1e9 +
                             (ts_end.tv_nsec - ts_start.tv_nsec);

        if (compressed_size == 0 || compressed_size >= MINIMEM_PAGE_SIZE) {
            incompressible++;
            total_original += n;
            pages++;
            continue;
        }

        clock_gettime(CLOCK_MONOTONIC, &ts_start);
        size_t decompressed_size = c->decompress(dst, compressed_size, dec, MINIMEM_PAGE_SIZE);
        clock_gettime(CLOCK_MONOTONIC, &ts_end);
        double decompress_ns = (ts_end.tv_sec - ts_start.tv_sec) * 1e9 +
                               (ts_end.tv_nsec - ts_start.tv_nsec);

        if (decompressed_size != MINIMEM_PAGE_SIZE ||
            memcmp(src, dec, MINIMEM_PAGE_SIZE) != 0) {
            fprintf(stderr, "ERROR: Roundtrip failed for page %d (algo=%s)\n",
                    pages, c->name);
            free(src); free(dst); free(dec); fclose(fp);
            return 1;
        }

        total_original += MINIMEM_PAGE_SIZE;
        total_compressed += compressed_size;
        total_compress_ns += compress_ns;
        total_decompress_ns += decompress_ns;
        algo_counts[algo]++;
        pages++;
    }

    printf("Results:\n");
    printf("  Pages processed:    %d\n", pages);
    printf("  Incompressible:      %d\n", incompressible);
    printf("  Compressible:        %d\n", pages - incompressible);
    printf("  Original bytes:      %ld\n", total_original);
    printf("  Compressed bytes:    %ld\n", total_compressed);
    if (total_original > 0)
        printf("  Compression ratio:   %.2f:1\n",
               (double)total_original / total_compressed);
    printf("  Savings:             %.1f%%\n",
           100.0 * (1.0 - (double)total_compressed / total_original));
    printf("  Avg compress latency: %.1f μs/page\n",
           total_compress_ns / (pages - incompressible) / 1000);
    printf("  Avg decompress latency: %.1f μs/page\n",
           total_decompress_ns / (pages - incompressible) / 1000);

    printf("\nAlgorithm distribution:\n");
    for (int i = 0; i < MINIMEM_ALGO_COUNT; i++) {
        if (algo_counts[i] > 0) {
            const struct minimem_compressor *c = minimem_get_compressor(i);
            printf("  %-12s: %d pages\n", c ? c->name : "?", algo_counts[i]);
        }
    }

    free(src);
    free(dst);
    free(dec);
    fclose(fp);
    return 0;
}