#ifndef MINIMEM_VRAM_H
#define MINIMEM_VRAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum minimem_vram_tier {
	MINIMEM_VRAM_HOT    = 0,
	MINIMEM_VRAM_WARM   = 1,
	MINIMEM_VRAM_COLD   = 2,
	MINIMEM_VRAM_FROZEN = 3,
};

enum minimem_vram_weight_format {
	MINIMEM_VRAM_FMT_FP32 = 0,
	MINIMEM_VRAM_FMT_FP16 = 1,
	MINIMEM_VRAM_FMT_BF16 = 2,
	MINIMEM_VRAM_FMT_INT8 = 3,
	MINIMEM_VRAM_FMT_INT4 = 4,
	MINIMEM_VRAM_FMT_UNKNOWN = 5,
};

struct minimem_vram_buf {
	void *ptr;
	void *compressed_ptr;
	size_t size;
	size_t compressed_size;
	int algo_id;
	enum minimem_vram_weight_format weight_format;
	enum minimem_vram_tier tier;
	bool is_device;
	uint64_t last_access_ns;
	uint64_t compress_ns;
	uint64_t decompress_ns;
};

struct minimem_vram_stats {
	size_t n_bufs;
	size_t total_vram_bytes;
	size_t total_compressed_bytes;
	size_t total_saved_bytes;
	size_t compress_count;
	size_t decompress_count;
	uint64_t compress_ns_total;
	uint64_t decompress_ns_total;
};

struct minimem_vram_ctx {
	struct minimem_vram_buf *bufs;
	size_t n_bufs;
	size_t cap;
	struct minimem_vram_stats stats;
};

struct minimem_vram_ctx *minimem_vram_ctx_create(size_t initial_cap);
void minimem_vram_ctx_destroy(struct minimem_vram_ctx *ctx);

int minimem_vram_register(struct minimem_vram_ctx *ctx, void *ptr, size_t size,
			   enum minimem_vram_weight_format fmt, bool is_device);
int minimem_vram_unregister(struct minimem_vram_ctx *ctx, void *ptr);

int minimem_vram_compress_buf(struct minimem_vram_ctx *ctx, size_t buf_idx,
			       enum minimem_vram_tier target_tier);
int minimem_vram_decompress_buf(struct minimem_vram_ctx *ctx, size_t buf_idx);

int minimem_vram_compress_all_idle(struct minimem_vram_ctx *ctx,
				    uint64_t idle_threshold_ns,
				    enum minimem_vram_tier target_tier);

size_t minimem_vram_buf_index(struct minimem_vram_ctx *ctx, void *ptr);
struct minimem_vram_buf *minimem_vram_find_buf(struct minimem_vram_ctx *ctx,
						 void *ptr);

void minimem_vram_touch(struct minimem_vram_ctx *ctx, size_t buf_idx);

enum minimem_vram_tier minimem_vram_advise_tier(
	const struct minimem_vram_ctx *ctx, size_t buf_idx,
	size_t idle_tokens);

const struct minimem_vram_stats *minimem_vram_get_stats(
	const struct minimem_vram_ctx *ctx);

const char *minimem_vram_tier_name(enum minimem_vram_tier tier);
const char *minimem_vram_fmt_name(enum minimem_vram_weight_format fmt);

#endif