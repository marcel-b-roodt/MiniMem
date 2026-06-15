#include "lib/compressors/zstd_dict.h"

#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>

size_t minimem_zstd_dict_compress(const uint8_t *src, size_t src_len,
                                  uint8_t *dst, size_t dst_cap)
{
    if (src_len == 0 || dst_cap == 0)
        return 0;

    size_t result = ZSTD_compress(dst, dst_cap, src, src_len, 1);
    if (ZSTD_isError(result))
        return 0;

    if (result >= src_len)
        return 0;

    return result;
}

size_t minimem_zstd_dict_decompress(const uint8_t *src, size_t src_len,
                                    uint8_t *dst, size_t dst_cap)
{
    if (src_len == 0 || dst_cap == 0)
        return 0;

    size_t result = ZSTD_decompress(dst, dst_cap, src, src_len);
    if (ZSTD_isError(result))
        return 0;

    return result;
}

size_t minimem_zstd_dict_compress_bound(size_t src_len)
{
    return ZSTD_compressBound(src_len);
}

bool minimem_zstd_dict_can_compress(const uint8_t *src, size_t src_len)
{
    (void)src;
    return src_len > 0;
}

const struct minimem_compressor minimem_zstd_dict_compressor = {
    .name = "zstd_dict",
    .id = MINIMEM_ALGO_ZSTD_DICT,
    .compress = minimem_zstd_dict_compress,
    .decompress = minimem_zstd_dict_decompress,
    .compress_bound = minimem_zstd_dict_compress_bound,
    .can_compress = minimem_zstd_dict_can_compress,
};