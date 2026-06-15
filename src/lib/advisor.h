#ifndef MINIMEM_ADVISOR_H
#define MINIMEM_ADVISOR_H

#include "lib/minimem.h"
#include <stddef.h>
#include <stdint.h>

struct minimem_page_stats {
    size_t total_bytes;
    size_t zero_bytes;
    size_t zero_byte_frac_x256;
    uint8_t most_common_byte;
    size_t most_common_count;
    size_t upper32_zero_count;
    size_t small_value_count;
    size_t small_delta_count;
    uint8_t max_byte;
    uint8_t min_byte;
};

struct minimem_page_stats minimem_analyze_page(const uint8_t *data, size_t len);

int minimem_advise_algorithm(const struct minimem_page_stats *stats);

int minimem_advise_best(const uint8_t *data, size_t len,
                        const int *algo_ids, size_t n_algos);

#endif