#ifndef MINIMEM_BDI_H
#define MINIMEM_BDI_H

#include "lib/minimem.h"

#define MINIMEM_BDI_CACHE_LINE_SIZE 64

size_t minimem_bdi_compress(const uint8_t *src, size_t src_len,
                            uint8_t *dst, size_t dst_cap);
size_t minimem_bdi_decompress(const uint8_t *src, size_t src_len,
                              uint8_t *dst, size_t dst_cap);
size_t minimem_bdi_compress_bound(size_t src_len);
bool minimem_bdi_can_compress(const uint8_t *src, size_t src_len);

extern const struct minimem_compressor minimem_bdi_compressor;

#endif