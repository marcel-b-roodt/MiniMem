#include "lib/advisor.h"
#include <stdlib.h>
#include <string.h>

struct minimem_page_stats minimem_analyze_page(const uint8_t *data, size_t len)
{
    struct minimem_page_stats stats = {0};
    stats.total_bytes = len;
    stats.max_byte = 0;
    stats.min_byte = 0xFF;

    size_t histogram[256] = {0};
    size_t upper32_zero = 0;
    size_t small_val = 0;

    for (size_t i = 0; i < len; i++) {
        uint8_t v = data[i];
        if (v == 0)
            stats.zero_bytes++;
        histogram[v]++;
        if (v < stats.min_byte) stats.min_byte = v;
        if (v > stats.max_byte) stats.max_byte = v;
        if (v <= 0x0F || v >= 0xF0)
            small_val++;
    }

    stats.zero_byte_frac_x256 = (stats.zero_bytes * 256) / len;

    uint8_t mode_byte = 0;
    size_t mode_count = 0;
    for (int i = 0; i < 256; i++) {
        if (histogram[i] > mode_count) {
            mode_count = histogram[i];
            mode_byte = (uint8_t)i;
        }
    }
    stats.most_common_byte = mode_byte;
    stats.most_common_count = mode_count;
    stats.small_value_count = small_val;

    if (len >= 8 && len % 8 == 0) {
        const uint64_t *words = (const uint64_t *)data;
        size_t n_words = len / 8;
        for (size_t i = 0; i < n_words; i++) {
            if ((words[i] & 0xFFFFFFFF00000000ULL) == 0)
                upper32_zero++;
        }
    }
    stats.upper32_zero_count = upper32_zero;

    if (len >= 64) {
        uint8_t prev = data[0];
        size_t delta_small = 0;
        for (size_t i = 1; i < len; i++) {
            int16_t delta = (int16_t)data[i] - (int16_t)prev;
            if (delta >= -16 && delta <= 15)
                delta_small++;
            prev = data[i];
        }
        stats.small_delta_count = delta_small;
    }

    return stats;
}

int minimem_advise_algorithm(const struct minimem_page_stats *stats)
{
    if (stats->zero_byte_frac_x256 >= 250)
        return MINIMEM_ALGO_SAME_PAGE;

    if (stats->most_common_count >= stats->total_bytes - 4)
        return MINIMEM_ALGO_SAME_PAGE;

    if (stats->upper32_zero_count > (stats->total_bytes / 8) / 2)
        return MINIMEM_ALGO_WKDM64;

    if (stats->zero_byte_frac_x256 >= 128)
        return MINIMEM_ALGO_WKDM;

    if (stats->max_byte - stats->min_byte <= 32)
        return MINIMEM_ALGO_BDI;

    return MINIMEM_ALGO_LZ4;
}

int minimem_advise_best(const uint8_t *data, size_t len,
                        const int *algo_ids, size_t n_algos)
{
    struct minimem_page_stats stats = minimem_analyze_page(data, len);
    int advised = minimem_advise_algorithm(&stats);

    size_t best_size = 0;
    int best_algo = -1;

    for (size_t i = 0; i < n_algos; i++) {
        const struct minimem_compressor *c = minimem_get_compressor(algo_ids[i]);
        if (!c || !c->can_compress(data, len))
            continue;

        size_t bound = c->compress_bound(len);
        uint8_t *buf = malloc(bound);
        if (!buf) continue;

        size_t csz = c->compress(data, len, buf, bound);
        if (csz > 0 && (best_algo == -1 || csz < best_size)) {
            best_size = csz;
            best_algo = algo_ids[i];
        }
        free(buf);
    }

    if (best_algo >= 0 && best_size < len)
        return best_algo;

    return advised;
}