#include "lib/test_data.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t splitmix64(uint64_t *state)
{
    uint64_t z = (*state += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

static void fill_random(uint8_t *buf, size_t len, uint64_t seed)
{
    uint64_t state = seed;
    for (size_t i = 0; i < len; i += sizeof(uint64_t)) {
        uint64_t val = splitmix64(&state);
        size_t copy = sizeof(uint64_t);
        if (i + copy > len)
            copy = len - i;
        memcpy(buf + i, &val, copy);
    }
}

static struct minimem_page_data *alloc_page(enum minimem_page_type type, size_t size)
{
    struct minimem_page_data *p = calloc(1, sizeof(*p));
    if (!p)
        return NULL;
    p->type = type;
    p->size = size;
    p->data = calloc(1, size);
    if (!p->data) {
        free(p);
        return NULL;
    }
    p->delta_base = NULL;
    strncpy(p->name, minimem_page_type_name(type), sizeof(p->name) - 1);
    strncpy(p->description, minimem_page_type_description(type),
            sizeof(p->description) - 1);
    return p;
}

static void generate_pointer_heavy(uint8_t *buf, size_t len, uint64_t seed)
{
    uint64_t state = seed;
    uint64_t base_ptr = 0xffff800000000000ULL;
    size_t n_words = len / sizeof(uint64_t);

    for (size_t i = 0; i < n_words; i++) {
        uint64_t val;
        uint64_t r = splitmix64(&state);
        if (r % 4 == 0) {
            val = 0;
        } else if (r % 4 == 1) {
            val = base_ptr + (splitmix64(&state) & 0x0fffffffULL);
        } else if (r % 4 == 2) {
            val = splitmix64(&state) & 0x0000000000ffffffULL;
        } else {
            val = base_ptr + ((r >> 8) & 0xfffULL);
        }
        memcpy(buf + i * sizeof(uint64_t), &val, sizeof(uint64_t));
    }
}

static void generate_integer_heavy(uint8_t *buf, size_t len, uint64_t seed)
{
    uint64_t state = seed;
    size_t n_words = len / sizeof(uint64_t);

    for (size_t i = 0; i < n_words; i++) {
        uint64_t r = splitmix64(&state);
        uint64_t val;
        if (r % 5 == 0) {
            val = 0;
        } else if (r % 5 == 1) {
            val = splitmix64(&state) & 0xffULL;
        } else if (r % 5 == 2) {
            val = splitmix64(&state) & 0xffffULL;
        } else if (r % 5 == 3) {
            val = splitmix64(&state) & 0xffffffffULL;
        } else {
            val = (splitmix64(&state) >> 16) & 0xffULL;
        }
        memcpy(buf + i * sizeof(uint64_t), &val, sizeof(uint64_t));
    }
}

static void generate_pte(uint8_t *buf, size_t len, uint64_t seed)
{
    uint64_t state = seed;
    size_t n_entries = len / sizeof(uint64_t);
    uint64_t base_pfn = 0x100000ULL;
    uint64_t flags = 0x8000000000000063ULL;

    for (size_t i = 0; i < n_entries; i++) {
        uint64_t r = splitmix64(&state);
        uint64_t pte;
        if (r % 10 == 0) {
            pte = 0;
        } else {
            uint64_t pfn = base_pfn + i + (splitmix64(&state) % 4);
            pte = (pfn << 12) | (flags & ~(0xfffff000ULL));
            if (r % 3 == 0)
                pte &= ~(1ULL << 5);
        }
        memcpy(buf + i * sizeof(uint64_t), &pte, sizeof(uint64_t));
    }
}

static uint16_t float_to_bf16(float f)
{
    uint32_t bits;
    memcpy(&bits, &f, sizeof(bits));
    return (uint16_t)(bits >> 16);
}

static void generate_ai_fp16(uint8_t *buf, size_t len, uint64_t seed)
{
    uint64_t state = seed;
    size_t n_values = len / sizeof(uint16_t);

    float base = -0.5f + (float)(splitmix64(&state) % 1000) / 1000.0f;
    float scale = 0.01f;

    float *values = malloc(n_values * sizeof(float));
    if (!values)
        return;

    for (size_t i = 0; i < n_values; i++) {
        uint64_t r = splitmix64(&state);
        if (r % 20 == 0) {
            values[i] = 0.0f;
        } else {
            float noise = (float)((int64_t)(r & 0xff) - 128) * scale;
            values[i] = base + noise;
        }
        uint16_t bf16 = float_to_bf16(values[i]);
        memcpy(buf + i * sizeof(uint16_t), &bf16, sizeof(uint16_t));
    }

    free(values);
}

static void generate_ai_int8(uint8_t *buf, size_t len, uint64_t seed)
{
    uint64_t state = seed;
    int8_t base = (int8_t)((splitmix64(&state) >> 8) & 0xff);

    for (size_t i = 0; i < len; i++) {
        uint64_t r = splitmix64(&state);
        if (r % 10 == 0) {
            buf[i] = 0;
        } else if (r % 5 == 0) {
            buf[i] = base;
        } else {
            int8_t delta = (int8_t)((r >> 16) & 0x1f) - 16;
            buf[i] = (int8_t)(base + delta);
        }
    }
}

static void generate_ai_sparse(uint8_t *buf, size_t len, uint64_t seed)
{
    uint64_t state = seed;
    memset(buf, 0, len);

    for (size_t i = 0; i < len; i += 64) {
        uint64_t r = splitmix64(&state);
        if (r % 3 == 0) {
            continue;
        }
        size_t end = i + 64;
        if (end > len)
            end = len;
        fill_random(buf + i, end - i, splitmix64(&state));
    }
}

static void generate_delta_pair(uint8_t *buf, size_t len, uint64_t seed)
{
    fill_random(buf, len, seed);
}

static void generate_mixed(uint8_t *buf, size_t len, uint64_t seed)
{
    uint64_t state = seed;
    size_t half = len / 2;
    fill_random(buf, half, seed);
    generate_pointer_heavy(buf + half, len - half, splitmix64(&state));
}

struct minimem_page_data *
minimem_generate_page(enum minimem_page_type type, uint64_t seed)
{
    struct minimem_page_data *p = alloc_page(type, MINIMEM_PAGE_SIZE);
    if (!p)
        return NULL;

    switch (type) {
    case MINIMEM_PAGE_RANDOM:
        fill_random(p->data, p->size, seed);
        break;
    case MINIMEM_PAGE_ZERO:
        memset(p->data, 0, p->size);
        break;
    case MINIMEM_PAGE_REPEAT_VAL: {
        uint64_t val = 0xDEADBEEFCAFEBABEULL;
        for (size_t i = 0; i < p->size; i += sizeof(uint64_t))
            memcpy(p->data + i, &val, sizeof(uint64_t));
        break;
    }
    case MINIMEM_PAGE_POINTER_HEAVY:
        generate_pointer_heavy(p->data, p->size, seed);
        break;
    case MINIMEM_PAGE_INTEGER_HEAVY:
        generate_integer_heavy(p->data, p->size, seed);
        break;
    case MINIMEM_PAGE_PTE:
        generate_pte(p->data, p->size, seed);
        break;
    case MINIMEM_PAGE_AI_FP16:
        generate_ai_fp16(p->data, p->size, seed);
        break;
    case MINIMEM_PAGE_AI_INT8:
        generate_ai_int8(p->data, p->size, seed);
        break;
    case MINIMEM_PAGE_AI_SPARSE:
        generate_ai_sparse(p->data, p->size, seed);
        break;
    case MINIMEM_PAGE_DELTA_PAIR: {
        generate_delta_pair(p->data, p->size, seed);
        p->delta_base = calloc(1, p->size);
        if (!p->delta_base) {
            free(p->data);
            free(p);
            return NULL;
        }
        memcpy(p->delta_base, p->data, p->size);
        uint64_t state = seed + 1;
        size_t change_count = p->size / 80;
        for (size_t i = 0; i < change_count; i++) {
            size_t offset = splitmix64(&state) % p->size;
            p->data[offset] = (uint8_t)(splitmix64(&state) & 0xff);
        }
        break;
    }
    case MINIMEM_PAGE_MIXED:
        generate_mixed(p->data, p->size, seed);
        break;
    default:
        free(p->data);
        free(p);
        return NULL;
    }
    return p;
}

void minimem_free_page(struct minimem_page_data *page)
{
    if (!page)
        return;
    free(page->data);
    free(page->delta_base);
    free(page);
}

const char *minimem_page_type_name(enum minimem_page_type type)
{
    static const char *names[] = {
        [MINIMEM_PAGE_RANDOM]        = "random",
        [MINIMEM_PAGE_ZERO]          = "zero",
        [MINIMEM_PAGE_REPEAT_VAL]    = "repeat_val",
        [MINIMEM_PAGE_POINTER_HEAVY] = "pointer_heavy",
        [MINIMEM_PAGE_INTEGER_HEAVY] = "integer_heavy",
        [MINIMEM_PAGE_PTE]           = "pte",
        [MINIMEM_PAGE_AI_FP16]       = "ai_fp16",
        [MINIMEM_PAGE_AI_INT8]        = "ai_int8",
        [MINIMEM_PAGE_AI_SPARSE]      = "ai_sparse",
        [MINIMEM_PAGE_DELTA_PAIR]      = "delta_pair",
        [MINIMEM_PAGE_MIXED]          = "mixed",
    };
    if (type >= MINIMEM_PAGE_TYPE_COUNT)
        return "unknown";
    return names[type];
}

const char *minimem_page_type_description(enum minimem_page_type type)
{
    static const char *descs[] = {
        [MINIMEM_PAGE_RANDOM]        = "Cryptographically random bytes - worst-case baseline",
        [MINIMEM_PAGE_ZERO]          = "All zeros - same-page detection",
        [MINIMEM_PAGE_REPEAT_VAL]    = "Repeated single 8-byte value (0xDEADBEEFCAFEBABE)",
        [MINIMEM_PAGE_POINTER_HEAVY] = "Simulated 64-bit pointer page with many zero upper bytes",
        [MINIMEM_PAGE_INTEGER_HEAVY] = "Simulated integer page with many small values",
        [MINIMEM_PAGE_PTE]           = "Simulated page table entries (PFN + flags)",
        [MINIMEM_PAGE_AI_FP16]       = "Simulated BF16 weight block with clustered values",
        [MINIMEM_PAGE_AI_INT8]        = "Simulated INT8 quantized weight block",
        [MINIMEM_PAGE_AI_SPARSE]      = "Simulated sparse weight block with many zero cache lines",
        [MINIMEM_PAGE_DELTA_PAIR]      = "Two pages differing by ~5% - delta encoding",
        [MINIMEM_PAGE_MIXED]          = "50% random + 50% pointer-heavy — general baseline",
    };
    if (type >= MINIMEM_PAGE_TYPE_COUNT)
        return "unknown";
    return descs[type];
}

struct minimem_page_data **
minimem_generate_all_pages(uint64_t seed)
{
    struct minimem_page_data **pages =
        calloc(MINIMEM_PAGE_TYPE_COUNT, sizeof(struct minimem_page_data *));
    if (!pages)
        return NULL;

    for (int i = 0; i < MINIMEM_PAGE_TYPE_COUNT; i++) {
        pages[i] = minimem_generate_page(i, seed + (uint64_t)i);
        if (!pages[i]) {
            minimem_free_all_pages(pages);
            return NULL;
        }
    }
    return pages;
}

void minimem_free_all_pages(struct minimem_page_data **pages)
{
    if (!pages)
        return;
    for (int i = 0; i < MINIMEM_PAGE_TYPE_COUNT; i++)
        minimem_free_page(pages[i]);
    free(pages);
}

struct minimem_real_data *
minimem_load_page_dump(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize <= 0 || fsize % MINIMEM_PAGE_SIZE != 0) {
        fclose(f);
        return NULL;
    }

    struct minimem_real_data *d = calloc(1, sizeof(*d));
    if (!d) {
        fclose(f);
        return NULL;
    }

    d->size = (size_t)fsize;
    d->data = malloc(d->size);
    if (!d->data) {
        free(d);
        fclose(f);
        return NULL;
    }

    if (fread(d->data, 1, d->size, f) != d->size) {
        free(d->data);
        free(d);
        fclose(f);
        return NULL;
    }

    fclose(f);
    snprintf(d->path, sizeof(d->path), "%s", path);
    snprintf(d->label, sizeof(d->label), "page_dump:%s", path);
    return d;
}

struct minimem_real_data *
minimem_load_safetensors_weights(const char *path, const char *tensor_name)
{
    (void)tensor_name;
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize <= 0) {
        fclose(f);
        return NULL;
    }

    struct minimem_real_data *d = calloc(1, sizeof(*d));
    if (!d) {
        fclose(f);
        return NULL;
    }

    d->size = (size_t)fsize;
    d->data = malloc(d->size);
    if (!d->data) {
        free(d);
        fclose(f);
        return NULL;
    }

    if (fread(d->data, 1, d->size, f) != d->size) {
        free(d->data);
        free(d);
        fclose(f);
        return NULL;
    }

    fclose(f);
    snprintf(d->path, sizeof(d->path), "%s", path);
    snprintf(d->label, sizeof(d->label), "safetensors:%s#%s",
             path, tensor_name ? tensor_name : "all");
    return d;
}

void minimem_free_real_data(struct minimem_real_data *data)
{
    if (!data)
        return;
    free(data->data);
    free(data);
}