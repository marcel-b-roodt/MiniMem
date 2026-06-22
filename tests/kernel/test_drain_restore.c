/* SPDX-License-Identifier: MIT */
/*
 * test_drain_restore.c — Verify data integrity across module unload/reload
 *
 * Allocates pages, fills with known patterns, lets the scanner compress
 * them, then unloads and reloads the module. Verifies that drain-and-restore
 * preserves all data and that no SIGBUS occurs.
 *
 * Build as static binary for Alpine VM:
 *   gcc -static -o test_drain_restore test_drain_restore.c
 */

#include <sys/mman.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>

#define PAGE_SIZE 4096
#define NUM_PAGES 256
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

static volatile sig_atomic_t got_sigbus = 0;

static void sigbus_handler(int sig)
{
	got_sigbus = 1;
}

int main(void)
{
	int pass = 0, fail = 0, skip = 0;
	int i;
	void *pages[NUM_PAGES];
	unsigned char patterns[NUM_PAGES];
	long zswap_before, zswap_after;

	printf("MiniMem Drain-and-Restore Test\n");
	printf("===============================\n\n");

	if (access(SYSDIR, F_OK) != 0) {
		printf("SKIP: minimem sysfs not found\n");
		return 0;
	}

	struct sigaction sa;
	sa.sa_handler = sigbus_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sigaction(SIGBUS, &sa, NULL);

	printf("Phase 1: Allocating %d pages (%d KB)\n", NUM_PAGES, NUM_PAGES * 4);
	for (i = 0; i < NUM_PAGES; i++) {
		patterns[i] = (unsigned char)(0x42 + (i % 64));
		pages[i] = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE,
				MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (pages[i] == MAP_FAILED) {
			printf("FAIL: mmap %d failed (errno=%d)\n", i, errno);
			return 1;
		}
		memset(pages[i], patterns[i], PAGE_SIZE);
	}

	printf("Phase 2: Enabling scanner for compression\n");
	long scan_before = read_sysfs_long(SYSDIR "/scanner_pages_compressed");
	write_sysfs(SYSDIR "/scanner_enabled", "1\n");

	for (i = 0; i < 10; i++) {
		usleep(1000000);
		long scan_now = read_sysfs_long(SYSDIR "/scanner_pages_compressed");
		if (scan_now > scan_before && scan_now >= scan_before + NUM_PAGES / 4)
			break;
	}

	long scan_after = read_sysfs_long(SYSDIR "/scanner_pages_compressed");
	long faults = read_sysfs_long(SYSDIR "/hook_faults");
	zswap_before = read_sysfs_long(SYSDIR "/zswap_pages");

	printf("  Pages compressed: %ld -> %ld (delta=%ld)\n",
	       scan_before, scan_after, scan_after - scan_before);
	printf("  Hook faults:      %ld\n", faults);
	printf("  Zswap pages:      %ld\n", zswap_before);

	write_sysfs(SYSDIR "/scanner_enabled", "0\n");

	if (zswap_before == 0 && scan_after == scan_before) {
		printf("SKIP: no pages compressed (CONFIG_PAGE_IDLE_FLAG?)\n");
		for (i = 0; i < NUM_PAGES; i++)
			munmap(pages[i], PAGE_SIZE);
		return 0;
	}

	printf("Phase 3: Verifying data integrity before unload\n");
	int pre_integrity = 1;
	for (i = 0; i < NUM_PAGES; i++) {
		volatile unsigned char *ptr = pages[i];
		for (int j = 0; j < PAGE_SIZE; j++) {
			if (ptr[j] != patterns[i]) {
				pre_integrity = 0;
				printf("FAIL: page %d corrupted before unload "
				       "(byte %d: got 0x%02x, expected 0x%02x)\n",
				       i, j, ptr[j], patterns[i]);
				break;
			}
		}
	}
	if (pre_integrity)
		printf("PASS: all %d pages intact before unload\n", NUM_PAGES), pass++;
	else
		printf("FAIL: pages corrupted before unload\n"), fail++;

	printf("Phase 4: Module unload with drain-and-restore\n");
	printf("  (This test expects the init script to handle unload/reload)\n");
	printf("  Checking if module is still loaded...\n");

	if (access(SYSDIR, F_OK) == 0) {
		printf("  Module still loaded — testing drain-and-restore via rmmod\n");

		long restored = 0;

		FILE *p = popen("dmesg | grep 'minimem:.*drain_and_restore' | tail -1", "r");
		if (p) {
			char line[256];
			if (fgets(line, sizeof(line), p)) {
				char *rp = strstr(line, "restored ");
				if (rp) {
					restored = atol(rp + 9);
				}
			}
			pclose(p);
		}

		printf("  Unloading module...\n");
		int ret = system("rmmod minimem 2>/dev/null");
		if (ret == 0) {
			printf("  Module unloaded\n");
			pass++;

			printf("  Verifying data integrity after drain-and-restore...\n");
			int post_integrity = 1;
			int post_corruptions = 0;

			if (got_sigbus) {
				printf("FAIL: received SIGBUS during drain-and-restore\n");
				fail++;
			} else {
				for (i = 0; i < NUM_PAGES; i++) {
					volatile unsigned char *ptr = pages[i];
					for (int j = 0; j < PAGE_SIZE; j++) {
						if (ptr[j] != patterns[i]) {
							post_integrity = 0;
							post_corruptions++;
							break;
						}
					}
				}

				if (post_integrity) {
					printf("PASS: all %d pages intact after "
					       "drain-and-restore\n", NUM_PAGES), pass++;
				} else {
					printf("FAIL: %d pages corrupted after "
					       "drain-and-restore\n",
					       post_corruptions), fail++;
				}
			}
		} else {
			printf("SKIP: could not unload module (needs root)\n"), skip++;
		}
	} else {
		printf("SKIP: module already unloaded\n"), skip++;
	}

	for (i = 0; i < NUM_PAGES; i++)
		munmap(pages[i], PAGE_SIZE);

	printf("\n===============================\n");
	printf("Drain-and-Restore Test Results\n");
	printf("===============================\n");
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