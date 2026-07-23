// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Target-header raw PTE primitive for the Pixel 7 panther RP path.
 *
 * This file is compiled with the pinned device kernel headers, not with the
 * compact KernelPatch KPM headers used by r0lab.c. Keep all target-mm layout
 * knowledge here and expose only the small opaque ABI in r0lab_raw.h.
 */

#include "r0lab_raw.h"

#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/mmap_lock.h>
#include <linux/pgtable.h>
#include <linux/spinlock.h>

#include <asm/pgtable.h>
#include <asm/pgtable-prot.h>
#include <asm/tlbflush.h>

#define R0LAB_RAW_EINVAL (-22)
#define R0LAB_RAW_ENOENT (-2)
#define R0LAB_RAW_EAGAIN (-11)

extern struct page *vmalloc_to_page(const void *addr);

unsigned long r0lab_raw_abi_page_size(void)
{
    return PAGE_SIZE;
}

unsigned long r0lab_raw_abi_mm_size(void)
{
    return sizeof(struct mm_struct);
}

unsigned long r0lab_raw_abi_vma_size(void)
{
    return sizeof(struct vm_area_struct);
}

unsigned long r0lab_raw_abi_pte_uxn_bit(void)
{
    return PTE_UXN;
}

unsigned long r0lab_raw_abi_pte_user_bit(void)
{
    return PTE_USER;
}

unsigned long r0lab_raw_abi_pte_valid_bit(void)
{
    return PTE_VALID;
}

static inline pte_t r0lab_raw_pte_from_value(unsigned long value)
{
    return __pte((pteval_t)value);
}

static inline unsigned long r0lab_raw_pte_value(pte_t pte)
{
    return (unsigned long)pte_val(pte);
}

static inline pgprot_t r0lab_raw_pte_pgprot(pte_t pte)
{
    return __pgprot(pte_val(pte) & ~PTE_ADDR_MASK);
}

static inline pte_t r0lab_raw_make_source_uxn(pte_t pte)
{
    return set_pte_bit(pte, __pgprot(PTE_UXN));
}

static inline pte_t r0lab_raw_make_shadow_rx(pte_t original,
                                             unsigned long shadow_pfn)
{
    return pfn_pte(shadow_pfn, r0lab_raw_pte_pgprot(original));
}

static inline spinlock_t *r0lab_raw_pte_lockptr(pmd_t *pmd)
{
    return &pmd_page(*pmd)->ptl;
}

static int r0lab_raw_walk_locked(struct mm_struct *mm, unsigned long address,
                                 struct vm_area_struct **vma_out,
                                 pte_t **ptep_out, spinlock_t **ptl_out)
{
    struct vm_area_struct *vma;
    pgd_t *pgd;
    p4d_t *p4d;
    pud_t *pud;
    pmd_t *pmd;
    pte_t *ptep;
    spinlock_t *ptl;

    if (!mm || !address || (address & (PAGE_SIZE - 1)) ||
        !vma_out || !ptep_out || !ptl_out)
        return R0LAB_RAW_EINVAL;

    vma = find_vma(mm, address);
    if (!vma || address < vma->vm_start ||
        address + PAGE_SIZE > vma->vm_end || vma->vm_mm != mm)
        return R0LAB_RAW_ENOENT;

    pgd = pgd_offset(mm, address);
    if (pgd_none(*pgd) || pgd_bad(*pgd))
        return R0LAB_RAW_ENOENT;
    p4d = p4d_offset(pgd, address);
    if (p4d_none(*p4d) || p4d_bad(*p4d))
        return R0LAB_RAW_ENOENT;
    pud = pud_offset(p4d, address);
    if (pud_none(*pud) || pud_bad(*pud) || pud_sect(*pud))
        return R0LAB_RAW_ENOENT;
    pmd = pmd_offset(pud, address);
    if (pmd_none(*pmd) || pmd_bad(*pmd) || pmd_sect(*pmd))
        return R0LAB_RAW_ENOENT;

    ptep = pte_offset_kernel(pmd, address);
    ptl = r0lab_raw_pte_lockptr(pmd);
    spin_lock(ptl);

    *vma_out = vma;
    *ptep_out = ptep;
    *ptl_out = ptl;
    return 0;
}

static bool r0lab_raw_admits_original_pte(pte_t pte)
{
    return pte_present(pte) && pte_valid_user(pte) && pte_user_exec(pte) &&
           !pte_write(pte) && !pte_special(pte) && !pte_cont(pte) &&
           !pte_devmap(pte) && !pte_tagged(pte);
}

static int r0lab_raw_replace_locked(struct mm_struct *mm,
                                    struct vm_area_struct *vma,
                                    unsigned long address, pte_t *ptep,
                                    pte_t replacement)
{
    pte_t old;

    old = ptep_get_and_clear(mm, address, ptep);
    flush_tlb_page(vma, address);
    set_pte_at(mm, address, ptep, replacement);
    return r0lab_raw_pte_value(old) ? 0 : R0LAB_RAW_EAGAIN;
}

int r0lab_raw_capture(struct r0lab_raw_page *page)
{
    struct mm_struct *mm;
    struct vm_area_struct *vma;
    pte_t *ptep;
    spinlock_t *ptl;
    pte_t pte;
    int result;

    if (!page || !page->mm || !page->address)
        return R0LAB_RAW_EINVAL;
    if (PAGE_SIZE != R0LAB_RAW_PAGE_SIZE)
        return R0LAB_RAW_EINVAL;

    mm = (struct mm_struct *)page->mm;
    mmap_read_lock(mm);
    result = r0lab_raw_walk_locked(mm, page->address, &vma, &ptep, &ptl);
    if (result)
        goto out_unlock_mmap;

    pte = READ_ONCE(*ptep);
    if (!r0lab_raw_admits_original_pte(pte)) {
        result = R0LAB_RAW_EINVAL;
        goto out_unlock_pte;
    }

    page->source_pfn = pte_pfn(pte);
    page->original_pte = r0lab_raw_pte_value(pte);
    page->source_uxn_pte = r0lab_raw_pte_value(r0lab_raw_make_source_uxn(pte));
    page->active_pte = page->original_pte;
    page->state = R0LAB_RAW_CAPTURED;
    result = 0;

out_unlock_pte:
    spin_unlock(ptl);
out_unlock_mmap:
    mmap_read_unlock(mm);
    return result;
}

void *r0lab_raw_source_kernel_address(const struct r0lab_raw_page *page)
{
    if (!page || !page->source_pfn)
        return NULL;
    return phys_to_virt((phys_addr_t)page->source_pfn << PAGE_SHIFT);
}

int r0lab_raw_shadow_pfn_from_kaddr(struct r0lab_raw_page *page)
{
    struct page *shadow_page;

    if (!page || !page->shadow_kaddr)
        return R0LAB_RAW_EINVAL;
    shadow_page = vmalloc_to_page(page->shadow_kaddr);
    if (!shadow_page)
        return R0LAB_RAW_ENOENT;
    page->shadow_pfn = page_to_pfn(shadow_page);
    return page->shadow_pfn ? 0 : R0LAB_RAW_ENOENT;
}

static int r0lab_raw_arm_source_uxn_common(struct r0lab_raw_page *page,
                                           int require_shadow)
{
    struct mm_struct *mm;
    struct vm_area_struct *vma;
    pte_t *ptep;
    spinlock_t *ptl;
    pte_t current_pte;
    pte_t original;
    pte_t source_uxn;
    pte_t shadow_rx;
    int result;

    if (!page || !page->mm || !page->address ||
        (require_shadow && !page->shadow_pfn) ||
        page->state != R0LAB_RAW_CAPTURED)
        return R0LAB_RAW_EINVAL;

    mm = (struct mm_struct *)page->mm;
    mmap_read_lock(mm);
    result = r0lab_raw_walk_locked(mm, page->address, &vma, &ptep, &ptl);
    if (result)
        goto out_unlock_mmap;

    current_pte = READ_ONCE(*ptep);
    if (r0lab_raw_pte_value(current_pte) != page->original_pte ||
        !r0lab_raw_admits_original_pte(current_pte)) {
        result = R0LAB_RAW_EAGAIN;
        goto out_unlock_pte;
    }
    original = r0lab_raw_pte_from_value(page->original_pte);
    source_uxn = r0lab_raw_make_source_uxn(original);
    shadow_rx = page->shadow_pfn ?
                r0lab_raw_make_shadow_rx(original, page->shadow_pfn) :
                r0lab_raw_pte_from_value(0);

    result = r0lab_raw_replace_locked(mm, vma, page->address, ptep, source_uxn);
    if (!result) {
        page->source_uxn_pte = r0lab_raw_pte_value(source_uxn);
        page->shadow_rx_pte = page->shadow_pfn ?
                              r0lab_raw_pte_value(shadow_rx) : 0;
        page->active_pte = page->source_uxn_pte;
        page->state = R0LAB_RAW_SOURCE_UXN;
    }

out_unlock_pte:
    spin_unlock(ptl);
out_unlock_mmap:
    mmap_read_unlock(mm);
    return result;
}

int r0lab_raw_arm_source_uxn(struct r0lab_raw_page *page)
{
    return r0lab_raw_arm_source_uxn_common(page, 1);
}

int r0lab_raw_arm_source_uxn_only(struct r0lab_raw_page *page)
{
    return r0lab_raw_arm_source_uxn_common(page, 0);
}

int r0lab_raw_activate_shadow(struct r0lab_raw_page *page)
{
    struct mm_struct *mm;
    struct vm_area_struct *vma;
    pte_t *ptep;
    spinlock_t *ptl;
    pte_t current_pte;
    pte_t shadow_rx;
    int result;

    if (!page || !page->mm || !page->address ||
        page->state != R0LAB_RAW_SOURCE_UXN || !page->shadow_rx_pte)
        return R0LAB_RAW_EINVAL;

    mm = (struct mm_struct *)page->mm;
    mmap_read_lock(mm);
    result = r0lab_raw_walk_locked(mm, page->address, &vma, &ptep, &ptl);
    if (result)
        goto out_unlock_mmap;

    current_pte = READ_ONCE(*ptep);
    if (r0lab_raw_pte_value(current_pte) != page->source_uxn_pte) {
        result = R0LAB_RAW_EAGAIN;
        goto out_unlock_pte;
    }
    shadow_rx = r0lab_raw_pte_from_value(page->shadow_rx_pte);
    result = r0lab_raw_replace_locked(mm, vma, page->address, ptep, shadow_rx);
    if (!result) {
        page->active_pte = page->shadow_rx_pte;
        page->state = R0LAB_RAW_SHADOW_RX;
    }

out_unlock_pte:
    spin_unlock(ptl);
out_unlock_mmap:
    mmap_read_unlock(mm);
    return result;
}

int r0lab_raw_begin_stepping(struct r0lab_raw_page *page)
{
    struct mm_struct *mm;
    struct vm_area_struct *vma;
    pte_t *ptep;
    spinlock_t *ptl;
    pte_t current_pte;
    pte_t original;
    int result;

    if (!page || !page->mm || !page->address ||
        page->state != R0LAB_RAW_SHADOW_RX || !page->original_pte)
        return R0LAB_RAW_EINVAL;

    mm = (struct mm_struct *)page->mm;
    mmap_read_lock(mm);
    result = r0lab_raw_walk_locked(mm, page->address, &vma, &ptep, &ptl);
    if (result)
        goto out_unlock_mmap;

    current_pte = READ_ONCE(*ptep);
    if (r0lab_raw_pte_value(current_pte) != page->shadow_rx_pte) {
        result = R0LAB_RAW_EAGAIN;
        goto out_unlock_pte;
    }

    original = r0lab_raw_pte_from_value(page->original_pte);
    result = r0lab_raw_replace_locked(mm, vma, page->address, ptep, original);
    if (!result) {
        page->active_pte = page->original_pte;
        page->state = R0LAB_RAW_ORIGINAL_STEP;
    }

out_unlock_pte:
    spin_unlock(ptl);
out_unlock_mmap:
    mmap_read_unlock(mm);
    return result;
}

int r0lab_raw_finish_stepping(struct r0lab_raw_page *page)
{
    struct mm_struct *mm;
    struct vm_area_struct *vma;
    pte_t *ptep;
    spinlock_t *ptl;
    pte_t current_pte;
    pte_t shadow_rx;
    int result;

    if (!page || !page->mm || !page->address ||
        page->state != R0LAB_RAW_ORIGINAL_STEP || !page->shadow_rx_pte)
        return R0LAB_RAW_EINVAL;

    mm = (struct mm_struct *)page->mm;
    mmap_read_lock(mm);
    result = r0lab_raw_walk_locked(mm, page->address, &vma, &ptep, &ptl);
    if (result)
        goto out_unlock_mmap;

    current_pte = READ_ONCE(*ptep);
    if (r0lab_raw_pte_value(current_pte) != page->original_pte) {
        result = R0LAB_RAW_EAGAIN;
        goto out_unlock_pte;
    }

    shadow_rx = r0lab_raw_pte_from_value(page->shadow_rx_pte);
    result = r0lab_raw_replace_locked(mm, vma, page->address, ptep, shadow_rx);
    if (!result) {
        page->active_pte = page->shadow_rx_pte;
        page->state = R0LAB_RAW_SHADOW_RX;
    }

out_unlock_pte:
    spin_unlock(ptl);
out_unlock_mmap:
    mmap_read_unlock(mm);
    return result;
}

int r0lab_raw_vma_matches(const struct r0lab_raw_page *page, void *vma_ptr,
                          unsigned long address)
{
    struct vm_area_struct *vma = (struct vm_area_struct *)vma_ptr;
    unsigned long page_address;

    if (!page || !page->mm || !page->address || !vma || !address)
        return 0;
    page_address = address & ~(PAGE_SIZE - 1UL);
    if (PAGE_SIZE != R0LAB_RAW_PAGE_SIZE)
        return 0;
    return vma->vm_mm == (struct mm_struct *)page->mm &&
           page_address == page->address &&
           page_address >= vma->vm_start &&
           page_address + PAGE_SIZE <= vma->vm_end;
}

int r0lab_raw_begin_gup_hide(struct r0lab_raw_page *page)
{
    struct mm_struct *mm;
    struct vm_area_struct *vma;
    pte_t *ptep;
    spinlock_t *ptl;
    pte_t current_pte;
    pte_t original;
    int result;

    if (!page || !page->mm || !page->address ||
        page->state != R0LAB_RAW_SHADOW_RX || !page->original_pte ||
        !page->shadow_rx_pte || page->gup_hide_active)
        return R0LAB_RAW_EINVAL;

    mm = (struct mm_struct *)page->mm;
    mmap_read_lock(mm);
    result = r0lab_raw_walk_locked(mm, page->address, &vma, &ptep, &ptl);
    if (result)
        goto out_unlock_mmap;

    current_pte = READ_ONCE(*ptep);
    if (r0lab_raw_pte_value(current_pte) != page->shadow_rx_pte) {
        result = R0LAB_RAW_EAGAIN;
        goto out_unlock_pte;
    }

    original = r0lab_raw_pte_from_value(page->original_pte);
    result = r0lab_raw_replace_locked(mm, vma, page->address, ptep, original);
    if (!result) {
        page->gup_saved_pte = page->shadow_rx_pte;
        page->gup_hide_active = 1;
        page->active_pte = page->original_pte;
        ++page->gup_begin_events;
    }

out_unlock_pte:
    spin_unlock(ptl);
out_unlock_mmap:
    mmap_read_unlock(mm);
    return result;
}

int r0lab_raw_finish_gup_hide(struct r0lab_raw_page *page)
{
    struct mm_struct *mm;
    struct vm_area_struct *vma;
    pte_t *ptep;
    spinlock_t *ptl;
    pte_t current_pte;
    pte_t replacement;
    unsigned long replacement_value;
    int result;

    if (!page || !page->mm || !page->address ||
        page->state != R0LAB_RAW_SHADOW_RX || !page->original_pte ||
        !page->shadow_rx_pte || !page->gup_hide_active)
        return R0LAB_RAW_EINVAL;

    mm = (struct mm_struct *)page->mm;
    mmap_read_lock(mm);
    result = r0lab_raw_walk_locked(mm, page->address, &vma, &ptep, &ptl);
    if (result)
        goto out_unlock_mmap;

    current_pte = READ_ONCE(*ptep);
    if (r0lab_raw_pte_value(current_pte) != page->original_pte) {
        result = R0LAB_RAW_EAGAIN;
        goto out_unlock_pte;
    }

    replacement_value = page->gup_saved_pte ? page->gup_saved_pte :
                                             page->shadow_rx_pte;
    replacement = r0lab_raw_pte_from_value(replacement_value);
    result = r0lab_raw_replace_locked(mm, vma, page->address, ptep,
                                      replacement);
    if (!result) {
        page->active_pte = replacement_value;
        page->gup_saved_pte = 0;
        page->gup_hide_active = 0;
        ++page->gup_finish_events;
    }

out_unlock_pte:
    spin_unlock(ptl);
out_unlock_mmap:
    mmap_read_unlock(mm);
    return result;
}

int r0lab_raw_begin_fork_hide(struct r0lab_raw_page *page, void *oldmm)
{
    struct mm_struct *mm;
    struct vm_area_struct *vma;
    pte_t *ptep;
    spinlock_t *ptl;
    pte_t current_pte;
    pte_t original;
    int result;

    if (!page || !page->mm || page->mm != oldmm || !page->address ||
        page->state != R0LAB_RAW_SHADOW_RX || !page->original_pte ||
        !page->shadow_rx_pte || page->gup_hide_active ||
        page->fork_hide_active)
        return R0LAB_RAW_EINVAL;

    mm = (struct mm_struct *)oldmm;
    mmap_read_lock(mm);
    result = r0lab_raw_walk_locked(mm, page->address, &vma, &ptep, &ptl);
    if (result)
        goto out_unlock_mmap;

    current_pte = READ_ONCE(*ptep);
    if (r0lab_raw_pte_value(current_pte) != page->shadow_rx_pte) {
        result = R0LAB_RAW_EAGAIN;
        goto out_unlock_pte;
    }

    original = r0lab_raw_pte_from_value(page->original_pte);
    result = r0lab_raw_replace_locked(mm, vma, page->address, ptep, original);
    if (!result) {
        page->fork_saved_pte = page->shadow_rx_pte;
        page->fork_hide_active = 1;
        page->active_pte = page->original_pte;
        ++page->fork_begin_events;
    }

out_unlock_pte:
    spin_unlock(ptl);
out_unlock_mmap:
    mmap_read_unlock(mm);
    return result;
}

int r0lab_raw_finish_fork_hide(struct r0lab_raw_page *page, void *oldmm)
{
    struct mm_struct *mm;
    struct vm_area_struct *vma;
    pte_t *ptep;
    spinlock_t *ptl;
    pte_t current_pte;
    pte_t replacement;
    unsigned long replacement_value;
    int result;

    if (!page || !page->mm || page->mm != oldmm || !page->address ||
        page->state != R0LAB_RAW_SHADOW_RX || !page->original_pte ||
        !page->shadow_rx_pte || !page->fork_hide_active)
        return R0LAB_RAW_EINVAL;

    mm = (struct mm_struct *)oldmm;
    mmap_read_lock(mm);
    result = r0lab_raw_walk_locked(mm, page->address, &vma, &ptep, &ptl);
    if (result)
        goto out_unlock_mmap;

    current_pte = READ_ONCE(*ptep);
    if (r0lab_raw_pte_value(current_pte) != page->original_pte) {
        result = R0LAB_RAW_EAGAIN;
        goto out_unlock_pte;
    }

    replacement_value = page->fork_saved_pte ? page->fork_saved_pte :
                                              page->shadow_rx_pte;
    replacement = r0lab_raw_pte_from_value(replacement_value);
    result = r0lab_raw_replace_locked(mm, vma, page->address, ptep,
                                      replacement);
    if (!result) {
        page->active_pte = replacement_value;
        page->fork_saved_pte = 0;
        page->fork_hide_active = 0;
        ++page->fork_finish_events;
    }

out_unlock_pte:
    spin_unlock(ptl);
out_unlock_mmap:
    mmap_read_unlock(mm);
    return result;
}

int r0lab_raw_restore_original(struct r0lab_raw_page *page)
{
    struct mm_struct *mm;
    struct vm_area_struct *vma;
    pte_t *ptep;
    spinlock_t *ptl;
    pte_t current_pte;
    pte_t original;
    unsigned long current_value;
    int result;

    if (!page || !page->mm || !page->address || !page->original_pte)
        return R0LAB_RAW_EINVAL;
    if (page->state != R0LAB_RAW_SOURCE_UXN &&
        page->state != R0LAB_RAW_SHADOW_RX &&
        page->state != R0LAB_RAW_ORIGINAL_STEP &&
        page->state != R0LAB_RAW_CAPTURED)
        return R0LAB_RAW_EINVAL;

    if (page->state == R0LAB_RAW_CAPTURED) {
        page->state = R0LAB_RAW_RESTORED;
        return 0;
    }

    mm = (struct mm_struct *)page->mm;
    mmap_read_lock(mm);
    result = r0lab_raw_walk_locked(mm, page->address, &vma, &ptep, &ptl);
    if (result)
        goto out_unlock_mmap;

    current_pte = READ_ONCE(*ptep);
    current_value = r0lab_raw_pte_value(current_pte);
    if (current_value != page->source_uxn_pte &&
        current_value != page->shadow_rx_pte &&
        current_value != page->original_pte) {
        result = R0LAB_RAW_EAGAIN;
        goto out_unlock_pte;
    }

    original = r0lab_raw_pte_from_value(page->original_pte);
    result = r0lab_raw_replace_locked(mm, vma, page->address, ptep, original);
    if (!result) {
        page->active_pte = page->original_pte;
        page->gup_saved_pte = 0;
        page->gup_hide_active = 0;
        page->fork_saved_pte = 0;
        page->fork_hide_active = 0;
        page->state = R0LAB_RAW_RESTORED;
    }

out_unlock_pte:
    spin_unlock(ptl);
out_unlock_mmap:
    mmap_read_unlock(mm);
    return result;
}
