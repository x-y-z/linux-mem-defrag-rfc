// SPDX-License-Identifier: GPL-2.0
#include <linux/mm.h>
#include <linux/rmap.h>
#include <linux/hugetlb.h>
#include <linux/swap.h>
#include <linux/swapops.h>

#include "internal.h"

static inline bool not_found(struct page_vma_mapped_walk *pvmw)
{
	page_vma_mapped_walk_done(pvmw);
	return false;
}

static bool map_pte(struct page_vma_mapped_walk *pvmw)
{
	pvmw->pte = pte_offset_map(pvmw->pmd, pvmw->address);
	if (!(pvmw->flags & PVMW_SYNC)) {
		if (pvmw->flags & PVMW_MIGRATION) {
			if (!is_swap_pte(*pvmw->pte))
				return false;
		} else {
			/*
			 * We get here when we are trying to unmap a private
			 * device page from the process address space. Such
			 * page is not CPU accessible and thus is mapped as
			 * a special swap entry, nonetheless it still does
			 * count as a valid regular mapping for the page (and
			 * is accounted as such in page maps count).
			 *
			 * So handle this special case as if it was a normal
			 * page mapping ie lock CPU page table and returns
			 * true.
			 *
			 * For more details on device private memory see HMM
			 * (include/linux/hmm.h or mm/hmm.c).
			 */
			if (is_swap_pte(*pvmw->pte)) {
				swp_entry_t entry;

				/* Handle un-addressable ZONE_DEVICE memory */
				entry = pte_to_swp_entry(*pvmw->pte);
				if (!is_device_private_entry(entry))
					return false;
			} else if (!pte_present(*pvmw->pte))
				return false;
		}
	}
	pvmw->ptl = pte_lockptr(pvmw->vma->vm_mm, pvmw->pmd);
	spin_lock(pvmw->ptl);
	return true;
}

static bool map_pmd(struct page_vma_mapped_walk *pvmw)
{
	pmd_t pmde;

	pvmw->pmd = pmd_offset(pvmw->pud, pvmw->address);
	pmde = READ_ONCE(*pvmw->pmd);
	if (pmd_trans_huge(pmde) || is_pmd_migration_entry(pmde)) {
		pvmw->ptl = pmd_lock(pvmw->vma->vm_mm, pvmw->pmd);
		return true;
	} else if (!pmd_present(pmde))
		return false;

	pvmw->ptl = pmd_lock(pvmw->vma->vm_mm, pvmw->pmd);
	return true;
}

static inline bool pfn_is_match(struct page *page, unsigned long pfn)
{
	unsigned long page_pfn = page_to_pfn(page);

	/* normal page and hugetlbfs page */
	if (!PageTransCompound(page) || PageHuge(page))
		return page_pfn == pfn;

	/* THP can be referenced by any subpage */
	return pfn >= page_pfn && pfn - page_pfn < hpage_nr_pages(page);
}

/**
 * check_pte - check if @pvmw->page is mapped at the @pvmw->pte
 *
 * page_vma_mapped_walk() found a place where @pvmw->page is *potentially*
 * mapped. check_pte() has to validate this.
 *
 * @pvmw->pte may point to empty PTE, swap PTE or PTE pointing to arbitrary
 * page.
 *
 * If PVMW_MIGRATION flag is set, returns true if @pvmw->pte contains migration
 * entry that points to @pvmw->page or any subpage in case of THP.
 *
 * If PVMW_MIGRATION flag is not set, returns true if @pvmw->pte points to
 * @pvmw->page or any subpage in case of THP.
 *
 * Otherwise, return false.
 *
 */
static bool check_pte(struct page_vma_mapped_walk *pvmw)
{
	unsigned long pfn;

	if (pvmw->flags & PVMW_MIGRATION) {
		swp_entry_t entry;
		if (!is_swap_pte(*pvmw->pte))
			return false;
		entry = pte_to_swp_entry(*pvmw->pte);

		if (!is_migration_entry(entry))
			return false;

		pfn = migration_entry_to_pfn(entry);
	} else if (is_swap_pte(*pvmw->pte)) {
		swp_entry_t entry;

		/* Handle un-addressable ZONE_DEVICE memory */
		entry = pte_to_swp_entry(*pvmw->pte);
		if (!is_device_private_entry(entry))
			return false;

		pfn = device_private_entry_to_pfn(entry);
	} else {
		if (!pte_present(*pvmw->pte))
			return false;

		pfn = pte_pfn(*pvmw->pte);
	}

	return pfn_is_match(pvmw->page, pfn);
}

/* 0: not mapped, 1: pmd_page, 2: pmd  */
static int check_pmd(struct page_vma_mapped_walk *pvmw)
{
	unsigned long pfn;

	if (likely(pmd_trans_huge(*pvmw->pmd))) {
		if (pvmw->flags & PVMW_MIGRATION)
			return 0;
		pfn = pmd_pfn(*pvmw->pmd);
		if (!pfn_is_match(pvmw->page, pfn))
			return 0;
		return 1;
	} else if (!pmd_present(*pvmw->pmd)) {
		if (thp_migration_supported()) {
			if (!(pvmw->flags & PVMW_MIGRATION))
				return 0;
			if (is_migration_entry(pmd_to_swp_entry(*pvmw->pmd))) {
				swp_entry_t entry = pmd_to_swp_entry(*pvmw->pmd);

				pfn = migration_entry_to_pfn(entry);
				if (!pfn_is_match(pvmw->page, pfn))
					return 0;
				return 1;
			}
		}
		return 0;
	}
	/* THP pmd was split under us: handle on pte level */
	spin_unlock(pvmw->ptl);
	pvmw->ptl = NULL;
	return 2;
}
/**
 * page_vma_mapped_walk - check if @pvmw->page is mapped in @pvmw->vma at
 * @pvmw->address
 * @pvmw: pointer to struct page_vma_mapped_walk. page, vma, address and flags
 * must be set. pmd, pte and ptl must be NULL.
 *
 * Returns true if the page is mapped in the vma. @pvmw->pmd and @pvmw->pte point
 * to relevant page table entries. @pvmw->ptl is locked. @pvmw->address is
 * adjusted if needed (for PTE-mapped THPs).
 *
 * If @pvmw->pmd is set but @pvmw->pte is not, you have found PMD-mapped page
 * (usually THP). For PTE-mapped THP, you should run page_vma_mapped_walk() in
 * a loop to find all PTEs that map the THP.
 *
 * For HugeTLB pages, @pvmw->pte is set to the relevant page table entry
 * regardless of which page table level the page is mapped at. @pvmw->pmd is
 * NULL.
 *
 * Retruns false if there are no more page table entries for the page in
 * the vma. @pvmw->ptl is unlocked and @pvmw->pte is unmapped.
 *
 * If you need to stop the walk before page_vma_mapped_walk() returned false,
 * use page_vma_mapped_walk_done(). It will do the housekeeping.
 */
bool page_vma_mapped_walk(struct page_vma_mapped_walk *pvmw)
{
	struct mm_struct *mm = pvmw->vma->vm_mm;
	struct page *page = pvmw->page;
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t pude;
	int pmd_res;

	if (!pvmw->pte && !pvmw->pmd && pvmw->pud)
		return not_found(pvmw);

	/* The only possible pmd mapping has been handled on last iteration */
	if (pvmw->pmd && !pvmw->pte)
		goto next_pmd;

	if (pvmw->pte)
		goto next_pte;

	if (unlikely(PageHuge(pvmw->page))) {
		/* when pud is not present, pte will be NULL */
		pvmw->pte = huge_pte_offset(mm, pvmw->address, page_size(page));
		if (!pvmw->pte)
			return false;

		pvmw->ptl = huge_pte_lockptr(page_hstate(page), mm, pvmw->pte);
		spin_lock(pvmw->ptl);
		if (!check_pte(pvmw))
			return not_found(pvmw);
		return true;
	}
restart:
	pgd = pgd_offset(mm, pvmw->address);
	if (!pgd_present(*pgd))
		return false;
	p4d = p4d_offset(pgd, pvmw->address);
	if (!p4d_present(*p4d))
		return false;
	pvmw->pud = pud_offset(p4d, pvmw->address);

	/*
	 * Make sure the pud value isn't cached in a register by the
	 * compiler and used as a stale value after we've observed a
	 * subsequent update.
	 */
	pude = READ_ONCE(*pvmw->pud);
	if (pud_trans_huge(pude)) {
		pvmw->ptl = pud_lock(mm, pvmw->pud);
		if (likely(pud_trans_huge(*pvmw->pud))) {
			if (pvmw->flags & PVMW_MIGRATION)
				return not_found(pvmw);
			if (pud_page(*pvmw->pud) != page)
				return not_found(pvmw);
			return true;
		} else {
			/* THP pud was split under us: handle on pmd level */
			spin_unlock(pvmw->ptl);
			pvmw->ptl = NULL;
		}
	} else if (!pud_present(pude))
		return false;

	if (!map_pmd(pvmw))
		goto next_pmd;
	/* pmd locked after map_pmd  */
	while (1) {
		pmd_res = check_pmd(pvmw);
		if (pmd_res == 1) /* pmd_page */
			return true;
		else if (pmd_res == 2) /* pmd entry  */
			goto pte_level;
next_pmd:
		/* Only PMD-mapped PUD THP has next pmd  */
		if (!(PageTransHuge(pvmw->page) && compound_order(pvmw->page) == HPAGE_PUD_ORDER))
			return not_found(pvmw);
		do {
			pvmw->address += HPAGE_PMD_SIZE;
			if (pvmw->address >= pvmw->vma->vm_end ||
			    pvmw->address >=
					__vma_address(pvmw->page, pvmw->vma) +
					hpage_nr_pages(pvmw->page) * PAGE_SIZE)
				return not_found(pvmw);
			/* Did we cross page table boundary? */
			if (pvmw->address % PUD_SIZE == 0) {
				if (pvmw->ptl) {
					spin_unlock(pvmw->ptl);
					pvmw->ptl = NULL;
				}
				goto restart;
			} else {
				pvmw->pmd++;
			}
		} while (pmd_none(*pvmw->pmd));

		if (!pvmw->ptl)
			pvmw->ptl = pmd_lock(mm, pvmw->pmd);
	}

pte_level:
	if (!map_pte(pvmw))
		goto next_pte;
	while (1) {
		if (check_pte(pvmw))
			return true;
next_pte:
		/* Seek to next pte only makes sense for THP */
		if (!PageTransHuge(pvmw->page) || PageHuge(pvmw->page))
			return not_found(pvmw);
		do {
			pvmw->address += PAGE_SIZE;
			if (pvmw->address >= pvmw->vma->vm_end ||
			    pvmw->address >=
					__vma_address(pvmw->page, pvmw->vma) +
					hpage_nr_pages(pvmw->page) * PAGE_SIZE)
				return not_found(pvmw);
			/* Did we cross page table boundary? */
			if (pvmw->address % PMD_SIZE == 0) {
				pte_unmap(pvmw->pte);
				if (pvmw->ptl) {
					spin_unlock(pvmw->ptl);
					pvmw->ptl = NULL;
				}
				goto restart;
			} else {
				pvmw->pte++;
			}
		} while (pte_none(*pvmw->pte));

		if (!pvmw->ptl) {
			pvmw->ptl = pte_lockptr(mm, pvmw->pmd);
			spin_lock(pvmw->ptl);
		}
	}
}

/**
 * page_mapped_in_vma - check whether a page is really mapped in a VMA
 * @page: the page to test
 * @vma: the VMA to test
 *
 * Returns 1 if the page is mapped into the page tables of the VMA, 0
 * if the page is not mapped into the page tables of this VMA.  Only
 * valid for normal file or anonymous VMAs.
 */
int page_mapped_in_vma(struct page *page, struct vm_area_struct *vma)
{
	struct page_vma_mapped_walk pvmw = {
		.page = page,
		.vma = vma,
		.flags = PVMW_SYNC,
	};
	unsigned long start, end;

	start = __vma_address(page, vma);
	end = start + PAGE_SIZE * (hpage_nr_pages(page) - 1);

	if (unlikely(end < vma->vm_start || start >= vma->vm_end))
		return 0;
	pvmw.address = max(start, vma->vm_start);
	if (!page_vma_mapped_walk(&pvmw))
		return 0;
	page_vma_mapped_walk_done(&pvmw);
	return 1;
}
