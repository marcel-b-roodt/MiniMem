#include "lib/bench_harness.h"
#include "lib/test_data.h"
#include "lib/compressors/same_page.h"
#include "lib/compressors/bdi.h"
#include "lib/compressors/wkdm.h"
#include "lib/compressors/wkdm64.h"
#include "lib/compressors/block_class.h"
#include "lib/compressors/lz4_wrap.h"
#include "lib/compressors/zstd_dict.h"

#include <stdio.h>
#include <stdlib.h>

#define SEED 42

static const struct minimem_compressor *algos[] = {
    &minimem_same_page_compressor,
    &minimem_bdi_compressor,
    &minimem_wkdm_compressor,
    &minimem_wkdm64_compressor,
    &minimem_block_class_compressor,
    &minimem_lz4_compressor,
    &minimem_zstd_dict_compressor,
};

static const char *algo_names[] = {
    "same_page",
    "bdi",
    "wkdm",
    "wkdm64",
    "block_class",
    "lz4",
    "zstd_dict",
};

#define NUM_ALGOS (sizeof(algos) / sizeof(algos[0]))

int main(void)
{
    struct minimem_page_data **pages = minimem_generate_all_pages(SEED);
    if (!pages) {
        fprintf(stderr, "Failed to generate test pages\n");
        return 1;
    }

    printf("MiniMem Benchmark Results\n");
    printf("=========================\n\n");

    printf("%-15s %-15s %10s %10s %10s %10s %10s\n",
           "Algorithm", "Page Type", "Ratio", "Comp MB/s", "Decomp MB/s",
           "Comp us", "Decomp us");
    printf("%-15s %-15s %10s %10s %10s %10s %10s\n",
           "---------", "---------", "----", "---------", "-----------",
           "-------", "--------");

    struct minimem_bench_result *all_results = NULL;
    size_t total_results = 0;

    for (size_t a = 0; a < NUM_ALGOS; a++) {
        for (int p = 0; p < MINIMEM_PAGE_TYPE_COUNT; p++) {
            struct minimem_bench_result r =
                minimem_bench_compressor(algos[a], pages[p]->data,
                                         pages[p]->size, p);

            const char *page_name = minimem_page_type_name(p);

            if (r.compressed_size > 0) {
                printf("%-15s %-15s %10.2f %10.2f %10.2f %10.4f %10.4f\n",
                       algo_names[a], page_name,
                       r.ratio,
                       r.compress_throughput_mbs,
                       r.decompress_throughput_mbs,
                       r.compress_latency_us,
                       r.decompress_latency_us);
            } else {
                printf("%-15s %-15s %10s %10s %10s %10s %10s\n",
                       algo_names[a], page_name,
                       "N/A", "N/A", "N/A", "N/A", "N/A");
            }

            total_results++;
        }
    }

    all_results = malloc(total_results * sizeof(struct minimem_bench_result));
    if (all_results) {
        size_t idx = 0;
        for (size_t a = 0; a < NUM_ALGOS; a++) {
            for (int p = 0; p < MINIMEM_PAGE_TYPE_COUNT; p++) {
                all_results[idx] = minimem_bench_compressor(
                    algos[a], pages[p]->data, pages[p]->size, p);
                idx++;
            }
        }

        minimem_bench_write_csv("reports/benchmarks_stage0.csv",
                                 all_results, total_results);
        minimem_bench_write_json("reports/benchmarks_stage0.json",
                                  all_results, total_results);
        printf("\nResults written to reports/benchmarks_stage0.csv and .json\n");
        free(all_results);
    }

    minimem_free_all_pages(pages);
    return 0;
}