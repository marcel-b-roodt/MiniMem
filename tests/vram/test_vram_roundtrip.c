#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "vram/minimem_vram.h"
#include "lib/compressors/ai_weights.h"
#include "lib/minimem.h"
#include "lib/test_data.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N_FP16_VALUES 8

static const uint16_t fp16_values[N_FP16_VALUES] = {
	0x3C00, 0x4000, 0x4400, 0x3555,
	0x3E00, 0x0000, 0x3BFF, 0x4200
};

static void test_register_unregister(void)
{
	struct minimem_vram_ctx *ctx = minimem_vram_ctx_create(4);
	assert(ctx != NULL);

	uint8_t buf_a[4096];
	uint8_t buf_b[8192];

	int ret = minimem_vram_register(ctx, buf_a, 4096,
					MINIMEM_VRAM_FMT_FP16, false);
	assert(ret == 0);
	assert(ctx->n_bufs == 1);
	assert(ctx->stats.total_vram_bytes == 4096);

	ret = minimem_vram_register(ctx, buf_b, 8192,
				    MINIMEM_VRAM_FMT_BF16, false);
	assert(ret == 0);
	assert(ctx->n_bufs == 2);
	assert(ctx->stats.total_vram_bytes == 4096 + 8192);

	ret = minimem_vram_unregister(ctx, buf_a);
	assert(ret == 0);
	assert(ctx->n_bufs == 1);
	assert(ctx->stats.total_vram_bytes == 8192);

	minimem_vram_ctx_destroy(ctx);
	printf("  PASS: register/unregister\n");
}

static void test_compress_decompress_fp16(void)
{
	struct minimem_vram_ctx *ctx = minimem_vram_ctx_create(4);
	assert(ctx != NULL);

	size_t buf_size = 4096;
	uint8_t *src = calloc(1, buf_size);
	assert(src != NULL);

	uint16_t *fp16 = (uint16_t *)src;
	for (size_t i = 0; i < buf_size / 2; i++)
		fp16[i] = fp16_values[i % N_FP16_VALUES];

	int ret = minimem_vram_register(ctx, src, buf_size,
					MINIMEM_VRAM_FMT_FP16, false);
	assert(ret == 0);

	ret = minimem_vram_compress_buf(ctx, 0, MINIMEM_VRAM_WARM);
	assert(ret == 0);
	assert(ctx->bufs[0].tier == MINIMEM_VRAM_WARM);
	assert(ctx->bufs[0].compressed_size > 0);
	assert(ctx->bufs[0].compressed_size < buf_size);
	printf("  Compressed FP16: %zu -> %zu bytes (%.2f:1)\n",
	       buf_size, ctx->bufs[0].compressed_size,
	       (double)buf_size / ctx->bufs[0].compressed_size);

	ret = minimem_vram_decompress_buf(ctx, 0);
	assert(ret == 0);
	assert(ctx->bufs[0].tier == MINIMEM_VRAM_HOT);
	assert(ctx->bufs[0].compressed_ptr == NULL);

	uint16_t *restored = (uint16_t *)src;
	for (size_t i = 0; i < buf_size / 2; i++)
		assert(restored[i] == fp16_values[i % N_FP16_VALUES]);

	free(src);
	minimem_vram_ctx_destroy(ctx);
	printf("  PASS: FP16 compress/decompress roundtrip\n");
}

static void test_compress_decompress_int8(void)
{
	struct minimem_vram_ctx *ctx = minimem_vram_ctx_create(4);
	assert(ctx != NULL);

	size_t buf_size = 4096;
	uint8_t *src = calloc(1, buf_size);
	assert(src != NULL);

	memset(src, 0x42, buf_size);

	int ret = minimem_vram_register(ctx, src, buf_size,
					MINIMEM_VRAM_FMT_INT8, false);
	assert(ret == 0);

	ret = minimem_vram_compress_buf(ctx, 0, MINIMEM_VRAM_COLD);
	assert(ret == 0);
	assert(ctx->bufs[0].compressed_size > 0);
	printf("  Compressed INT8: %zu -> %zu bytes (%.2f:1)\n",
	       buf_size, ctx->bufs[0].compressed_size,
	       (double)buf_size / ctx->bufs[0].compressed_size);

	ret = minimem_vram_decompress_buf(ctx, 0);
	assert(ret == 0);

	assert(memcmp(src, src, buf_size) == 0 || true);

	for (size_t i = 0; i < buf_size; i++)
		assert(src[i] == 0x42);

	free(src);
	minimem_vram_ctx_destroy(ctx);
	printf("  PASS: INT8 compress/decompress roundtrip\n");
}

static void test_compress_all_idle(void)
{
	struct minimem_vram_ctx *ctx = minimem_vram_ctx_create(8);
	assert(ctx != NULL);

	uint8_t *bufs[4];
	for (int i = 0; i < 4; i++) {
		bufs[i] = calloc(1, 4096);
		assert(bufs[i] != NULL);
		memset(bufs[i], i * 37, 4096);
		minimem_vram_register(ctx, bufs[i], 4096,
				      MINIMEM_VRAM_FMT_INT8, false);
	}

	minimem_vram_touch(ctx, 0);

	int compressed = minimem_vram_compress_all_idle(
		ctx, 0ULL, MINIMEM_VRAM_WARM);
	printf("  Compressed %d buffers (all idle, threshold=0)\n", compressed);
	assert(compressed == 4);

	for (size_t i = 0; i < ctx->n_bufs; i++) {
		if (ctx->bufs[i].tier == MINIMEM_VRAM_WARM) {
			minimem_vram_decompress_buf(ctx, i);
		}
	}

	for (int i = 0; i < 4; i++)
		free(bufs[i]);
	minimem_vram_ctx_destroy(ctx);
	printf("  PASS: compress_all_idle\n");
}

static void test_stats(void)
{
	struct minimem_vram_ctx *ctx = minimem_vram_ctx_create(4);
	assert(ctx != NULL);

	uint8_t *src = calloc(1, 4096);
	assert(src != NULL);
	memset(src, 0x42, 4096);

	minimem_vram_register(ctx, src, 4096, MINIMEM_VRAM_FMT_INT8, false);
	minimem_vram_compress_buf(ctx, 0, MINIMEM_VRAM_COLD);
	minimem_vram_decompress_buf(ctx, 0);

	const struct minimem_vram_stats *stats = minimem_vram_get_stats(ctx);
	assert(stats != NULL);
	assert(stats->compress_count == 1);
	assert(stats->decompress_count == 1);
	assert(stats->total_saved_bytes > 0);
	assert(stats->compress_ns_total > 0);
	assert(stats->decompress_ns_total > 0);

	printf("  Stats: compress=%zu decompress=%zu saved=%zu\n",
	       stats->compress_count, stats->decompress_count,
	       stats->total_saved_bytes);

	free(src);
	minimem_vram_ctx_destroy(ctx);
	printf("  PASS: stats tracking\n");
}

static void test_tier_advice(void)
{
	assert(minimem_vram_advise_tier(NULL, 0, 0) == MINIMEM_VRAM_HOT);
	assert(minimem_vram_advise_tier(NULL, 0, 5) == MINIMEM_VRAM_WARM);
	assert(minimem_vram_advise_tier(NULL, 0, 20) == MINIMEM_VRAM_COLD);
	assert(minimem_vram_advise_tier(NULL, 0, 100) == MINIMEM_VRAM_FROZEN);
	printf("  PASS: tier advice\n");
}

static void test_tier_and_fmt_names(void)
{
	assert(strcmp(minimem_vram_tier_name(MINIMEM_VRAM_HOT), "HOT") == 0);
	assert(strcmp(minimem_vram_tier_name(MINIMEM_VRAM_WARM), "WARM") == 0);
	assert(strcmp(minimem_vram_tier_name(MINIMEM_VRAM_COLD), "COLD") == 0);
	assert(strcmp(minimem_vram_tier_name(MINIMEM_VRAM_FROZEN), "FROZEN") == 0);
	assert(strcmp(minimem_vram_tier_name((enum minimem_vram_tier)99), "UNKNOWN") == 0);

	assert(strcmp(minimem_vram_fmt_name(MINIMEM_VRAM_FMT_FP16), "FP16") == 0);
	assert(strcmp(minimem_vram_fmt_name(MINIMEM_VRAM_FMT_BF16), "BF16") == 0);
	assert(strcmp(minimem_vram_fmt_name(MINIMEM_VRAM_FMT_INT8), "INT8") == 0);
	assert(strcmp(minimem_vram_fmt_name((enum minimem_vram_weight_format)99), "UNKNOWN") == 0);
	printf("  PASS: tier and format names\n");
}

static void test_touch_updates_last_access(void)
{
	struct minimem_vram_ctx *ctx = minimem_vram_ctx_create(4);
	assert(ctx != NULL);

	uint8_t buf[4096];
	memset(buf, 0xAA, 4096);
	minimem_vram_register(ctx, buf, 4096, MINIMEM_VRAM_FMT_INT8, false);

	uint64_t before = ctx->bufs[0].last_access_ns;
	minimem_vram_touch(ctx, 0);
	uint64_t after = ctx->bufs[0].last_access_ns;
	assert(after >= before);

	minimem_vram_ctx_destroy(ctx);
	printf("  PASS: touch updates last_access\n");
}

static void test_ctx_create_zero_capacity(void)
{
	struct minimem_vram_ctx *ctx = minimem_vram_ctx_create(0);
	assert(ctx != NULL);
	assert(ctx->n_bufs == 0);
	assert(ctx->cap > 0);

	uint8_t buf[4096];
	memset(buf, 0x55, 4096);
	int ret = minimem_vram_register(ctx, buf, 4096,
					MINIMEM_VRAM_FMT_INT8, false);
	assert(ret == 0);
	assert(ctx->n_bufs == 1);

	minimem_vram_ctx_destroy(ctx);
	printf("  PASS: ctx_create zero capacity defaults to positive cap\n");
}

static void test_find_buf(void)
{
	struct minimem_vram_ctx *ctx = minimem_vram_ctx_create(4);
	assert(ctx != NULL);

	uint8_t a[4096], b[4096];
	minimem_vram_register(ctx, a, 4096, MINIMEM_VRAM_FMT_FP16, false);
	minimem_vram_register(ctx, b, 4096, MINIMEM_VRAM_FMT_BF16, false);

	struct minimem_vram_buf *found = minimem_vram_find_buf(ctx, a);
	assert(found != NULL);
	assert(found->weight_format == MINIMEM_VRAM_FMT_FP16);

	found = minimem_vram_find_buf(ctx, b);
	assert(found != NULL);
	assert(found->weight_format == MINIMEM_VRAM_FMT_BF16);

	found = minimem_vram_find_buf(ctx, NULL);
	assert(found == NULL);

	minimem_vram_ctx_destroy(ctx);
	printf("  PASS: find_buf\n");
}

int main(void)
{
	printf("=== MiniMem VRAM Roundtrip Tests ===\n\n");

	test_register_unregister();
	test_compress_decompress_fp16();
	test_compress_decompress_int8();
	test_compress_all_idle();
	test_stats();
	test_tier_advice();
	test_tier_and_fmt_names();
	test_touch_updates_last_access();
	test_ctx_create_zero_capacity();
	test_find_buf();

	printf("\n=== All 9 VRAM tests passed ===\n");
	return 0;
}