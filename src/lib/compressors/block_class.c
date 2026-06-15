#include "lib/compressors/block_class.h"
#include <string.h>

static size_t bits_for_range(uint8_t range);

static uint8_t classify_block_impl(const uint8_t *block, size_t block_size,
                                    uint8_t *uniform_val_out,
                                    uint8_t *range_min_out, uint8_t *range_max_out,
                                    size_t *nz_count_out)
{
    uint8_t first = block[0];
    bool all_same = true;
    bool all_zero = true;
    uint8_t min_val = 0xFF;
    uint8_t max_val = 0x00;
    size_t nz_count = 0;

    for (size_t i = 0; i < block_size; i++) {
        uint8_t v = block[i];
        if (v != 0) { all_zero = false; nz_count++; }
        if (v != first) all_same = false;
        if (v < min_val) min_val = v;
        if (v > max_val) max_val = v;
    }

    if (all_zero) { if (nz_count_out) *nz_count_out = 0; return MINIMEM_BLOCK_TYPE_ZERO; }
    if (all_same) { if (uniform_val_out) *uniform_val_out = first; if (nz_count_out) *nz_count_out = nz_count; return MINIMEM_BLOCK_TYPE_UNIFORM; }
    if (nz_count <= MINIMEM_BLOCK_SPARSE_THRESHOLD && nz_count < block_size) { if (nz_count_out) *nz_count_out = nz_count; return MINIMEM_BLOCK_TYPE_SPARSE; }

    uint8_t range = max_val - min_val;
    size_t bpv = bits_for_range(range);
    if (bpv > 0 && bpv <= MINIMEM_BLOCK_RANGE_MAX_BITS) {
        if (range_min_out) *range_min_out = min_val;
        if (range_max_out) *range_max_out = max_val;
        if (nz_count_out) *nz_count_out = nz_count;
        return MINIMEM_BLOCK_TYPE_SMALL_RANGE;
    }

    if (nz_count_out) *nz_count_out = nz_count;
    return MINIMEM_BLOCK_TYPE_DENSE;
}

uint8_t minimem_classify_block(const uint8_t *block, size_t block_size)
{
    return classify_block_impl(block, block_size, NULL, NULL, NULL, NULL);
}

#define BITS_PER_TYPE 3

static void pack_types(const uint8_t *types, size_t n_types, uint8_t *out)
{
    memset(out, 0, (n_types * BITS_PER_TYPE + 7) / 8);
    for (size_t i = 0; i < n_types; i++) {
        size_t bit_pos = i * BITS_PER_TYPE;
        out[bit_pos / 8] |= ((types[i] & 0x07) << (bit_pos % 8));
        if (bit_pos % 8 + BITS_PER_TYPE > 8)
            out[bit_pos / 8 + 1] |= ((types[i] & 0x07) >> (8 - bit_pos % 8));
    }
}

static uint8_t unpack_type(const uint8_t *packed, size_t index)
{
    size_t bit_pos = index * BITS_PER_TYPE;
    if (bit_pos % 8 + BITS_PER_TYPE <= 8)
        return (packed[bit_pos / 8] >> (bit_pos % 8)) & 0x07;
    return ((packed[bit_pos / 8] >> (bit_pos % 8)) |
            (packed[bit_pos / 8 + 1] << (8 - bit_pos % 8))) & 0x07;
}

static size_t bits_for_range(uint8_t range)
{
    if (range == 0) return 0;
    if (range <= 1) return 1;
    if (range <= 3) return 2;
    if (range <= 7) return 3;
    if (range <= 15) return 4;
    if (range <= 31) return 5;
    if (range <= 63) return 6;
    if (range <= 127) return 7;
    return 8;
}

size_t minimem_block_class_compress(const uint8_t *src, size_t src_len,
                                    uint8_t *dst, size_t dst_cap)
{
    if (src_len == 0 || src_len % MINIMEM_BLOCK_SIZE != 0)
        return 0;

    size_t n_blocks = src_len / MINIMEM_BLOCK_SIZE;
    uint8_t types[MINIMEM_BLOCKS_PER_PAGE];
    uint8_t uniform_vals[MINIMEM_BLOCKS_PER_PAGE];
    uint8_t range_mins[MINIMEM_BLOCKS_PER_PAGE];
    uint8_t range_deltas[MINIMEM_BLOCKS_PER_PAGE];

    for (size_t b = 0; b < n_blocks; b++) {
        const uint8_t *block = src + b * MINIMEM_BLOCK_SIZE;
        uint8_t range_min = 0, range_max = 0;
        size_t nz_count;
        types[b] = classify_block_impl(block, MINIMEM_BLOCK_SIZE,
                                       &uniform_vals[b], &range_min, &range_max,
                                       &nz_count);
        range_mins[b] = range_min;
        range_deltas[b] = range_max - range_min;
    }

    size_t header_bytes = (n_blocks * BITS_PER_TYPE + 7) / 8;

    uint8_t *out = dst;
    uint16_t n_blocks_le = (uint16_t)n_blocks;
    memcpy(out, &n_blocks_le, 2); out += 2;
    uint16_t hdr_le = (uint16_t)header_bytes;
    memcpy(out, &hdr_le, 2); out += 2;
    pack_types(types, n_blocks, out); out += header_bytes;

    for (size_t b = 0; b < n_blocks; b++) {
        const uint8_t *block = src + b * MINIMEM_BLOCK_SIZE;

        switch (types[b]) {
        case MINIMEM_BLOCK_TYPE_ZERO:
            break;

        case MINIMEM_BLOCK_TYPE_UNIFORM:
            *out++ = uniform_vals[b];
            break;

        case MINIMEM_BLOCK_TYPE_SPARSE: {
            size_t bitmap_bytes = (MINIMEM_BLOCK_SIZE + 7) / 8;
            uint8_t bitmap[8] = {0};
            for (size_t i = 0; i < MINIMEM_BLOCK_SIZE; i++) {
                if (block[i] != 0)
                    bitmap[i / 8] |= (1u << (i % 8));
            }
            memcpy(out, bitmap, bitmap_bytes); out += bitmap_bytes;
            for (size_t i = 0; i < MINIMEM_BLOCK_SIZE; i++) {
                if (block[i] != 0)
                    *out++ = block[i];
            }
            break;
        }

        case MINIMEM_BLOCK_TYPE_SMALL_RANGE: {
            *out++ = range_mins[b];
            *out++ = range_deltas[b];
            size_t bpv = bits_for_range(range_deltas[b]);
            if (bpv == 0)
                break;
            size_t total_bits = MINIMEM_BLOCK_SIZE * bpv;
            size_t packed_bytes = (total_bits + 7) / 8;
            uint8_t packed[MINIMEM_BLOCK_SIZE] = {0};
            size_t bit_pos = 0;
            for (size_t i = 0; i < MINIMEM_BLOCK_SIZE; i++) {
                uint8_t delta = block[i] - range_mins[b];
                for (size_t bit = 0; bit < bpv; bit++) {
                    if (delta & (1u << bit)) {
                        size_t abs_bit = bit_pos + bit;
                        packed[abs_bit / 8] |= (1u << (abs_bit % 8));
                    }
                }
                bit_pos += bpv;
            }
            memcpy(out, packed, packed_bytes); out += packed_bytes;
            break;
        }

        case MINIMEM_BLOCK_TYPE_DENSE:
            memcpy(out, block, MINIMEM_BLOCK_SIZE);
            out += MINIMEM_BLOCK_SIZE;
            break;
        }
    }

    size_t result = (size_t)(out - dst);
    if (result >= src_len || result > dst_cap)
        return 0;
    return result;
}

size_t minimem_block_class_decompress(const uint8_t *src, size_t src_len,
                                      uint8_t *dst, size_t dst_cap)
{
    if (src_len < 4)
        return 0;

    const uint8_t *in = src;
    uint16_t n_blocks;
    uint16_t header_bytes;
    memcpy(&n_blocks, in, 2); in += 2;
    memcpy(&header_bytes, in, 2); in += 2;

    if (n_blocks == 0 || n_blocks > MINIMEM_BLOCKS_PER_PAGE)
        return 0;
    if ((size_t)(4 + header_bytes) > src_len)
        return 0;

    size_t expected_size = (size_t)n_blocks * MINIMEM_BLOCK_SIZE;
    if (expected_size > dst_cap)
        return 0;

    const uint8_t *type_data = in;
    in += header_bytes;

    uint8_t *out = dst;

    for (size_t b = 0; b < n_blocks; b++) {
        uint8_t type = unpack_type(type_data, b);

        switch (type) {
        case MINIMEM_BLOCK_TYPE_ZERO:
            memset(out, 0, MINIMEM_BLOCK_SIZE);
            out += MINIMEM_BLOCK_SIZE;
            break;

        case MINIMEM_BLOCK_TYPE_UNIFORM:
            if (in >= src + src_len) return 0;
            memset(out, *in, MINIMEM_BLOCK_SIZE);
            in++;
            out += MINIMEM_BLOCK_SIZE;
            break;

        case MINIMEM_BLOCK_TYPE_SPARSE: {
            size_t bitmap_bytes = (MINIMEM_BLOCK_SIZE + 7) / 8;
            if (in + bitmap_bytes > src + src_len) return 0;
            const uint8_t *bitmap = in; in += bitmap_bytes;
            memset(out, 0, MINIMEM_BLOCK_SIZE);
            for (size_t i = 0; i < MINIMEM_BLOCK_SIZE; i++) {
                if (bitmap[i / 8] & (1u << (i % 8))) {
                    if (in >= src + src_len) return 0;
                    out[i] = *in++;
                }
            }
            out += MINIMEM_BLOCK_SIZE;
            break;
        }

        case MINIMEM_BLOCK_TYPE_SMALL_RANGE: {
            if (in + 2 > src + src_len) return 0;
            uint8_t min_val = *in++;
            uint8_t range = *in++;
            size_t bpv = bits_for_range(range);

            if (bpv == 0) {
                memset(out, min_val, MINIMEM_BLOCK_SIZE);
                out += MINIMEM_BLOCK_SIZE;
                break;
            }

            size_t total_bits = MINIMEM_BLOCK_SIZE * bpv;
            size_t packed_bytes = (total_bits + 7) / 8;
            if (in + packed_bytes > src + src_len) return 0;

            const uint8_t *packed = in; in += packed_bytes;
            size_t bit_pos = 0;
            for (size_t i = 0; i < MINIMEM_BLOCK_SIZE; i++) {
                uint8_t delta = 0;
                for (size_t bit = 0; bit < bpv; bit++) {
                    size_t abs_bit = bit_pos + bit;
                    if (packed[abs_bit / 8] & (1u << (abs_bit % 8)))
                        delta |= (1u << bit);
                }
                out[i] = min_val + delta;
                bit_pos += bpv;
            }
            out += MINIMEM_BLOCK_SIZE;
            break;
        }

        case MINIMEM_BLOCK_TYPE_DENSE:
            if (in + MINIMEM_BLOCK_SIZE > src + src_len) return 0;
            memcpy(out, in, MINIMEM_BLOCK_SIZE);
            in += MINIMEM_BLOCK_SIZE;
            out += MINIMEM_BLOCK_SIZE;
            break;

        default:
            return 0;
        }
    }

    return expected_size;
}

size_t minimem_block_class_compress_bound(size_t src_len)
{
    return src_len + 256;
}

bool minimem_block_class_can_compress(const uint8_t *src, size_t src_len)
{
    if (src_len == 0 || src_len % MINIMEM_BLOCK_SIZE != 0)
        return false;
    (void)src;
    return true;
}

const struct minimem_compressor minimem_block_class_compressor = {
    .name = "block_class",
    .id = MINIMEM_ALGO_BLOCK_CLASS,
    .compress = minimem_block_class_compress,
    .decompress = minimem_block_class_decompress,
    .compress_bound = minimem_block_class_compress_bound,
    .can_compress = minimem_block_class_can_compress,
};