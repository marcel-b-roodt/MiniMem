/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * minimem_compat_check.c — Runtime kernel compatibility verification
 *
 * Performs comprehensive checks before the module loads. If any
 * critical check fails, the module refuses to load with a clear
 * diagnostic message explaining what's wrong and how to fix it.
 *
 * Non-critical failures (e.g., CONFIG_PAGE_IDLE_FLAG missing) are
 * reported but don't prevent loading — they just disable the feature
 * that depends on them.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/kprobes.h>
#include <linux/version.h>
#include <linux/swap.h>
#include <linux/swapops.h>
#include <linux/pgtable.h>
#include <linux/zsmalloc.h>

#include "minimem_compat_check.h"
#include "minimem_pte.h"

#define MINIMEM_MIN_KERNEL_MAJOR 5
#define MINIMEM_MIN_KERNEL_MINOR 10

static unsigned long (*p_kallsyms_lookup_name)(const char *name);

unsigned long minimem_compat_kallsyms_lookup_name(void)
{
	return (unsigned long)p_kallsyms_lookup_name;
}

static int resolve_kallsyms_for_check(void)
{
	struct kprobe kp;
	int ret;

	memset(&kp, 0, sizeof(kp));
	kp.symbol_name = "kallsyms_lookup_name";

	ret = register_kprobe(&kp);
	if (ret) {
		pr_warn("minimem: compat: failed to register kprobe on "
			"kallsyms_lookup_name: %d\n", ret);
		return ret;
	}

	p_kallsyms_lookup_name = (unsigned long (*)(const char *))kp.addr;
	unregister_kprobe(&kp);

	if (!p_kallsyms_lookup_name) {
		pr_warn("minimem: compat: kallsyms_lookup_name resolved to NULL\n");
		return -ENOENT;
	}

	return 0;
}

static void *check_symbol(const char *name)
{
	if (!p_kallsyms_lookup_name)
		return NULL;
	return (void *)p_kallsyms_lookup_name(name);
}

static bool verify_swp_type(void)
{
	swp_entry_t test_entry;
	unsigned long type_val;

	test_entry = minimem_swp_entry(MINIMEM_SWP_PTE_MARKER_TYPE, 0);
	type_val = minimem_swp_type(test_entry);

	if (type_val != MINIMEM_SWP_PTE_MARKER_TYPE) {
		pr_warn("minimem: compat: SWP type roundtrip failed: "
			"expected %u, got %lu\n",
			MINIMEM_SWP_PTE_MARKER_TYPE, type_val);
		return false;
	}

	pr_info("minimem: compat: SWP type roundtrip OK (type=%u)\n",
		MINIMEM_SWP_PTE_MARKER_TYPE);
	return true;
}

static bool verify_pte_marker_not_collision(void)
{
	pte_t pte;
	swp_entry_t entry;
	unsigned long offset_val;

	entry = make_minimem_entry(0);
	pte = minimem_swp_entry_to_pte(entry);

	if (pte_present(pte)) {
		pr_warn("minimem: compat: PTE marker appears as present! "
			"Encoding is wrong for this kernel.\n");
		return false;
	}

	entry = minimem_pte_to_swp_entry(pte);
	if (!is_minimem_entry(entry)) {
		pr_warn("minimem: compat: PTE marker roundtrip failed! "
			"Entry does not decode as minimem.\n");
		return false;
	}

	if (minimem_entry_index(entry) != 0) {
		pr_warn("minimem: compat: PTE marker index roundtrip failed! "
			"Expected 0, got %lu\n", minimem_entry_index(entry));
		return false;
	}

	offset_val = minimem_swp_offset(entry);
	if (!(offset_val & PTE_MARKER_MINIMEM)) {
		pr_warn("minimem: compat: PTE_MARKER_MINIMEM bit not set in offset!\n");
		return false;
	}

	pr_info("minimem: compat: PTE marker roundtrip OK "
		"(type=%lu, offset=0x%lx, minimem_bit=%lu)\n",
		minimem_swp_type(entry), offset_val,
		(offset_val & PTE_MARKER_MINIMEM) >> 3);
	return true;
}

static bool verify_pte_mkwrite_signature(void)
{
	void *addr;

	addr = check_symbol("pte_mkwrite");
	if (!addr) {
		pr_warn("minimem: compat: pte_mkwrite symbol not found\n");
		return false;
	}

	pr_info("minimem: compat: pte_mkwrite found at %px (2-arg VMA variant)\n",
		addr);
	return true;
}

static bool verify_required_symbols(void)
{
	static const char * const required[] = {
		"__pte_offset_map_lock",
		"pte_mkwrite",
		"folio_add_new_anon_rmap",
		NULL,
	};
	int i;
	bool all_ok = true;

	for (i = 0; required[i]; i++) {
		void *addr = check_symbol(required[i]);
		if (addr) {
			pr_info("minimem: compat: %s resolved at %px\n",
				required[i], addr);
		} else {
			pr_warn("minimem: compat: REQUIRED symbol %s NOT FOUND\n",
				required[i]);
			all_ok = false;
		}
	}

	return all_ok;
}

static bool verify_optional_symbols(void)
{
	static const char * const optional[] = {
		"folio_add_lru_vma",
		NULL,
	};
	int i;

	for (i = 0; optional[i]; i++) {
		void *addr = check_symbol(optional[i]);
		if (addr) {
			pr_info("minimem: compat: %s resolved at %px (optional)\n",
				optional[i], addr);
		} else {
			pr_warn("minimem: compat: optional symbol %s not found "
				"(feature degraded)\n", optional[i]);
		}
	}

	return true;
}

static bool verify_kernel_patches(void)
{
	void *reg = check_symbol("minimem_register_fault_handler");
	void *unreg = check_symbol("minimem_unregister_fault_handler");
	void *zap = check_symbol("minimem_zap_cb");

	if (reg && unreg) {
		pr_info("minimem: compat: kernel patches detected "
			"(fault handler registration available)\n");
		if (zap)
			pr_info("minimem: compat: zap callback pointer found\n");
		else
			pr_warn("minimem: compat: zap callback pointer not found "
				"(process exit may leak compressed pages)\n");
		return true;
	}

	pr_info("minimem: compat: kernel patches not detected "
		"(kprobe fallback will be used, sweep pass disabled)\n");
	return false;
}

static bool verify_zsmalloc(void)
{
	struct zs_pool *test_pool;

	test_pool = zs_create_pool("minimem_compat_test");
	if (!test_pool) {
		pr_warn("minimem: compat: zs_create_pool failed! "
			"Is CONFIG_ZSMALLOC enabled?\n");
		return false;
	}

	zs_destroy_pool(test_pool);
	pr_info("minimem: compat: zsmalloc pool create/destroy OK\n");
	return true;
}

int minimem_compat_check(struct minimem_compat_result *result)
{
	memset(result, 0, sizeof(*result));
	result->fail_count = 0;

	result->kernel_major = LINUX_VERSION_CODE >> 16;
	result->kernel_minor = (LINUX_VERSION_CODE >> 8) & 0xFF;
	result->kernel_patch = LINUX_VERSION_CODE & 0xFF;

	pr_info("minimem: compat: checking kernel %u.%u.%u\n",
		result->kernel_major, result->kernel_minor, result->kernel_patch);

	/* 1. Kernel version check */
	if (result->kernel_major > MINIMEM_MIN_KERNEL_MAJOR ||
	    (result->kernel_major == MINIMEM_MIN_KERNEL_MAJOR &&
	     result->kernel_minor >= MINIMEM_MIN_KERNEL_MINOR)) {
		result->kernel_version_ok = true;
		pr_info("minimem: compat: kernel version OK "
			"(%u.%u.%u >= %u.%u)\n",
			result->kernel_major, result->kernel_minor,
			result->kernel_patch,
			MINIMEM_MIN_KERNEL_MAJOR,
			MINIMEM_MIN_KERNEL_MINOR);
	} else {
		result->kernel_version_ok = false;
		result->fail_count++;
		pr_warn("minimem: compat: kernel version TOO OLD "
			"(%u.%u.%u < %u.%u minimum)\n",
			result->kernel_major, result->kernel_minor,
			result->kernel_patch,
			MINIMEM_MIN_KERNEL_MAJOR,
			MINIMEM_MIN_KERNEL_MINOR);
	}

	/* 2. Kallsyms availability (required for all symbol resolution) */
	if (resolve_kallsyms_for_check() == 0) {
		result->kallsyms_ok = true;
		pr_info("minimem: compat: kallsyms_lookup_name available\n");
	} else {
		result->kallsyms_ok = false;
		result->fail_count++;
		pr_warn("minimem: compat: kallsyms_lookup_name NOT available — "
			"cannot resolve kernel symbols. "
			"Enable CONFIG_KALLSYMS_ALL=y in kernel config.\n");
	}

	/* 3. Kprobes availability */
	{
		struct kprobe test_kp;
		memset(&test_kp, 0, sizeof(test_kp));
		test_kp.symbol_name = "do_swap_page";
		if (register_kprobe(&test_kp) == 0) {
			result->kprobes_ok = true;
			unregister_kprobe(&test_kp);
			pr_info("minimem: compat: kprobes available (can register "
				"on do_swap_page)\n");
		} else {
			result->kprobes_ok = false;
			pr_warn("minimem: compat: kprobes NOT available — "
				"fault interception disabled. "
				"Enable CONFIG_KPROBES=y in kernel config.\n");
		}
	}

	/* 4. Required kernel symbols */
	if (result->kallsyms_ok) {
		result->required_symbols_ok = verify_required_symbols();
		verify_optional_symbols();
	} else {
		result->required_symbols_ok = false;
	}
	if (!result->required_symbols_ok)
		result->fail_count++;

	/* 5. PTE marker encoding verification */
	result->swp_type_ok = verify_swp_type();
	if (!result->swp_type_ok) {
		result->fail_count++;
	} else {
		if (!verify_pte_marker_not_collision()) {
			result->swp_type_ok = false;
			result->fail_count++;
		}
	}

	/* 6. pte_mkwrite signature */
	if (result->kallsyms_ok) {
		result->pte_mkwrite_ok = verify_pte_mkwrite_signature();
		result->pte_mkwrite_two_args = result->pte_mkwrite_ok;
	} else {
		result->pte_mkwrite_ok = false;
		result->pte_mkwrite_two_args = false;
	}
	if (!result->pte_mkwrite_ok)
		result->fail_count++;

	/* 7. Kernel patches (optional, but important) */
	if (result->kallsyms_ok)
		result->kernel_patches_ok = verify_kernel_patches();
	else
		result->kernel_patches_ok = false;

	/* 8. CONFIG_PAGE_IDLE_FLAG (optional — scanner needs it) */
#ifdef CONFIG_PAGE_IDLE_FLAG
	result->page_idle_ok = true;
	pr_info("minimem: compat: CONFIG_PAGE_IDLE_FLAG=y (scanner available)\n");
#else
	result->page_idle_ok = false;
	pr_warn("minimem: compat: CONFIG_PAGE_IDLE_FLAG not set — "
		"scanner will be disabled. "
		"Enable CONFIG_PAGE_IDLE_FLAG=y for transparent compression.\n");
#endif

	/* 9. zsmalloc availability */
	result->zsmalloc_ok = verify_zsmalloc();
	if (!result->zsmalloc_ok)
		result->fail_count++;

	/* 10. folio_add_new_anon_rmap availability */
	if (result->kallsyms_ok) {
		void *addr = check_symbol("folio_add_new_anon_rmap");
		result->folio_rmap_ok = (addr != NULL);
		if (addr) {
			pr_info("minimem: compat: folio_add_new_anon_rmap at %px\n",
				addr);
		} else {
			pr_warn("minimem: compat: folio_add_new_anon_rmap NOT FOUND — "
				"decompressed pages will not be on the LRU. "
				"Kernel 6.1+ required.\n");
			result->fail_count++;
		}
	}

	/* Detect the actual SWP type value for informational purposes */
	{
		swp_entry_t test = minimem_swp_entry(MINIMEM_SWP_PTE_MARKER_TYPE,
						      PTE_MARKER_MINIMEM);
		result->detected_swp_type = minimem_swp_type(test);
	}

	return result->fail_count == 0 ? 0 : -ENODEV;
}

void minimem_compat_report(const struct minimem_compat_result *r)
{
	pr_info("minimem: === Compatibility Report ===\n");
	pr_info("minimem: Kernel: %u.%u.%u\n",
		r->kernel_major, r->kernel_minor, r->kernel_patch);
	pr_info("minimem: Kernel version:      %s\n",
		r->kernel_version_ok ? "OK" : "FAIL (too old)");
	pr_info("minimem: Kallsyms:            %s\n",
		r->kallsyms_ok ? "OK" : "FAIL (need CONFIG_KALLSYMS_ALL)");
	pr_info("minimem: Kprobes:             %s\n",
		r->kprobes_ok ? "OK" : "WARN (need CONFIG_KPROBES)");
	pr_info("minimem: Required symbols:    %s\n",
		r->required_symbols_ok ? "OK" : "FAIL");
	pr_info("minimem: PTE marker encoding: %s (type=%u)\n",
		r->swp_type_ok ? "OK" : "FAIL", r->detected_swp_type);
	pr_info("minimem: pte_mkwrite:         %s (%s)\n",
		r->pte_mkwrite_ok ? "OK" : "FAIL",
		r->pte_mkwrite_two_args ? "2-arg VMA" : "unknown");
	pr_info("minimem: Kernel patches:       %s\n",
		r->kernel_patches_ok ? "DETECTED (full mode)" :
		"NOT DETECTED (kprobe fallback, no sweep)");
	pr_info("minimem: CONFIG_PAGE_IDLE_FLAG: %s\n",
		r->page_idle_ok ? "enabled" : "DISABLED (scanner offline)");
	pr_info("minimem: zsmalloc:            %s\n",
		r->zsmalloc_ok ? "OK" : "FAIL (need CONFIG_ZSMALLOC)");
	pr_info("minimem: folio rmap:          %s\n",
		r->folio_rmap_ok ? "OK" : "FAIL (kernel 6.1+ required)");

	if (r->fail_count == 0) {
		pr_info("minimem: === ALL CHECKS PASSED ===\n");
	} else {
		pr_warn("minimem: === %d CRITICAL CHECK(S) FAILED — "
			"module will not load ===\n", r->fail_count);
		pr_warn("minimem: Common fixes:\n");
		if (!r->kallsyms_ok)
			pr_warn("minimem:   - Enable CONFIG_KALLSYMS_ALL=y\n");
		if (!r->kprobes_ok)
			pr_warn("minimem:   - Enable CONFIG_KPROBES=y\n");
		if (!r->swp_type_ok)
			pr_warn("minimem:   - Apply kernel patches (patches/ dir) "
				"for correct PTE marker handling\n");
		if (!r->required_symbols_ok)
			pr_warn("minimem:   - Kernel too old or missing exports; "
				"kernel 6.1+ recommended\n");
		if (!r->zsmalloc_ok)
			pr_warn("minimem:   - Enable CONFIG_ZSMALLOC=y\n");
		if (!r->folio_rmap_ok)
			pr_warn("minimem:   - Upgrade to kernel 6.1+ for "
				"folio_add_new_anon_rmap\n");
	}
}