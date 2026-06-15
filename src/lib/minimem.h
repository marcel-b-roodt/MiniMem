#ifndef MINIMEM_H
#define MINIMEM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MINIMEM_PAGE_SIZE 4096
#define MINIMEM_CACHE_LINE_SIZE 64
#define MINIMEM_COMPRESS_BOUND_FACTOR 2

#define MINIMEM_ALGO_SAME_PAGE 0
#define MINIMEM_ALGO_BDI        1
#define MINIMEM_ALGO_WKDM       2
#define MINIMEM_ALGO_LZ4        3
#define MINIMEM_ALGO_LZSSE8     4
#define MINIMEM_ALGO_ZSTD_DICT  5
#define MINIMEM_ALGO_DELTA      6
#define MINIMEM_ALGO_WKDM64      7
#define MINIMEM_ALGO_BLOCK_CLASS 8
#define MINIMEM_ALGO_COUNT       9

#define MINIMEM_OK       0
#define MINIMEM_ERROR   (-1)
#define MINIMEM_NOSPACE (-2)

struct minimem_compressor {
    const char *name;
    int id;
    size_t (*compress)(const uint8_t *src, size_t src_len,
                       uint8_t *dst, size_t dst_cap);
    size_t (*decompress)(const uint8_t *src, size_t src_len,
                         uint8_t *dst, size_t dst_cap);
    size_t (*compress_bound)(size_t src_len);
    bool (*can_compress)(const uint8_t *src, size_t src_len);
};

struct minimem_result {
    int algo_id;
    size_t original_size;
    size_t compressed_size;
    double compress_latency_ns;
    double decompress_latency_ns;
};

static inline size_t minimem_compress_bound(size_t src_len)
{
    return src_len * MINIMEM_COMPRESS_BOUND_FACTOR;
}

const struct minimem_compressor *minimem_get_compressor(int algo_id);

#endif