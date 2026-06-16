/* SPDX-License-Identifier: MIT */
/*
 * test_stress_concurrent.c — Concurrent fault stress test for MiniMem
 *
 * Tests that multiple threads faulting on the same compressed page
 * simultaneously all get correct data without crashes, corruption,
 * or kernel oops.
 *
 * Build: gcc -static -lpthread -o test_stress_concurrent test_stress_concurrent.c
 * Run:   ./test_stress_concurrent [num_pages] [num_threads] [rounds]
 */

#include <sys/mman.h>
#include <sys/wait.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <signal.h>

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

struct fault_args {
	void *page;
	size_t index;
	int pattern;
	int *errors;
};

static void *fault_thread(void *arg)
{
	struct fault_args *a = arg;
	unsigned char *p = (unsigned char *)a->page;
	int i, mismatches = 0;

	for (i = 0; i < (int)PAGE_SIZE; i++) {
		if (p[i] != (unsigned char)a->pattern) {
			mismatches++;
			if (mismatches <= 3) {
				fprintf(stderr,
					"  thread page %zu offset %d: expected 0x%02x got 0x%02x\n",
					a->index, i, a->pattern, p[i]);
			}
		}
	}

	if (mismatches > 0)
		(*a->errors) += mismatches;

	return NULL;
}

int main(int argc, char *argv[])
{
	int num_pages = argc > 1 ? atoi(argv[1]) : 16;
	int num_threads = argc > 2 ? atoi(argv[2]) : 8;
	int rounds = argc > 3 ? atoi(argv[3]) : 3;
	int failures = 0;
	int round, pg, t;

	if (num_pages < 1 || num_threads < 1 || rounds < 1) {
		fprintf(stderr, "Invalid arguments\n");
		return 1;
	}

	printf("=== Concurrent Fault Stress Test ===\n");
	printf("  Pages: %d, Threads/page: %d, Rounds: %d\n",
	       num_pages, num_threads, rounds);

	void **pages = calloc(num_pages, sizeof(void *));
	int *patterns = calloc(num_pages, sizeof(int));
	pthread_t *threads = calloc(num_threads, sizeof(pthread_t));
	struct fault_args *args = calloc(num_threads, sizeof(struct fault_args));

	if (!pages || !patterns || !threads || !args) {
		fprintf(stderr, "malloc failed\n");
		return 1;
	}

	for (round = 0; round < rounds; round++) {
		printf("\n--- Round %d ---\n", round + 1);

		long faults_before = read_sysfs_long(SYSDIR "/hook_faults");

		for (pg = 0; pg < num_pages; pg++) {
			patterns[pg] = (pg * 37 + round * 13) & 0xFF;
			pages[pg] = mmap(NULL, PAGE_SIZE,
					 PROT_READ | PROT_WRITE,
					 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
			if (pages[pg] == MAP_FAILED) {
				fprintf(stderr, "mmap failed for page %d\n", pg);
				failures++;
				continue;
			}
			memset(pages[pg], patterns[pg], PAGE_SIZE);
		}

		char addr_buf[32];
		for (pg = 0; pg < num_pages; pg++) {
			if (pages[pg] == MAP_FAILED)
				continue;
			snprintf(addr_buf, sizeof(addr_buf), "0x%lx",
				  (unsigned long)pages[pg]);
			write_debugfs(DEBUGDIR "/compress", addr_buf);
		}

		usleep(100000);

		int total_errors = 0;

		for (pg = 0; pg < num_pages; pg++) {
			if (pages[pg] == MAP_FAILED)
				continue;

			for (t = 0; t < num_threads; t++) {
				args[t].page = pages[pg];
				args[t].index = pg;
				args[t].pattern = patterns[pg];
				args[t].errors = &total_errors;
				pthread_create(&threads[t], NULL, fault_thread, &args[t]);
			}

			for (t = 0; t < num_threads; t++) {
				pthread_join(threads[t], NULL);
			}
		}

		long faults_after = read_sysfs_long(SYSDIR "/hook_faults");
		long faults_delta = faults_after - faults_before;

		printf("  Faults this round: %ld\n", faults_delta);
		printf("  Byte mismatches: %d\n", total_errors);

		if (total_errors > 0) {
			failures++;
			printf("  FAIL: data corruption detected!\n");
		} else {
			printf("  PASS: all bytes correct\n");
		}

		for (pg = 0; pg < num_pages; pg++) {
			if (pages[pg] != MAP_FAILED)
				munmap(pages[pg], PAGE_SIZE);
		}
	}

	free(pages);
	free(patterns);
	free(threads);
	free(args);

	printf("\n=== Results: %d rounds, %d failures ===\n", rounds, failures);
	return failures > 0 ? 1 : 0;
}