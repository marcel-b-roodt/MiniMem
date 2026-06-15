#ifndef MINIMEM_TEST_DATA_H
#define MINIMEM_TEST_DATA_H

#include <stddef.h>
#include <stdint.h>

#define MINIMEM_PAGE_SIZE 4096

enum minimem_page_type {
    MINIMEM_PAGE_RANDOM,
    MINIMEM_PAGE_ZERO,
    MINIMEM_PAGE_REPEAT_VAL,
    MINIMEM_PAGE_POINTER_HEAVY,
    MINIMEM_PAGE_INTEGER_HEAVY,
    MINIMEM_PAGE_PTE,
    MINIMEM_PAGE_AI_FP16,
    MINIMEM_PAGE_AI_INT8,
    MINIMEM_PAGE_AI_SPARSE,
    MINIMEM_PAGE_DELTA_PAIR,
    MINIMEM_PAGE_MIXED,
    MINIMEM_PAGE_TYPE_COUNT,
};

struct minimem_page_data {
    enum minimem_page_type type;
    size_t size;
    uint8_t *data;
    uint8_t *delta_base;
    char name[32];
    char description[128];
};

struct minimem_page_data *
minimem_generate_page(enum minimem_page_type type, uint64_t seed);

void minimem_free_page(struct minimem_page_data *page);

const char *minimem_page_type_name(enum minimem_page_type type);

const char *minimem_page_type_description(enum minimem_page_type type);

struct minimem_page_data **
minimem_generate_all_pages(uint64_t seed);

void minimem_free_all_pages(struct minimem_page_data **pages);

struct minimem_real_data {
    char path[256];
    size_t size;
    uint8_t *data;
    char label[64];
};

struct minimem_real_data *
minimem_load_page_dump(const char *path);

struct minimem_real_data *
minimem_load_safetensors_weights(const char *path, const char *tensor_name);

void minimem_free_real_data(struct minimem_real_data *data);

#endif