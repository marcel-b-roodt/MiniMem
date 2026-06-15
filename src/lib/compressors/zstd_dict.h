#ifndef MINIMEM_ZSTD_DICT_H
#define MINIMEM_ZSTD_DICT_H

#include "lib/minimem.h"

size_t minimem_zstd_dict_compress(const uint8_t *src, size_t src_len,
                                  uint8_t *dst, size_t dst_cap);
size_t minimem_zstd_dict_decompress(const uint8_t *src, size_t src_len,
                                    uint8_t *dst, size_t dst_cap);
size_t minimem_zstd_dict_compress_bound(size_t src_len);
bool minimem_zstd_dict_can_compress(const uint8_t *src, size_t src_len);

extern const struct minimem_compressor minimem_zstd_dict_compressor;

#endif