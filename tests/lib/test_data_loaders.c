#include <criterion/criterion.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

#include "lib/minimem.h"
#include "lib/test_data.h"

#define DATA_DIR "tests/data"

Test(data_loaders, load_page_dump_missing_file)
{
    struct minimem_real_data *d = minimem_load_page_dump("nonexistent_file.bin");
    cr_assert_eq(d, NULL, "Should return NULL for missing file");
}

Test(data_loaders, load_safetensors_missing_file)
{
    struct minimem_real_data *d = minimem_load_safetensors_weights("nonexistent.safetensors", "weight");
    cr_assert_eq(d, NULL, "Should return NULL for missing file");
}

Test(data_loaders, page_type_names)
{
    cr_assert_str_eq(minimem_page_type_name(MINIMEM_PAGE_RANDOM), "random");
    cr_assert_str_eq(minimem_page_type_name(MINIMEM_PAGE_ZERO), "zero");
    cr_assert_str_eq(minimem_page_type_name(MINIMEM_PAGE_REPEAT_VAL), "repeat_val");
    cr_assert_str_eq(minimem_page_type_name(MINIMEM_PAGE_POINTER_HEAVY), "pointer_heavy");
    cr_assert_str_eq(minimem_page_type_name(MINIMEM_PAGE_INTEGER_HEAVY), "integer_heavy");
    cr_assert_str_eq(minimem_page_type_name(MINIMEM_PAGE_PTE), "pte");
    cr_assert_str_eq(minimem_page_type_name(MINIMEM_PAGE_AI_FP16), "ai_fp16");
    cr_assert_str_eq(minimem_page_type_name(MINIMEM_PAGE_AI_INT8), "ai_int8");
    cr_assert_str_eq(minimem_page_type_name(MINIMEM_PAGE_AI_SPARSE), "ai_sparse");
    cr_assert_str_eq(minimem_page_type_name(MINIMEM_PAGE_DELTA_PAIR), "delta_pair");
    cr_assert_str_eq(minimem_page_type_name(MINIMEM_PAGE_MIXED), "mixed");
}

Test(data_loaders, page_type_descriptions)
{
    for (int i = 0; i < MINIMEM_PAGE_TYPE_COUNT; i++) {
        const char *desc = minimem_page_type_description(i);
        cr_assert_neq(desc, NULL, "Page type %d should have a description", i);
        cr_assert_gt(strlen(desc), 0, "Page type %d description should not be empty", i);
    }
}