#include "lib/compressors/lz4_wrap.h"

#ifdef MINIMEM_KERNEL
#include <linux/lz4.h>
#include <linux/slab.h>

size_t minimem_lz4_compress(const uint8_t *src, size_t src_len,
                             uint8_t *dst, size_t dst_cap)
{
    if (src_len == 0 || dst_cap == 0)
        return 0;

    void *wrkmem = kmalloc(LZ4_MEM_COMPRESS, GFP_KERNEL);
    if (!wrkmem)
        return 0;

    int result = LZ4_compress_default((const char *)src, (char *)dst,
                                       (int)src_len, (int)dst_cap, wrkmem);
    kfree(wrkmem);

    if (result <= 0)
        return 0;

    if ((size_t)result >= src_len)
        return 0;

    return (size_t)result;
}

size_t minimem_lz4_decompress(const uint8_t *src, size_t src_len,
                               uint8_t *dst, size_t dst_cap)
{
    if (src_len == 0 || dst_cap == 0)
        return 0;

    int result = LZ4_decompress_safe((const char *)src, (char *)dst,
                                      (int)src_len, (int)dst_cap);
    if (result < 0)
        return 0;

    return (size_t)result;
}

size_t minimem_lz4_compress_bound(size_t src_len)
{
    return (size_t)LZ4_compressBound((int)src_len);
}

bool minimem_lz4_can_compress(const uint8_t *src, size_t src_len)
{
    (void)src;
    return src_len > 0;
}

const struct minimem_compressor minimem_lz4_compressor = {
    .name = "lz4",
    .id = MINIMEM_ALGO_LZ4,
    .compress = minimem_lz4_compress,
    .decompress = minimem_lz4_decompress,
    .compress_bound = minimem_lz4_compress_bound,
    .can_compress = minimem_lz4_can_compress,
};

#else

#define LZ4_STATIC_LINKING_ONLY
#define LZ4_DISABLE_DEPRECATE_WARNINGS
#include "lib/vendor/lz4/lz4.h"

size_t minimem_lz4_compress(const uint8_t *src, size_t src_len,
                             uint8_t *dst, size_t dst_cap)
{
    if (src_len == 0 || dst_cap == 0)
        return 0;

    int result = LZ4_compress_default((const char *)src, (char *)dst,
                                       (int)src_len, (int)dst_cap);
    if (result <= 0)
        return 0;

    if ((size_t)result >= src_len)
        return 0;

    return (size_t)result;
}

size_t minimem_lz4_decompress(const uint8_t *src, size_t src_len,
                               uint8_t *dst, size_t dst_cap)
{
    if (src_len == 0 || dst_cap == 0)
        return 0;

    int result = LZ4_decompress_safe((const char *)src, (char *)dst,
                                      (int)src_len, (int)dst_cap);
    if (result < 0)
        return 0;

    return (size_t)result;
}

size_t minimem_lz4_compress_bound(size_t src_len)
{
    return (size_t)LZ4_compressBound((int)src_len);
}

bool minimem_lz4_can_compress(const uint8_t *src, size_t src_len)
{
    (void)src;
    return src_len > 0;
}

const struct minimem_compressor minimem_lz4_compressor = {
    .name = "lz4",
    .id = MINIMEM_ALGO_LZ4,
    .compress = minimem_lz4_compress,
    .decompress = minimem_lz4_decompress,
    .compress_bound = minimem_lz4_compress_bound,
    .can_compress = minimem_lz4_can_compress,
};

#endif