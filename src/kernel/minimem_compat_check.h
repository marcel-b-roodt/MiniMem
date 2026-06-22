/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * minimem_compat_check.h — Runtime kernel compatibility verification
 *
 * Before loading, MiniMem must verify that the running kernel provides
 * all required symbols, PTE layouts, and configurations. This module
 * performs those checks and refuses to load if critical requirements
 * are not met, printing a clear diagnostic.
 *
 * Checks performed:
 *   1. Kernel version (minimum 5.10)
 *   2. SWP_PTE_MARKER type value matches our assumption (31 on x86-64)
 *   3. Required kernel symbols resolvable
 *   4. CONFIG_PAGE_IDLE_FLAG available (scanner requirement)
 *   5. Kernel patches detected (optional, enables sweep pass)
 *   6. Kallsyms available (required for symbol resolution)
 *   7. Kprobes available (required for kprobe fallback)
 *   8. zsmalloc available (required for compressed page storage)
 *   9. folio_add_new_anon_rmap signature (kernel version dependent)
 *  10. pte_mkwrite signature (1-arg vs 2-arg)
 *
 * After minimem_compat_check() succeeds, the following guarantees hold:
 *   - All required symbols are resolved and stored
 *   - PTE marker encoding is valid for this kernel
 *   - Either kernel patches or kprobe fallback is available
 *   - zsmalloc pool creation will succeed
 */

#ifndef MINIMEM_KERNEL_COMPAT_CHECK_H
#define MINIMEM_KERNEL_COMPAT_CHECK_H

#include <linux/types.h>

struct minimem_compat_result {
	bool kernel_version_ok;
	bool swp_type_ok;
	bool kallsyms_ok;
	bool kprobes_ok;
	bool zsmalloc_ok;
	bool page_idle_ok;
	bool kernel_patches_ok;
	bool required_symbols_ok;
	bool folio_rmap_ok;
	bool pte_mkwrite_ok;

	unsigned int kernel_major;
	unsigned int kernel_minor;
	unsigned int kernel_patch;

	unsigned int detected_swp_type;
	bool pte_mkwrite_two_args;

	int fail_count;
};

int minimem_compat_check(struct minimem_compat_result *result);
void minimem_compat_report(const struct minimem_compat_result *result);

/*
 * Returns the kallsyms_lookup_name function pointer resolved during
 * the compat check. Returns NULL if the compat check has not been
 * run or if kallsyms was not available.
 * This allows other subsystems (hook module) to reuse the resolved
 * address instead of registering their own kprobe.
 */
unsigned long minimem_compat_kallsyms_lookup_name(void);

#endif /* MINIMEM_KERNEL_COMPAT_CHECK_H */