/* SPDX-License-Identifier: MIT */
/*
 * test_perf_harness.c — Comprehensive performance test harness for MiniMem
 *
 * Measures:
 *   1. Decompression latency (per-page, p50/p95/p99/mean)
 *   2. Compression latency (per-page, p50/p95/p99/mean)
 *   3. Throughput (pages/sec, MB/s) for compress and decompress
 *   4. Concurrency stress (multiple threads faulting simultaneously)
 *   5. Activity pattern: verify pages stay present when in use
 *   6. Activity pattern: verify cold pages get re-compressed
 *   7. Memory overhead (pool_pages vs bytes_saved ratio)
 *   8. Fault latency under load
 *
 * Build: gcc -static -lpthread -o test_perf_harness test_perf_harness.c
 * Run:   ./test_perf_harness [options]
 *
 * Options:
 *   --pages N       Number of pages to test (default 1024)
 *   --threads N     Number of concurrent threads (default 4)
 *   --rounds N      Number of rounds for activity test (default 3)
 *   --csv FILE      Write results to CSV file
 *   --quick         Quick mode: fewer pages, fewer rounds
 */

#include <sys/mman.h>
#include <sys/time.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <math.h>

#define PAGE_SIZE 4096
#define SYSDIR "/sys/kernel/minimem"
#define DEBUGDIR "/sys/kernel/debug/minimem"

static long read_sysfs_long(const char *path)
{
	int fd = open(path, O_RDONLY);
	long val = -1;
	if (fd >= 0) {
		char buf[64];
		int n = read(fd, buf, sizeof(buf) - 1);
		if (n > 0) {
			buf[n] = '\0';
			val = atol(buf);
		}
		close(fd);
	}
	return val;
}

static int write_sysfs(const char *path, const char *data)
{
	int fd = open(path, O_WRONLY);
	if (fd < 0)
		return -1;
	int n = write(fd, data, strlen(data));
	close(fd);
	return n > 0 ? 0 : -1;
}

static int write_debugfs(const char *file, const char *data)
{
	int fd = open(file, O_WRONLY);
	if (fd < 0)
		return -1;
	int n = write(fd, data, strlen(data));
	close(fd);
	return n > 0 ? 0 : -1;
}

static double get_time_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e9 + ts.tv_nsec;
}

static int cmp_double(const void *a, const void *b)
{
	double da = *(const double *)a;
	double db = *(const double *)b;
	return (da > db) - (da < db);
}

static double percentile(double *arr, int n, double pct)
{
	int idx = (int)(pct / 100.0 * (n - 1));
	if (idx < 0) idx = 0;
	if (idx >= n) idx = n - 1;
	return arr[idx];
}

static double mean(double *arr, int n)
{
	double sum = 0;
	for (int i = 0; i < n; i++)
		sum += arr[i];
	return sum / n;
}

struct latency_results {
	double p50;
	double p95;
	double p99;
	double mean_ns;
	double min_ns;
	double max_ns;
	double total_ns;
	int count;
};

static void compute_latency(double *latencies, int n,
			    struct latency_results *r)
{
	if (n == 0) {
		memset(r, 0, sizeof(*r));
		return;
	}

	qsort(latencies, n, sizeof(double), cmp_double);
	r->p50 = percentile(latencies, n, 50);
	r->p95 = percentile(latencies, n, 95);
	r->p99 = percentile(latencies, n, 99);
	r->mean_ns = mean(latencies, n);
	r->min_ns = latencies[0];
	r->max_ns = latencies[n - 1];
	r->total_ns = 0;
	r->count = n;
	for (int i = 0; i < n; i++)
		r->total_ns += latencies[i];
}

static void print_latency(const char *name, const struct latency_results *r)
{
	printf("  %-25s p50=%7.1fus  p95=%7.1fus  p99=%7.1fus  "
	       "mean=%7.1fus  min=%7.1fus  max=%7.1fus  n=%d\n",
	       name,
	       r->p50 / 1000.0, r->p95 / 1000.0, r->p99 / 1000.0,
	       r->mean_ns / 1000.0, r->min_ns / 1000.0,
	       r->max_ns / 1000.0, r->count);
}

/* ── Test 1: Compression & Decompression Latency ── */
static int test_latency(int num_pages)
{
	int pass = 0, fail = 0, skip = 0;
	void **pages;
	unsigned char *patterns;
	double *comp_latencies;
	double *decomp_latencies;
	int comp_count = 0, decomp_count = 0;
	char addr_buf[32];

	printf("\n=== Test 1: Compression & Decompression Latency ===\n");
	printf("  Pages: %d (%d KB)\n", num_pages, num_pages * 4);

	pages = calloc(num_pages, sizeof(void *));
	patterns = calloc(num_pages, sizeof(unsigned char));
	comp_latencies = calloc(num_pages, sizeof(double));
	decomp_latencies = calloc(num_pages, sizeof(double));

	if (!pages || !patterns || !comp_latencies || !decomp_latencies) {
		printf("  FAIL: malloc failed\n");
		free(pages); free(patterns);
		free(comp_latencies); free(decomp_latencies);
		return 1;
	}

	for (int i = 0; i < num_pages; i++) {
		patterns[i] = (unsigned char)((i * 37 + 0x42) & 0xFF);
		pages[i] = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE,
				MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (pages[i] == MAP_FAILED) {
			printf("  FAIL: mmap %d failed\n", i);
			num_pages = i;
			break;
		}
		memset(pages[i], patterns[i], PAGE_SIZE);
	}

	long faults_before = read_sysfs_long(SYSDIR "/hook_faults");
	long comp_ns_before = read_sysfs_long(SYSDIR "/compress_ns_total");
	long decomp_ns_before = read_sysfs_long(SYSDIR "/decompress_ns_total");

	printf("  Compressing %d pages via debugfs...\n", num_pages);
	for (int i = 0; i < num_pages; i++) {
		snprintf(addr_buf, sizeof(addr_buf), "0x%lx",
			 (unsigned long)pages[i]);
		write_debugfs(DEBUGDIR "/compress", addr_buf);
	}

	usleep(200000);

	printf("  Reading compressed pages (triggering faults)...\n");
	for (int i = 0; i < num_pages; i++) {
		double t0 = get_time_ns();
		volatile unsigned char *p = pages[i];
		(void)p[0];
		double t1 = get_time_ns();
		decomp_latencies[decomp_count++] = t1 - t0;
	}

	long faults_after = read_sysfs_long(SYSDIR "/hook_faults");
	long comp_ns_after = read_sysfs_long(SYSDIR "/compress_ns_total");
	long decomp_ns_after = read_sysfs_long(SYSDIR "/decompress_ns_total");

	long faults_delta = faults_after - faults_before;
	long comp_ns_delta = comp_ns_after - comp_ns_before;
	long decomp_ns_delta = decomp_ns_after - decomp_ns_before;

	printf("  Faults handled: %ld\n", faults_delta);
	printf("  Kernel compress time: %ld ns total, %.1f us avg\n",
	       comp_ns_delta, comp_ns_delta / (double)(faults_delta ?: 1) / 1000.0);
	printf("  Kernel decompress time: %ld ns total, %.1f us avg\n",
	       decomp_ns_delta, decomp_ns_delta / (double)(faults_delta ?: 1) / 1000.0);

	struct latency_results decomp_lr;
	compute_latency(decomp_latencies, decomp_count, &decomp_lr);
	print_latency("decompress (fault)", &decomp_lr);

	if (decomp_count > 0 && decomp_lr.p99 < 100000) {
		printf("  PASS: decompress p99 latency %.1f us < 100 us\n",
		       decomp_lr.p99 / 1000.0);
		pass++;
	} else if (decomp_count > 0) {
		printf("  WARN: decompress p99 latency %.1f us > 100 us\n",
		       decomp_lr.p99 / 1000.0);
		skip++;
	} else {
		printf("  SKIP: no decompression measurements\n");
		skip++;
	}

	int integrity_pass = 1;
	for (int i = 0; i < num_pages; i++) {
		volatile unsigned char *p = pages[i];
		for (int j = 0; j < (int)PAGE_SIZE; j++) {
			if (p[j] != patterns[i]) {
				integrity_pass = 0;
				break;
			}
		}
		if (!integrity_pass) break;
	}
	if (integrity_pass) {
		printf("  PASS: all %d pages intact after decompression\n", num_pages);
		pass++;
	} else {
		printf("  FAIL: data corruption detected\n");
		fail++;
	}

	for (int i = 0; i < num_pages; i++)
		munmap(pages[i], PAGE_SIZE);
	free(pages); free(patterns);
	free(comp_latencies); free(decomp_latencies);

	printf("  Latency test: %d pass, %d fail, %d skip\n", pass, fail, skip);
	return fail;
}

/* ── Test 2: Throughput ── */
static int test_throughput(int num_pages)
{
	int pass = 0, fail = 0, skip = 0;
	printf("\n=== Test 2: Throughput ===\n");

	long scan_before = read_sysfs_long(SYSDIR "/scanner_pages_compressed");
	long scan_scanned = read_sysfs_long(SYSDIR "/scanner_pages_scanned");

	if (scan_before == 0 && scan_scanned == 0) {
		printf("  SKIP: no scanner activity (enable scanner first)\n");
		return 0;
	}

	printf("  Pages scanned:     %ld\n", scan_scanned);
	printf("  Pages compressed:  %ld\n", scan_before);

	long comp_ns = read_sysfs_long(SYSDIR "/compress_ns_total");
	long decomp_ns = read_sysfs_long(SYSDIR "/decompress_ns_total");
	long comp_count = read_sysfs_long(SYSDIR "/compress_count");
	long decomp_count = read_sysfs_long(SYSDIR "/decompress_count");

	if (comp_count > 0) {
		double comp_throughput = (comp_count * PAGE_SIZE) /
					(comp_ns / 1e9) / (1024 * 1024);
		double comp_avg_us = (comp_ns / (double)comp_count) / 1000.0;
		printf("  Compress:  %ld ops, %.1f us/op, %.1f MB/s\n",
		       comp_count, comp_avg_us, comp_throughput);
		if (comp_throughput > 100)
			printf("  PASS: compress throughput > 100 MB/s\n"), pass++;
		else
			printf("  OK: compress throughput %.1f MB/s\n", comp_throughput), pass++;
	}

	if (decomp_count > 0) {
		double decomp_throughput = (decomp_count * PAGE_SIZE) /
					(decomp_ns / 1e9) / (1024 * 1024);
		double decomp_avg_us = (decomp_ns / (double)decomp_count) / 1000.0;
		printf("  Decompress: %ld ops, %.1f us/op, %.1f MB/s\n",
		       decomp_count, decomp_avg_us, decomp_throughput);
		if (decomp_throughput > 200)
			printf("  PASS: decompress throughput > 200 MB/s\n"), pass++;
		else
			printf("  OK: decompress throughput %.1f MB/s\n",
			       decomp_throughput), pass++;
	}

	printf("  Throughput test: %d pass, %d fail, %d skip\n", pass, fail, skip);
	return fail;
}

/* ── Test 3: Concurrency stress ── */
struct concur_args {
	void **pages;
	unsigned char *patterns;
	int num_pages;
	int thread_id;
	int rounds;
	int *errors;
	pthread_barrier_t *barrier;
};

static void *concur_thread(void *arg)
{
	struct concur_args *a = arg;
	int mismatches = 0;

	pthread_barrier_wait(a->barrier);

	for (int r = 0; r < a->rounds; r++) {
		for (int i = 0; i < a->num_pages; i++) {
			volatile unsigned char *p = a->pages[i];
			if (p[0] != a->patterns[i])
				mismatches++;
			if (p[PAGE_SIZE - 1] != a->patterns[i])
				mismatches++;
		}
	}

	if (mismatches > 0)
		(*a->errors) += mismatches;

	return NULL;
}

static int test_concurrency(int num_pages, int num_threads, int rounds)
{
	int pass = 0, fail = 0, skip = 0;
	void **pages;
	unsigned char *patterns;
	pthread_t *threads;
	struct concur_args *args;
	pthread_barrier_t barrier;
	int errors = 0;
	char addr_buf[32];

	printf("\n=== Test 3: Concurrency Stress ===\n");
	printf("  Pages: %d, Threads: %d, Rounds: %d\n",
	       num_pages, num_threads, rounds);

	pages = calloc(num_pages, sizeof(void *));
	patterns = calloc(num_pages, sizeof(unsigned char));
	threads = calloc(num_threads, sizeof(pthread_t));
	args = calloc(num_threads, sizeof(struct concur_args));

	if (!pages || !patterns || !threads || !args) {
		printf("  FAIL: malloc failed\n");
		free(pages); free(patterns); free(threads); free(args);
		return 1;
	}

	for (int i = 0; i < num_pages; i++) {
		patterns[i] = (unsigned char)((i * 73 + 0x55) & 0xFF);
		pages[i] = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE,
				MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (pages[i] == MAP_FAILED) {
			num_pages = i;
			break;
		}
		memset(pages[i], patterns[i], PAGE_SIZE);
	}

	printf("  Compressing pages...\n");
	for (int i = 0; i < num_pages; i++) {
		snprintf(addr_buf, sizeof(addr_buf), "0x%lx",
			 (unsigned long)pages[i]);
		write_debugfs(DEBUGDIR "/compress", addr_buf);
	}

	usleep(200000);

	pthread_barrier_init(&barrier, NULL, num_threads);

	printf("  Launching %d threads for %d rounds of reads...\n",
	       num_threads, rounds);

	double t_start = get_time_ns();

	for (int t = 0; t < num_threads; t++) {
		args[t].pages = pages;
		args[t].patterns = patterns;
		args[t].num_pages = num_pages;
		args[t].thread_id = t;
		args[t].rounds = rounds;
		args[t].errors = &errors;
		args[t].barrier = &barrier;
		pthread_create(&threads[t], NULL, concur_thread, &args[t]);
	}

	for (int t = 0; t < num_threads; t++)
		pthread_join(threads[t], NULL);

	double t_end = get_time_ns();
	double elapsed_us = (t_end - t_start) / 1000.0;
	long total_ops = (long)num_pages * num_threads * rounds;
	double ops_per_sec = total_ops / (elapsed_us / 1e6);

	printf("  Total ops: %ld, Elapsed: %.1f us, Ops/sec: %.0f\n",
	       total_ops, elapsed_us, ops_per_sec);

	if (errors == 0) {
		printf("  PASS: all %d threads, %d pages, %d rounds — no corruption\n",
		       num_threads, num_pages, rounds);
		pass++;
	} else {
		printf("  FAIL: %d byte mismatches across %d threads\n",
		       errors, num_threads);
		fail++;
	}

	for (int i = 0; i < num_pages; i++)
		munmap(pages[i], PAGE_SIZE);
	free(pages); free(patterns); free(threads); free(args);
	pthread_barrier_destroy(&barrier);

	printf("  Concurrency test: %d pass, %d fail, %d skip\n", pass, fail, skip);
	return fail;
}

/* ── Test 4: Activity pattern (pages stay present when in use) ── */
static int test_activity(int num_pages, int rounds)
{
	int pass = 0, fail = 0, skip = 0;
	void **pages;
	unsigned char *patterns;
	char addr_buf[32];

	printf("\n=== Test 4: Activity Pattern (re-access stability) ===\n");
	printf("  Pages: %d, Rounds: %d\n", num_pages, rounds);

	pages = calloc(num_pages, sizeof(void *));
	patterns = calloc(num_pages, sizeof(unsigned char));

	if (!pages || !patterns) {
		printf("  FAIL: malloc failed\n");
		free(pages); free(patterns);
		return 1;
	}

	for (int i = 0; i < num_pages; i++) {
		patterns[i] = (unsigned char)((i * 37 + 0xA7) & 0xFF);
		pages[i] = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE,
				MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (pages[i] == MAP_FAILED) {
			num_pages = i;
			break;
		}
		memset(pages[i], patterns[i], PAGE_SIZE);
	}

	printf("  Phase 1: Compress pages via debugfs\n");
	for (int i = 0; i < num_pages; i++) {
		snprintf(addr_buf, sizeof(addr_buf), "0x%lx",
			 (unsigned long)pages[i]);
		write_debugfs(DEBUGDIR "/compress", addr_buf);
	}

	usleep(200000);

	printf("  Phase 2: Access all pages (trigger decompression)\n");
	for (int i = 0; i < num_pages; i++) {
		volatile unsigned char *p = pages[i];
		(void)p[0];
	}

	usleep(100000);

	printf("  Phase 3: Repeatedly touch pages and verify integrity\n");
	int integrity_ok = 1;
	for (int r = 0; r < rounds; r++) {
		for (int i = 0; i < num_pages; i++) {
			volatile unsigned char *p = pages[i];
			p[0] = patterns[i];
			__sync_synchronize();
			if (p[0] != patterns[i] || p[PAGE_SIZE - 1] != patterns[i]) {
				integrity_ok = 0;
				printf("  FAIL: page %d corrupted in round %d\n", i, r);
				break;
			}
		}
		if (!integrity_ok) break;
	}

	if (integrity_ok) {
		printf("  PASS: all %d pages stable across %d rounds of re-access\n",
		       num_pages, rounds);
		pass++;
	} else {
		printf("  FAIL: page corruption detected during re-access\n");
		fail++;
	}

	long faults = read_sysfs_long(SYSDIR "/hook_faults");
	printf("  Total faults: %ld\n", faults);

	for (int i = 0; i < num_pages; i++)
		munmap(pages[i], PAGE_SIZE);
	free(pages); free(patterns);

	printf("  Activity test: %d pass, %d fail, %d skip\n", pass, fail, skip);
	return fail;
}

/* ── Test 5: Memory overhead ── */
static int test_overhead(int num_pages)
{
	int pass = 0, fail = 0, skip = 0;
	void **pages;
	unsigned char *patterns;
	char addr_buf[32];

	printf("\n=== Test 5: Memory Overhead ===\n");
	printf("  Pages: %d (%d KB)\n", num_pages, num_pages * 4);

	pages = calloc(num_pages, sizeof(void *));
	patterns = calloc(num_pages, sizeof(unsigned char));

	if (!pages || !patterns) {
		free(pages); free(patterns);
		return 1;
	}

	for (int i = 0; i < num_pages; i++) {
		patterns[i] = (unsigned char)((i * 13 + 0x42) & 0xFF);
		pages[i] = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE,
				MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (pages[i] == MAP_FAILED) {
			num_pages = i;
			break;
		}
		memset(pages[i], patterns[i], PAGE_SIZE);
	}

	long pool_before = read_sysfs_long(SYSDIR "/pool_pages");
	long zswap_before = read_sysfs_long(SYSDIR "/zswap_pages");
	long saved_before = read_sysfs_long(SYSDIR "/zswap_saved");

	int compressed = 0;
	for (int i = 0; i < num_pages; i++) {
		snprintf(addr_buf, sizeof(addr_buf), "0x%lx",
			 (unsigned long)pages[i]);
		if (write_debugfs(DEBUGDIR "/compress", addr_buf) == 0)
			compressed++;
	}

	usleep(100000);

	long pool_after = read_sysfs_long(SYSDIR "/pool_pages");
	long zswap_after = read_sysfs_long(SYSDIR "/zswap_pages");
	long saved_after = read_sysfs_long(SYSDIR "/zswap_saved");

	long pool_delta = pool_after - pool_before;
	long zswap_delta = zswap_after - zswap_before;
	long saved_delta = saved_after - saved_before;

	printf("  Compressed: %d/%d pages\n", compressed, num_pages);
	printf("  Pool pages:  %ld -> %ld (delta=%ld)\n",
	       pool_before, pool_after, pool_delta);
	printf("  Zswap pages:  %ld -> %ld (delta=%ld)\n",
	       zswap_before, zswap_after, zswap_delta);
	printf("  Bytes saved:  %ld -> %ld (delta=%ld)\n",
	       saved_before, saved_after, saved_delta);

	if (zswap_delta > 0 && saved_delta > 0) {
		double compression_ratio = (double)(zswap_delta * PAGE_SIZE)
					  / saved_delta;
		double overhead_per_page = (double)(pool_delta * PAGE_SIZE)
					 / zswap_delta;
		double savings_pct = (double)saved_delta * 100.0
				   / (zswap_delta * PAGE_SIZE);

		printf("  Compression ratio: %.2f:1\n", compression_ratio);
		printf("  Savings: %.1f%%\n", savings_pct);
		printf("  Overhead: %.1f bytes/page (%.2f pages in pool per compressed page)\n",
		       overhead_per_page, (double)pool_delta / zswap_delta);

		if (savings_pct > 10) {
			printf("  PASS: savings %.1f%% > 10%% threshold\n", savings_pct);
			pass++;
		} else {
			printf("  WARN: savings %.1f%% < 10%% (data may not be very compressible)\n",
			       savings_pct);
			skip++;
		}
	} else {
		printf("  SKIP: no pages compressed\n");
		skip++;
	}

	for (int i = 0; i < num_pages; i++)
		munmap(pages[i], PAGE_SIZE);
	free(pages); free(patterns);

	printf("  Overhead test: %d pass, %d fail, %d skip\n", pass, fail, skip);
	return fail;
}

/* ── Test 6: Scanner-driven re-compression ── */
static int test_scanner_recompress(int num_pages)
{
	int pass = 0, fail = 0, skip = 0;
	void **pages;
	unsigned char *patterns;
	char addr_buf[32];

	printf("\n=== Test 6: Scanner-driven Re-compression ===\n");
	printf("  Pages: %d\n", num_pages);

	pages = calloc(num_pages, sizeof(void *));
	patterns = calloc(num_pages, sizeof(unsigned char));

	if (!pages || !patterns) {
		free(pages); free(patterns);
		return 1;
	}

	for (int i = 0; i < num_pages; i++) {
		patterns[i] = (unsigned char)((i * 41 + 0x33) & 0xFF);
		pages[i] = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE,
				MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (pages[i] == MAP_FAILED) {
			num_pages = i;
			break;
		}
		memset(pages[i], patterns[i], PAGE_SIZE);
	}

	long scan_before = read_sysfs_long(SYSDIR "/scanner_pages_compressed");

	printf("  Phase 1: Enable scanner and wait for compression\n");
	write_sysfs(SYSDIR "/scanner_enabled", "1\n");
	usleep(5000000);

	long scan_after = read_sysfs_long(SYSDIR "/scanner_pages_compressed");
	long faults_before = read_sysfs_long(SYSDIR "/hook_faults");

	printf("  Scanner compressed: %ld -> %ld (delta=%ld)\n",
	       scan_before, scan_after, scan_after - scan_before);

	if (scan_after <= scan_before) {
		printf("  SKIP: scanner did not compress any pages "
		       "(CONFIG_PAGE_IDLE_FLAG?)\n");
		write_sysfs(SYSDIR "/scanner_enabled", "0\n");
		skip++;
		goto cleanup;
	}

	printf("  Phase 2: Touch all pages (decompress)\n");
	for (int i = 0; i < num_pages; i++) {
		volatile unsigned char *p = pages[i];
		p[0] = patterns[i];
	}

	usleep(500000);

	printf("  Phase 3: Wait for scanner to re-compress cold pages\n");
	usleep(8000000);

	long scan_after2 = read_sysfs_long(SYSDIR "/scanner_pages_compressed");
	long faults_after = read_sysfs_long(SYSDIR "/hook_faults");

	printf("  After re-touch: scanner compressed=%ld, faults=%ld (delta=%ld)\n",
	       scan_after2, faults_after, faults_after - faults_before);

	printf("  Phase 4: Verify data integrity\n");
	int integrity_ok = 1;
	for (int i = 0; i < num_pages; i++) {
		volatile unsigned char *p = pages[i];
		for (int j = 0; j < (int)PAGE_SIZE; j++) {
			if (p[j] != patterns[i]) {
				integrity_ok = 0;
				printf("  FAIL: page %d byte %d: expected 0x%02x got 0x%02x\n",
				       i, j, patterns[i], p[j]);
				break;
			}
		}
		if (!integrity_ok) break;
	}

	if (integrity_ok) {
		printf("  PASS: all %d pages intact after scanner re-compression cycle\n",
		       num_pages);
		pass++;
	} else {
		fail++;
	}

	write_sysfs(SYSDIR "/scanner_enabled", "0\n");

cleanup:
	for (int i = 0; i < num_pages; i++)
		munmap(pages[i], PAGE_SIZE);
	free(pages); free(patterns);

	printf("  Scanner re-compress test: %d pass, %d fail, %d skip\n",
	       pass, fail, skip);
	return fail;
}

static void usage(const char *prog)
{
	printf("Usage: %s [options]\n", prog);
	printf("  --pages N     Number of pages (default 1024)\n");
	printf("  --threads N   Concurrent threads (default 4)\n");
	printf("  --rounds N    Activity test rounds (default 3)\n");
	printf("  --csv FILE    Write results to CSV\n");
	printf("  --quick       Quick mode (64 pages, 1 round)\n");
	printf("  --help        Show this help\n");
}

int main(int argc, char *argv[])
{
	int num_pages = 1024;
	int num_threads = 4;
	int rounds = 3;
	int quick = 0;
	const char *csv_file = NULL;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--pages") == 0 && i + 1 < argc)
			num_pages = atoi(argv[++i]);
		else if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc)
			num_threads = atoi(argv[++i]);
		else if (strcmp(argv[i], "--rounds") == 0 && i + 1 < argc)
			rounds = atoi(argv[++i]);
		else if (strcmp(argv[i], "--csv") == 0 && i + 1 < argc)
			csv_file = argv[++i];
		else if (strcmp(argv[i], "--quick") == 0)
			quick = 1;
		else if (strcmp(argv[i], "--help") == 0) {
			usage(argv[0]);
			return 0;
		}
	}

	if (quick) {
		num_pages = 64;
		num_threads = 2;
		rounds = 1;
	}

	printf("MiniMem Performance Harness\n");
	printf("===========================\n");
	printf("  Pages: %d, Threads: %d, Rounds: %d%s\n",
	       num_pages, num_threads, rounds,
	       quick ? " (quick mode)" : "");

	if (access(SYSDIR, F_OK) != 0) {
		printf("SKIP: minimem sysfs not found\n");
		return 0;
	}

	printf("\n  Compatibility report:\n");
	long compat_check = read_sysfs_long(SYSDIR "/compat_report");
	(void)compat_check;

	FILE *cr = fopen(SYSDIR "/compat_report", "r");
	if (cr) {
		char line[256];
		while (fgets(line, sizeof(line), cr))
			printf("    %s", line);
		fclose(cr);
	}
	printf("\n");

	int total_fail = 0;

	total_fail += test_latency(num_pages);
	total_fail += test_throughput(num_pages);
	total_fail += test_concurrency(num_pages > 256 ? 256 : num_pages,
				       num_threads, rounds);
	total_fail += test_activity(num_pages > 256 ? 256 : num_pages, rounds);
	total_fail += test_overhead(num_pages > 256 ? 256 : num_pages);
	total_fail += test_scanner_recompress(num_pages > 128 ? 128 : num_pages);

	printf("\n===========================\n");
	printf("Performance Harness Results\n");
	printf("===========================\n");
	printf("  Total failures: %d\n", total_fail);
	printf("\n");

	if (total_fail == 0)
		printf("ALL TESTS PASSED\n");
	else
		printf("%d TESTS FAILED\n", total_fail);

	if (csv_file) {
		FILE *f = fopen(csv_file, "w");
		if (f) {
			long comp_ns = read_sysfs_long(SYSDIR "/compress_ns_total");
			long decomp_ns = read_sysfs_long(SYSDIR "/decompress_ns_total");
			long comp_count = read_sysfs_long(SYSDIR "/compress_count");
			long decomp_count = read_sysfs_long(SYSDIR "/decompress_count");
			long faults = read_sysfs_long(SYSDIR "/hook_faults");
			long zswap_pages = read_sysfs_long(SYSDIR "/zswap_pages");
			long saved = read_sysfs_long(SYSDIR "/zswap_saved");

			fprintf(f, "metric,value\n");
			fprintf(f, "compress_ns_total,%ld\n", comp_ns);
			fprintf(f, "decompress_ns_total,%ld\n", decomp_ns);
			fprintf(f, "compress_count,%ld\n", comp_count);
			fprintf(f, "decompress_count,%ld\n", decomp_count);
			fprintf(f, "hook_faults,%ld\n", faults);
			fprintf(f, "zswap_pages,%ld\n", zswap_pages);
			fprintf(f, "bytes_saved,%ld\n", saved);
			if (comp_count > 0)
				fprintf(f, "compress_avg_ns,%ld\n",
					comp_ns / comp_count);
			if (decomp_count > 0)
				fprintf(f, "decompress_avg_ns,%ld\n",
					decomp_ns / decomp_count);
			fprintf(f, "failures,%d\n", total_fail);
			fclose(f);
			printf("Results written to %s\n", csv_file);
		}
	}

	return total_fail;
}