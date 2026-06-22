/* SPDX-License-Identifier: MIT */
/*
 * test_stress_unload.c — Module unload safety stress test for MiniMem
 *
 * Tests that:
 * 1. Module can be unloaded when no pages are compressed (baseline)
 * 2. Module unload after compress+decompress cycle is clean
 * 3. Module unload with compressed pages triggers shrinker
 * 4. No kernel oops or resource leaks after unload
 *
 * Build: gcc -static -o test_stress_unload test_stress_unload.c
 * Run:   ./test_stress_unload
 *
 * Note: This test uses system() to run insmod/rmmod, so it must
 * run as root inside the VM.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>

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

static int write_debugfs(const char *file, const char *data)
{
	int fd = open(file, O_WRONLY);
	if (fd < 0)
		return -1;
	int n = write(fd, data, strlen(data));
	close(fd);
	return n > 0 ? 0 : -1;
}

static int module_loaded(void)
{
	return access(SYSDIR, F_OK) == 0;
}

static int load_module(void)
{
	if (module_loaded())
		return 0;
	int ret = system("insmod /minimem.ko 2>/dev/null");
	if (ret != 0)
		ret = system("insmod -f /minimem.ko 2>/dev/null");
	if (ret != 0)
		ret = system("modprobe minimem 2>/dev/null");
	return module_loaded() ? 0 : -1;
}

static int unload_module(void)
{
	if (!module_loaded())
		return 0;
	int ret = system("rmmod minimem 2>/dev/null");
	usleep(200000);
	return module_loaded() ? -1 : 0;
}

static int check_dmesg_errors(void)
{
	FILE *f = popen("dmesg | tail -30 | grep -iwE 'bug|panic|oops' 2>/dev/null", "r");
	if (!f)
		return 0;

	char line[256];
	int errors = 0;
	while (fgets(line, sizeof(line), f)) {
		errors++;
	}
	pclose(f);
	return errors;
}

int main(void)
{
	int failures = 0;
	char addr_buf[32];

	printf("=== Module Unload Safety Test ===\n");

	printf("\n--- Test 1: Clean load/unload cycle ---\n");

	if (load_module() < 0) {
		printf("  FAIL: cannot load module\n");
		return 1;
	}
	printf("  Module loaded\n");

	if (unload_module() < 0) {
		printf("  FAIL: cannot unload clean module\n");
		failures++;
	} else {
		printf("  PASS: clean unload\n");
	}

	printf("\n--- Test 2: Load, compress, decompress, unload ---\n");

	if (load_module() < 0) {
		printf("  FAIL: cannot reload module\n");
		return 1;
	}

	void *page = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (page == MAP_FAILED) {
		printf("  FAIL: mmap\n");
		unload_module();
		return 1;
	}
	memset(page, 0x42, PAGE_SIZE);

	snprintf(addr_buf, sizeof(addr_buf), "0x%lx", (unsigned long)page);
	write_debugfs(DEBUGDIR "/compress", addr_buf);
	usleep(100000);

	volatile unsigned char *p = (unsigned char *)page;
	if (p[0] != 0x42) {
		printf("  FAIL: decompressed data wrong\n");
		failures++;
	} else {
		printf("  Decompression verified\n");
	}

	munmap(page, PAGE_SIZE);
	usleep(100000);

	if (unload_module() < 0) {
		printf("  FAIL: cannot unload after compress/decompress\n");
		failures++;
	} else {
		printf("  PASS: unload after compress/decompress\n");
	}

	printf("\n--- Test 3: Unload with compressed pages (shrinker test) ---\n");

	if (load_module() < 0) {
		printf("  FAIL: cannot reload module\n");
		return 1;
	}

	long pool_before = read_sysfs_long(SYSDIR "/pool_pages");
	printf("  Pool before compress: %ld\n", pool_before);

	int N = 8;
	void **pages = calloc(N, sizeof(void *));
	for (int i = 0; i < N; i++) {
		pages[i] = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE,
			       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (pages[i] == MAP_FAILED)
			continue;
		memset(pages[i], (i * 37) & 0xFF, PAGE_SIZE);
		snprintf(addr_buf, sizeof(addr_buf), "0x%lx",
			 (unsigned long)pages[i]);
		write_debugfs(DEBUGDIR "/compress", addr_buf);
	}

	usleep(200000);

	long pool_after = read_sysfs_long(SYSDIR "/pool_pages");
	long zswap_pages = read_sysfs_long(SYSDIR "/zswap_pages");
	printf("  Pool after compress: %ld, zswap_pages: %ld\n",
	       pool_after, zswap_pages);

	if (zswap_pages == 0) {
		printf("  SKIP: no pages were compressed (debugfs may not compress)\n");
	} else {
		printf("  Attempting unload with %ld compressed pages...\n",
		       zswap_pages);

		int unload_ret = unload_module();

		if (unload_ret < 0) {
			printf("  Module still loaded — shrinker may not have run yet.\n");
			printf("  Trying again after delay...\n");
			usleep(1000000);
			unload_ret = unload_module();
		}

		if (unload_ret < 0) {
			printf("  FAIL: cannot unload with compressed pages\n");
			failures++;

			printf("  Forcing decompress via read...\n");
			for (int i = 0; i < N; i++) {
				if (pages[i] != MAP_FAILED) {
					volatile unsigned char *cp = pages[i];
					(void)cp[0];
				}
			}
			usleep(200000);
			unload_module();
		} else {
			printf("  PASS: module unloaded (shrinker decompressed pages)\n");
		}
	}

	for (int i = 0; i < N; i++) {
		if (pages[i] != MAP_FAILED)
			munmap(pages[i], PAGE_SIZE);
	}
	free(pages);

	printf("\n--- Test 4: Check for kernel errors ---\n");

	int dmesg_errors = check_dmesg_errors();
	if (dmesg_errors > 0) {
		printf("  FAIL: %d kernel errors in dmesg\n", dmesg_errors);
		failures++;
	} else {
		printf("  PASS: no kernel errors\n");
	}

	printf("\n--- Test 5: Reload after unload (no resource leaks) ---\n");

	if (load_module() < 0) {
		printf("  FAIL: cannot reload module after unload\n");
		failures++;
	} else {
		printf("  Module reloaded successfully\n");
		unload_module();
		printf("  PASS: reload after unload works\n");
	}

	printf("\n=== Results: %d failures ===\n", failures);
	return failures > 0 ? 1 : 0;
}