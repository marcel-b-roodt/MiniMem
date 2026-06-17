#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 199309L
#endif

#include "minimem_vram.h"
#include "lib/compressors/ai_weights.h"
#include "lib/minimem.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int weight_fmt_to_algo(enum minimem_vram_weight_format fmt)
{
	switch (fmt) {
	case MINIMEM_VRAM_FMT_FP16:
		return MINIMEM_ALGO_AI_FP16;
	case MINIMEM_VRAM_FMT_BF16:
		return MINIMEM_ALGO_AI_BF16;
	case MINIMEM_VRAM_FMT_INT8:
		return MINIMEM_ALGO_AI_INT8;
	default:
		return MINIMEM_ALGO_LZ4;
	}
}

static int weight_fmt_to_ai_fmt(enum minimem_vram_weight_format fmt)
{
	switch (fmt) {
	case MINIMEM_VRAM_FMT_FP16:
		return MINIMEM_AI_FMT_FP16;
	case MINIMEM_VRAM_FMT_BF16:
		return MINIMEM_AI_FMT_BF16;
	case MINIMEM_VRAM_FMT_INT8:
		return MINIMEM_AI_FMT_INT8;
	default:
		return MINIMEM_AI_FMT_FP16;
	}
}

struct minimem_vram_ctx *minimem_vram_ctx_create(size_t initial_cap)
{
	struct minimem_vram_ctx *ctx = calloc(1, sizeof(*ctx));
	if (!ctx)
		return NULL;

	ctx->cap = initial_cap > 0 ? initial_cap : 64;
	ctx->bufs = calloc(ctx->cap, sizeof(*ctx->bufs));
	if (!ctx->bufs) {
		free(ctx);
		return NULL;
	}

	ctx->n_bufs = 0;
	memset(&ctx->stats, 0, sizeof(ctx->stats));
	return ctx;
}

void minimem_vram_ctx_destroy(struct minimem_vram_ctx *ctx)
{
	if (!ctx)
		return;

	for (size_t i = 0; i < ctx->n_bufs; i++) {
		if (ctx->bufs[i].compressed_ptr)
			free(ctx->bufs[i].compressed_ptr);
	}

	free(ctx->bufs);
	free(ctx);
}

int minimem_vram_register(struct minimem_vram_ctx *ctx, void *ptr, size_t size,
			   enum minimem_vram_weight_format fmt, bool is_device)
{
	if (!ctx || !ptr)
		return -1;

	if (ctx->n_bufs >= ctx->cap) {
		size_t new_cap = ctx->cap * 2;
		struct minimem_vram_buf *new_bufs = realloc(ctx->bufs,
							    new_cap * sizeof(*new_bufs));
		if (!new_bufs)
			return -1;
		memset(new_bufs + ctx->cap, 0,
		       (new_cap - ctx->cap) * sizeof(*new_bufs));
		ctx->bufs = new_bufs;
		ctx->cap = new_cap;
	}

	struct minimem_vram_buf *buf = &ctx->bufs[ctx->n_bufs];
	buf->ptr = ptr;
	buf->compressed_ptr = NULL;
	buf->size = size;
	buf->compressed_size = 0;
	buf->algo_id = weight_fmt_to_algo(fmt);
	buf->weight_format = fmt;
	buf->tier = MINIMEM_VRAM_HOT;
	buf->is_device = is_device;
	buf->last_access_ns = now_ns();
	buf->compress_ns = 0;
	buf->decompress_ns = 0;

	ctx->n_bufs++;
	ctx->stats.n_bufs++;
	ctx->stats.total_vram_bytes += size;
	return 0;
}

int minimem_vram_unregister(struct minimem_vram_ctx *ctx, void *ptr)
{
	if (!ctx || !ptr)
		return -1;

	for (size_t i = 0; i < ctx->n_bufs; i++) {
		if (ctx->bufs[i].ptr == ptr) {
			if (ctx->bufs[i].compressed_ptr)
				free(ctx->bufs[i].compressed_ptr);

			ctx->stats.total_vram_bytes -= ctx->bufs[i].size;
			ctx->stats.n_bufs--;

			if (i < ctx->n_bufs - 1)
				ctx->bufs[i] = ctx->bufs[ctx->n_bufs - 1];

			memset(&ctx->bufs[ctx->n_bufs - 1], 0,
			       sizeof(ctx->bufs[ctx->n_bufs - 1]));
			ctx->n_bufs--;
			return 0;
		}
	}

	return -1;
}

int minimem_vram_compress_buf(struct minimem_vram_ctx *ctx, size_t buf_idx,
			       enum minimem_vram_tier target_tier)
{
	if (!ctx || buf_idx >= ctx->n_bufs)
		return -1;

	struct minimem_vram_buf *buf = &ctx->bufs[buf_idx];

	if (buf->tier != MINIMEM_VRAM_HOT)
		return -1;

	if (buf->is_device && target_tier != MINIMEM_VRAM_WARM)
		return -1;

	size_t bound = minimem_ai_weights_compress_bound(buf->size);
	uint8_t *dst = malloc(bound);
	if (!dst)
		return -1;

	int ai_fmt = weight_fmt_to_ai_fmt(buf->weight_format);
	uint64_t t0 = now_ns();
	size_t compressed = minimem_ai_weights_compress(
		(const uint8_t *)buf->ptr, buf->size, dst, bound, ai_fmt);
	uint64_t t1 = now_ns();

	if (compressed == 0 || compressed >= buf->size) {
		free(dst);
		return -1;
	}

	buf->compressed_ptr = dst;
	buf->compressed_size = compressed;
	buf->tier = target_tier;
	buf->compress_ns = t1 - t0;

	ctx->stats.total_compressed_bytes += compressed;
	ctx->stats.total_saved_bytes += buf->size - compressed;
	ctx->stats.compress_count++;
	ctx->stats.compress_ns_total += buf->compress_ns;

	return 0;
}

int minimem_vram_decompress_buf(struct minimem_vram_ctx *ctx, size_t buf_idx)
{
	if (!ctx || buf_idx >= ctx->n_bufs)
		return -1;

	struct minimem_vram_buf *buf = &ctx->bufs[buf_idx];

	if (buf->tier == MINIMEM_VRAM_HOT)
		return -1;

	if (!buf->compressed_ptr || buf->compressed_size == 0)
		return -1;

	int ai_fmt = weight_fmt_to_ai_fmt(buf->weight_format);
	uint64_t t0 = now_ns();
	size_t decompressed = minimem_ai_weights_decompress(
		(const uint8_t *)buf->compressed_ptr, buf->compressed_size,
		(uint8_t *)buf->ptr, buf->size, ai_fmt);
	uint64_t t1 = now_ns();

	if (decompressed != buf->size)
		return -1;

	free(buf->compressed_ptr);
	buf->compressed_ptr = NULL;
	buf->compressed_size = 0;
	buf->tier = MINIMEM_VRAM_HOT;
	buf->decompress_ns = t1 - t0;
	buf->last_access_ns = now_ns();

	ctx->stats.decompress_count++;
	ctx->stats.decompress_ns_total += buf->decompress_ns;

	return 0;
}

int minimem_vram_compress_all_idle(struct minimem_vram_ctx *ctx,
				    uint64_t idle_threshold_ns,
				    enum minimem_vram_tier target_tier)
{
	if (!ctx)
		return -1;

	uint64_t cutoff = now_ns() - idle_threshold_ns;
	int compressed = 0;

	for (size_t i = 0; i < ctx->n_bufs; i++) {
		if (ctx->bufs[i].tier == MINIMEM_VRAM_HOT &&
		    ctx->bufs[i].last_access_ns < cutoff) {
			if (minimem_vram_compress_buf(ctx, i, target_tier) == 0)
				compressed++;
		}
	}

	return compressed;
}

size_t minimem_vram_buf_index(struct minimem_vram_ctx *ctx, void *ptr)
{
	if (!ctx || !ptr)
		return (size_t)-1;

	for (size_t i = 0; i < ctx->n_bufs; i++) {
		if (ctx->bufs[i].ptr == ptr)
			return i;
	}

	return (size_t)-1;
}

struct minimem_vram_buf *minimem_vram_find_buf(struct minimem_vram_ctx *ctx,
						 void *ptr)
{
	size_t idx = minimem_vram_buf_index(ctx, ptr);
	if (idx == (size_t)-1)
		return NULL;
	return &ctx->bufs[idx];
}

void minimem_vram_touch(struct minimem_vram_ctx *ctx, size_t buf_idx)
{
	if (!ctx || buf_idx >= ctx->n_bufs)
		return;
	ctx->bufs[buf_idx].last_access_ns = now_ns();
}

enum minimem_vram_tier minimem_vram_advise_tier(
	const struct minimem_vram_ctx *ctx, size_t buf_idx,
	size_t idle_tokens)
{
	(void)ctx;
	(void)buf_idx;

	if (idle_tokens > 64)
		return MINIMEM_VRAM_FROZEN;
	if (idle_tokens > 16)
		return MINIMEM_VRAM_COLD;
	if (idle_tokens > 4)
		return MINIMEM_VRAM_WARM;
	return MINIMEM_VRAM_HOT;
}

const struct minimem_vram_stats *minimem_vram_get_stats(
	const struct minimem_vram_ctx *ctx)
{
	return ctx ? &ctx->stats : NULL;
}

const char *minimem_vram_tier_name(enum minimem_vram_tier tier)
{
	switch (tier) {
	case MINIMEM_VRAM_HOT:
		return "HOT";
	case MINIMEM_VRAM_WARM:
		return "WARM";
	case MINIMEM_VRAM_COLD:
		return "COLD";
	case MINIMEM_VRAM_FROZEN:
		return "FROZEN";
	default:
		return "UNKNOWN";
	}
}

const char *minimem_vram_fmt_name(enum minimem_vram_weight_format fmt)
{
	switch (fmt) {
	case MINIMEM_VRAM_FMT_FP32:
		return "FP32";
	case MINIMEM_VRAM_FMT_FP16:
		return "FP16";
	case MINIMEM_VRAM_FMT_BF16:
		return "BF16";
	case MINIMEM_VRAM_FMT_INT8:
		return "INT8";
	case MINIMEM_VRAM_FMT_INT4:
		return "INT4";
	default:
		return "UNKNOWN";
	}
}