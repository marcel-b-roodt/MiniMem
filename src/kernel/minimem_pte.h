/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * minimem_pte.h — PTE marker encoding for MiniMem compressed pages
 *
 * On x86-64, a swap entry is encoded as:
 *   Bits 59-63: type (5 bits)
 *   Bits 0-58:  offset (59 bits, but actual usable depends on PROTNONE)
 *
 * The offset is bit-inverted and shifted for sign extension:
 *   __swp_entry(type, offset) = { (~(offset) << SWP_OFFSET_SHIFT >> SWP_TYPE_BITS)
 *                                  | (type << (64 - SWP_TYPE_BITS)) }
 *   __swp_type(entry) = entry.val >> (64 - SWP_TYPE_BITS)
 *   __swp_offset(entry) = ~(entry.val) << SWP_TYPE_BITS >> SWP_OFFSET_SHIFT
 *
 * SWP_PTE_MARKER = type 31 (the last type value).
 * PTE markers use the offset field for flags in the low 3 bits:
 *   BIT(0) = UFFD_WP
 *   BIT(1) = POISONED
 *   BIT(2) = GUARD
 *
 * MiniMem defines PTE_MARKER_MINIMEM = BIT(3) in the offset field,
 * storing the compression map index in bits 4+.
 *
 * We provide self-contained implementations of swp_entry()/swp_type()/
 * swp_offset()/pte_to_swp_entry()/swp_entry_to_pte() to avoid
 * depending on <linux/swapops.h> which has conditional definitions
 * that may not be available to out-of-tree modules.
 */

#ifndef MINIMEM_KERNEL_PTE_H
#define MINIMEM_KERNEL_PTE_H

#include <linux/types.h>
#include <asm/pgtable.h>

/*
 * x86-64 swap entry layout constants.
 * These MUST match the kernel's definitions in arch/x86/include/asm/pgtable_64.h
 */
#define MINIMEM_SWP_TYPE_BITS		5
#define MINIMEM_SWP_TYPE_SHIFT		(64 - MINIMEM_SWP_TYPE_BITS)
#define MINIMEM_SWP_OFFSET_SHIFT	14  /* _PAGE_BIT_PROTNONE + 1 + SWP_TYPE_BITS = 9 + 5 = 14 */

/*
 * SWP_PTE_MARKER type value = 31 on standard x86-64 configs.
 * This is the last value in the 5-bit type field.
 */
#define MINIMEM_SWP_PTE_MARKER_TYPE	31

/*
 * PTE marker bit for MiniMem. Must not conflict with:
 *   PTE_MARKER_UFFD_WP = BIT(0)
 *   PTE_MARKER_POISONED = BIT(1)
 *   PTE_MARKER_GUARD    = BIT(2)
 */
#define PTE_MARKER_MINIMEM		BIT(3)

/*
 * Shift for the MiniMem map index within the swp_offset field.
 * Bits 0-2: PTE marker flags
 * Bit 3: PTE_MARKER_MINIMEM
 * Bits 4+: map index
 */
#define MINIMEM_INDEX_SHIFT		4

/*
 * Self-contained swap entry operations for x86-64.
 * These replicate the kernel's __swp_entry, __swp_type, __swp_offset
 * from arch/x86/include/asm/pgtable_64.h.
 */
static inline unsigned long minimem_swp_type(swp_entry_t entry)
{
	return entry.val >> MINIMEM_SWP_TYPE_SHIFT;
}

static inline unsigned long minimem_swp_offset(swp_entry_t entry)
{
	return (~entry.val << MINIMEM_SWP_TYPE_BITS) >> MINIMEM_SWP_OFFSET_SHIFT;
}

static inline swp_entry_t minimem_swp_entry(unsigned long type,
					     unsigned long offset)
{
	swp_entry_t e;
	e.val = ((~offset << MINIMEM_SWP_OFFSET_SHIFT) >> MINIMEM_SWP_TYPE_BITS)
		| (type << MINIMEM_SWP_TYPE_SHIFT);
	return e;
}

static inline swp_entry_t minimem_pte_to_swp_entry(pte_t pte)
{
	swp_entry_t e;
	e.val = pte_val(pte);
	return e;
}

static inline pte_t minimem_swp_entry_to_pte(swp_entry_t entry)
{
	return __pte(entry.val);
}

/*
 * Construct a MiniMem swap entry from a compression map index.
 * Uses SWP_PTE_MARKER type (31) with PTE_MARKER_MINIMEM flag.
 */
static inline swp_entry_t make_minimem_entry(unsigned long map_index)
{
	unsigned long offset = PTE_MARKER_MINIMEM | (map_index << MINIMEM_INDEX_SHIFT);
	return minimem_swp_entry(MINIMEM_SWP_PTE_MARKER_TYPE, offset);
}

/*
 * Check whether a swap entry is a MiniMem compressed page marker.
 */
static inline bool is_minimem_entry(swp_entry_t entry)
{
	if (minimem_swp_type(entry) != MINIMEM_SWP_PTE_MARKER_TYPE)
		return false;
	return (minimem_swp_offset(entry) & PTE_MARKER_MINIMEM) != 0;
}

/*
 * Extract the compression map index from a MiniMem swap entry.
 */
static inline unsigned long minimem_entry_index(swp_entry_t entry)
{
	return minimem_swp_offset(entry) >> MINIMEM_INDEX_SHIFT;
}

/*
 * Check whether a PTE contains a MiniMem compressed page marker.
 * A non-present PTE that encodes a MiniMem swap entry indicates
 * a compressed page that should be decompressed on fault.
 */
static inline bool is_minimem_pte(pte_t pte)
{
	swp_entry_t entry;

	if (pte_present(pte))
		return false;

	entry = minimem_pte_to_swp_entry(pte);
	return is_minimem_entry(entry);
}

/*
 * Extract the MiniMem map index from a PTE.
 */
static inline unsigned long minimem_pte_index(pte_t pte)
{
	return minimem_entry_index(minimem_pte_to_swp_entry(pte));
}

/*
 * Convert a MiniMem swap entry to a PTE.
 */
static inline pte_t minimem_entry_to_pte(swp_entry_t entry)
{
	return minimem_swp_entry_to_pte(entry);
}

/*
 * Convert a virtual address to a compression map index.
 * The map uses (vaddr >> PAGE_SHIFT) as the xarray key.
 */
static inline unsigned long minimem_vaddr_to_index(unsigned long vaddr)
{
	return vaddr >> PAGE_SHIFT;
}

/*
 * Convert a compression map index back to a page-aligned virtual address.
 */
static inline unsigned long minimem_index_to_vaddr(unsigned long index)
{
	return index << PAGE_SHIFT;
}

#endif /* MINIMEM_KERNEL_PTE_H */