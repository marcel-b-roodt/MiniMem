#ifndef MINIMEM_DELTA_H
#define MINIMEM_DELTA_H

#include "lib/minimem.h"

size_t minimem_delta_compress(const uint8_t *src, size_t src_len,
                              uint8_t *dst, size_t dst_cap);
size_t minimem_delta_decompress(const uint8_t *src, size_t src_len,
                                uint8_t *dst, size_t dst_cap);
size_t minimem_delta_compress_bound(size_t src_len);
bool minimem_delta_can_compress(const uint8_t *src, size_t src_len);

size_t minimem_delta_xor(const uint8_t *src, const uint8_t *base,
                         size_t len, uint8_t *dst, size_t dst_cap);
size_t minimem_delta_xor_recover(const uint8_t *delta, size_t delta_len,
                                 const uint8_t *base, size_t base_len,
                                 uint8_t *dst, size_t dst_cap);

extern const struct minimem_compressor minimem_delta_compressor;

#endif