#include "lib/compressors/bdi.h"
#include <string.h>

#define BDI_LINE_SIZE      64
#define BDI_LINE_WORDS     (BDI_LINE_SIZE / sizeof(uint64_t))
#define BDI_NUM_ENTRIES    16

enum bdi_line_type {
    BDI_LINE_UNCOMPRESSED = 0,
    BDI_LINE_ZEROS        = 1,
    BDI_LINE_BASE_DELTA   = 2,
    BDI_LINE_BASE_DELTA2  = 3,
};

static enum bdi_line_type bdi_classify_line(const uint8_t *line,
                                            uint64_t *base_out,
                                            uint8_t *delta_bits_out)
{
    const uint64_t *words = (const uint64_t *)line;
    uint64_t base = words[0];

    bool all_zero = true;
    bool all_same = true;
    uint64_t max_delta = 0;

    for (size_t i = 0; i < BDI_LINE_WORDS; i++) {
        if (words[i] != 0)
            all_zero = false;
        if (words[i] != base)
            all_same = false;

        uint64_t delta;
        if (words[i] >= base)
            delta = words[i] - base;
        else
            delta = base - words[i];

        if (delta > max_delta)
            max_delta = delta;
    }

    if (all_zero) {
        *base_out = 0;
        *delta_bits_out = 0;
        return BDI_LINE_ZEROS;
    }

    if (all_same) {
        *base_out = base;
        *delta_bits_out = 0;
        return BDI_LINE_BASE_DELTA;
    }

    if (max_delta < 256) {
        *base_out = base;
        *delta_bits_out = 8;
        return BDI_LINE_BASE_DELTA2;
    }

    return BDI_LINE_UNCOMPRESSED;
}

size_t minimem_bdi_compress(const uint8_t *src, size_t src_len,
                            uint8_t *dst, size_t dst_cap)
{
    if (src_len == 0 || src_len % BDI_LINE_SIZE != 0)
        return 0;

    size_t n_lines = src_len / BDI_LINE_SIZE;
    size_t out_pos = 0;

    uint32_t n = (uint32_t)n_lines;
    if (out_pos + 4 > dst_cap)
        return 0;
    memcpy(dst + out_pos, &n, 4);
    out_pos += 4;

    for (size_t line = 0; line < n_lines; line++) {
        const uint8_t *line_ptr = src + line * BDI_LINE_SIZE;
        uint64_t base;
        uint8_t delta_bits;
        enum bdi_line_type type = bdi_classify_line(line_ptr, &base, &delta_bits);

        switch (type) {
        case BDI_LINE_ZEROS:
            if (out_pos + 1 > dst_cap)
                return 0;
            dst[out_pos] = BDI_LINE_ZEROS;
            out_pos += 1;
            break;

        case BDI_LINE_BASE_DELTA:
            if (out_pos + 1 + 8 > dst_cap)
                return 0;
            dst[out_pos] = BDI_LINE_BASE_DELTA;
            out_pos += 1;
            memcpy(dst + out_pos, &base, 8);
            out_pos += 8;
            break;

        case BDI_LINE_BASE_DELTA2: {
            size_t deltas_size = (delta_bits * BDI_LINE_WORDS + 7) / 8;
            size_t line_compressed = 1 + 8 + deltas_size;
            if (line_compressed >= BDI_LINE_SIZE)
                goto uncompressed;

            if (out_pos + line_compressed > dst_cap)
                return 0;

            dst[out_pos] = BDI_LINE_BASE_DELTA2;
            out_pos += 1;
            memcpy(dst + out_pos, &base, 8);
            out_pos += 8;

            memset(dst + out_pos, 0, deltas_size);
            const uint64_t *words = (const uint64_t *)line_ptr;
            for (size_t i = 0; i < BDI_LINE_WORDS; i++) {
                int64_t signed_delta = (int64_t)(words[i]) - (int64_t)base;
                uint64_t unsigned_delta = (uint64_t)signed_delta;
                for (int b = 0; b < delta_bits; b++) {
                    size_t bit_idx = i * delta_bits + b;
                    if (unsigned_delta & ((uint64_t)1 << b))
                        dst[out_pos + bit_idx / 8] |= (1 << (bit_idx % 8));
                }
            }
            out_pos += deltas_size;
            break;
        }

        uncompressed:
        default:
            if (out_pos + 1 + BDI_LINE_SIZE > dst_cap)
                return 0;
            dst[out_pos] = BDI_LINE_UNCOMPRESSED;
            out_pos += 1;
            memcpy(dst + out_pos, line_ptr, BDI_LINE_SIZE);
            out_pos += BDI_LINE_SIZE;
            break;
        }
    }

    if (out_pos >= src_len)
        return 0;

    return out_pos;
}

size_t minimem_bdi_decompress(const uint8_t *src, size_t src_len,
                              uint8_t *dst, size_t dst_cap)
{
    if (src_len < 4)
        return 0;

    uint32_t n_lines;
    memcpy(&n_lines, src, 4);

    size_t total = (size_t)n_lines * BDI_LINE_SIZE;
    if (total == 0 || dst_cap < total)
        return 0;

    size_t in_pos = 4;
    size_t out_pos = 0;

    for (uint32_t line = 0; line < n_lines; line++) {
        if (in_pos >= src_len)
            return 0;

        uint8_t type = src[in_pos];
        in_pos++;

        switch (type) {
        case BDI_LINE_ZEROS:
            if (out_pos + BDI_LINE_SIZE > dst_cap)
                return 0;
            memset(dst + out_pos, 0, BDI_LINE_SIZE);
            out_pos += BDI_LINE_SIZE;
            break;

        case BDI_LINE_BASE_DELTA:
            if (in_pos + 8 > src_len)
                return 0;
            if (out_pos + BDI_LINE_SIZE > dst_cap)
                return 0;
            {
                uint64_t base;
                memcpy(&base, src + in_pos, 8);
                in_pos += 8;
                uint64_t *dst_words = (uint64_t *)(dst + out_pos);
                for (size_t i = 0; i < BDI_LINE_WORDS; i++)
                    dst_words[i] = base;
                out_pos += BDI_LINE_SIZE;
            }
            break;

        case BDI_LINE_BASE_DELTA2: {
            if (in_pos + 8 > src_len)
                return 0;
            if (out_pos + BDI_LINE_SIZE > dst_cap)
                return 0;
            uint64_t base;
            memcpy(&base, src + in_pos, 8);
            in_pos += 8;

            uint8_t delta_bits = 8;
            size_t deltas_size = (delta_bits * BDI_LINE_WORDS + 7) / 8;

            if (in_pos + deltas_size > src_len)
                return 0;

            uint64_t *dst_words = (uint64_t *)(dst + out_pos);
            for (size_t i = 0; i < BDI_LINE_WORDS; i++) {
                uint64_t unsigned_delta = 0;
                for (int b = 0; b < delta_bits; b++) {
                    size_t bit_idx = i * delta_bits + b;
                    if (src[in_pos + bit_idx / 8] & (1 << (bit_idx % 8)))
                        unsigned_delta |= ((uint64_t)1 << b);
                }
                int64_t signed_delta;
                if (unsigned_delta & ((uint64_t)1 << (delta_bits - 1)))
                    signed_delta = (int64_t)(unsigned_delta | (~(((uint64_t)1 << delta_bits) - 1)));
                else
                    signed_delta = (int64_t)unsigned_delta;

                dst_words[i] = (uint64_t)((int64_t)base + signed_delta);
            }
            in_pos += deltas_size;
            out_pos += BDI_LINE_SIZE;
            break;
        }

        default:
            if (in_pos + BDI_LINE_SIZE > src_len)
                return 0;
            if (out_pos + BDI_LINE_SIZE > dst_cap)
                return 0;
            memcpy(dst + out_pos, src + in_pos, BDI_LINE_SIZE);
            in_pos += BDI_LINE_SIZE;
            out_pos += BDI_LINE_SIZE;
            break;
        }
    }

    return out_pos;
}

size_t minimem_bdi_compress_bound(size_t src_len)
{
    if (src_len == 0 || src_len % BDI_LINE_SIZE != 0)
        return src_len + 256;

    size_t n_lines = src_len / BDI_LINE_SIZE;
    size_t worst_per_line = 1 + 8 + (8 * BDI_LINE_WORDS + 7) / 8;
    return 4 + n_lines * worst_per_line;
}

bool minimem_bdi_can_compress(const uint8_t *src, size_t src_len)
{
    if (src_len == 0 || src_len % BDI_LINE_SIZE != 0)
        return false;

    size_t n_lines = src_len / BDI_LINE_SIZE;
    size_t compressible = 0;

    for (size_t line = 0; line < n_lines; line++) {
        uint64_t base;
        uint8_t delta_bits;
        enum bdi_line_type type = bdi_classify_line(
            src + line * BDI_LINE_SIZE, &base, &delta_bits);
        if (type != BDI_LINE_UNCOMPRESSED)
            compressible++;
    }

    return compressible > 0;
}

const struct minimem_compressor minimem_bdi_compressor = {
    .name = "bdi",
    .id = MINIMEM_ALGO_BDI,
    .compress = minimem_bdi_compress,
    .decompress = minimem_bdi_decompress,
    .compress_bound = minimem_bdi_compress_bound,
    .can_compress = minimem_bdi_can_compress,
};