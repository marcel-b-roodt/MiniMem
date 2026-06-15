#include "lib/compressors/same_page.h"
#include <string.h>

bool minimem_same_page_can_compress(const uint8_t *src, size_t src_len)
{
    if (src_len == 0)
        return false;

    const uint8_t first = src[0];
    for (size_t i = 1; i < src_len; i++) {
        if (src[i] != first)
            return false;
    }
    return true;
}

size_t minimem_same_page_compress(const uint8_t *src, size_t src_len,
                                  uint8_t *dst, size_t dst_cap)
{
    if (src_len == 0)
        return 0;

    const uint8_t first = src[0];
    for (size_t i = 1; i < src_len; i++) {
        if (src[i] != first)
            return 0;
    }

    if (dst_cap < sizeof(uint32_t) + 1)
        return 0;

    dst[0] = first;
    uint32_t len = (uint32_t)src_len;
    memcpy(dst + 1, &len, sizeof(uint32_t));
    return 1 + sizeof(uint32_t);
}

size_t minimem_same_page_decompress(const uint8_t *src, size_t src_len,
                                    uint8_t *dst, size_t dst_cap)
{
    if (src_len < 1 + sizeof(uint32_t))
        return 0;

    uint32_t len;
    memcpy(&len, src + 1, sizeof(uint32_t));

    if (dst_cap < len)
        return 0;

    memset(dst, src[0], len);
    return len;
}

size_t minimem_same_page_compress_bound(size_t src_len)
{
    (void)src_len;
    return 1 + sizeof(uint32_t);
}

const struct minimem_compressor minimem_same_page_compressor = {
    .name = "same_page",
    .id = MINIMEM_ALGO_SAME_PAGE,
    .compress = minimem_same_page_compress,
    .decompress = minimem_same_page_decompress,
    .compress_bound = minimem_same_page_compress_bound,
    .can_compress = minimem_same_page_can_compress,
};