#include "lib/compressors/lzsse8.h"

size_t minimem_lzsse8_compress(const uint8_t *src, size_t src_len,
                               uint8_t *dst, size_t dst_cap)
{
    (void)src; (void)src_len; (void)dst; (void)dst_cap;
    return 0;
}

size_t minimem_lzsse8_decompress(const uint8_t *src, size_t src_len,
                                 uint8_t *dst, size_t dst_cap)
{
    (void)src; (void)src_len; (void)dst; (void)dst_cap;
    return 0;
}

size_t minimem_lzsse8_compress_bound(size_t src_len)
{
    return src_len + src_len / 16 + 64;
}

bool minimem_lzsse8_can_compress(const uint8_t *src, size_t src_len)
{
    (void)src; (void)src_len;
    return false;
}

const struct minimem_compressor minimem_lzsse8_compressor = {
    .name = "lzsse8",
    .id = MINIMEM_ALGO_LZSSE8,
    .compress = minimem_lzsse8_compress,
    .decompress = minimem_lzsse8_decompress,
    .compress_bound = minimem_lzsse8_compress_bound,
    .can_compress = minimem_lzsse8_can_compress,
};