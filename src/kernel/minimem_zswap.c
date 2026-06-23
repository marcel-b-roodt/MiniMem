/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * minimem_zswap.c — zsmalloc-backed compressed page storage for MiniMem
 *
 * Manages compressed page storage in a zsmalloc pool. Each page is
 * compressed using the advisor-selected algorithm, stored in zsmalloc,
 * and tracked in the compression map (xarray).
 *
 * Uses the 6.x zsmalloc API:
 *   zs_malloc(pool, size, flags, nid)        — allocate
 *   zs_obj_write(pool, handle, buf, len)     — write compressed data
 *   zs_obj_read_begin/end(pool, handle, buf) — read compressed data
 *   zs_free(pool, handle)                    — free
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/highmem.h>
#include <linux/zsmalloc.h>
#include <linux/ktime.h>
#include <linux/cpumask.h>
#include <linux/sched.h>
#include <linux/kthread.h>
#include <linux/rmap.h>
#include <linux/swap.h>
#include <linux/pgtable.h>
#include <linux/swapops.h>

#include "minimem_zswap.h"
#include "minimem_compress.h"
#include "minimem_map.h"
#include "minimem_scanner.h"
#include "minimem_proc_stats.h"
#include "minimem_hook.h"
#include "minimem_pte.h"

static struct zs_pool *minimem_pool;
static struct minimem_map minimem_map;

static atomic64_t mm_max_pool_pages = ATOMIC64_INIT(0);
static atomic64_t mm_stored_pages;
static atomic64_t mm_total_bytes;
static atomic64_t mm_bytes_saved;
static atomic64_t mm_zap_cb_count;
static atomic64_t mm_zap_cb_miss_count;

int minimem_zswap_init(void)
{
	minimem_pool = zs_create_pool("minimem");
	if (!minimem_pool)
		return -ENOMEM;

	atomic64_set(&mm_stored_pages, 0);
	atomic64_set(&mm_total_bytes, 0);
	atomic64_set(&mm_bytes_saved, 0);
	atomic64_set(&mm_zap_cb_count, 0);
	atomic64_set(&mm_zap_cb_miss_count, 0);

	return minimem_map_init(&minimem_map);
}

void minimem_zswap_exit(void)
{
	minimem_map_remove_all(&minimem_map);
	minimem_map_destroy(&minimem_map);

	if (minimem_pool) {
		zs_destroy_pool(minimem_pool);
		minimem_pool = NULL;
	}
}

struct zs_pool *minimem_zswap_pool(void)
{
	return minimem_pool;
}

struct minimem_map *minimem_zswap_map(void)
{
	return &minimem_map;
}

int minimem_compress_and_store(unsigned long vaddr, struct page *page)
{
	struct minimem_compress_result res;
	void *src_addr, *dst_buf, *local_buf;
	unsigned long handle;
	size_t copy_len;
	int ret, nid;

	if (!minimem_pool || !page)
		return -EINVAL;

	if (atomic64_read(&mm_max_pool_pages) > 0 &&
	    atomic64_read(&mm_stored_pages) >= atomic64_read(&mm_max_pool_pages))
		return -ENOSPC;

	src_addr = kmap_local_page(page);
	if (!src_addr)
		return -EFAULT;

	preempt_disable();
	dst_buf = minimem_get_compress_buf();
	if (!dst_buf) {
		preempt_enable();
		kunmap_local(src_addr);
		return -EINVAL;
	}

	ret = minimem_compress_page(src_addr, MINIMEM_PAGE_SIZE, &res);
	if (ret != MINIMEM_OK) {
		preempt_enable();
		kunmap_local(src_addr);
		if (ret == MINIMEM_INCOMPRESSIBLE)
			return MINIMEM_INCOMPRESSIBLE;
		return ret;
	}

	if (res.compressed_size == 0 ||
	    res.compressed_size >= MINIMEM_PAGE_SIZE) {
		preempt_enable();
		kunmap_local(src_addr);
		return MINIMEM_INCOMPRESSIBLE;
	}

	{
		long min_pct = minimem_scanner_min_savings_pct();
		long savings_pct = (100L * (MINIMEM_PAGE_SIZE - res.compressed_size))
				   / MINIMEM_PAGE_SIZE;

		if (savings_pct < min_pct) {
			preempt_enable();
			kunmap_local(src_addr);
			return MINIMEM_INCOMPRESSIBLE;
		}
	}

	copy_len = res.compressed_size;

	local_buf = kmalloc(copy_len, GFP_ATOMIC);
	if (!local_buf) {
		preempt_enable();
		kunmap_local(src_addr);
		return -ENOMEM;
	}
	memcpy(local_buf, dst_buf, copy_len);
	preempt_enable();
	kunmap_local(src_addr);

	nid = page_to_nid(page);
	handle = zs_malloc(minimem_pool, copy_len, GFP_NOIO, nid);
	if (!handle) {
		kfree(local_buf);
		return -ENOMEM;
	}

	zs_obj_write(minimem_pool, handle, local_buf, copy_len);
	kfree(local_buf);

	ret = minimem_map_store(&minimem_map, vaddr, res.algo_id,
				res.compressed_size, handle);
	if (ret) {
		zs_free(minimem_pool, handle);
		return ret;
	}

	atomic64_inc(&mm_stored_pages);
	atomic64_add(res.compressed_size, &mm_total_bytes);
	atomic64_add(res.original_size - res.compressed_size,
		     &mm_bytes_saved);

	minimem_proc_stats_compress(res.original_size - res.compressed_size,
				    res.compress_ns);

	return MINIMEM_OK;
}

int minimem_decompress_and_restore(unsigned long vaddr, struct page *page)
{
	struct minimem_map_entry entry;
	struct minimem_decompress_result dres;
	void *local_buf, *local_addr, *dst_addr;
	unsigned long handle;
	int ret;

	if (!minimem_pool)
		return -EINVAL;

	ret = minimem_map_lookup(&minimem_map, vaddr, &entry);
	if (ret)
		return ret;

	handle = entry.zs_handle;

	local_buf = kmalloc(entry.compressed_len, GFP_KERNEL);
	if (!local_buf)
		return -ENOMEM;

	local_addr = zs_obj_read_begin(minimem_pool, handle, local_buf);
	if (!local_addr) {
		kfree(local_buf);
		return -EIO;
	}

	if (page) {
		dst_addr = kmap_local_page(page);
		if (!dst_addr) {
			zs_obj_read_end(minimem_pool, handle, local_addr);
			kfree(local_buf);
			return -EFAULT;
		}
	} else {
		preempt_disable();
		dst_addr = minimem_get_decompress_buf();
	}

	ret = minimem_decompress_page(local_addr, entry.compressed_len,
				      entry.algo_id, dst_addr,
				      MINIMEM_PAGE_SIZE, &dres);

	if (page)
		kunmap_local(dst_addr);
	else
		preempt_enable();

	zs_obj_read_end(minimem_pool, handle, local_addr);
	kfree(local_buf);

	if (ret != MINIMEM_OK)
		return ret;

	minimem_proc_stats_decompress(dres.decompress_ns);

	minimem_map_remove(&minimem_map, vaddr);
	zs_free(minimem_pool, handle);

	atomic64_dec(&mm_stored_pages);
	atomic64_sub(entry.compressed_len, &mm_total_bytes);

	return MINIMEM_OK;
}

int minimem_zswap_remove(unsigned long vaddr)
{
	struct minimem_map_entry entry;
	int ret;

	ret = minimem_map_lookup(&minimem_map, vaddr, &entry);
	if (ret)
		return ret;

	ret = minimem_map_remove(&minimem_map, vaddr);
	if (ret == 0) {
		zs_free(minimem_pool, entry.zs_handle);
		atomic64_dec(&mm_stored_pages);
		atomic64_sub(entry.compressed_len, &mm_total_bytes);
	}

	return ret;
}

unsigned long minimem_zswap_total_bytes(void)
{
	return (unsigned long)atomic64_read(&mm_total_bytes);
}

unsigned long minimem_zswap_stored_pages(void)
{
	return (unsigned long)atomic64_read(&mm_stored_pages);
}

unsigned long minimem_zswap_bytes_saved(void)
{
	return (unsigned long)atomic64_read(&mm_bytes_saved);
}

unsigned long minimem_zswap_max_pool_pages(void)
{
	return (unsigned long)atomic64_read(&mm_max_pool_pages);
}

void minimem_zswap_set_max_pool_pages(unsigned long max)
{
	atomic64_set(&mm_max_pool_pages, (s64)max);
}

unsigned long minimem_zswap_zap_cb_count(void)
{
	return (unsigned long)atomic64_read(&mm_zap_cb_count);
}

unsigned long minimem_zswap_zap_cb_miss_count(void)
{
	return (unsigned long)atomic64_read(&mm_zap_cb_miss_count);
}

/*
 * Decompress all stored pages and restore their PTEs to present.
 * Walks all processes' page tables, finds MiniMem PTE markers,
 * decompresses the corresponding data, allocates new pages,
 * and installs present PTEs.
 *
 * Called during module unload to ensure no data is lost.
 */
long minimem_zswap_drain_and_restore(void)
{
	struct task_struct *task;
	unsigned long restored = 0;
	unsigned long failed = 0;
	long stored = atomic64_read(&minimem_map.count);

	if (stored == 0)
		return 0;

	pr_info("minimem: drain_and_restore — %ld compressed pages to restore\n",
		stored);

	rcu_read_lock();
	for_each_process(task) {
		struct mm_struct *mm;
		struct vm_area_struct *vma;
		unsigned long vaddr;

		mm = get_task_mm(task);
		if (!mm)
			continue;

		if (!mmget_not_zero(mm)) {
			mmput(mm);
			continue;
		}

		if (!mmap_read_trylock(mm)) {
			mmput(mm);
			continue;
		}

		VMA_ITERATOR(vmi, mm, 0);
		for_each_vma(vmi, vma) {
			pgd_t *pgd;
			p4d_t *p4d;
			pud_t *pud;
			pmd_t *pmd;
			pte_t *ptep, old_pte, new_pte;
			spinlock_t *ptl;

			if (!vma)
				break;
			if (!vma_is_anonymous(vma))
				continue;
			if (!vma->vm_start || !vma->vm_end)
				continue;
			if (vma->vm_flags & (VM_PFNMAP | VM_IO))
				continue;

			for (vaddr = vma->vm_start; vaddr < vma->vm_end;
			     vaddr += PAGE_SIZE) {
				struct minimem_map_entry entry;
				struct page *new_page;
				void *src_buf, *zs_addr, *dst_addr;
				struct minimem_decompress_result dres;
				int ret;

				pgd = pgd_offset(mm, vaddr);
				if (pgd_none(*pgd) || pgd_bad(*pgd))
					continue;

				p4d = p4d_offset(pgd, vaddr);
				if (p4d_none(*p4d) || p4d_bad(*p4d))
					continue;

				pud = pud_offset(p4d, vaddr);
				if (pud_none(*pud) || pud_bad(*pud))
					continue;

				pmd = pmd_offset(pud, vaddr);
				if (pmd_none(*pmd) || pmd_trans_huge(*pmd))
					continue;

				ptep = minimem_pte_offset_map_lock(mm, pmd,
								    vaddr,
								    &ptl);
				if (!ptep)
					continue;

				old_pte = *ptep;

				if (!is_minimem_pte(old_pte)) {
					minimem_pte_unmap_unlock(ptep, ptl);
					continue;
				}

				minimem_pte_unmap_unlock(ptep, ptl);

				ret = minimem_map_lookup(&minimem_map, vaddr,
							&entry);
				if (ret) {
					failed++;
					continue;
				}

				new_page = alloc_page_vma(GFP_HIGHUSER_MOVABLE,
							  vma, vaddr);
				if (!new_page) {
					failed++;
					continue;
				}

				src_buf = kmalloc(entry.compressed_len,
						  GFP_KERNEL);
				if (!src_buf) {
					__free_page(new_page);
					failed++;
					continue;
				}

				zs_addr = zs_obj_read_begin(minimem_pool, entry.zs_handle,
						  src_buf);
				if (!zs_addr) {
					kfree(src_buf);
					__free_page(new_page);
					failed++;
					continue;
				}

				dst_addr = kmap_local_page(new_page);
				ret = minimem_decompress_page(zs_addr,
							      entry.compressed_len,
							      entry.algo_id,
							      dst_addr,
							      MINIMEM_PAGE_SIZE,
							      &dres);
				kunmap_local(dst_addr);

				zs_obj_read_end(minimem_pool, entry.zs_handle,
						zs_addr);
				kfree(src_buf);

				if (ret != MINIMEM_OK) {
					__free_page(new_page);
					minimem_map_remove(&minimem_map, vaddr);
					zs_free(minimem_pool, entry.zs_handle);
					atomic64_dec(&mm_stored_pages);
					atomic64_sub(entry.compressed_len,
						     &mm_total_bytes);
					failed++;
					continue;
				}

				new_pte = minimem_mk_pte(new_page,
							 vma->vm_page_prot);
				if (vma->vm_flags & VM_WRITE)
					new_pte = minimem_pte_mkwrite(new_pte, vma);
				new_pte = pte_mkyoung(new_pte);

				ptep = minimem_pte_offset_map_lock(mm, pmd,
								    vaddr,
								    &ptl);
				if (ptep) {
					if (pte_same(*ptep, old_pte)) {
						minimem_set_pte_at(mm, vaddr,
								   ptep,
								   new_pte);
						percpu_counter_add(
							&mm->rss_stat[MM_ANONPAGES],
							1);
						minimem_folio_add_new_anon_rmap(
							page_folio(new_page),
							vma, vaddr);
						minimem_folio_add_lru_vma(
							page_folio(new_page),
							vma);
					} else {
						__free_page(new_page);
					}
					minimem_pte_unmap_unlock(ptep, ptl);
				} else {
					__free_page(new_page);
				}

				minimem_map_remove(&minimem_map, vaddr);
				zs_free(minimem_pool, entry.zs_handle);
				atomic64_dec(&mm_stored_pages);
				atomic64_sub(entry.compressed_len,
					     &mm_total_bytes);

				restored++;
			}
		}

		mmap_read_unlock(mm);
		mmput(mm);
	}
	rcu_read_unlock();

	pr_info("minimem: drain_and_restore — %lu pages restored, %lu failed\n",
		restored, failed);

	return restored;
}

void minimem_zswap_zap_cb(struct vm_area_struct *vma,
			  unsigned long addr, swp_entry_t entry)
{
	unsigned long vaddr = addr & PAGE_MASK;
	struct minimem_map_entry map_entry;
	int ret;
	static bool first_call = true;

	atomic64_inc(&mm_zap_cb_count);

	if (first_call) {
		first_call = false;
		pr_info("minimem: zap_cb first invocation addr=0x%lx "
			"entry.val=0x%lx swp_type=%lu offset=0x%lx\n",
			addr, entry.val, (unsigned long)swp_type(entry),
			(unsigned long)swp_offset(entry));
	}

	ret = minimem_map_lookup(&minimem_map, vaddr, &map_entry);
	if (ret) {
		atomic64_inc(&mm_zap_cb_miss_count);
		pr_debug("minimem: zap_cb: map_lookup failed for vaddr=0x%lx\n", vaddr);
		return;
	}

	minimem_map_remove(&minimem_map, vaddr);
	zs_free(minimem_pool, map_entry.zs_handle);
	atomic64_dec(&mm_stored_pages);
	atomic64_sub(map_entry.compressed_len, &mm_total_bytes);
}