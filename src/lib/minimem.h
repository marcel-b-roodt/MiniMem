#ifndef MINIMEM_H
#define MINIMEM_H

#ifdef MINIMEM_KERNEL
#include <linux/types.h>
#include <linux/stddef.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/bug.h>
#include <linux/module.h>
typedef u8 uint8_t;
typedef u16 uint16_t;
typedef u32 uint32_t;
typedef u64 uint64_t;
#else
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#endif

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
#define MINIMEM_ALGO_AI_FP16    9
#define MINIMEM_ALGO_AI_BF16   10
#define MINIMEM_ALGO_AI_INT8   11
#define MINIMEM_ALGO_COUNT     12

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

#ifndef MINIMEM_KERNEL
const struct minimem_compressor *minimem_get_compressor(int algo_id);
#endif

#endif