/* SPDX-License-Identifier: MIT */
/*
 * test_stress_pressure.c — Memory pressure + shrinker stress test for MiniMem
 *
 * Allocates most of available RAM, compresses pages via debugfs,
 * then fills remaining memory to trigger shrinker. Verifies no
 * kernel oops, no livelock, and that compressed pages decompress
 * correctly under pressure.
 *
 * Build: gcc -static -o test_stress_pressure test_stress_pressure.c
 * Run:   ./test_stress_pressure [reserve_mb] [compress_mb]
 */

#include <sys/mman.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#define PAGE_SIZE 4096
#define MB (1024 * 1024)
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

static long get_available_kb(void)
{
	long avail = -1;
	FILE *f = fopen("/proc/meminfo", "r");
	if (f) {
		char line[256];
		while (fgets(line, sizeof(line), f)) {
			if (strncmp(line, "MemAvailable:", 13) == 0) {
				avail = atol(line + 13);
				break;
			}
		}
		fclose(f);
	}
	return avail;
}

int main(int argc, char *argv[])
{
	int reserve_mb = argc > 1 ? atoi(argv[1]) : 32;
	int compress_mb = argc > 2 ? atoi(argv[2]) : 4;
	int failures = 0;

	printf("=== Memory Pressure Stress Test ===\n");
	printf("  Reserve: %d MB, Compress target: %d MB\n", reserve_mb, compress_mb);

	long avail_kb = get_available_kb();
	if (avail_kb < 0) {
		printf("  Cannot read MemAvailable, using defaults\n");
		avail_kb = 256 * 1024;
	}
	printf("  Available memory: %ld KB\n", avail_kb);

	int compress_pages = compress_mb * (MB / PAGE_SIZE);
	void **compress_bufs = calloc(compress_pages, sizeof(void *));
	int *patterns = calloc(compress_pages, sizeof(int));

	if (!compress_bufs || !patterns) {
		fprintf(stderr, "malloc failed\n");
		return 1;
	}

	printf("\n--- Phase 1: Allocate + compress pages ---\n");

	long pool_before = read_sysfs_long(SYSDIR "/pool_pages");
	long saved_before = read_sysfs_long(SYSDIR "/zswap_saved");

	for (int i = 0; i < compress_pages; i++) {
		patterns[i] = (i * 53) & 0xFF;
		compress_bufs[i] = mmap(NULL, PAGE_SIZE,
					 PROT_READ | PROT_WRITE,
					 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (compress_bufs[i] == MAP_FAILED) {
			fprintf(stderr, "mmap failed at page %d\n", i);
			failures++;
			compress_pages = i;
			break;
		}
		memset(compress_bufs[i], patterns[i], PAGE_SIZE);
	}

	char addr_buf[32];
	int compressed_count = 0;
	for (int i = 0; i < compress_pages; i++) {
		if (compress_bufs[i] == MAP_FAILED)
			continue;
		snprintf(addr_buf, sizeof(addr_buf), "0x%lx",
			  (unsigned long)compress_bufs[i]);
		if (write_debugfs(DEBUGDIR "/compress", addr_buf) == 0)
			compressed_count++;
	}

	long pool_after = read_sysfs_long(SYSDIR "/pool_pages");
	long saved_after = read_sysfs_long(SYSDIR "/zswap_saved");

	printf("  Compressed: %d/%d pages\n", compressed_count, compress_pages);
	printf("  Pool pages: %ld -> %ld\n", pool_before, pool_after);
	printf("  Bytes saved: %ld -> %ld\n", saved_before, saved_after);

	printf("\n--- Phase 2: Apply memory pressure ---\n");

	long pressure_kb = avail_kb - reserve_mb * 1024;
	if (pressure_kb < 0)
		pressure_kb = 0;

	int pressure_pages = (int)(pressure_kb * 1024 / PAGE_SIZE);
	if (pressure_pages > 200000)
		pressure_pages = 200000;

	printf("  Allocating %d pressure pages (%ld KB)...\n",
	       pressure_pages, (long)pressure_pages * 4);

	void **pressure_bufs = calloc(pressure_pages, sizeof(void *));
	int pressure_count = 0;

	for (int i = 0; i < pressure_pages; i++) {
		pressure_bufs[i] = mmap(NULL, PAGE_SIZE,
					 PROT_READ | PROT_WRITE,
					 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (pressure_bufs[i] == MAP_FAILED)
			break;
		memset(pressure_bufs[i], 0xAA, PAGE_SIZE);
		pressure_count++;

		if (i % 4096 == 0) {
			long pool_now = read_sysfs_long(SYSDIR "/pool_pages");
			if (pool_now < pool_after / 2) {
				printf("  Shrinker kicked in at page %d\n", i);
				break;
			}
		}
	}

	long pool_during = read_sysfs_long(SYSDIR "/pool_pages");
	printf("  Pressure pages allocated: %d\n", pressure_count);
	printf("  Pool pages during pressure: %ld\n", pool_during);

	printf("\n--- Phase 3: Verify compressed pages still correct ---\n");

	int corrupt_count = 0;
	long faults_before = read_sysfs_long(SYSDIR "/hook_faults");

	for (int i = 0; i < compress_pages; i++) {
		if (compress_bufs[i] == MAP_FAILED)
			continue;
		unsigned char *p = (unsigned char *)compress_bufs[i];
		int mismatches = 0;
		for (int j = 0; j < (int)PAGE_SIZE; j++) {
			if (p[j] != (unsigned char)patterns[i])
				mismatches++;
		}
		if (mismatches > 0) {
			corrupt_count++;
			if (corrupt_count <= 3)
				fprintf(stderr, "  Page %d: %d byte mismatches\n",
					i, mismatches);
		}
	}

	long faults_after = read_sysfs_long(SYSDIR "/hook_faults");

	printf("  Verified: %d pages, %d corrupt\n", compress_pages, corrupt_count);
	printf("  Faults during verify: %ld\n", faults_after - faults_before);

	if (corrupt_count > 0) {
		printf("  FAIL: data corruption under pressure!\n");
		failures++;
	} else {
		printf("  PASS: all pages correct\n");
	}

	printf("\n--- Phase 4: Cleanup ---\n");

	for (int i = 0; i < pressure_count; i++)
		munmap(pressure_bufs[i], PAGE_SIZE);
	free(pressure_bufs);

	for (int i = 0; i < compress_pages; i++) {
		if (compress_bufs[i] != MAP_FAILED)
			munmap(compress_bufs[i], PAGE_SIZE);
	}
	free(compress_bufs);
	free(patterns);

	usleep(500000);

	long pool_final = read_sysfs_long(SYSDIR "/pool_pages");
	printf("  Pool pages after cleanup: %ld\n", pool_final);

	printf("\n=== Results: %d failures ===\n", failures);
	return failures > 0 ? 1 : 0;
}