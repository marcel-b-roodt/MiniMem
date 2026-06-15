#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 199309L
#endif

#include "lib/bench_harness.h"
#include "lib/test_data.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct minimem_bench_result
minimem_bench_compressor(const struct minimem_compressor *c,
                         const uint8_t *src, size_t src_len,
                         int page_type)
{
    struct minimem_bench_result r = {0};
    r.algo_id = c->id;
    r.page_type = page_type;
    r.original_size = src_len;

    size_t bound = c->compress_bound(src_len);
    if (bound == 0)
        bound = src_len * 2;

    uint8_t *compressed = malloc(bound);
    uint8_t *decompressed = malloc(src_len);
    if (!compressed || !decompressed) {
        free(compressed);
        free(decompressed);
        return r;
    }

    struct timespec start, end;

    clock_gettime(CLOCK_MONOTONIC, &start);
    size_t csz = 0;
    for (int i = 0; i < MINIMEM_BENCH_ITERATIONS; i++) {
        csz = c->compress(src, src_len, compressed, bound);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    if (csz == 0) {
        free(compressed);
        free(decompressed);
        return r;
    }

    r.compressed_size = csz;
    r.ratio = (double)src_len / (double)csz;
    r.compress_latency_us = bench_elapsed_ns(&start, &end) /
                            (double)MINIMEM_BENCH_ITERATIONS / 1000.0;
    r.compress_throughput_mbs = ((double)src_len * MINIMEM_BENCH_ITERATIONS) /
                                 (bench_elapsed_ns(&start, &end) / 1e9) /
                                 (1024.0 * 1024.0);

    clock_gettime(CLOCK_MONOTONIC, &start);
    size_t dsz = 0;
    for (int i = 0; i < MINIMEM_BENCH_ITERATIONS; i++) {
        dsz = c->decompress(compressed, csz, decompressed, src_len);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    r.decompress_latency_us = bench_elapsed_ns(&start, &end) /
                              (double)MINIMEM_BENCH_ITERATIONS / 1000.0;
    r.decompress_throughput_mbs = ((double)src_len * MINIMEM_BENCH_ITERATIONS) /
                                  (bench_elapsed_ns(&start, &end) / 1e9) /
                                  (1024.0 * 1024.0);

    (void)dsz;
    free(compressed);
    free(decompressed);
    return r;
}

struct minimem_bench_result *
minimem_bench_compressor_all(const struct minimem_compressor *c,
                              uint64_t seed)
{
    struct minimem_bench_result *results =
        calloc(MINIMEM_PAGE_TYPE_COUNT, sizeof(struct minimem_bench_result));
    if (!results)
        return NULL;

    for (int i = 0; i < MINIMEM_PAGE_TYPE_COUNT; i++) {
        struct minimem_page_data *page = minimem_generate_page(i, seed);
        if (!page)
            continue;
        results[i] = minimem_bench_compressor(c, page->data, page->size, i);
        minimem_free_page(page);
    }

    return results;
}

int minimem_bench_write_json(const char *path,
                              const struct minimem_bench_result *results,
                              size_t n_results)
{
    FILE *f = fopen(path, "w");
    if (!f)
        return -1;

    fprintf(f, "{\n  \"results\": [\n");
    for (size_t i = 0; i < n_results; i++) {
        const struct minimem_bench_result *r = &results[i];
        fprintf(f, "    {\n");
        fprintf(f, "      \"algo_id\": %d,\n", r->algo_id);
        fprintf(f, "      \"page_type\": %d,\n", r->page_type);
        fprintf(f, "      \"original_size\": %zu,\n", r->original_size);
        fprintf(f, "      \"compressed_size\": %zu,\n", r->compressed_size);
        fprintf(f, "      \"ratio\": %.4f,\n", r->ratio);
        fprintf(f, "      \"compress_throughput_mbs\": %.2f,\n", r->compress_throughput_mbs);
        fprintf(f, "      \"decompress_throughput_mbs\": %.2f,\n", r->decompress_throughput_mbs);
        fprintf(f, "      \"compress_latency_us\": %.4f,\n", r->compress_latency_us);
        fprintf(f, "      \"decompress_latency_us\": %.4f\n", r->decompress_latency_us);
        fprintf(f, "    }%s\n", i < n_results - 1 ? "," : "");
    }
    fprintf(f, "  ]\n}\n");
    fclose(f);
    return 0;
}

int minimem_bench_write_csv(const char *path,
                             const struct minimem_bench_result *results,
                             size_t n_results)
{
    FILE *f = fopen(path, "w");
    if (!f)
        return -1;

    fprintf(f, "algo_id,page_type,original_size,compressed_size,ratio,"
               "compress_throughput_mbs,decompress_throughput_mbs,"
               "compress_latency_us,decompress_latency_us\n");
    for (size_t i = 0; i < n_results; i++) {
        const struct minimem_bench_result *r = &results[i];
        fprintf(f, "%d,%d,%zu,%zu,%.4f,%.2f,%.2f,%.4f,%.4f\n",
                r->algo_id, r->page_type,
                r->original_size, r->compressed_size, r->ratio,
                r->compress_throughput_mbs, r->decompress_throughput_mbs,
                r->compress_latency_us, r->decompress_latency_us);
    }
    fclose(f);
    return 0;
}