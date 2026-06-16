#ifndef MINIMEM_AI_WEIGHTS_H
#define MINIMEM_AI_WEIGHTS_H

#include "lib/minimem.h"

#define MINIMEM_AI_BLOCK_SIZE 64
#define MINIMEM_AI_BLOCKS_PER_PAGE (MINIMEM_PAGE_SIZE / MINIMEM_AI_BLOCK_SIZE)

#define MINIMEM_AI_FMT_FP16 0
#define MINIMEM_AI_FMT_BF16 1
#define MINIMEM_AI_FMT_INT8 2
#define MINIMEM_AI_FMT_INT4 3

size_t minimem_ai_weights_compress(const uint8_t *src, size_t src_len,
                                   uint8_t *dst, size_t dst_cap,
                                   int weight_format);

size_t minimem_ai_weights_decompress(const uint8_t *src, size_t src_len,
                                     uint8_t *dst, size_t dst_cap,
                                     int weight_format);

size_t minimem_ai_weights_compress_bound(size_t src_len);

bool minimem_ai_weights_can_compress(const uint8_t *src, size_t src_len);

extern const struct minimem_compressor minimem_ai_fp16_compressor;
extern const struct minimem_compressor minimem_ai_bf16_compressor;
extern const struct minimem_compressor minimem_ai_int8_compressor;

#endif