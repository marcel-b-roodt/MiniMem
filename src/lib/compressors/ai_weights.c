#include "lib/compressors/ai_weights.h"
#include <string.h>

static size_t bits_needed(uint8_t range)
{
	if (range == 0) return 0;
	if (range <= 1) return 1;
	if (range <= 3) return 2;
	if (range <= 7) return 3;
	if (range <= 15) return 4;
	if (range <= 31) return 5;
	if (range <= 63) return 6;
	return 8;
}

/*
 * BYTE_STREAM_SPLIT for 16-bit values.
 * Separates high bytes and low bytes into two contiguous streams,
 * then block-classifies each stream independently.
 *
 * Format:
 *   2 bytes: n_blocks (uint16_t)
 *   2 bytes: header_bytes (uint16_t)
 *   header_bytes: 3-bit block types (low-byte stream)
 *   header_bytes: 3-bit block types (high-byte stream)
 *   For each block (low bytes first, then high bytes):
 *     - ZERO: nothing
 *     - UNIFORM: 1 byte value
 *     - SPARSE: 8-byte bitmap + non-zero values
 *     - SMALL_RANGE: 2 bytes (min, range) + bit-packed deltas
 *     - DENSE: 32 raw bytes
 */

#define BT_ZERO       0
#define BT_SPARSE     1
#define BT_UNIFORM    2
#define BT_SMALL_RANGE 3
#define BT_DENSE      4
#define BT_COUNT      5
#define BITS_PER_BT   3

static void pack_bt(const uint8_t *types, size_t n, uint8_t *out)
{
	memset(out, 0, (n * BITS_PER_BT + 7) / 8);
	for (size_t i = 0; i < n; i++) {
		size_t bit_pos = i * BITS_PER_BT;
		out[bit_pos / 8] |= ((types[i] & 0x07) << (bit_pos % 8));
		if (bit_pos % 8 + BITS_PER_BT > 8)
			out[bit_pos / 8 + 1] |=
				((types[i] & 0x07) >> (8 - bit_pos % 8));
	}
}

static uint8_t unpack_bt(const uint8_t *packed, size_t index)
{
	size_t bit_pos = index * BITS_PER_BT;
	if (bit_pos % 8 + BITS_PER_BT <= 8)
		return (packed[bit_pos / 8] >> (bit_pos % 8)) & 0x07;
	return ((packed[bit_pos / 8] >> (bit_pos % 8)) |
		(packed[bit_pos / 8 + 1] << (8 - bit_pos % 8))) & 0x07;
}

static uint8_t classify_byte_block(const uint8_t *block, size_t len,
				   uint8_t *uniform_out,
				   uint8_t *range_min_out,
				   uint8_t *range_out)
{
	bool all_zero = true;
	bool all_same = true;
	uint8_t first = block[0];
	uint8_t min_v = 0xFF, max_v = 0x00;
	size_t nz = 0;

	for (size_t i = 0; i < len; i++) {
		uint8_t v = block[i];
		if (v != 0) { all_zero = false; nz++; }
		if (v != first) all_same = false;
		if (v < min_v) min_v = v;
		if (v > max_v) max_v = v;
	}

	if (all_zero) return BT_ZERO;
	if (all_same) { if (uniform_out) *uniform_out = first; return BT_UNIFORM; }
	if (nz <= 16 && nz < len) return BT_SPARSE;

	uint8_t range = max_v - min_v;
	size_t bpv = bits_needed(range);
	if (bpv > 0 && bpv <= 6) {
		if (range_min_out) *range_min_out = min_v;
		if (range_out) *range_out = range;
		return BT_SMALL_RANGE;
	}

	return BT_DENSE;
}

static size_t encode_byte_block(const uint8_t *block, size_t len, uint8_t type,
				uint8_t uniform_val, uint8_t range_min,
				uint8_t range, uint8_t *dst)
{
	uint8_t *out = dst;

	switch (type) {
	case BT_ZERO:
		return 0;
	case BT_UNIFORM:
		*out = uniform_val;
		return 1;
	case BT_SPARSE: {
		uint8_t bitmap[8] = {0};
		for (size_t i = 0; i < len; i++) {
			if (block[i] != 0)
				bitmap[i / 8] |= (1u << (i % 8));
		}
		memcpy(out, bitmap, (len + 7) / 8); out += (len + 7) / 8;
		for (size_t i = 0; i < len; i++) {
			if (block[i] != 0)
				*out++ = block[i];
		}
		return (size_t)(out - dst);
	}
	case BT_SMALL_RANGE: {
		*out++ = range_min;
		*out++ = range;
		size_t bpv = bits_needed(range);
		if (bpv == 0) return 2;
		size_t total_bits = len * bpv;
		size_t packed_bytes = (total_bits + 7) / 8;
		uint8_t packed[64] = {0};
		size_t bit_pos = 0;
		for (size_t i = 0; i < len; i++) {
			uint8_t delta = block[i] - range_min;
			for (size_t b = 0; b < bpv; b++) {
				if (delta & (1u << b)) {
					size_t abs_bit = bit_pos + b;
					packed[abs_bit / 8] |=
						(1u << (abs_bit % 8));
				}
			}
			bit_pos += bpv;
		}
		memcpy(out, packed, packed_bytes); out += packed_bytes;
		return (size_t)(out - dst);
	}
	case BT_DENSE:
		memcpy(out, block, len);
		return len;
	default:
		return 0;
	}
}

static size_t compress_split16(const uint8_t *src, size_t src_len,
			      uint8_t *dst, size_t dst_cap)
{
	if (src_len == 0 || src_len % MINIMEM_AI_BLOCK_SIZE != 0)
		return 0;

	size_t n_blocks = src_len / MINIMEM_AI_BLOCK_SIZE;
	size_t half_block = MINIMEM_AI_BLOCK_SIZE / 2;

	uint8_t lo_types[MINIMEM_AI_BLOCKS_PER_PAGE] = {0};
	uint8_t hi_types[MINIMEM_AI_BLOCKS_PER_PAGE] = {0};
	uint8_t lo_uniform[MINIMEM_AI_BLOCKS_PER_PAGE];
	uint8_t hi_uniform[MINIMEM_AI_BLOCKS_PER_PAGE];
	uint8_t lo_rmin[MINIMEM_AI_BLOCKS_PER_PAGE];
	uint8_t hi_rmin[MINIMEM_AI_BLOCKS_PER_PAGE];
	uint8_t lo_range[MINIMEM_AI_BLOCKS_PER_PAGE];
	uint8_t hi_range[MINIMEM_AI_BLOCKS_PER_PAGE];

	uint8_t lo_buf[MINIMEM_AI_BLOCK_SIZE / 2];
	uint8_t hi_buf[MINIMEM_AI_BLOCK_SIZE / 2];

	for (size_t b = 0; b < n_blocks; b++) {
		const uint8_t *block = src + b * MINIMEM_AI_BLOCK_SIZE;

		for (size_t i = 0; i < half_block; i++) {
			lo_buf[i] = block[i * 2];
			hi_buf[i] = block[i * 2 + 1];
		}

		lo_types[b] = classify_byte_block(lo_buf, half_block,
						  &lo_uniform[b],
						  &lo_rmin[b],
						  &lo_range[b]);
		hi_types[b] = classify_byte_block(hi_buf, half_block,
						  &hi_uniform[b],
						  &hi_rmin[b],
						  &hi_range[b]);
	}

	size_t header_bytes = (n_blocks * BITS_PER_BT * 2 + 7) / 8;

	uint8_t *out = dst;
	uint16_t nb = (uint16_t)n_blocks;
	memcpy(out, &nb, 2); out += 2;
	uint16_t hb = (uint16_t)header_bytes;
	memcpy(out, &hb, 2); out += 2;

	pack_bt(lo_types, n_blocks, out); out += header_bytes;
	pack_bt(hi_types, n_blocks, out); out += header_bytes;

	for (size_t b = 0; b < n_blocks; b++) {
		const uint8_t *block = src + b * MINIMEM_AI_BLOCK_SIZE;
		for (size_t i = 0; i < half_block; i++) {
			lo_buf[i] = block[i * 2];
			hi_buf[i] = block[i * 2 + 1];
		}

		size_t written = encode_byte_block(lo_buf, half_block,
						  lo_types[b],
						  lo_uniform[b],
						  lo_rmin[b],
						  lo_range[b], out);
		out += written;

		written = encode_byte_block(hi_buf, half_block,
					   hi_types[b],
					   hi_uniform[b],
					   hi_rmin[b],
					   hi_range[b], out);
		out += written;
	}

	size_t result = (size_t)(out - dst);
	if (result >= src_len || result > dst_cap)
		return 0;

	return result;
}

/*
 * INT8 weight compressor with row-delta prediction.
 * XOR each row with the previous row before block classification.
 * Row width = 64 bytes (one cache line / one block).
 *
 * Format:
 *   2 bytes: n_blocks
 *   2 bytes: header_bytes
 *   header_bytes: 3-bit block types
 *   first row: raw (no delta base)
 *   subsequent rows: block-classified delta data
 */
static size_t compress_int8(const uint8_t *src, size_t src_len,
			   uint8_t *dst, size_t dst_cap)
{
	if (src_len == 0 || src_len % MINIMEM_AI_BLOCK_SIZE != 0)
		return 0;

	size_t n_blocks = src_len / MINIMEM_AI_BLOCK_SIZE;

	uint8_t types[MINIMEM_AI_BLOCKS_PER_PAGE] = {0};
	uint8_t uniform_vals[MINIMEM_AI_BLOCKS_PER_PAGE] = {0};
	uint8_t range_mins[MINIMEM_AI_BLOCKS_PER_PAGE] = {0};
	uint8_t range_deltas[MINIMEM_AI_BLOCKS_PER_PAGE] = {0};

	uint8_t delta_buf[MINIMEM_AI_BLOCK_SIZE];

	for (size_t b = 0; b < n_blocks; b++) {
		const uint8_t *block = src + b * MINIMEM_AI_BLOCK_SIZE;

		if (b > 0) {
			const uint8_t *prev = src + (b - 1) * MINIMEM_AI_BLOCK_SIZE;
			for (size_t i = 0; i < MINIMEM_AI_BLOCK_SIZE; i++)
				delta_buf[i] = block[i] ^ prev[i];
			types[b] = classify_byte_block(delta_buf,
						       MINIMEM_AI_BLOCK_SIZE,
						       &uniform_vals[b],
						       &range_mins[b],
						       &range_deltas[b]);
		} else {
			types[b] = classify_byte_block(block,
						       MINIMEM_AI_BLOCK_SIZE,
						       &uniform_vals[b],
						       &range_mins[b],
						       &range_deltas[b]);
		}
	}

	size_t header_bytes = (n_blocks * BITS_PER_BT + 7) / 8;

	uint8_t *out = dst;
	uint16_t nb = (uint16_t)n_blocks;
	memcpy(out, &nb, 2); out += 2;
	uint16_t hb = (uint16_t)header_bytes;
	memcpy(out, &hb, 2); out += 2;
	pack_bt(types, n_blocks, out); out += header_bytes;

	for (size_t b = 0; b < n_blocks; b++) {
		const uint8_t *block = src + b * MINIMEM_AI_BLOCK_SIZE;

		if (b > 0) {
			const uint8_t *prev = src + (b - 1) * MINIMEM_AI_BLOCK_SIZE;
			for (size_t i = 0; i < MINIMEM_AI_BLOCK_SIZE; i++)
				delta_buf[i] = block[i] ^ prev[i];
			size_t w = encode_byte_block(delta_buf,
						     MINIMEM_AI_BLOCK_SIZE,
						     types[b],
						     uniform_vals[b],
						     range_mins[b],
						     range_deltas[b], out);
			out += w;
		} else {
			size_t w = encode_byte_block(block,
						     MINIMEM_AI_BLOCK_SIZE,
						     types[b],
						     uniform_vals[b],
						     range_mins[b],
						     range_deltas[b], out);
			out += w;
		}
	}

	size_t result = (size_t)(out - dst);
	if (result >= src_len || result > dst_cap)
		return 0;

	return result;
}

/*
 * Decompress for split-16 format.
 */
static size_t decompress_split16(const uint8_t *src, size_t src_len,
				uint8_t *dst, size_t dst_cap)
{
	if (src_len < 4)
		return 0;

	const uint8_t *in = src;
	uint16_t n_blocks, header_bytes;
	memcpy(&n_blocks, in, 2); in += 2;
	memcpy(&header_bytes, in, 2); in += 2;

	if (n_blocks == 0 || n_blocks > MINIMEM_AI_BLOCKS_PER_PAGE)
		return 0;
	if ((size_t)(4 + header_bytes * 2) > src_len)
		return 0;

	size_t expected = (size_t)n_blocks * MINIMEM_AI_BLOCK_SIZE;
	if (expected > dst_cap)
		return 0;

	const uint8_t *lo_types_data = in; in += header_bytes;
	const uint8_t *hi_types_data = in; in += header_bytes;

	size_t half_block = MINIMEM_AI_BLOCK_SIZE / 2;
	uint8_t *out = dst;

	for (size_t b = 0; b < n_blocks; b++) {
		uint8_t lo_type = unpack_bt(lo_types_data, b);
		uint8_t hi_type = unpack_bt(hi_types_data, b);

		uint8_t lo_buf[32] = {0};
		uint8_t hi_buf[32] = {0};

		/* Decode low bytes */
		switch (lo_type) {
		case BT_ZERO:
			memset(lo_buf, 0, half_block);
			break;
		case BT_UNIFORM:
			if (in >= src + src_len) return 0;
			memset(lo_buf, *in++, half_block);
			break;
		case BT_SPARSE: {
			size_t bm_bytes = (half_block + 7) / 8;
			if (in + bm_bytes > src + src_len) return 0;
			const uint8_t *bm = in; in += bm_bytes;
			memset(lo_buf, 0, half_block);
			for (size_t i = 0; i < half_block; i++) {
				if (bm[i / 8] & (1u << (i % 8))) {
					if (in >= src + src_len) return 0;
					lo_buf[i] = *in++;
				}
			}
			break;
		}
		case BT_SMALL_RANGE: {
			if (in + 2 > src + src_len) return 0;
			uint8_t rmin = *in++;
			uint8_t range = *in++;
			size_t bpv = bits_needed(range);
			if (bpv == 0) {
				memset(lo_buf, rmin, half_block);
				break;
			}
			size_t total_bits = half_block * bpv;
			size_t packed_bytes = (total_bits + 7) / 8;
			if (in + packed_bytes > src + src_len) return 0;
			const uint8_t *packed = in; in += packed_bytes;
			size_t bit_pos = 0;
			for (size_t i = 0; i < half_block; i++) {
				uint8_t delta = 0;
				for (size_t bit = 0; bit < bpv; bit++) {
					size_t abs_bit = bit_pos + bit;
					if (packed[abs_bit / 8] &
					    (1u << (abs_bit % 8)))
						delta |= (1u << bit);
				}
				lo_buf[i] = rmin + delta;
				bit_pos += bpv;
			}
			break;
		}
		case BT_DENSE:
			if (in + half_block > src + src_len) return 0;
			memcpy(lo_buf, in, half_block);
			in += half_block;
			break;
		default:
			return 0;
		}

		/* Decode high bytes (same logic) */
		switch (hi_type) {
		case BT_ZERO:
			memset(hi_buf, 0, half_block);
			break;
		case BT_UNIFORM:
			if (in >= src + src_len) return 0;
			memset(hi_buf, *in++, half_block);
			break;
		case BT_SPARSE: {
			size_t bm_bytes = (half_block + 7) / 8;
			if (in + bm_bytes > src + src_len) return 0;
			const uint8_t *bm = in; in += bm_bytes;
			memset(hi_buf, 0, half_block);
			for (size_t i = 0; i < half_block; i++) {
				if (bm[i / 8] & (1u << (i % 8))) {
					if (in >= src + src_len) return 0;
					hi_buf[i] = *in++;
				}
			}
			break;
		}
		case BT_SMALL_RANGE: {
			if (in + 2 > src + src_len) return 0;
			uint8_t rmin = *in++;
			uint8_t range = *in++;
			size_t bpv = bits_needed(range);
			if (bpv == 0) {
				memset(hi_buf, rmin, half_block);
				break;
			}
			size_t total_bits = half_block * bpv;
			size_t packed_bytes = (total_bits + 7) / 8;
			if (in + packed_bytes > src + src_len) return 0;
			const uint8_t *packed = in; in += packed_bytes;
			size_t bit_pos = 0;
			for (size_t i = 0; i < half_block; i++) {
				uint8_t delta = 0;
				for (size_t bit = 0; bit < bpv; bit++) {
					size_t abs_bit = bit_pos + bit;
					if (packed[abs_bit / 8] &
					    (1u << (abs_bit % 8)))
						delta |= (1u << bit);
				}
				hi_buf[i] = rmin + delta;
				bit_pos += bpv;
			}
			break;
		}
		case BT_DENSE:
			if (in + half_block > src + src_len) return 0;
			memcpy(hi_buf, in, half_block);
			in += half_block;
			break;
		default:
			return 0;
		}

		/* Interleave low and high bytes */
		for (size_t i = 0; i < half_block; i++) {
			out[i * 2] = lo_buf[i];
			out[i * 2 + 1] = hi_buf[i];
		}
		out += MINIMEM_AI_BLOCK_SIZE;
	}

	return expected;
}

static size_t decompress_int8(const uint8_t *src, size_t src_len,
			     uint8_t *dst, size_t dst_cap)
{
	if (src_len < 4)
		return 0;

	const uint8_t *in = src;
	uint16_t n_blocks, header_bytes;
	memcpy(&n_blocks, in, 2); in += 2;
	memcpy(&header_bytes, in, 2); in += 2;

	if (n_blocks == 0 || n_blocks > MINIMEM_AI_BLOCKS_PER_PAGE)
		return 0;
	if ((size_t)(4 + header_bytes) > src_len)
		return 0;

	size_t expected = (size_t)n_blocks * MINIMEM_AI_BLOCK_SIZE;
	if (expected > dst_cap)
		return 0;

	const uint8_t *types_data = in; in += header_bytes;

	uint8_t *out = dst;
	uint8_t prev_block[MINIMEM_AI_BLOCK_SIZE];
	memset(prev_block, 0, MINIMEM_AI_BLOCK_SIZE);

	for (size_t b = 0; b < n_blocks; b++) {
		uint8_t type = unpack_bt(types_data, b);
		uint8_t cur_block[MINIMEM_AI_BLOCK_SIZE];

		switch (type) {
		case BT_ZERO:
			memset(cur_block, 0, MINIMEM_AI_BLOCK_SIZE);
			break;
		case BT_UNIFORM:
			if (in >= src + src_len) return 0;
			memset(cur_block, *in++, MINIMEM_AI_BLOCK_SIZE);
			break;
		case BT_SPARSE: {
			size_t bm_bytes = (MINIMEM_AI_BLOCK_SIZE + 7) / 8;
			if (in + bm_bytes > src + src_len) return 0;
			const uint8_t *bm = in; in += bm_bytes;
			memset(cur_block, 0, MINIMEM_AI_BLOCK_SIZE);
			for (size_t i = 0; i < MINIMEM_AI_BLOCK_SIZE; i++) {
				if (bm[i / 8] & (1u << (i % 8))) {
					if (in >= src + src_len) return 0;
					cur_block[i] = *in++;
				}
			}
			break;
		}
		case BT_SMALL_RANGE: {
			if (in + 2 > src + src_len) return 0;
			uint8_t rmin = *in++;
			uint8_t range = *in++;
			size_t bpv = bits_needed(range);
			if (bpv == 0) {
				memset(cur_block, rmin, MINIMEM_AI_BLOCK_SIZE);
				break;
			}
			size_t total_bits = MINIMEM_AI_BLOCK_SIZE * bpv;
			size_t packed_bytes = (total_bits + 7) / 8;
			if (in + packed_bytes > src + src_len) return 0;
			const uint8_t *packed = in; in += packed_bytes;
			size_t bit_pos = 0;
			for (size_t i = 0; i < MINIMEM_AI_BLOCK_SIZE; i++) {
				uint8_t delta = 0;
				for (size_t bit = 0; bit < bpv; bit++) {
					size_t abs_bit = bit_pos + bit;
					if (packed[abs_bit / 8] &
					    (1u << (abs_bit % 8)))
						delta |= (1u << bit);
				}
				cur_block[i] = rmin + delta;
				bit_pos += bpv;
			}
			break;
		}
		case BT_DENSE:
			if (in + MINIMEM_AI_BLOCK_SIZE > src + src_len) return 0;
			memcpy(cur_block, in, MINIMEM_AI_BLOCK_SIZE);
			in += MINIMEM_AI_BLOCK_SIZE;
			break;
		default:
			return 0;
		}

		if (b > 0) {
			for (size_t i = 0; i < MINIMEM_AI_BLOCK_SIZE; i++)
				out[i] = cur_block[i] ^ prev_block[i];
		} else {
			memcpy(out, cur_block, MINIMEM_AI_BLOCK_SIZE);
		}

		memcpy(prev_block, (b > 0) ? out : cur_block,
		       MINIMEM_AI_BLOCK_SIZE);
		out += MINIMEM_AI_BLOCK_SIZE;
	}

	return expected;
}

size_t minimem_ai_weights_compress(const uint8_t *src, size_t src_len,
				   uint8_t *dst, size_t dst_cap,
				   int weight_format)
{
	if (src_len == 0 || src_len % MINIMEM_AI_BLOCK_SIZE != 0)
		return 0;

	switch (weight_format) {
	case MINIMEM_AI_FMT_FP16:
	case MINIMEM_AI_FMT_BF16:
		return compress_split16(src, src_len, dst, dst_cap);
	case MINIMEM_AI_FMT_INT8:
		return compress_int8(src, src_len, dst, dst_cap);
	default:
		return 0;
	}
}

size_t minimem_ai_weights_decompress(const uint8_t *src, size_t src_len,
				     uint8_t *dst, size_t dst_cap,
				     int weight_format)
{
	switch (weight_format) {
	case MINIMEM_AI_FMT_FP16:
	case MINIMEM_AI_FMT_BF16:
		return decompress_split16(src, src_len, dst, dst_cap);
	case MINIMEM_AI_FMT_INT8:
		return decompress_int8(src, src_len, dst, dst_cap);
	default:
		return 0;
	}
}

size_t minimem_ai_weights_compress_bound(size_t src_len)
{
	return src_len + 256;
}

bool minimem_ai_weights_can_compress(const uint8_t *src, size_t src_len)
{
	if (src_len == 0 || src_len % MINIMEM_AI_BLOCK_SIZE != 0)
		return false;
	(void)src;
	return true;
}

static size_t ai_fp16_compress(const uint8_t *src, size_t src_len,
			       uint8_t *dst, size_t dst_cap)
{
	return compress_split16(src, src_len, dst, dst_cap);
}

static size_t ai_fp16_decompress(const uint8_t *src, size_t src_len,
				  uint8_t *dst, size_t dst_cap)
{
	return decompress_split16(src, src_len, dst, dst_cap);
}

static size_t ai_bf16_compress(const uint8_t *src, size_t src_len,
			       uint8_t *dst, size_t dst_cap)
{
	return compress_split16(src, src_len, dst, dst_cap);
}

static size_t ai_bf16_decompress(const uint8_t *src, size_t src_len,
				  uint8_t *dst, size_t dst_cap)
{
	return decompress_split16(src, src_len, dst, dst_cap);
}

static size_t ai_int8_compress(const uint8_t *src, size_t src_len,
			       uint8_t *dst, size_t dst_cap)
{
	return compress_int8(src, src_len, dst, dst_cap);
}

static size_t ai_int8_decompress(const uint8_t *src, size_t src_len,
				  uint8_t *dst, size_t dst_cap)
{
	return decompress_int8(src, src_len, dst, dst_cap);
}

const struct minimem_compressor minimem_ai_fp16_compressor = {
	.name = "ai_fp16",
	.id = MINIMEM_ALGO_AI_FP16,
	.compress = ai_fp16_compress,
	.decompress = ai_fp16_decompress,
	.compress_bound = minimem_ai_weights_compress_bound,
	.can_compress = minimem_ai_weights_can_compress,
};

const struct minimem_compressor minimem_ai_bf16_compressor = {
	.name = "ai_bf16",
	.id = MINIMEM_ALGO_AI_BF16,
	.compress = ai_bf16_compress,
	.decompress = ai_bf16_decompress,
	.compress_bound = minimem_ai_weights_compress_bound,
	.can_compress = minimem_ai_weights_can_compress,
};

const struct minimem_compressor minimem_ai_int8_compressor = {
	.name = "ai_int8",
	.id = MINIMEM_ALGO_AI_INT8,
	.compress = ai_int8_compress,
	.decompress = ai_int8_decompress,
	.compress_bound = minimem_ai_weights_compress_bound,
	.can_compress = minimem_ai_weights_can_compress,
};