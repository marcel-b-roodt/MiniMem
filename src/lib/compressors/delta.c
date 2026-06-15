#include "lib/compressors/delta.h"
#include <string.h>

size_t minimem_delta_xor(const uint8_t *src, const uint8_t *base,
                         size_t len, uint8_t *dst, size_t dst_cap)
{
    if (len == 0 || dst_cap < len)
        return 0;

    for (size_t i = 0; i < len; i++)
        dst[i] = src[i] ^ base[i];

    return len;
}

size_t minimem_delta_xor_recover(const uint8_t *delta, size_t delta_len,
                                 const uint8_t *base, size_t base_len,
                                 uint8_t *dst, size_t dst_cap)
{
    if (delta_len == 0 || delta_len != base_len || dst_cap < delta_len)
        return 0;

    for (size_t i = 0; i < delta_len; i++)
        dst[i] = delta[i] ^ base[i];

    return delta_len;
}

size_t minimem_delta_compress(const uint8_t *src, size_t src_len,
                              uint8_t *dst, size_t dst_cap)
{
    (void)src; (void)src_len; (void)dst; (void)dst_cap;
    return 0;
}

size_t minimem_delta_decompress(const uint8_t *src, size_t src_len,
                                uint8_t *dst, size_t dst_cap)
{
    (void)src; (void)src_len; (void)dst; (void)dst_cap;
    return 0;
}

size_t minimem_delta_compress_bound(size_t src_len)
{
    return src_len * 2;
}

bool minimem_delta_can_compress(const uint8_t *src, size_t src_len)
{
    (void)src; (void)src_len;
    return false;
}

const struct minimem_compressor minimem_delta_compressor = {
    .name = "delta",
    .id = MINIMEM_ALGO_DELTA,
    .compress = minimem_delta_compress,
    .decompress = minimem_delta_decompress,
    .compress_bound = minimem_delta_compress_bound,
    .can_compress = minimem_delta_can_compress,
};