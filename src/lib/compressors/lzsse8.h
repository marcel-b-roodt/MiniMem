#ifndef MINIMEM_LZSSE8_H
#define MINIMEM_LZSSE8_H

#include "lib/minimem.h"

size_t minimem_lzsse8_compress(const uint8_t *src, size_t src_len,
                               uint8_t *dst, size_t dst_cap);
size_t minimem_lzsse8_decompress(const uint8_t *src, size_t src_len,
                                 uint8_t *dst, size_t dst_cap);
size_t minimem_lzsse8_compress_bound(size_t src_len);
bool minimem_lzsse8_can_compress(const uint8_t *src, size_t src_len);

extern const struct minimem_compressor minimem_lzsse8_compressor;

#endif