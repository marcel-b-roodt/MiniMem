#include "lib/compressors/wkdm64.h"
#include <string.h>

#define WKDM64_TAG_BITS    2
#define WKDM64_TAG_ZERO    0
#define WKDM64_TAG_EXACT   1
#define WKDM64_TAG_PARTIAL 2
#define WKDM64_TAG_MISS    3

#define WKDM64_HASH(V) ((uint32_t)(((V) >> 3) & 0x1F))

static inline uint8_t wkdm64_get_tag(const uint8_t *tags, size_t word_idx)
{
    size_t bit_idx = word_idx * WKDM64_TAG_BITS;
    if (bit_idx % 8 + WKDM64_TAG_BITS <= 8)
        return (tags[bit_idx / 8] >> (bit_idx % 8)) & 0x03;
    return ((tags[bit_idx / 8] >> (bit_idx % 8)) |
            (tags[bit_idx / 8 + 1] << (8 - bit_idx % 8))) & 0x03;
}

static inline void wkdm64_set_tag(uint8_t *tags, size_t word_idx, uint8_t tag)
{
    size_t bit_idx = word_idx * WKDM64_TAG_BITS;
    tags[bit_idx / 8] |= (tag << (bit_idx % 8));
    if (bit_idx % 8 + WKDM64_TAG_BITS > 8)
        tags[bit_idx / 8 + 1] |= (tag >> (8 - bit_idx % 8));
}

static size_t wkdm64_compress_impl(const uint8_t *src, size_t src_len,
                                    uint8_t *dst, size_t dst_cap,
                                    struct minimem_wkdm64_scratch *scratch)
{
    if (src_len == 0 || src_len % 8 != 0)
        return 0;

    const uint64_t *words = (const uint64_t *)src;
    uint32_t n_words = (uint32_t)(src_len / 8);

    uint64_t dict[MINIMEM_WKDM64_DICT_SIZE];
    memset(dict, 0, sizeof(dict));

    uint8_t *tag_buf = scratch->tag_buf;
    uint8_t *exact_buf = scratch->exact_buf;
    uint8_t *partial_buf = scratch->partial_buf;
    uint8_t *miss_buf = scratch->miss_buf;

    size_t tag_bytes = ((size_t)n_words * WKDM64_TAG_BITS + 7) / 8;
    memset(tag_buf, 0, tag_bytes);

    size_t exact_count = 0;
    size_t partial_count = 0;
    size_t miss_count = 0;

    for (size_t i = 0; i < n_words; i++) {
        uint64_t val = words[i];

        if (val == 0) {
            wkdm64_set_tag(tag_buf, i, WKDM64_TAG_ZERO);
        } else {
            uint32_t hash = WKDM64_HASH(val);
            if (dict[hash] == val) {
                wkdm64_set_tag(tag_buf, i, WKDM64_TAG_EXACT);
                exact_buf[exact_count++] = (uint8_t)hash;
            } else if ((val & 0xFFFFFFFF00000000ULL) == 0) {
                wkdm64_set_tag(tag_buf, i, WKDM64_TAG_PARTIAL);
                uint32_t lo = (uint32_t)(val & 0xFFFFFFFFULL);
                memcpy(partial_buf + partial_count * 4, &lo, 4);
                partial_count++;
                dict[hash] = val;
            } else {
                wkdm64_set_tag(tag_buf, i, WKDM64_TAG_MISS);
                memcpy(miss_buf + miss_count * 8, &val, 8);
                miss_count++;
                dict[hash] = val;
            }
        }
    }

    size_t partial_bytes = partial_count * 4;
    size_t miss_bytes = miss_count * 8;
    size_t result_size = 12 + tag_bytes + exact_count + partial_bytes + miss_bytes;

    if (result_size >= src_len || result_size > dst_cap)
        return 0;

    uint8_t *out = dst;
    memcpy(out, &n_words, 4); out += 4;
    uint32_t ec = (uint32_t)exact_count;
    uint32_t pc = (uint32_t)partial_count;
    memcpy(out, &ec, 4); out += 4;
    memcpy(out, &pc, 4); out += 4;

    memcpy(out, tag_buf, tag_bytes); out += tag_bytes;
    memcpy(out, exact_buf, exact_count); out += exact_count;
    memcpy(out, partial_buf, partial_bytes); out += partial_bytes;
    memcpy(out, miss_buf, miss_bytes); out += miss_bytes;

    return (size_t)(out - dst);
}

static size_t wkdm64_decompress_impl(const uint8_t *src, size_t src_len,
                                      uint8_t *dst, size_t dst_cap)
{
    if (src_len < 12)
        return 0;

    const uint8_t *in = src;
    uint32_t n_words, exact_count, partial_count;
    memcpy(&n_words, in, 4); in += 4;
    memcpy(&exact_count, in, 4); in += 4;
    memcpy(&partial_count, in, 4); in += 4;

    size_t expected_size = (size_t)n_words * 8;
    if (expected_size == 0 || expected_size > dst_cap)
        return 0;

    size_t tag_bits = (size_t)n_words * WKDM64_TAG_BITS;
    size_t tag_bytes = (tag_bits + 7) / 8;

    if (in + tag_bytes > src + src_len)
        return 0;
    const uint8_t *tags = in;
    in += tag_bytes;

    if (in + exact_count > src + src_len)
        return 0;
    const uint8_t *exact_area = in;
    in += exact_count;

    size_t partial_bytes = (size_t)partial_count * 4;
    if (in + partial_bytes > src + src_len)
        return 0;
    const uint8_t *partial_area = in;
    in += partial_bytes;

    size_t miss_words = 0;
    for (size_t i = 0; i < n_words; i++) {
        if (wkdm64_get_tag(tags, i) == WKDM64_TAG_MISS)
            miss_words++;
    }

    size_t miss_bytes = miss_words * 8;
    if (in + miss_bytes > src + src_len)
        return 0;
    const uint8_t *miss_area = in;

    uint64_t dict[MINIMEM_WKDM64_DICT_SIZE];
    memset(dict, 0, sizeof(dict));
    uint64_t *dst_words = (uint64_t *)dst;

    size_t exact_idx = 0;
    size_t partial_idx = 0;
    size_t miss_idx = 0;

    for (size_t i = 0; i < n_words; i++) {
        uint8_t tag = wkdm64_get_tag(tags, i);

        switch (tag) {
        case WKDM64_TAG_ZERO:
            dst_words[i] = 0;
            break;
        case WKDM64_TAG_EXACT:
            if (exact_idx >= exact_count)
                return 0;
            dst_words[i] = dict[exact_area[exact_idx++]];
            break;
        case WKDM64_TAG_PARTIAL:
            if (partial_idx >= partial_count)
                return 0;
            {
                uint32_t lo;
                memcpy(&lo, partial_area + partial_idx * 4, 4);
                uint64_t val = (uint64_t)lo;
                dst_words[i] = val;
                dict[WKDM64_HASH(val)] = val;
                partial_idx++;
            }
            break;
        case WKDM64_TAG_MISS:
            if (miss_idx >= miss_words)
                return 0;
            memcpy(&dst_words[i], miss_area + miss_idx * 8, 8);
            dict[WKDM64_HASH(dst_words[i])] = dst_words[i];
            miss_idx++;
            break;
        default:
            return 0;
        }
    }

    return expected_size;
}

size_t minimem_wkdm64_compress(const uint8_t *src, size_t src_len,
                               uint8_t *dst, size_t dst_cap)
{
    struct minimem_wkdm64_scratch scratch;
    return wkdm64_compress_impl(src, src_len, dst, dst_cap, &scratch);
}

size_t minimem_wkdm64_compress_scratch(const uint8_t *src, size_t src_len,
                                        uint8_t *dst, size_t dst_cap,
                                        struct minimem_wkdm64_scratch *scratch)
{
    return wkdm64_compress_impl(src, src_len, dst, dst_cap, scratch);
}

size_t minimem_wkdm64_decompress(const uint8_t *src, size_t src_len,
                                 uint8_t *dst, size_t dst_cap)
{
    return wkdm64_decompress_impl(src, src_len, dst, dst_cap);
}

size_t minimem_wkdm64_compress_bound(size_t src_len)
{
    return src_len + 512;
}

bool minimem_wkdm64_can_compress(const uint8_t *src, size_t src_len)
{
    if (src_len == 0 || src_len % sizeof(uint64_t) != 0)
        return false;
    (void)src;
    return true;
}

const struct minimem_compressor minimem_wkdm64_compressor = {
    .name = "wkdm64",
    .id = MINIMEM_ALGO_WKDM64,
    .compress = minimem_wkdm64_compress,
    .decompress = minimem_wkdm64_decompress,
    .compress_bound = minimem_wkdm64_compress_bound,
    .can_compress = minimem_wkdm64_can_compress,
};