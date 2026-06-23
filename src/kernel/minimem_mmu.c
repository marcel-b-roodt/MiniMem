/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * minimem_mmu.c — MMU notifier for MiniMem process exit cleanup
 *
 * On stock kernels (without CONFIG_MINIMEM), the kernel's zap_pte_range()
 * doesn't know how to handle PTE_MARKER_MINIMEM entries. When a process
 * exits with compressed pages, the zsmalloc allocations would leak and
 * "unrecognized swap entry" warnings would appear.
 *
 * This module registers an MMU notifier for each process that has compressed
 * pages. The release callback (process exit) walks the page tables, finds
 * MiniMem PTE markers, clears them, and frees the zsmalloc allocations.
 *
 * Registration is deferred via a workqueue because mmu_notifier_get()
 * internally takes mmap_write_lock, which would deadlock if called while
 * holding mmap_read_lock (as the scanner does).
 *
 * On kernels with CONFIG_MINIMEM, the kernel's zap_pte_range() already calls
 * minimem_zap_cb, so this MMU notifier is redundant but harmless.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/mmu_notifier.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/rwsem.h>

#include "minimem_hook.h"
#include "minimem_pte.h"
#include "minimem_map.h"
#include "minimem_zswap.h"
#include "minimem_mmu.h"

struct minimem_mmu_sub {
	struct mmu_notifier notifier;
	struct mm_struct *mm;
	struct list_head list;
};

static atomic64_t mmu_release_count;
static atomic64_t mmu_release_pages;
static atomic64_t mmu_invalidate_count;
static bool minimem_mmu_exiting;

static struct workqueue_struct *minimem_mmu_wq;
static struct work_struct minimem_mmu_work;
static struct delayed_work minimem_mmu_dwork;
static DEFINE_XARRAY(minimem_pending_mms);
static LIST_HEAD(minimem_registered_subs);
static DEFINE_SPINLOCK(minimem_mmu_lock);

/*
 * Walk page tables in the given range, find MiniMem PTE markers,
 * clear them from the page table, collect their addresses,
 * and free the corresponding zsmalloc allocations.
 *
 * Called from MMU notifier release callback (process exit).
 * mmap_write_lock is held by exit_mmap, so VMA iteration is safe.
 * We release the PTL before calling minimem_zswap_remove() because
 * it may sleep (rwsem write lock in minimem_map_remove). We collect
 * addresses under PTL, clear PTEs under PTL, then remove from the
 * map outside the PTL.
 *
 * Returns the number of MiniMem entries freed.
 */
static unsigned long minimem_zap_range(struct mm_struct *mm,
				       unsigned long start,
				       unsigned long end)
{
	unsigned long addr, freed = 0;
	struct vm_area_struct *vma;
	unsigned long *vaddrs;
	int vaddr_count = 0, vaddr_cap = 256;
	int i;

	vaddrs = kmalloc_array(vaddr_cap, sizeof(unsigned long), GFP_KERNEL);
	if (!vaddrs)
		return 0;

	VMA_ITERATOR(vmi, mm, start);
	for_each_vma(vmi, vma) {
		pgd_t *pgd;
		p4d_t *p4d;
		pud_t *pud;
		pmd_t *pmd;
		pte_t *ptep;
		spinlock_t *ptl;
		unsigned long vma_end;

		if (vma->vm_start >= end)
			break;

		if (vma->vm_flags & (VM_SHARED | VM_IO | VM_PFNMAP))
			continue;

		vma_end = min(vma->vm_end, end);
		addr = max(vma->vm_start, start);

		for (; addr < vma_end; addr += PAGE_SIZE) {
			pgd = pgd_offset(mm, addr);
			if (pgd_none(*pgd) || pgd_bad(*pgd))
				continue;

			p4d = p4d_offset(pgd, addr);
			if (p4d_none(*p4d) || p4d_bad(*p4d))
				continue;

			pud = pud_offset(p4d, addr);
			if (pud_none(*pud) || pud_bad(*pud))
				continue;

			pmd = pmd_offset(pud, addr);
			if (pmd_none(*pmd) || pmd_trans_huge(*pmd))
				continue;

			ptep = minimem_pte_offset_map_lock(mm, pmd, addr, &ptl);
			if (!ptep)
				continue;

			if (!is_minimem_pte(*ptep)) {
				minimem_pte_unmap_unlock(ptep, ptl);
				continue;
			}

			pte_clear(mm, addr, ptep);
			minimem_pte_unmap_unlock(ptep, ptl);

			if (vaddr_count >= vaddr_cap) {
				unsigned long *new_arr;
				vaddr_cap *= 2;
				new_arr = krealloc(vaddrs,
						   vaddr_cap * sizeof(unsigned long),
						   GFP_KERNEL);
				if (!new_arr)
					goto remove;
				vaddrs = new_arr;
			}
			vaddrs[vaddr_count++] = addr & PAGE_MASK;
		}
	}

remove:
	for (i = 0; i < vaddr_count; i++) {
		if (minimem_zswap_remove(vaddrs[i]) == 0)
			freed++;
	}

	kfree(vaddrs);
	return freed;
}

static struct mmu_notifier *minimem_mmu_alloc_notifier(struct mm_struct *mm)
{
	struct minimem_mmu_sub *sub;

	sub = kzalloc(sizeof(*sub), GFP_KERNEL);
	if (!sub)
		return ERR_PTR(-ENOMEM);

	sub->mm = mm;

	spin_lock(&minimem_mmu_lock);
	list_add(&sub->list, &minimem_registered_subs);
	spin_unlock(&minimem_mmu_lock);

	return &sub->notifier;
}

static void minimem_mmu_free_notifier(struct mmu_notifier *subscription)
{
	struct minimem_mmu_sub *sub;

	sub = container_of(subscription, struct minimem_mmu_sub, notifier);

	spin_lock(&minimem_mmu_lock);
	list_del(&sub->list);
	spin_unlock(&minimem_mmu_lock);

	kfree(sub);
}

static void minimem_mmu_release(struct mmu_notifier *subscription,
				struct mm_struct *mm)
{
	unsigned long freed;

	if (minimem_mmu_exiting)
		goto put;

	pr_debug("minimem: mmu_release for mm=%p\n", mm);

	freed = minimem_zap_range(mm, 0, ULONG_MAX);
	atomic64_inc(&mmu_release_count);
	atomic64_add(freed, &mmu_release_pages);
	if (freed)
		pr_info("minimem: mmu_release freed %lu compressed pages\n",
			freed);

put:

	mmu_notifier_put(subscription);
}

static int minimem_mmu_invalidate_range_start(struct mmu_notifier *subscription,
					      const struct mmu_notifier_range *range)
{
	return 0;
}

static const struct mmu_notifier_ops minimem_mmu_ops = {
	.alloc_notifier = minimem_mmu_alloc_notifier,
	.free_notifier = minimem_mmu_free_notifier,
	.release = minimem_mmu_release,
	.invalidate_range_start = minimem_mmu_invalidate_range_start,
};

int minimem_mmu_register(struct mm_struct *mm)
{
	struct mmu_notifier *mn;

	if (!mm)
		return -EINVAL;

	mn = mmu_notifier_get(&minimem_mmu_ops, mm);
	if (IS_ERR(mn))
		return PTR_ERR(mn);

	return 0;
}

/*
 * Register MMU notifier with mmap_write_lock already held.
 * Calls mmu_notifier_get_locked internally.
 */
int minimem_mmu_register_locked(struct mm_struct *mm)
{
	struct mmu_notifier *mn;

	if (!mm)
		return -EINVAL;

	mn = mmu_notifier_get_locked(&minimem_mmu_ops, mm);
	if (IS_ERR(mn))
		return PTR_ERR(mn);

	return 0;
}

static void minimem_mmu_register_work_impl(void)
{
	struct mm_struct *mm;
	unsigned long index;
	unsigned long *entries = NULL;
	int count = 0, cap = 32;
	int i;

	entries = kmalloc_array(cap, sizeof(unsigned long), GFP_KERNEL);
	if (!entries)
		return;

	spin_lock(&minimem_mmu_lock);
	xa_for_each(&minimem_pending_mms, index, mm) {
		if (count >= cap) {
			unsigned long *new_arr;
			cap *= 2;
			new_arr = krealloc(entries,
					   cap * sizeof(unsigned long),
					   GFP_ATOMIC);
			if (!new_arr)
				break;
			entries = new_arr;
		}
		entries[count++] = (unsigned long)mm;
		xa_erase(&minimem_pending_mms, index);
	}
	spin_unlock(&minimem_mmu_lock);

	for (i = 0; i < count; i++) {
		mm = (struct mm_struct *)entries[i];
		/*
		 * We hold an mmget reference from minimem_mmu_register_deferred.
		 * Use down_write_trylock to avoid blocking if the mm is in use.
		 * If we can't get the write lock, re-queue for later.
		 */
		if (!down_write_trylock(&mm->mmap_lock)) {
			spin_lock(&minimem_mmu_lock);
			xa_store(&minimem_pending_mms, (unsigned long)mm,
				 mm, GFP_ATOMIC);
			spin_unlock(&minimem_mmu_lock);
			if (minimem_mmu_wq)
				queue_delayed_work(minimem_mmu_wq,
						   &minimem_mmu_dwork,
						   msecs_to_jiffies(50));
			continue;
		}

		minimem_mmu_register_locked(mm);
		up_write(&mm->mmap_lock);
		mmput(mm);
	}

	kfree(entries);
}

static void minimem_mmu_register_work_func(struct work_struct *work)
{
	minimem_mmu_register_work_impl();
}

static void minimem_mmu_register_dwork_func(struct work_struct *work)
{
	minimem_mmu_register_work_impl();
}

void minimem_mmu_register_deferred(struct mm_struct *mm)
{
	unsigned long index = (unsigned long)mm;

	if (!mm)
		return;

	if (!mmget_not_zero(mm))
		return;

	spin_lock(&minimem_mmu_lock);
	xa_store(&minimem_pending_mms, index, mm, GFP_ATOMIC);
	spin_unlock(&minimem_mmu_lock);

	if (minimem_mmu_wq)
		queue_work(minimem_mmu_wq, &minimem_mmu_work);
}

int minimem_mmu_init(void)
{
	minimem_mmu_wq = alloc_workqueue("minimem_mmu_wq", 0, 1);
	if (!minimem_mmu_wq)
		return -ENOMEM;

	INIT_WORK(&minimem_mmu_work, minimem_mmu_register_work_func);
	INIT_DELAYED_WORK(&minimem_mmu_dwork, minimem_mmu_register_dwork_func);
	xa_init(&minimem_pending_mms);
	INIT_LIST_HEAD(&minimem_registered_subs);
	return 0;
}

void minimem_mmu_exit(void)
{
	struct minimem_mmu_sub *sub, *tmp;
	LIST_HEAD(to_release);

	minimem_mmu_exiting = true;

	if (minimem_mmu_wq) {
		drain_workqueue(minimem_mmu_wq);
		flush_workqueue(minimem_mmu_wq);
		destroy_workqueue(minimem_mmu_wq);
		minimem_mmu_wq = NULL;
	}

	xa_destroy(&minimem_pending_mms);

	/*
	 * Release all MMU notifiers for live processes. Without this,
	 * the kernel retains dangling pointers to minimem_mmu_ops after
	 * module unload, causing oops when a process triggers
	 * invalidate_range_start (e.g., via munmap or page fault).
	 *
	 * We collect all notifier subs from our linked list, then
	 * call mmu_notifier_put() for each to release our reference.
	 * The alloc_notifier callback adds to the list, and the
	 * free_notifier callback removes from the list, so this list
	 * contains exactly the notifiers we've registered.
	 */
	spin_lock(&minimem_mmu_lock);
	list_splice_init(&minimem_registered_subs, &to_release);
	spin_unlock(&minimem_mmu_lock);

	list_for_each_entry_safe(sub, tmp, &to_release, list) {
		unsigned int users = READ_ONCE(sub->notifier.users);

		while (users--)
			mmu_notifier_put(&sub->notifier);
	}

	mmu_notifier_synchronize();
}

unsigned long minimem_mmu_release_count(void)
{
	return (unsigned long)atomic64_read(&mmu_release_count);
}

unsigned long minimem_mmu_release_pages(void)
{
	return (unsigned long)atomic64_read(&mmu_release_pages);
}

unsigned long minimem_mmu_invalidate_count(void)
{
	return (unsigned long)atomic64_read(&mmu_invalidate_count);
}