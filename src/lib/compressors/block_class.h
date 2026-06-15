#ifndef MINIMEM_BLOCK_CLASS_H
#define MINIMEM_BLOCK_CLASS_H

#include "lib/minimem.h"

#define MINIMEM_BLOCK_SIZE       64
#define MINIMEM_BLOCKS_PER_PAGE (MINIMEM_PAGE_SIZE / MINIMEM_BLOCK_SIZE)

#define MINIMEM_BLOCK_TYPE_ZERO       0
#define MINIMEM_BLOCK_TYPE_SPARSE     1
#define MINIMEM_BLOCK_TYPE_UNIFORM    2
#define MINIMEM_BLOCK_TYPE_SMALL_RANGE 3
#define MINIMEM_BLOCK_TYPE_DENSE      4
#define MINIMEM_BLOCK_TYPE_COUNT      5

#define MINIMEM_BLOCK_SPARSE_THRESHOLD  16
#define MINIMEM_BLOCK_RANGE_MAX_BITS    6

size_t minimem_block_class_compress(const uint8_t *src, size_t src_len,
                                    uint8_t *dst, size_t dst_cap);
size_t minimem_block_class_decompress(const uint8_t *src, size_t src_len,
                                      uint8_t *dst, size_t dst_cap);
size_t minimem_block_class_compress_bound(size_t src_len);
bool minimem_block_class_can_compress(const uint8_t *src, size_t src_len);

uint8_t minimem_classify_block(const uint8_t *block, size_t block_size);

extern const struct minimem_compressor minimem_block_class_compressor;

#endif