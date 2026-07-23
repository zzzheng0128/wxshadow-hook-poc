// SPDX-License-Identifier: GPL-2.0-or-later
/* Compile-only proof that the pinned target headers provide the raw MM ABI. */

#include <linux/mm_types.h>
#include <linux/mmap_lock.h>
#include <linux/pgtable.h>
#include <linux/spinlock.h>

#include <asm/pgtable.h>
#include <asm/tlbflush.h>

unsigned long r0lab_target_mm_size(void)
{
    return sizeof(struct mm_struct);
}

unsigned long r0lab_target_vma_size(void)
{
    return sizeof(struct vm_area_struct);
}

unsigned long r0lab_target_page_size(void)
{
    return PAGE_SIZE;
}

static inline spinlock_t *r0lab_target_pte_lockptr(pmd_t *pmd)
{
    return &pmd_page(*pmd)->ptl;
}

void r0lab_target_mm_compile_probe(struct mm_struct *mm,
                                   struct vm_area_struct *vma, pmd_t *pmd,
                                   unsigned long address)
{
    pte_t *ptep;
    spinlock_t *ptl;

    mmap_read_lock(mm);
    ptep = pte_offset_kernel(pmd, address);
    ptl = r0lab_target_pte_lockptr(pmd);
    spin_lock(ptl);
    flush_tlb_page(vma, address);
    spin_unlock(ptl);
    mmap_read_unlock(mm);
}

pte_t r0lab_target_bbm_compile_probe(struct mm_struct *mm,
                                     struct vm_area_struct *vma, pte_t *ptep,
                                     unsigned long address, pte_t replacement)
{
    pte_t old;

    old = ptep_get_and_clear(mm, address, ptep);
    flush_tlb_page(vma, address);
    set_pte_at(mm, address, ptep, replacement);
    return old;
}
