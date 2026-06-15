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

TestSuite(lz4, .init = suite_setup, .fini = suite_teardown);

Test(lz4, roundtrip_random)
{
    minimem_assert_roundtrip(&minimem_lz4_compressor,
                             pages[MINIMEM_PAGE_RANDOM]->data,
                             pages[MINIMEM_PAGE_RANDOM]->size);
}

Test(lz4, roundtrip_zero)
{
    minimem_assert_roundtrip(&minimem_lz4_compressor,
                             pages[MINIMEM_PAGE_ZERO]->data,
                             pages[MINIMEM_PAGE_ZERO]->size);
}

Test(lz4, roundtrip_repeat_val)
{
    minimem_assert_roundtrip(&minimem_lz4_compressor,
                             pages[MINIMEM_PAGE_REPEAT_VAL]->data,
                             pages[MINIMEM_PAGE_REPEAT_VAL]->size);
}

Test(lz4, roundtrip_pointer_heavy)
{
    minimem_assert_roundtrip(&minimem_lz4_compressor,
                             pages[MINIMEM_PAGE_POINTER_HEAVY]->data,
                             pages[MINIMEM_PAGE_POINTER_HEAVY]->size);
}

Test(lz4, roundtrip_integer_heavy)
{
    minimem_assert_roundtrip(&minimem_lz4_compressor,
                             pages[MINIMEM_PAGE_INTEGER_HEAVY]->data,
                             pages[MINIMEM_PAGE_INTEGER_HEAVY]->size);
}

Test(lz4, roundtrip_pte)
{
    minimem_assert_roundtrip(&minimem_lz4_compressor,
                             pages[MINIMEM_PAGE_PTE]->data,
                             pages[MINIMEM_PAGE_PTE]->size);
}

Test(lz4, roundtrip_ai_fp16)
{
    minimem_assert_roundtrip(&minimem_lz4_compressor,
                             pages[MINIMEM_PAGE_AI_FP16]->data,
                             pages[MINIMEM_PAGE_AI_FP16]->size);
}

Test(lz4, roundtrip_ai_int8)
{
    minimem_assert_roundtrip(&minimem_lz4_compressor,
                             pages[MINIMEM_PAGE_AI_INT8]->data,
                             pages[MINIMEM_PAGE_AI_INT8]->size);
}

Test(lz4, roundtrip_ai_sparse)
{
    minimem_assert_roundtrip(&minimem_lz4_compressor,
                             pages[MINIMEM_PAGE_AI_SPARSE]->data,
                             pages[MINIMEM_PAGE_AI_SPARSE]->size);
}

Test(lz4, roundtrip_mixed)
{
    minimem_assert_roundtrip(&minimem_lz4_compressor,
                             pages[MINIMEM_PAGE_MIXED]->data,
                             pages[MINIMEM_PAGE_MIXED]->size);
}

Test(lz4, no_expansion)
{
    for (int i = 0; i < MINIMEM_PAGE_TYPE_COUNT; i++) {
        minimem_assert_no_expansion(&minimem_lz4_compressor,
                                    pages[i]->data, pages[i]->size);
    }
}

Test(lz4, compress_bound)
{
    for (size_t len = 1; len <= 65536; len *= 2) {
        size_t bound = minimem_lz4_compress_bound(len);
        cr_assert_gt(bound, len, "compress_bound should be > src_len");
    }
}

Test(lz4, can_compress)
{
    uint8_t data[4096];
    memset(data, 0, sizeof(data));
    cr_assert(minimem_lz4_can_compress(data, sizeof(data)));

    cr_assert(minimem_lz4_can_compress(data, 1));
    cr_assert(!minimem_lz4_can_compress(data, 0));
}

Test(lz4, ratio_zero_page)
{
    struct minimem_page_data *zero_page = minimem_generate_page(MINIMEM_PAGE_ZERO, SEED);
    cr_assert_neq(zero_page, NULL);

    struct minimem_bench_result r =
        minimem_bench_compressor(&minimem_lz4_compressor,
                                 zero_page->data, zero_page->size,
                                 MINIMEM_PAGE_ZERO);
    cr_assert_gt(r.ratio, 1.0, "LZ4 should compress zero page at all");
    cr_assert_gt(r.compressed_size, 0, "LZ4 should produce output");

    minimem_free_page(zero_page);
}

Test(lz4, ratio_repeat_val)
{
    struct minimem_page_data *rv_page = minimem_generate_page(MINIMEM_PAGE_REPEAT_VAL, SEED);
    cr_assert_neq(rv_page, NULL);

    struct minimem_bench_result r =
        minimem_bench_compressor(&minimem_lz4_compressor,
                                 rv_page->data, rv_page->size,
                                 MINIMEM_PAGE_REPEAT_VAL);
    cr_assert_gt(r.ratio, 10.0,
        "LZ4 should achieve >10:1 on repeated-value page, got %.1f:1", r.ratio);

    minimem_free_page(rv_page);
}

Test(lz4, ratio_pointer_heavy)
{
    struct minimem_page_data *ph_page = minimem_generate_page(MINIMEM_PAGE_POINTER_HEAVY, SEED);
    cr_assert_neq(ph_page, NULL);

    struct minimem_bench_result r =
        minimem_bench_compressor(&minimem_lz4_compressor,
                                 ph_page->data, ph_page->size,
                                 MINIMEM_PAGE_POINTER_HEAVY);
    cr_assert_gt(r.ratio, 1.2,
        "LZ4 should achieve >1.2:1 on pointer-heavy page, got %.1f:1", r.ratio);

    minimem_free_page(ph_page);
}

Test(lz4, decompress_latency)
{
    struct minimem_page_data *ph_page = minimem_generate_page(MINIMEM_PAGE_POINTER_HEAVY, SEED);
    cr_assert_neq(ph_page, NULL);

    struct minimem_bench_result r =
        minimem_bench_compressor(&minimem_lz4_compressor,
                                 ph_page->data, ph_page->size,
                                 MINIMEM_PAGE_POINTER_HEAVY);
    cr_assert_gt(r.decompress_latency_us, 0.0, "Should have non-zero latency measurement");
    cr_assert_lt(r.decompress_latency_us, 100.0,
        "LZ4 decompress should be <100us per 4KB page, got %.1f us",
        r.decompress_latency_us);

    minimem_free_page(ph_page);
}