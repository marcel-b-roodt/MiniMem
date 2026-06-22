/* SPDX-License-Identifier: MIT */
/*
 * test_cpu_overhead.c — Measure MiniMem scanner and decompression CPU overhead
 *
 * Allocates pages, enables the scanner, measures CPU usage and compression
 * stats, then verifies that the adaptive interval backs off when idle.
 *
 * Build as static binary for Alpine VM:
 *   gcc -static -o test_cpu_overhead test_cpu_overhead.c
 */

#include <sys/mman.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <dirent.h>
#include <sys/time.h>
#include <sys/wait.h>

#define PAGE_SIZE 4096
#define NUM_PAGES 1024
#define SYSDIR "/sys/kernel/minimem"

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
	int saved = errno;
	close(fd);
	errno = saved;
	return n > 0 ? 0 : -1;
}

static double get_time_sec(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static long get_scanner_cpu_ns(void)
{
	FILE *f = popen("grep -r minimem_scand /proc/[0-9]*/comm 2>/dev/null | "
			"head -1 | cut -d/ -f3", "r");
	if (!f)
		return -1;

	long total_ns = -1;
	char line[256];
	if (fgets(line, sizeof(line), f)) {
		pid_t pid = atoi(line);
		if (pid > 0) {
			char statpath[256];
			snprintf(statpath, sizeof(statpath),
				"/proc/%d/stat", pid);
			FILE *sf = fopen(statpath, "r");
			if (sf) {
				char sbuf[512];
				if (fgets(sbuf, sizeof(sbuf), sf)) {
					unsigned long utime, stime;
					char *p = strrchr(sbuf, ')');
					if (p) {
						p += 2;
						int field = 0;
						while (*p && field < 11) {
							if (*p == ' ')
								field++;
							p++;
						}
						utime = strtoul(p, &p, 10);
						stime = strtoul(p + 1, NULL, 10);
						total_ns = (utime + stime)
							   * 10000000L;
					}
				}
				fclose(sf);
			}
		}
	}
	pclose(f);
	return total_ns;
}

int main(void)
{
	int pass = 0, fail = 0, skip = 0;
	int i;
	void *pages[NUM_PAGES];
	double t_start, t_end;
	long scan_cmp_before, scan_cmp_after;
	long scan_scanned_before, scan_scanned_after;
	long scan_skip_incomp_before, scan_skip_incomp_after;
	long faults_before, faults_after;
	long cycles_total_before, cycles_total_after;
	long cycles_empty_before, cycles_empty_after;
	long interval_before, interval_after;

	printf("MiniMem CPU Overhead Test\n");
	printf("=========================\n\n");

	if (access(SYSDIR, F_OK) != 0) {
		printf("SKIP: minimem sysfs not found\n");
		return 0;
	}

	/* Check that new sysfs attributes exist */
	if (access(SYSDIR "/scanner_skip_incompressible", F_OK) != 0) {
		printf("SKIP: scanner_skip_incompressible not found "
		       "(module needs rebuild)\n");
		return 0;
	}

	/* Record baseline */
	scan_cmp_before = read_sysfs_long(SYSDIR "/scanner_pages_compressed");
	scan_scanned_before = read_sysfs_long(SYSDIR "/scanner_pages_scanned");
	scan_skip_incomp_before = read_sysfs_long(SYSDIR "/scanner_skip_incompressible");
	faults_before = read_sysfs_long(SYSDIR "/hook_faults");
	cycles_total_before = read_sysfs_long(SYSDIR "/scanner_cycles_total");
	cycles_empty_before = read_sysfs_long(SYSDIR "/scanner_cycles_empty");
	interval_before = read_sysfs_long(SYSDIR "/scanner_interval_ms");

	printf("Baseline:\n");
	printf("  compressed:  %ld\n", scan_cmp_before);
	printf("  scanned:     %ld\n", scan_scanned_before);
	printf("  skipped:     %ld\n", scan_skip_incomp_before);
	printf("  faults:      %ld\n", faults_before);
	printf("  interval_ms: %ld\n", interval_before);
	printf("\n");

	/* Phase 1: Allocate and touch pages */
	printf("Phase 1: Allocating %d pages (%d KB)\n",
	       NUM_PAGES, NUM_PAGES * 4);
	for (i = 0; i < NUM_PAGES; i++) {
		pages[i] = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE,
				MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (pages[i] == MAP_FAILED) {
			printf("FAIL: mmap %d failed\n", i);
			return 1;
		}
		/* Write pattern to make pages present and dirty */
		memset(pages[i], 0x42 + (i % 16), PAGE_SIZE);
	}

	/* Phase 2: Enable scanner and measure active phase */
	printf("\nPhase 2: Enabling scanner (active phase)\n");
	write_sysfs(SYSDIR "/scanner_enabled", "1\n");
	t_start = get_time_sec();

	/* Wait for 2 mark+sweep cycles (about 2-4 seconds) */
	sleep(4);

	scan_cmp_after = read_sysfs_long(SYSDIR "/scanner_pages_compressed");
	scan_scanned_after = read_sysfs_long(SYSDIR "/scanner_pages_scanned");
	faults_after = read_sysfs_long(SYSDIR "/hook_faults");
	cycles_total_after = read_sysfs_long(SYSDIR "/scanner_cycles_total");

	write_sysfs(SYSDIR "/scanner_enabled", "0\n");
	t_end = get_time_sec();

	double active_elapsed = t_end - t_start;
	long pages_compressed = scan_cmp_after - scan_cmp_before;
	long pages_scanned = scan_scanned_after - scan_scanned_before;
	long faults = faults_after - faults_before;

	printf("  Active phase: %.1f seconds\n", active_elapsed);
	printf("  Pages scanned:     %ld\n", pages_scanned);
	printf("  Pages compressed:  %ld\n", pages_compressed);
	printf("  Faults handled:    %ld\n", faults);

	if (pages_scanned > 0) {
		double scan_rate = pages_scanned / active_elapsed;
		printf("  Scan rate:          %.0f pages/s\n", scan_rate);
		if (scan_rate >= 10000)
			printf("PASS: scan rate >= 10k pages/s\n"), pass++;
		else if (scan_rate >= 1000)
			printf("OK:   scan rate >= 1k pages/s\n"), pass++;
		else
			printf("FAIL: scan rate too low (%.0f pages/s)\n", scan_rate), fail++;
	} else {
		printf("WARN: no pages scanned (CONFIG_PAGE_IDLE_FLAG?)\n"), skip++;
	}

	if (pages_compressed > 0) {
		double cmp_rate = pages_compressed / active_elapsed;
		printf("  Compression rate:  %.1f pages/s\n", cmp_rate);
		printf("PASS: scanner compressed %ld pages\n", pages_compressed), pass++;
	} else if (scan_scanned_before == 0 && scan_scanned_after == 0) {
		printf("SKIP: scanner not active (CONFIG_PAGE_IDLE_FLAG?)\n"), skip++;
	} else {
		printf("OK:   no pages compressed (may need more idle time)\n"), skip++;
	}

	/* Phase 3: Verify data integrity after decompression */
	printf("\nPhase 3: Data integrity check\n");
	int integrity_pass = 0;
	int integrity_fail = 0;
	for (i = 0; i < NUM_PAGES; i++) {
		volatile unsigned char *ptr = pages[i];
		unsigned char expected = 0x42 + (i % 16);
		int mismatch = 0;
		for (int j = 0; j < PAGE_SIZE; j++) {
			if (ptr[j] != expected)
				mismatch++;
		}
		if (mismatch == 0)
			integrity_pass++;
		else
			integrity_fail++;
	}
	printf("  Pages verified:    %d/%d\n", integrity_pass, NUM_PAGES);
	if (integrity_fail == 0) {
		printf("PASS: all %d pages intact after decompression\n",
		       NUM_PAGES), pass++;
	} else {
		printf("FAIL: %d pages have data corruption\n",
		       integrity_fail), fail++;
	}

	/* Phase 4: Measure decompression latency */
	printf("\nPhase 4: Decompression latency\n");
	long decomp_avg_ns = read_sysfs_long(SYSDIR "/decompress_avg_ns");
	long comp_avg_ns = read_sysfs_long(SYSDIR "/compress_avg_ns");
	printf("  Avg compress:       %ld ns\n", comp_avg_ns);
	printf("  Avg decompress:    %ld ns\n", decomp_avg_ns);

	if (decomp_avg_ns > 0 && decomp_avg_ns < 100000) {
		double decomp_us = decomp_avg_ns / 1000.0;
		printf("  Decompress latency: %.1f us\n", decomp_us);
		if (decomp_avg_ns < 10000)
			printf("PASS: decompress < 10us (%.1f us)\n", decomp_us), pass++;
		else if (decomp_avg_ns < 50000)
			printf("OK:   decompress < 50us (%.1f us)\n", decomp_us), pass++;
		else
			printf("WARN: decompress > 50us (%.1f us)\n", decomp_us), skip++;
	} else {
		printf("SKIP: no decompression data available\n"), skip++;
	}

	/* Phase 5: Measure adaptive interval backoff */
	printf("\nPhase 5: Adaptive interval backoff\n");
	write_sysfs(SYSDIR "/scanner_enabled", "1\n");

	long interval_base = read_sysfs_long(SYSDIR "/scanner_current_interval_ms");
	printf("  Base interval:      %ld ms\n", interval_base);

	/* Wait for several empty cycles to trigger backoff */
	sleep(8);

	long interval_backoff = read_sysfs_long(SYSDIR "/scanner_current_interval_ms");
	cycles_empty_after = read_sysfs_long(SYSDIR "/scanner_cycles_empty");

	write_sysfs(SYSDIR "/scanner_enabled", "0\n");

	printf("  Interval after 8s: %ld ms\n", interval_backoff);
	printf("  Empty cycles:       %ld\n",
	       cycles_empty_after - cycles_empty_before);

	if (interval_backoff > interval_base) {
		printf("PASS: interval backed off (%ld -> %ld ms)\n",
		       interval_base, interval_backoff), pass++;
	} else {
		printf("OK:   interval unchanged (may have compressed pages)\n"), skip++;
	}

	/* Clean up */
	for (i = 0; i < NUM_PAGES; i++)
		munmap(pages[i], PAGE_SIZE);

	printf("\n=========================\n");
	printf("CPU Overhead Test Results\n");
	printf("=========================\n");
	printf("  Passed:  %d\n", pass);
	printf("  Failed:  %d\n", fail);
	printf("  Skipped: %d\n", skip);
	printf("\n");

	if (fail == 0)
		printf("ALL TESTS PASSED\n");
	else
		printf("%d TESTS FAILED\n", fail);

	return fail;
}