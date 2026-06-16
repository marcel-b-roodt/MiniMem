#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 199309L
#endif

#include <criterion/criterion.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lib/minimem.h"
#include "lib/test_data.h"
#include "lib/compressors/same_page.h"
#include "lib/compressors/bdi.h"
#include "lib/compressors/wkdm.h"
#include "lib/compressors/wkdm64.h"
#include "lib/compressors/block_class.h"
#include "lib/advisor.h"
#include "lib/compressors/lz4_wrap.h"
#include "lib/compressors/delta.h"
#include "lib/compressors/zstd_dict.h"
#include "lib/compressors/ai_weights.h"
#include "lib/test_roundtrip.h"
#include "lib/bench_harness.h"

#define SEED 42

static struct minimem_page_data **pages;

static void suite_setup(void)
{
    pages = minimem_generate_all_pages(SEED);
    cr_assert_neq(pages, NULL, "Failed to generate test pages");
}

static void suite_teardown(void)
{
    minimem_free_all_pages(pages);
}

TestSuite(algorithms, .init = suite_setup, .fini = suite_teardown);

Test(algorithms, bdi_roundtrip_pointer_heavy)
{
    minimem_assert_roundtrip(&minimem_bdi_compressor,
                             pages[MINIMEM_PAGE_POINTER_HEAVY]->data,
                             pages[MINIMEM_PAGE_POINTER_HEAVY]->size);
}

Test(algorithms, bdi_roundtrip_integer_heavy)
{
    minimem_assert_roundtrip(&minimem_bdi_compressor,
                             pages[MINIMEM_PAGE_INTEGER_HEAVY]->data,
                             pages[MINIMEM_PAGE_INTEGER_HEAVY]->size);
}

Test(algorithms, bdi_roundtrip_pte)
{
    minimem_assert_roundtrip(&minimem_bdi_compressor,
                             pages[MINIMEM_PAGE_PTE]->data,
                             pages[MINIMEM_PAGE_PTE]->size);
}

Test(algorithms, bdi_roundtrip_zero)
{
    minimem_assert_roundtrip(&minimem_bdi_compressor,
                             pages[MINIMEM_PAGE_ZERO]->data,
                             pages[MINIMEM_PAGE_ZERO]->size);
}

Test(algorithms, bdi_can_compress_zero)
{
    cr_assert(minimem_bdi_can_compress(pages[MINIMEM_PAGE_ZERO]->data,
                                         pages[MINIMEM_PAGE_ZERO]->size),
              "BDI should be able to compress zero pages");
}

Test(algorithms, bdi_can_compress_repeat_val)
{
    cr_assert(minimem_bdi_can_compress(pages[MINIMEM_PAGE_REPEAT_VAL]->data,
                                         pages[MINIMEM_PAGE_REPEAT_VAL]->size),
              "BDI should be able to compress repeated-value pages");
}

Test(algorithms, wkdm_roundtrip_pointer_heavy)
{
    minimem_assert_roundtrip(&minimem_wkdm_compressor,
                             pages[MINIMEM_PAGE_POINTER_HEAVY]->data,
                             pages[MINIMEM_PAGE_POINTER_HEAVY]->size);
}

Test(algorithms, wkdm_roundtrip_integer_heavy)
{
    minimem_assert_roundtrip(&minimem_wkdm_compressor,
                             pages[MINIMEM_PAGE_INTEGER_HEAVY]->data,
                             pages[MINIMEM_PAGE_INTEGER_HEAVY]->size);
}

Test(algorithms, wkdm_roundtrip_zero)
{
    minimem_assert_roundtrip(&minimem_wkdm_compressor,
                             pages[MINIMEM_PAGE_ZERO]->data,
                             pages[MINIMEM_PAGE_ZERO]->size);
}

Test(algorithms, wkdm_roundtrip_pte)
{
    minimem_assert_roundtrip(&minimem_wkdm_compressor,
                             pages[MINIMEM_PAGE_PTE]->data,
                             pages[MINIMEM_PAGE_PTE]->size);
}

Test(algorithms, wkdm_can_compress)
{
    cr_assert(minimem_wkdm_can_compress(pages[MINIMEM_PAGE_POINTER_HEAVY]->data,
                                         pages[MINIMEM_PAGE_POINTER_HEAVY]->size),
              "WKdm should be able to compress pointer-heavy pages");
}

Test(algorithms, wkdm64_roundtrip_pointer_heavy)
{
    minimem_assert_roundtrip(&minimem_wkdm64_compressor,
                             pages[MINIMEM_PAGE_POINTER_HEAVY]->data,
                             pages[MINIMEM_PAGE_POINTER_HEAVY]->size);
}

Test(algorithms, wkdm64_roundtrip_integer_heavy)
{
    minimem_assert_roundtrip(&minimem_wkdm64_compressor,
                             pages[MINIMEM_PAGE_INTEGER_HEAVY]->data,
                             pages[MINIMEM_PAGE_INTEGER_HEAVY]->size);
}

Test(algorithms, wkdm64_roundtrip_zero)
{
    minimem_assert_roundtrip(&minimem_wkdm64_compressor,
                             pages[MINIMEM_PAGE_ZERO]->data,
                             pages[MINIMEM_PAGE_ZERO]->size);
}

Test(algorithms, wkdm64_roundtrip_pte)
{
    minimem_assert_roundtrip(&minimem_wkdm64_compressor,
                             pages[MINIMEM_PAGE_PTE]->data,
                             pages[MINIMEM_PAGE_PTE]->size);
}

Test(algorithms, wkdm64_roundtrip_mixed)
{
    minimem_assert_roundtrip(&minimem_wkdm64_compressor,
                             pages[MINIMEM_PAGE_MIXED]->data,
                             pages[MINIMEM_PAGE_MIXED]->size);
}

Test(algorithms, wkdm64_can_compress)
{
    cr_assert(minimem_wkdm64_can_compress(pages[MINIMEM_PAGE_POINTER_HEAVY]->data,
                                           pages[MINIMEM_PAGE_POINTER_HEAVY]->size),
              "WKdm64 should be able to compress pointer-heavy pages");
}

Test(algorithms, block_class_roundtrip_zero)
{
    minimem_assert_roundtrip(&minimem_block_class_compressor,
                             pages[MINIMEM_PAGE_ZERO]->data,
                             pages[MINIMEM_PAGE_ZERO]->size);
}

Test(algorithms, block_class_roundtrip_int8)
{
    minimem_assert_roundtrip(&minimem_block_class_compressor,
                             pages[MINIMEM_PAGE_AI_INT8]->data,
                             pages[MINIMEM_PAGE_AI_INT8]->size);
}

Test(algorithms, block_class_roundtrip_sparse)
{
    minimem_assert_roundtrip(&minimem_block_class_compressor,
                             pages[MINIMEM_PAGE_AI_SPARSE]->data,
                             pages[MINIMEM_PAGE_AI_SPARSE]->size);
}

Test(algorithms, block_class_roundtrip_fp16)
{
    minimem_assert_roundtrip(&minimem_block_class_compressor,
                             pages[MINIMEM_PAGE_AI_FP16]->data,
                             pages[MINIMEM_PAGE_AI_FP16]->size);
}

Test(algorithms, block_class_roundtrip_mixed)
{
    minimem_assert_roundtrip(&minimem_block_class_compressor,
                             pages[MINIMEM_PAGE_MIXED]->data,
                             pages[MINIMEM_PAGE_MIXED]->size);
}

Test(algorithms, block_class_roundtrip_random)
{
    minimem_assert_roundtrip(&minimem_block_class_compressor,
                             pages[MINIMEM_PAGE_RANDOM]->data,
                             pages[MINIMEM_PAGE_RANDOM]->size);
}

Test(algorithms, block_class_roundtrip_pte)
{
    minimem_assert_roundtrip(&minimem_block_class_compressor,
                             pages[MINIMEM_PAGE_PTE]->data,
                             pages[MINIMEM_PAGE_PTE]->size);
}

Test(algorithms, delta_xor_roundtrip)
{
    struct minimem_page_data *pair = minimem_generate_page(MINIMEM_PAGE_DELTA_PAIR, SEED);
    cr_assert_neq(pair, NULL);
    cr_assert_neq(pair->delta_base, NULL);

    uint8_t *delta = malloc(pair->size);
    uint8_t *recovered = malloc(pair->size);
    cr_assert_neq(delta, NULL);
    cr_assert_neq(recovered, NULL);

    size_t delta_len = minimem_delta_xor(pair->data, pair->delta_base,
                                          pair->size, delta, pair->size);
    cr_assert_eq(delta_len, pair->size, "XOR delta should produce output of same size");

    size_t recovered_len = minimem_delta_xor_recover(delta, delta_len,
                                                       pair->delta_base, pair->size,
                                                       recovered, pair->size);
    cr_assert_eq(recovered_len, pair->size, "XOR recovery should produce output of same size");
    cr_assert_eq(memcmp(pair->data, recovered, pair->size), 0,
                 "XOR delta roundtrip should recover original data");

    free(delta);
    free(recovered);
    minimem_free_page(pair);
}

Test(algorithms, delta_xor_identity)
{
    uint8_t data[4096];
    uint8_t base[4096];
    uint8_t delta[4096];
    uint8_t recovered[4096];

    memset(data, 0xAB, sizeof(data));
    memset(base, 0xCD, sizeof(base));

    size_t delta_len = minimem_delta_xor(data, base, sizeof(data), delta, sizeof(data));
    cr_assert_eq(delta_len, sizeof(data));

    size_t recovered_len = minimem_delta_xor_recover(delta, delta_len,
                                                       base, sizeof(base),
                                                       recovered, sizeof(recovered));
    cr_assert_eq(recovered_len, sizeof(data));
    cr_assert_eq(memcmp(data, recovered, sizeof(data)), 0,
                 "XOR delta should roundtrip correctly on arbitrary data");
}

Test(algorithms, delta_xor_same_pages)
{
    uint8_t data[4096];
    uint8_t delta[4096];

    memset(data, 0x42, sizeof(data));

    size_t delta_len = minimem_delta_xor(data, data, sizeof(data), delta, sizeof(data));
    cr_assert_eq(delta_len, sizeof(data));

    for (size_t i = 0; i < sizeof(data); i++) {
        cr_assert_eq(delta[i], 0, "XOR of identical pages should be all zeros");
    }
}

Test(algorithms, all_algorithms_no_expansion)
{
    const struct minimem_compressor *algos[] = {
        &minimem_lz4_compressor,
        &minimem_bdi_compressor,
        &minimem_wkdm_compressor,
        &minimem_wkdm64_compressor,
        &minimem_block_class_compressor,
    };

    for (size_t a = 0; a < sizeof(algos) / sizeof(algos[0]); a++) {
        for (int i = 0; i < MINIMEM_PAGE_TYPE_COUNT; i++) {
            minimem_assert_no_expansion(algos[a], pages[i]->data, pages[i]->size);
        }
    }
}

Test(algorithms, registry_lookup)
{
    for (int i = 0; i < MINIMEM_ALGO_COUNT; i++) {
        const struct minimem_compressor *c = minimem_get_compressor(i);
        cr_assert_neq(c, NULL, "Compressor %d should be registered", i);
        cr_assert_eq(c->id, i, "Compressor ID should match index");
    }

    cr_assert_eq(minimem_get_compressor(-1), NULL, "Invalid ID should return NULL");
    cr_assert_eq(minimem_get_compressor(MINIMEM_ALGO_COUNT), NULL,
                 "Out of range ID should return NULL");
}

Test(algorithms, compress_bound_consistency)
{
    const struct minimem_compressor *algos[] = {
        &minimem_same_page_compressor,
        &minimem_bdi_compressor,
        &minimem_wkdm_compressor,
        &minimem_lz4_compressor,
        &minimem_zstd_dict_compressor,
        &minimem_delta_compressor,
    };

    for (size_t a = 0; a < sizeof(algos) / sizeof(algos[0]); a++) {
        size_t bound = algos[a]->compress_bound(MINIMEM_PAGE_SIZE);
        cr_assert_gt(bound, 0, "%s: compress_bound should be > 0", algos[a]->name);
    }
}

Test(algorithms, zstd_roundtrip_zero)
{
    minimem_assert_roundtrip(&minimem_zstd_dict_compressor,
                             pages[MINIMEM_PAGE_ZERO]->data,
                             pages[MINIMEM_PAGE_ZERO]->size);
}

Test(algorithms, zstd_roundtrip_pointer_heavy)
{
    minimem_assert_roundtrip(&minimem_zstd_dict_compressor,
                             pages[MINIMEM_PAGE_POINTER_HEAVY]->data,
                             pages[MINIMEM_PAGE_POINTER_HEAVY]->size);
}

Test(algorithms, zstd_roundtrip_pte)
{
    minimem_assert_roundtrip(&minimem_zstd_dict_compressor,
                             pages[MINIMEM_PAGE_PTE]->data,
                             pages[MINIMEM_PAGE_PTE]->size);
}

Test(algorithms, zstd_roundtrip_mixed)
{
    minimem_assert_roundtrip(&minimem_zstd_dict_compressor,
                             pages[MINIMEM_PAGE_MIXED]->data,
                             pages[MINIMEM_PAGE_MIXED]->size);
}

Test(algorithms, advisor_picks_same_page_for_zeros)
{
    struct minimem_page_stats stats = minimem_analyze_page(
        pages[MINIMEM_PAGE_ZERO]->data, pages[MINIMEM_PAGE_ZERO]->size);
    int algo = minimem_advise_algorithm(&stats);
    cr_assert_eq(algo, MINIMEM_ALGO_SAME_PAGE,
                 "Advisor should pick SAME_PAGE for zero page, got %d", algo);
}

Test(algorithms, advisor_picks_wkdm_for_pointers)
{
    struct minimem_page_stats stats = minimem_analyze_page(
        pages[MINIMEM_PAGE_POINTER_HEAVY]->data, pages[MINIMEM_PAGE_POINTER_HEAVY]->size);
    int algo = minimem_advise_algorithm(&stats);
    cr_assert(algo == MINIMEM_ALGO_WKDM || algo == MINIMEM_ALGO_WKDM64,
              "Advisor should pick WKdm for pointer-heavy page, got %d", algo);
}

Test(algorithms, advisor_best_finds_compression)
{
    int algos[] = { MINIMEM_ALGO_SAME_PAGE, MINIMEM_ALGO_BDI,
                    MINIMEM_ALGO_WKDM, MINIMEM_ALGO_LZ4 };
    int best = minimem_advise_best(
        pages[MINIMEM_PAGE_ZERO]->data, pages[MINIMEM_PAGE_ZERO]->size,
        algos, sizeof(algos) / sizeof(algos[0]));
    cr_assert(best >= 0, "Advisor should find a compressible algorithm");
}

Test(algorithms, ai_fp16_roundtrip_ai_fp16_page)
{
    minimem_assert_roundtrip(&minimem_ai_fp16_compressor,
                             pages[MINIMEM_PAGE_AI_FP16]->data,
                             pages[MINIMEM_PAGE_AI_FP16]->size);
}

Test(algorithms, ai_fp16_roundtrip_zero)
{
    minimem_assert_roundtrip(&minimem_ai_fp16_compressor,
                             pages[MINIMEM_PAGE_ZERO]->data,
                             pages[MINIMEM_PAGE_ZERO]->size);
}

Test(algorithms, ai_fp16_roundtrip_mixed)
{
    minimem_assert_roundtrip(&minimem_ai_fp16_compressor,
                             pages[MINIMEM_PAGE_MIXED]->data,
                             pages[MINIMEM_PAGE_MIXED]->size);
}

Test(algorithms, ai_bf16_roundtrip_ai_fp16_page)
{
    minimem_assert_roundtrip(&minimem_ai_bf16_compressor,
                             pages[MINIMEM_PAGE_AI_FP16]->data,
                             pages[MINIMEM_PAGE_AI_FP16]->size);
}

Test(algorithms, ai_bf16_roundtrip_zero)
{
    minimem_assert_roundtrip(&minimem_ai_bf16_compressor,
                             pages[MINIMEM_PAGE_ZERO]->data,
                             pages[MINIMEM_PAGE_ZERO]->size);
}

Test(algorithms, ai_int8_roundtrip_ai_int8_page)
{
    minimem_assert_roundtrip(&minimem_ai_int8_compressor,
                             pages[MINIMEM_PAGE_AI_INT8]->data,
                             pages[MINIMEM_PAGE_AI_INT8]->size);
}

Test(algorithms, ai_int8_roundtrip_zero)
{
    minimem_assert_roundtrip(&minimem_ai_int8_compressor,
                             pages[MINIMEM_PAGE_ZERO]->data,
                             pages[MINIMEM_PAGE_ZERO]->size);
}

Test(algorithms, ai_int8_roundtrip_integer_heavy)
{
    minimem_assert_roundtrip(&minimem_ai_int8_compressor,
                             pages[MINIMEM_PAGE_INTEGER_HEAVY]->data,
                             pages[MINIMEM_PAGE_INTEGER_HEAVY]->size);
}

Test(algorithms, ai_no_expansion)
{
    const struct minimem_compressor *algos[] = {
        &minimem_ai_fp16_compressor,
        &minimem_ai_bf16_compressor,
        &minimem_ai_int8_compressor,
    };
    for (size_t a = 0; a < sizeof(algos) / sizeof(algos[0]); a++) {
        for (int i = 0; i < MINIMEM_PAGE_TYPE_COUNT; i++) {
            minimem_assert_no_expansion(algos[a], pages[i]->data, pages[i]->size);
        }
    }
}