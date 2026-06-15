#ifndef MINIMEM_WKDM64_H
#define MINIMEM_WKDM64_H

#include "lib/minimem.h"

#define MINIMEM_WKDM64_DICT_SIZE     32
#define MINIMEM_WKDM64_SCRATCH_SIZE  (MINIMEM_PAGE_SIZE + 1024)

struct minimem_wkdm64_scratch {
    uint8_t tag_buf[MINIMEM_PAGE_SIZE / 4];
    uint8_t exact_buf[MINIMEM_PAGE_SIZE / 4];
    uint8_t partial_buf[MINIMEM_PAGE_SIZE];
    uint8_t miss_buf[MINIMEM_PAGE_SIZE];
};

size_t minimem_wkdm64_compress(const uint8_t *src, size_t src_len,
                               uint8_t *dst, size_t dst_cap);

size_t minimem_wkdm64_compress_scratch(const uint8_t *src, size_t src_len,
                                        uint8_t *dst, size_t dst_cap,
                                        struct minimem_wkdm64_scratch *scratch);

size_t minimem_wkdm64_decompress(const uint8_t *src, size_t src_len,
                                 uint8_t *dst, size_t dst_cap);

size_t minimem_wkdm64_compress_bound(size_t src_len);
bool minimem_wkdm64_can_compress(const uint8_t *src, size_t src_len);

extern const struct minimem_compressor minimem_wkdm64_compressor;

#endif