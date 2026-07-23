// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef R0LAB_RAW_H
#define R0LAB_RAW_H

#define R0LAB_RAW_PAGE_SIZE 4096UL

enum r0lab_raw_state {
    R0LAB_RAW_EMPTY = 0,
    R0LAB_RAW_CAPTURED = 1,
    R0LAB_RAW_SOURCE_UXN = 2,
    R0LAB_RAW_SHADOW_RX = 3,
    R0LAB_RAW_ORIGINAL_STEP = 4,
    R0LAB_RAW_RESTORED = 5,
    R0LAB_RAW_POISONED = 6,
    R0LAB_RAW_ORIGINAL_READ = 7,
};

struct r0lab_raw_page {
    void *mm;
    unsigned long address;
    void *shadow_kaddr;
    unsigned long source_pfn;
    unsigned long shadow_pfn;
    unsigned long original_pte;
    unsigned long source_uxn_pte;
    unsigned long shadow_rx_pte;
    unsigned long active_pte;
    unsigned long gup_saved_pte;
    unsigned long gup_hide_active;
    unsigned long gup_begin_events;
    unsigned long gup_finish_events;
    unsigned long fork_saved_pte;
    unsigned long fork_hide_active;
    unsigned long fork_begin_events;
    unsigned long fork_finish_events;
    unsigned long read_cycle_saved_pte;
    unsigned long read_cycle_active;
    unsigned long read_cycle_begin_events;
    unsigned long read_cycle_finish_events;
    unsigned long state;
};

unsigned long r0lab_raw_abi_page_size(void);
unsigned long r0lab_raw_abi_mm_size(void);
unsigned long r0lab_raw_abi_vma_size(void);
unsigned long r0lab_raw_abi_pte_uxn_bit(void);
unsigned long r0lab_raw_abi_pte_user_bit(void);
unsigned long r0lab_raw_abi_pte_valid_bit(void);

int r0lab_raw_capture(struct r0lab_raw_page *page);
void *r0lab_raw_source_kernel_address(const struct r0lab_raw_page *page);
int r0lab_raw_shadow_pfn_from_kaddr(struct r0lab_raw_page *page);
int r0lab_raw_arm_source_uxn(struct r0lab_raw_page *page);
int r0lab_raw_arm_source_uxn_only(struct r0lab_raw_page *page);
int r0lab_raw_activate_shadow(struct r0lab_raw_page *page);
int r0lab_raw_begin_stepping(struct r0lab_raw_page *page);
int r0lab_raw_finish_stepping(struct r0lab_raw_page *page);
int r0lab_raw_begin_read_cycle(struct r0lab_raw_page *page);
int r0lab_raw_finish_read_cycle(struct r0lab_raw_page *page);
int r0lab_raw_vma_matches(const struct r0lab_raw_page *page, void *vma,
                          unsigned long address);
int r0lab_raw_begin_gup_hide(struct r0lab_raw_page *page);
int r0lab_raw_finish_gup_hide(struct r0lab_raw_page *page);
int r0lab_raw_begin_fork_hide(struct r0lab_raw_page *page, void *oldmm);
int r0lab_raw_finish_fork_hide(struct r0lab_raw_page *page, void *oldmm);
int r0lab_raw_restore_original(struct r0lab_raw_page *page);

#endif
