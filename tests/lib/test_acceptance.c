#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 199309L
#endif

#include <criterion/criterion.h>
#include <criterion/parameterized.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lib/minimem.h"
#include "lib/test_data.h"
#include "lib/compressors/same_page.h"
#include "lib/compressors/bdi.h"
#include "lib/compressors/wkdm.h"
#include "lib/compressors/lz4_wrap.h"
#include "lib/compressors/lzsse8.h"
#include "lib/compressors/zstd_dict.h"
#include "lib/compressors/delta.h"
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

TestSuite(acceptance, .init = suite_setup, .fini = suite_teardown);

Test(acceptance, same_page_zero_roundtrip)
{
    struct minimem_page_data *zero_page = minimem_generate_page(MINIMEM_PAGE_ZERO, SEED);
    cr_assert_neq(zero_page, NULL);
    minimem_assert_roundtrip(&minimem_same_page_compressor,
                             zero_page->data, zero_page->size);
    minimem_free_page(zero_page);
}

Test(acceptance, same_page_repeat_val_not_compressible)
{
    struct minimem_page_data *rv_page = minimem_generate_page(MINIMEM_PAGE_REPEAT_VAL, SEED);
    cr_assert_neq(rv_page, NULL);
    cr_assert_eq(minimem_same_page_can_compress(rv_page->data, rv_page->size),
                 false,
                 "same_page should not compress REPEAT_VAL (bytes differ)");
    minimem_free_page(rv_page);
}

Test(acceptance, same_page_cannot_compress_random)
{
    struct minimem_page_data *rand_page = minimem_generate_page(MINIMEM_PAGE_RANDOM, SEED);
    cr_assert_neq(rand_page, NULL);
    cr_assert_eq(minimem_same_page_can_compress(rand_page->data, rand_page->size),
                 false,
                 "same_page should not compress random data");
    minimem_free_page(rand_page);
}

Test(acceptance, same_page_ratio_zero)
{
    struct minimem_page_data *zero_page = minimem_generate_page(MINIMEM_PAGE_ZERO, SEED);
    cr_assert_neq(zero_page, NULL);

    size_t bound = minimem_same_page_compress_bound(zero_page->size);
    uint8_t *compressed = malloc(bound);
    size_t csz = minimem_same_page_compress(zero_page->data, zero_page->size,
                                            compressed, bound);
    cr_assert_gt(csz, 0);
    double ratio = (double)zero_page->size / (double)csz;
    cr_assert_gt(ratio, 100.0,
        "same_page on zero data should achieve >100:1 ratio, got %.1f:1", ratio);

    free(compressed);
    minimem_free_page(zero_page);
}

Test(acceptance, same_page_decompress_latency)
{
    struct minimem_page_data *zero_page = minimem_generate_page(MINIMEM_PAGE_ZERO, SEED);
    cr_assert_neq(zero_page, NULL);

    size_t bound = minimem_same_page_compress_bound(zero_page->size);
    uint8_t *compressed = malloc(bound);
    uint8_t *decompressed = malloc(zero_page->size);

    size_t csz = minimem_same_page_compress(zero_page->data, zero_page->size,
                                            compressed, bound);
    cr_assert_gt(csz, 0);

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < 1000; i++) {
        minimem_same_page_decompress(compressed, csz, decompressed, zero_page->size);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    double avg_ns = bench_elapsed_ns(&start, &end) / 1000.0;
    cr_assert_lt(avg_ns, 1000.0,
        "same_page decompress should be <1us avg, got %.0f ns", avg_ns);

    free(compressed);
    free(decompressed);
    minimem_free_page(zero_page);
}

Test(acceptance, roundtrip_all_page_types)
{
    for (int i = 0; i < MINIMEM_PAGE_TYPE_COUNT; i++) {
        cr_assert_neq(pages[i], NULL,
            "Page type %d (%s) should not be NULL", i,
            minimem_page_type_name(i));
        cr_assert_eq(pages[i]->size, MINIMEM_PAGE_SIZE,
            "Page type %d (%s) should have size %d, got %zu",
            i, minimem_page_type_name(i), MINIMEM_PAGE_SIZE, pages[i]->size);
    }
}

Test(acceptance, test_data_deterministic)
{
    struct minimem_page_data *p1 = minimem_generate_page(MINIMEM_PAGE_RANDOM, SEED);
    struct minimem_page_data *p2 = minimem_generate_page(MINIMEM_PAGE_RANDOM, SEED);
    cr_assert_neq(p1, NULL);
    cr_assert_neq(p2, NULL);
    cr_assert_eq(memcmp(p1->data, p2->data, p1->size), 0,
        "Same seed should produce identical pages");
    minimem_free_page(p1);
    minimem_free_page(p2);
}

Test(acceptance, test_data_different_seeds)
{
    struct minimem_page_data *p1 = minimem_generate_page(MINIMEM_PAGE_RANDOM, SEED);
    struct minimem_page_data *p2 = minimem_generate_page(MINIMEM_PAGE_RANDOM, SEED + 1);
    cr_assert_neq(p1, NULL);
    cr_assert_neq(p2, NULL);
    cr_assert_neq(memcmp(p1->data, p2->data, p1->size), 0,
        "Different seeds should produce different pages");
    minimem_free_page(p1);
    minimem_free_page(p2);
}