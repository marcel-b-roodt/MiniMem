#ifndef MINIMEM_LZ4_WRAP_H
#define MINIMEM_LZ4_WRAP_H

#include "lib/minimem.h"

size_t minimem_lz4_compress(const uint8_t *src, size_t src_len,
                            uint8_t *dst, size_t dst_cap);
size_t minimem_lz4_decompress(const uint8_t *src, size_t src_len,
                              uint8_t *dst, size_t dst_cap);
size_t minimem_lz4_compress_bound(size_t src_len);
bool minimem_lz4_can_compress(const uint8_t *src, size_t src_len);

extern const struct minimem_compressor minimem_lz4_compressor;

#endif