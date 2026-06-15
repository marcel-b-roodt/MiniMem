#ifndef MINIMEM_TEST_ROUNDTRIP_H
#define MINIMEM_TEST_ROUNDTRIP_H

#include <string.h>
#include <stddef.h>
#include <stdint.h>
#include <criterion/criterion.h>
#include "lib/minimem.h"

static inline void minimem_assert_roundtrip(
    const struct minimem_compressor *c,
    const uint8_t *src, size_t src_len)
{
    size_t bound = c->compress_bound(src_len);
    uint8_t *compressed = malloc(bound);
    uint8_t *decompressed = malloc(src_len);
    cr_assert_neq(compressed, NULL, "compressed buffer allocation failed");
    cr_assert_neq(decompressed, NULL, "decompressed buffer allocation failed");

    size_t csz = c->compress(src, src_len, compressed, bound);
    if (csz == 0) {
        free(compressed);
        free(decompressed);
        return;
    }

    cr_assert_leq(csz, src_len,
        "%s: compressed size %zu > original size %zu (expansion)", c->name, csz, src_len);

    size_t dsz = c->decompress(compressed, csz, decompressed, src_len);
    cr_assert_eq(dsz, src_len,
        "%s: decompressed size %zu != original size %zu", c->name, dsz, src_len);
    cr_assert_eq(memcmp(src, decompressed, src_len), 0,
        "%s: decompressed data does not match original", c->name);

    free(compressed);
    free(decompressed);
}

static inline void minimem_assert_no_expansion(
    const struct minimem_compressor *c,
    const uint8_t *src, size_t src_len)
{
    size_t bound = c->compress_bound(src_len);
    uint8_t *compressed = malloc(bound);
    cr_assert_neq(compressed, NULL, "compressed buffer allocation failed");

    size_t csz = c->compress(src, src_len, compressed, bound);
    if (csz > 0) {
        cr_assert_leq(csz, src_len,
            "%s: compressed size %zu > original size %zu (expansion)", c->name, csz, src_len);
    }

    free(compressed);
}

#endif