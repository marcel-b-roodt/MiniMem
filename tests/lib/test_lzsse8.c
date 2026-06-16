/* SPDX-License-Identifier: MIT */
/*
 * test_lzsse8.c — Standalone LZSSE8 roundtrip and benchmark test
 *
 * Verifies that LZSSE8 compression/decompression works correctly
 * across all synthetic page types, and measures throughput.
 *
 * Build:
 *   cc -o test_lzsse8 tests/lib/test_lzsse8.c \
 *      -I src -DMINIMEM_BUILD -L build -lminimem -lzstd -lstdc++ -lm \
 *      -Wl,-rpath,build
 *
 * Run:
 *   LD_LIBRARY_PATH=build ./test_lzsse8
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lib/minimem.h"
#include "lib/advisor.h"
#include "lib/compressors/lzsse8.h"
#include "lib/compressors/lz4_wrap.h"
#include "lib/test_data.h"

#define BUF_SIZE (MINIMEM_PAGE_SIZE * MINIMEM_COMPRESS_BOUND_FACTOR)

static int test_lzsse8_roundtrip(void)
{
    struct minimem_page_data **pages = minimem_generate_all_pages(42);
    const char *page_names[] = {
        "random", "zero", "repeat_val", "pointer_heavy",
        "integer_heavy", "pte", "ai_fp16", "ai_int8",
        "ai_sparse", "delta_pair", "mixed"
    };
    int pass = 0, fail = 0, skip = 0;

    printf("LZSSE8 roundtrip test:\n");

    for (int p = 0; p < MINIMEM_PAGE_TYPE_COUNT; p++) {
        uint8_t *compressed = malloc(BUF_SIZE);
        uint8_t *decompressed = malloc(MINIMEM_PAGE_SIZE);
        if (!compressed || !decompressed) {
            printf("  %-15s: MALLOC FAIL\n", page_names[p]);
            fail++;
            free(compressed);
            free(decompressed);
            continue;
        }

        size_t csz = minimem_lzsse8_compress(pages[p]->data,
                                              MINIMEM_PAGE_SIZE,
                                              compressed, BUF_SIZE);
        if (csz == 0) {
            printf("  %-15s: SKIP (incompressible or no SSE4.1)\n", page_names[p]);
            skip++;
            free(compressed);
            free(decompressed);
            continue;
        }

        if (csz >= MINIMEM_PAGE_SIZE) {
            printf("  %-15s: FAIL (compressed %zu >= %d)\n",
                   page_names[p], csz, MINIMEM_PAGE_SIZE);
            fail++;
            free(compressed);
            free(decompressed);
            continue;
        }

        size_t dsz = minimem_lzsse8_decompress(compressed, csz,
                                                 decompressed,
                                                 MINIMEM_PAGE_SIZE);
        if (dsz != MINIMEM_PAGE_SIZE) {
            printf("  %-15s: FAIL (decompressed %zu != %d)\n",
                   page_names[p], dsz, MINIMEM_PAGE_SIZE);
            fail++;
            free(compressed);
            free(decompressed);
            continue;
        }

        if (memcmp(pages[p]->data, decompressed, MINIMEM_PAGE_SIZE) != 0) {
            printf("  %-15s: FAIL (data mismatch)\n", page_names[p]);
            fail++;
            free(compressed);
            free(decompressed);
            continue;
        }

        double ratio = (double)MINIMEM_PAGE_SIZE / csz;
        printf("  %-15s: PASS (%5.2f:1, %zu bytes)\n", page_names[p], ratio, csz);
        pass++;
        free(compressed);
        free(decompressed);
    }

    minimem_free_all_pages(pages);

    printf("  Results: %d pass, %d fail, %d skip\n\n", pass, fail, skip);
    return fail;
}

static int test_lzsse8_benchmark(void)
{
    struct minimem_page_data **pages = minimem_generate_all_pages(42);
    const char *page_names[] = {
        "random", "zero", "repeat_val", "pointer_heavy",
        "integer_heavy", "pte", "ai_fp16", "ai_int8",
        "ai_sparse", "delta_pair", "mixed"
    };

    printf("LZSSE8 decompression benchmark (target: <10us per 4KB page):\n");

    for (int p = 0; p < MINIMEM_PAGE_TYPE_COUNT; p++) {
        uint8_t *compressed = malloc(BUF_SIZE);
        uint8_t *decompressed = malloc(MINIMEM_PAGE_SIZE);
        if (!compressed || !decompressed) {
            free(compressed);
            free(decompressed);
            continue;
        }

        size_t csz = minimem_lzsse8_compress(pages[p]->data,
                                              MINIMEM_PAGE_SIZE,
                                              compressed, BUF_SIZE);
        if (csz == 0 || csz >= MINIMEM_PAGE_SIZE) {
            free(compressed);
            free(decompressed);
            continue;
        }

        struct timespec start, end;
        const int iters = 1000;
        clock_gettime(CLOCK_MONOTONIC, &start);
        for (int i = 0; i < iters; i++) {
            minimem_lzsse8_decompress(compressed, csz, decompressed,
                                       MINIMEM_PAGE_SIZE);
        }
        clock_gettime(CLOCK_MONOTONIC, &end);

        double us = ((end.tv_sec - start.tv_sec) * 1e9 +
                     (end.tv_nsec - start.tv_nsec)) / iters / 1000.0;
        double mb_s = (MINIMEM_PAGE_SIZE / (us / 1e6)) / 1e6;

        printf("  %-15s: %6.2f us  %5.0f MB/s  (%5.2f:1) %s\n",
               page_names[p], us, mb_s,
               (double)MINIMEM_PAGE_SIZE / csz,
               us < 10.0 ? "OK" : "SLOW");

        free(compressed);
        free(decompressed);
    }

    minimem_free_all_pages(pages);
    printf("\n");
    return 0;
}

static int test_lzsse8_vs_lz4(void)
{
    struct minimem_page_data **pages = minimem_generate_all_pages(42);
    const struct minimem_compressor *lzsse = &minimem_lzsse8_compressor;
    const struct minimem_compressor *lz4 = &minimem_lz4_compressor;
    const char *page_names[] = {
        "random", "zero", "repeat_val", "pointer_heavy",
        "integer_heavy", "pte", "ai_fp16", "ai_int8",
        "ai_sparse", "delta_pair", "mixed"
    };

    printf("LZSSE8 vs LZ4 comparison:\n");
    printf("  %-15s  %10s  %10s  %10s\n", "Page", "LZSSE8", "LZ4", "Winner");
    printf("  %-15s  %10s  %10s  %10s\n", "", "ratio", "ratio", "");

    int lzsse_wins = 0, lz4_wins = 0;

    for (int p = 0; p < MINIMEM_PAGE_TYPE_COUNT; p++) {
        uint8_t *buf1 = malloc(BUF_SIZE);
        uint8_t *buf2 = malloc(BUF_SIZE);
        if (!buf1 || !buf2) { free(buf1); free(buf2); continue; }

        size_t lzsse_sz = lzsse->compress(pages[p]->data, MINIMEM_PAGE_SIZE,
                                            buf1, BUF_SIZE);
        size_t lz4_sz = lz4->compress(pages[p]->data, MINIMEM_PAGE_SIZE,
                                        buf2, BUF_SIZE);

        double lzsse_ratio = lzsse_sz > 0 ? (double)MINIMEM_PAGE_SIZE / lzsse_sz : 0;
        double lz4_ratio = lz4_sz > 0 ? (double)MINIMEM_PAGE_SIZE / lz4_sz : 0;

        const char *winner = "-";
        if (lzsse_sz > 0 && lz4_sz > 0) {
            if (lzsse_ratio > lz4_ratio * 1.05) { winner = "LZSSE8"; lzsse_wins++; }
            else if (lz4_ratio > lzsse_ratio * 1.05) { winner = "LZ4"; lz4_wins++; }
            else winner = "tie";
        } else if (lzsse_sz > 0) { winner = "LZSSE8"; lzsse_wins++; }
        else if (lz4_sz > 0) { winner = "LZ4"; lz4_wins++; }

        printf("  %-15s  %10.2f  %10.2f  %10s\n",
               page_names[p], lzsse_ratio, lz4_ratio, winner);

        free(buf1);
        free(buf2);
    }

    minimem_free_all_pages(pages);
    printf("  LZSSE8 wins: %d, LZ4 wins: %d\n\n", lzsse_wins, lz4_wins);
    return 0;
}

int main(void)
{
    printf("MiniMem LZSSE8 Test Suite\n");
    printf("========================\n\n");

    int failures = 0;

    failures += test_lzsse8_roundtrip();
    failures += test_lzsse8_benchmark();
    failures += test_lzsse8_vs_lz4();

    printf("========================\n");
    if (failures == 0)
        printf("ALL TESTS PASSED\n");
    else
        printf("%d TESTS FAILED\n", failures);

    return failures;
}