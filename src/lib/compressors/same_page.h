#ifndef MINIMEM_SAME_PAGE_H
#define MINIMEM_SAME_PAGE_H

#include "lib/minimem.h"

size_t minimem_same_page_compress(const uint8_t *src, size_t src_len,
                                  uint8_t *dst, size_t dst_cap);
size_t minimem_same_page_decompress(const uint8_t *src, size_t src_len,
                                    uint8_t *dst, size_t dst_cap);
size_t minimem_same_page_compress_bound(size_t src_len);
bool minimem_same_page_can_compress(const uint8_t *src, size_t src_len);

extern const struct minimem_compressor minimem_same_page_compressor;

#endif