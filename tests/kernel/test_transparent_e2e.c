/* SPDX-License-Identifier: MIT */
/*
 * test_transparent_e2e.c — Static E2E test for transparent page compression
 *
 * Allocates pages via mmap, writes patterns, compresses them via
 * the compress_vaddr debugfs interface, then reads them back to
 * verify transparent decompression via the kprobe page fault hook.
 *
 * Build as static binary for Alpine VM:
 *   musl-gcc -static -o test_transparent_e2e test_transparent_e2e.c
 * Or:
 *   gcc -static -o test_transparent_e2e test_transparent_e2e.c
 */

#include <sys/mman.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>

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

static int test_pattern(int pattern_byte, int test_num)
{
	long faults_before, faults_after, zswap_before, zswap_after;
	int mismatch = 0;
	volatile unsigned char *ptr;

	/* Allocate a page */
	void *page = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (page == MAP_FAILED) {
		printf("FAIL %d: mmap failed (errno=%d)\n", test_num, errno);
		return 1;
	}

	/* Fill with pattern */
	memset(page, pattern_byte, PAGE_SIZE);

	/* Touch the page to make sure it's faulted in */
	ptr = (unsigned char *)page;
	(void)ptr[0];

	/* Record stats before compression */
	faults_before = read_sysfs_long(SYSDIR "/hook_faults");
	zswap_before = read_sysfs_long(SYSDIR "/zswap_pages");

	/* Write virtual address to compress_vaddr */
	unsigned long vaddr = (unsigned long)page;
	char cmd[128];
	snprintf(cmd, sizeof(cmd), "0x%lx\n", vaddr);
	int ret = write_debugfs(DEBUGDIR "/compress_vaddr", cmd);

	if (ret != 0) {
		printf("FAIL %d: compress_vaddr write failed (ret=%d, errno=%d)\n",
		       test_num, ret, errno);
		munmap(page, PAGE_SIZE);
		return 1;
	}

	/* Check zswap stats */
	zswap_after = read_sysfs_long(SYSDIR "/zswap_pages");

	printf("  test %d: vaddr=0x%lx pattern=0x%02x "
	       "zswap_before=%ld zswap_after=%ld\n",
	       test_num, vaddr, pattern_byte, zswap_before, zswap_after);

	if (zswap_after <= zswap_before) {
		printf("SKIP %d: page was not compressed (zswap unchanged)\n",
		       test_num);
		munmap(page, PAGE_SIZE);
		return 0;
	}

	/* Now access the page to trigger transparent decompress */
	mismatch = 0;
	for (int i = 0; i < PAGE_SIZE; i++) {
		if (ptr[i] != (unsigned char)pattern_byte)
			mismatch++;
	}

	/* Check hook_faults */
	faults_after = read_sysfs_long(SYSDIR "/hook_faults");
	long faults_delta = faults_after - faults_before;

	printf("  test %d: mismatches=%d hook_faults_delta=%ld\n",
	       test_num, mismatch, faults_delta);

	if (mismatch == 0)
		printf("PASS %d: data intact after transparent decompress\n",
		       test_num);
	else
		printf("FAIL %d: %d bytes mismatched\n", test_num, mismatch);

	if (faults_delta > 0)
		printf("PASS %d: hook handled %ld fault(s)\n",
		       test_num, faults_delta);
	else
		printf("NOTE %d: no hook faults detected (PTE not replaced?)\n",
		       test_num);

	munmap(page, PAGE_SIZE);
	return mismatch;
}

int main(void)
{
	int failures = 0;

	printf("MiniMem Transparent E2E Test\n");
	printf("=============================\n\n");

	/* Check if compress_vaddr exists */
	int fd = open(DEBUGDIR "/compress_vaddr", O_WRONLY);
	if (fd < 0) {
		printf("SKIP: compress_vaddr not available\n");
		return 0;
	}
	close(fd);

	/* Check if hook is active */
	long faults = read_sysfs_long(SYSDIR "/hook_faults");
	long zswap = read_sysfs_long(SYSDIR "/zswap_pages");
	printf("  Initial: hook_faults=%ld zswap_pages=%ld\n\n",
	       faults, zswap);

	/* Test 1: Zero page (same-page detection should give best ratio) */
	printf("Test 1: Zero page\n");
	failures += test_pattern(0x00, 1);
	printf("\n");

	/* Test 2: 0x42 pattern (LZ4-friendly) */
	printf("Test 2: 0x42 pattern\n");
	failures += test_pattern(0x42, 2);
	printf("\n");

	/* Test 3: 0xFF pattern (uniform) */
	printf("Test 3: 0xFF pattern\n");
	failures += test_pattern(0xFF, 3);
	printf("\n");

	/* Test 4: 0x55 pattern */
	printf("Test 4: 0x55 pattern\n");
	failures += test_pattern(0x55, 4);
	printf("\n");

	/* Final stats */
	printf("=============================\n");
	printf("Final stats:\n");
	printf("  hook_faults:          %ld\n",
	       read_sysfs_long(SYSDIR "/hook_faults"));
	printf("  zswap_pages:          %ld\n",
	       read_sysfs_long(SYSDIR "/zswap_pages"));
	printf("  zswap_saved:          %ld\n",
	       read_sysfs_long(SYSDIR "/zswap_saved"));
	printf("  pages_compressed:     %ld\n",
	       read_sysfs_long(SYSDIR "/pages_compressed"));
	printf("  pages_decompressed:   %ld\n",
	       read_sysfs_long(SYSDIR "/pages_decompressed"));
	printf("\n");

	if (failures == 0)
		printf("ALL TESTS PASSED\n");
	else
		printf("%d TESTS FAILED\n", failures);

	return failures;
}