/* SPDX-License-Identifier: GPL-2.0-only OR BSD-3-Clause */
/*
 * test_minimem_kernel.c — Userspace test driver for kernel module logic
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 199309L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>

#include "lib/minimem.h"
#include "lib/compressors/same_page.h"
#include "lib/compressors/bdi.h"
#include "lib/compressors/wkdm.h"
#include "lib/compressors/wkdm64.h"
#include "lib/compressors/lz4_wrap.h"
#include "lib/compressors/block_class.h"
#include "lib/advisor.h"
#include "lib/test_data.h"

#define PAGE_SIZE 4096
#define NUM_TESTS 1000

static void test_same_page_detection(void)
{
	uint8_t zero_page[PAGE_SIZE];
	uint8_t uniform_page[PAGE_SIZE];
	uint8_t mixed_page[PAGE_SIZE];

	memset(zero_page, 0, PAGE_SIZE);
	memset(uniform_page, 0x42, PAGE_SIZE);

	struct minimem_page_data *pd = minimem_generate_page(MINIMEM_PAGE_POINTER_HEAVY, 42);
	memcpy(mixed_page, pd->data, PAGE_SIZE);
	minimem_free_page(pd);

	printf("  same_page zero:       %s\n",
	       minimem_same_page_can_compress(zero_page, PAGE_SIZE) ? "PASS" : "FAIL");
	printf("  same_page uniform:    %s\n",
	       minimem_same_page_can_compress(uniform_page, PAGE_SIZE) ? "PASS" : "FAIL");
	printf("  same_page mixed:      %s\n",
	       !minimem_same_page_can_compress(mixed_page, PAGE_SIZE) ? "PASS" : "FAIL");
}

static void test_advisor_classification(void)
{
	struct minimem_page_data **pages = minimem_generate_all_pages(42);
	const char *names[] = {
		"random", "zero", "repeat_val", "pointer_heavy",
		"integer_heavy", "pte", "ai_fp16", "ai_int8",
		"ai_sparse", "delta_pair", "mixed"
	};

	printf("\n  Advisor classification:\n");
	for (int i = 0; i < MINIMEM_PAGE_TYPE_COUNT; i++) {
		struct minimem_page_stats stats = minimem_analyze_page(
			pages[i]->data, pages[i]->size);
		int algo = minimem_advise_algorithm(&stats);
		const char *algo_names[] = {
			"SAME_PAGE", "BDI", "WKDM", "LZ4",
			"LZSSE8", "ZSTD_DICT", "DELTA", "WKDM64", "BLOCK_CLASS"
		};
		printf("    %-15s → %s (zero_frac=%zu/256, upper32_zero=%zu)\n",
		       names[i],
		       algo >= 0 && algo < MINIMEM_ALGO_COUNT ?
			   algo_names[algo] : "UNKNOWN",
		       stats.zero_byte_frac_x256,
		       stats.upper32_zero_count);
	}

	minimem_free_all_pages(pages);
}

static void test_compress_decompress_all(void)
{
	struct minimem_page_data **pages = minimem_generate_all_pages(42);

	const struct minimem_compressor *compressors[] = {
		&minimem_same_page_compressor,
		&minimem_bdi_compressor,
		&minimem_wkdm_compressor,
		&minimem_wkdm64_compressor,
		&minimem_lz4_compressor,
		&minimem_block_class_compressor,
	};
	const char *algo_names[] = {
		"same_page", "bdi", "wkdm", "wkdm64", "lz4", "block_class"
	};
	const char *page_names[] = {
		"random", "zero", "repeat_val", "pointer_heavy",
		"integer_heavy", "pte", "ai_fp16", "ai_int8",
		"ai_sparse", "delta_pair", "mixed"
	};

	printf("\n  Compress/decompress roundtrip:\n");
	int pass = 0, fail = 0, skip = 0;

	for (int a = 0; a < 6; a++) {
		for (int p = 0; p < MINIMEM_PAGE_TYPE_COUNT; p++) {
			size_t bound = compressors[a]->compress_bound(pages[p]->size);
			uint8_t *compressed = malloc(bound);
			uint8_t *decompressed = malloc(pages[p]->size);

			size_t csz = compressors[a]->compress(
				pages[p]->data, pages[p]->size, compressed, bound);

			if (csz == 0) {
				skip++;
				free(compressed);
				free(decompressed);
				continue;
			}

			size_t dsz = compressors[a]->decompress(
				compressed, csz, decompressed, pages[p]->size);

			if (dsz != pages[p]->size ||
			    memcmp(pages[p]->data, decompressed, pages[p]->size) != 0) {
				printf("    FAIL: %s on %s (csz=%zu, dsz=%zu)\n",
				       algo_names[a], page_names[p], csz, dsz);
				fail++;
			} else {
				pass++;
			}

			free(compressed);
			free(decompressed);
		}
	}

	printf("    Results: %d pass, %d fail, %d skip (incompressible)\n",
	       pass, fail, skip);

	minimem_free_all_pages(pages);
}

static void test_map_basic(void)
{
	printf("\n  Compression map (userspace simulation):\n");

	/* Simulate the map operations using a simple hash table */
	enum { MAP_SIZE = 1024 };
	struct {
		unsigned long vaddr;
		int algo_id;
		size_t compressed_len;
		int occupied;
	} map[MAP_SIZE];

	memset(map, 0, sizeof(map));

	/* Store a few entries */
	unsigned long test_addrs[] = { 0x1000, 0x2000, 0x3000 };
	int test_algos[] = { MINIMEM_ALGO_SAME_PAGE, MINIMEM_ALGO_WKDM, MINIMEM_ALGO_LZ4 };

	for (int i = 0; i < 3; i++) {
		unsigned long idx = (test_addrs[i] >> 12) % MAP_SIZE;
		map[idx].vaddr = test_addrs[i];
		map[idx].algo_id = test_algos[i];
		map[idx].compressed_len = 100 + i * 50;
		map[idx].occupied = 1;
	}

	/* Look up entries */
	int found = 0;
	for (int i = 0; i < 3; i++) {
		unsigned long idx = (test_addrs[i] >> 12) % MAP_SIZE;
		if (map[idx].occupied && map[idx].vaddr == test_addrs[i])
			found++;
	}

	printf("    Map store/lookup: %s (%d/3 entries found)\n",
	       found == 3 ? "PASS" : "FAIL", found);
}

static void test_latency_budget(void)
{
	struct minimem_page_data **pages = minimem_generate_all_pages(42);
	const struct minimem_compressor *fast_compressors[] = {
		&minimem_same_page_compressor,
		&minimem_bdi_compressor,
		&minimem_wkdm64_compressor,
		&minimem_lz4_compressor,
	};
	const char *names[] = { "same_page", "bdi", "wkdm64", "lz4" };

	printf("\n  Latency budget check (target: <10us per 4KB page decompress):\n");

	for (int a = 0; a < 4; a++) {
		for (int p = 0; p < MINIMEM_PAGE_TYPE_COUNT; p++) {
			size_t bound = fast_compressors[a]->compress_bound(pages[p]->size);
			uint8_t *compressed = malloc(bound);
			uint8_t *decompressed = malloc(pages[p]->size);

			size_t csz = fast_compressors[a]->compress(
				pages[p]->data, pages[p]->size, compressed, bound);
			if (csz == 0) {
				free(compressed);
				free(decompressed);
				continue;
			}

			struct timespec start, end;
			clock_gettime(CLOCK_MONOTONIC, &start);
			for (int i = 0; i < 100; i++) {
				fast_compressors[a]->decompress(
					compressed, csz, decompressed, pages[p]->size);
			}
			clock_gettime(CLOCK_MONOTONIC, &end);

			double us = ((end.tv_sec - start.tv_sec) * 1e9 +
				      (end.tv_nsec - start.tv_nsec)) / 100.0 / 1000.0;

			printf("    %-10s %-15s: %6.2f us %s\n",
			       names[a], minimem_page_type_name(p), us,
			       us < 10.0 ? "OK" : "SLOW");

			free(compressed);
			free(decompressed);
		}
	}

	minimem_free_all_pages(pages);
}

int main(void)
{
	printf("MiniMem kernel module logic tests\n");
	printf("==================================\n\n");

	test_same_page_detection();
	test_advisor_classification();
	test_compress_decompress_all();
	test_map_basic();
	test_latency_budget();

	printf("\nDone.\n");
	return 0;
}