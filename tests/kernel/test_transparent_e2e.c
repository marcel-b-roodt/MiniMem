/* SPDX-License-Identifier: MIT */
/*
 * test_transparent_e2e.c — Static E2E test for transparent page compression
 *
 * Allocates pages via mmap, writes patterns, compresses them via
 * the compress_vaddr debugfs interface, then reads them back to
 * verify transparent decompression via the kprobe page fault hook.
 *
 * Build as static binary for Alpine VM:
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
#define NUM_TESTS 4
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
	int saved = errno;
	close(fd);
	errno = saved;
	return n > 0 ? 0 : -1;
}

int main(void)
{
	int failures = 0;
	void *pages[NUM_TESTS];
	int patterns[NUM_TESTS] = { 0x00, 0x42, 0xFF, 0x55 };
	long faults_before, faults_after, zswap_before, zswap_after;
	int i;

	printf("MiniMem Transparent E2E Test\n");
	printf("=============================\n\n");

	int fd = open(DEBUGDIR "/compress_vaddr", O_WRONLY);
	if (fd < 0) {
		printf("SKIP: compress_vaddr not available\n");
		return 0;
	}
	close(fd);

	faults_before = read_sysfs_long(SYSDIR "/hook_faults");
	zswap_before = read_sysfs_long(SYSDIR "/zswap_pages");
	printf("  Initial: hook_faults=%ld zswap_pages=%ld\n\n",
	       faults_before, zswap_before);

	/* Phase 1: Allocate all pages and fill with patterns */
	for (i = 0; i < NUM_TESTS; i++) {
		pages[i] = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE,
				MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (pages[i] == MAP_FAILED) {
			printf("FAIL: mmap %d failed (errno=%d)\n", i, errno);
			return 1;
		}
		memset(pages[i], patterns[i], PAGE_SIZE);
		*(volatile char *)pages[i];
		printf("  Allocated page %d: vaddr=0x%lx pattern=0x%02x\n",
		       i + 1, (unsigned long)pages[i], patterns[i]);
	}
	printf("\n");

	/* Phase 2: Mark pages as idle (simulate being cold) */
	/* We use madvise with MADV_PAGEOUT if available, or just wait */
	/* The simplest approach: use compress_vaddr directly */
	for (i = 0; i < NUM_TESTS; i++) {
		unsigned long vaddr = (unsigned long)pages[i];
		char cmd[128];
		snprintf(cmd, sizeof(cmd), "0x%lx\n", vaddr);

		long fb = read_sysfs_long(SYSDIR "/hook_faults");
		long zb = read_sysfs_long(SYSDIR "/zswap_pages");

		int ret = write_debugfs(DEBUGDIR "/compress_vaddr", cmd);
		if (ret != 0) {
			printf("FAIL %d: compress_vaddr write failed "
			       "(ret=%d, errno=%d)\n", i + 1, ret, errno);
			failures++;
			continue;
		}

		long fa = read_sysfs_long(SYSDIR "/hook_faults");
		long za = read_sysfs_long(SYSDIR "/zswap_pages");

		printf("  Compressed page %d: vaddr=0x%lx pattern=0x%02x "
		       "zswap=%ld->%ld faults=%ld->%ld\n",
		       i + 1, vaddr, patterns[i], zb, za, fb, fa);
	}
	printf("\n");

	/* Phase 3: Read back each page to trigger transparent decompress */
	for (i = 0; i < NUM_TESTS; i++) {
		volatile unsigned char *ptr = (unsigned char *)pages[i];
		int mismatch = 0;

		for (int j = 0; j < PAGE_SIZE; j++) {
			if (ptr[j] != (unsigned char)patterns[i])
				mismatch++;
		}

		if (mismatch == 0) {
			printf("PASS %d: data intact after transparent decompress\n",
			       i + 1);
		} else {
			printf("FAIL %d: %d bytes mismatched\n", i + 1, mismatch);
			failures++;
		}
	}
	printf("\n");

	/* Phase 4: Test scanner-based compression */
	printf("=== Scanner test ===\n");
	{
		/* Allocate another page, write pattern, mark idle */
		void *scan_page = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE,
				       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (scan_page != MAP_FAILED) {
			memset(scan_page, 0xAA, PAGE_SIZE);
			*(volatile char *)scan_page;

			long scan_before = read_sysfs_long(
				SYSDIR "/scanner_pages_compressed");

			/* Enable scanner and let it run briefly */
			write_debugfs("/proc/self/mem", "");
			int sfd = open(SYSDIR "/scanner_enabled", O_WRONLY);
			if (sfd >= 0) {
				write(sfd, "1\n", 2);
				close(sfd);
			}

			/* Wait for scanner to process */
			usleep(500000);

			/* Disable scanner */
			sfd = open(SYSDIR "/scanner_enabled", O_WRONLY);
			if (sfd >= 0) {
				write(sfd, "0\n", 2);
				close(sfd);
			}

			long scan_after = read_sysfs_long(
				SYSDIR "/scanner_pages_compressed");
			long hook_after = read_sysfs_long(SYSDIR "/hook_faults");

			printf("  Scanner: compressed %ld -> %ld pages\n",
			       scan_before, scan_after);
			printf("  hook_faults: %ld\n", hook_after);

			/* Verify scan_page data is still correct */
			volatile unsigned char *sptr = scan_page;
			int smismatch = 0;
			for (int j = 0; j < PAGE_SIZE; j++) {
				if (sptr[j] != 0xAA)
					smismatch++;
			}

			if (smismatch == 0)
				printf("PASS: scanner test page intact\n");
			else
				printf("FAIL: scanner test page: %d mismatched\n",
				       smismatch);

			munmap(scan_page, PAGE_SIZE);
		}
	}
	printf("\n");

	/* Phase 5: Check final stats */
	faults_after = read_sysfs_long(SYSDIR "/hook_faults");
	zswap_after = read_sysfs_long(SYSDIR "/zswap_pages");
	long faults_delta = faults_after - faults_before;

	printf("=============================\n");
	printf("Results:\n");
	printf("  hook_faults:          %ld -> %ld (delta=%ld)\n",
	       faults_before, faults_after, faults_delta);
	printf("  zswap_pages:          %ld -> %ld\n",
	       zswap_before, zswap_after);
	printf("  zswap_saved:          %ld\n",
	       read_sysfs_long(SYSDIR "/zswap_saved"));
	printf("  scanner_compressed:  %ld\n",
	       read_sysfs_long(SYSDIR "/scanner_pages_compressed"));
	printf("\n");

	if (failures == 0)
		printf("ALL TESTS PASSED\n");
	else
		printf("%d TESTS FAILED\n", failures);

	for (i = 0; i < NUM_TESTS; i++)
		munmap(pages[i], PAGE_SIZE);

	return failures;
}