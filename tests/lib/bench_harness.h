#ifndef MINIMEM_BENCH_HARNESS_H
#define MINIMEM_BENCH_HARNESS_H

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 199309L
#endif

#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include "lib/minimem.h"

struct minimem_bench_result {
    double compress_throughput_mbs;
    double decompress_throughput_mbs;
    double compress_latency_us;
    double decompress_latency_us;
    double ratio;
    size_t original_size;
    size_t compressed_size;
    int algo_id;
    int page_type;
};

#define MINIMEM_BENCH_ITERATIONS 1000

static inline double bench_elapsed_ns(const struct timespec *start,
                                       const struct timespec *end)
{
    return (double)(end->tv_sec - start->tv_sec) * 1e9 +
           (double)(end->tv_nsec - start->tv_nsec);
}

struct minimem_bench_result
minimem_bench_compressor(const struct minimem_compressor *c,
                         const uint8_t *src, size_t src_len,
                         int page_type);

struct minimem_bench_result *
minimem_bench_compressor_all(const struct minimem_compressor *c,
                              uint64_t seed);

int minimem_bench_write_json(const char *path,
                              const struct minimem_bench_result *results,
                              size_t n_results);

int minimem_bench_write_csv(const char *path,
                             const struct minimem_bench_result *results,
                             size_t n_results);

#endif