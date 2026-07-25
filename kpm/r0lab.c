// SPDX-License-Identifier: GPL-2.0-or-later
// Controlled M0-M5 lab: session-scoped HWBP, UXN, clone, and lifecycle experiments.

#include <asm/current.h>
#include <asm/ptrace.h>
#include <common.h>
#include <compiler.h>
#include <hook.h>
#include <kpmodule.h>
#include <kputils.h>
#include <linux/pid.h>
#include <linux/kernel.h>
#include <linux/printk.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/string.h>

#include "r0lab_raw.h"

#define R0LAB_EVENT_CAPACITY 64
#define R0LAB_EVENT_SNAPSHOT_CAPACITY 10
#define R0LAB_OUTPUT_CAPACITY 2048
#define R0LAB_TOKEN_MAX 18
#define R0LAB_HWBP_SLOT_CAPACITY 8
#define R0LAB_HWBP_EVENT_COUNT 2
#define R0LAB_M3_PAGE_SIZE 4096UL
#define R0LAB_M3_ESR_EC_SHIFT 26U
#define R0LAB_M3_ESR_EC_IABT_LOW 0x20U
#define R0LAB_M3_ESR_EC_DABT_LOW 0x24U
#define R0LAB_M3_ESR_FSC_TYPE 0x3cU
#define R0LAB_M3_ESR_FSC_TRANSLATION 0x04U
#define R0LAB_M3_ESR_FSC_PERM 0x0cU
#define R0LAB_M3_ESR_WNR 0x40U
#define R0LAB_M3_DRAIN_LIMIT 2000U
#define R0LAB_M5_MONITOR_INTERVAL_MS 20U
#define R0LAB_M5_TEST_ARM_DELAY_MS 200U
#define R0LAB_M4_SEQUENCE_BYTES 8UL
#define R0LAB_RAW_CODE_MOV_W0_99 0x52800c60U
#define R0LAB_RAW_CODE_MOV_X0_X1 0xaa0103e0U
#define R0LAB_S4_RAW_REG_INDEX 1U
#define R0LAB_S4_RAW_REG_VALUE 73ULL
#define R0LAB_RAW_HWCAP_WORDS 2U
#define R0LAB_RAW_STATIC_KEY_SIZE 16U
#define R0LAB_RAW_ARM64_NCAPS 76U
#define R0LAB_S4_BRK_COMMENT 7U
#define R0LAB_FAULT_FLAG_WRITE 0x01U
#define R0LAB_FAULT_FLAG_USER 0x40U
#define R0LAB_FAULT_FLAG_REMOTE 0x80U
#define R0LAB_FAULT_FLAG_INSTRUCTION 0x100U
#define R0LAB_FAULT_KIND_READ 1U
#define R0LAB_FAULT_KIND_WRITE 2U
#define R0LAB_FAULT_KIND_EXEC 3U
#define R0LAB_ABORT_PROBE_SOURCE_NONE 0U
#define R0LAB_ABORT_PROBE_SOURCE_READ_TRANSLATION 1U
#define R0LAB_ABORT_PROBE_SOURCE_WRITE_PERMISSION 2U
#define R0LAB_ABORT_PROBE_SOURCE_READ_PERMISSION 3U
#define R0LAB_PRCTL_MAGIC 0x52304c42U
#define R0LAB_PRCTL_OP_READ_CYCLE 1U
#define R0LAB_PRCTL_OP_PATCH_WORD 2U
#define R0LAB_PRCTL_OP_RELEASE_PATCH 3U
#define R0LAB_PRCTL_OP_PATCH_RANGE 4U
#define R0LAB_PRCTL_OP_RELEASE_RANGE 5U
#define R0LAB_PATCH_RECORD_CAPACITY 1024U
#define R0LAB_PATCH_DIRTY_BITMAP_SIZE (R0LAB_RAW_PAGE_SIZE / 8U)
#define R0LAB_RAW_PAGE_SLOT_CAPACITY 2U
#define R0LAB_RAW_PRIMARY_SLOT 0U
#define R0LAB_S4_DESCRIPTOR_BRK_OFFSET 0U
#define R0LAB_S4_DESCRIPTOR_STEP_OFFSET 4U

#define R0LAB_EINVAL (-22)
#define R0LAB_EPERM (-1)
#define R0LAB_ENOSYS (-38)
#define R0LAB_EBUSY (-16)
#define R0LAB_ENOENT (-2)
#define R0LAB_EAGAIN (-11)
#define R0LAB_ESRCH (-3)
#define R0LAB_ENOMEM (-12)
#define R0LAB_ENOSPC (-28)
#define R0LAB_EFAULT (-14)

enum r0lab_event_op {
    R0LAB_EVENT_LOAD = 1,
    R0LAB_EVENT_REJECT = 2,
    R0LAB_EVENT_STATUS = 3,
    R0LAB_EVENT_ARM = 4,
    R0LAB_EVENT_CLOSE = 5,
    R0LAB_EVENT_UNLOAD = 6,
    R0LAB_EVENT_SUMMARY = 7,
    R0LAB_EVENT_HWBP_ENTRY = 8,
    R0LAB_EVENT_HWBP_RETURN = 9,
    R0LAB_EVENT_HWBP_ARM = 10,
    R0LAB_EVENT_HWBP_CLEAR = 11,
    R0LAB_EVENT_M3_ARM = 12,
    R0LAB_EVENT_M3_FAULT = 13,
    R0LAB_EVENT_M3_CLEAR = 14,
    R0LAB_EVENT_M4_ARM = 15,
    R0LAB_EVENT_M4_REDIRECT = 16,
    R0LAB_EVENT_M4_CLEAR = 17,
    R0LAB_EVENT_TARGET_EXIT = 18,
    R0LAB_EVENT_TEST_FAULT = 19,
    R0LAB_EVENT_RAW_ARM = 20,
    R0LAB_EVENT_RAW_ACTIVATE = 21,
    R0LAB_EVENT_RAW_CLEAR = 22,
    R0LAB_EVENT_S4_ARM = 23,
    R0LAB_EVENT_S4_BRK_OBSERVED = 24,
    R0LAB_EVENT_S4_STEP_OBSERVED = 25,
    R0LAB_EVENT_S4_CLEAR = 26,
    R0LAB_EVENT_RAW_GUP_BEGIN = 27,
    R0LAB_EVENT_RAW_GUP_FINISH = 28,
    R0LAB_EVENT_RAW_GUP_HOOK_BEGIN = 29,
    R0LAB_EVENT_RAW_GUP_HOOK_FINISH = 30,
    R0LAB_EVENT_RAW_FORK_HOOK_BEGIN = 31,
    R0LAB_EVENT_RAW_FORK_HOOK_FINISH = 32,
    R0LAB_EVENT_RAW_FAULT_HOOK_HIT = 33,
    R0LAB_EVENT_RAW_EXIT_MMAP_HIT = 34,
    R0LAB_EVENT_RAW_SYSCALL_HOOK_HIT = 35,
    R0LAB_EVENT_RAW_READ_CYCLE_BEGIN = 36,
    R0LAB_EVENT_RAW_READ_CYCLE_FINISH = 37,
    R0LAB_EVENT_RAW_SYSCALL_READ_CYCLE_BEGIN = 38,
    R0LAB_EVENT_RAW_ABORT_PROBE_HIT = 39,
    R0LAB_EVENT_RAW_ABORT_WRITE_RELEASE = 40,
    R0LAB_EVENT_S4_REG_WRITE = 41,
    R0LAB_EVENT_RAW_PRCTL_TRIGGER = 42,
    R0LAB_EVENT_RAW_PRCTL_PATCH = 43,
    R0LAB_EVENT_RAW_PRCTL_RELEASE = 44,
    R0LAB_EVENT_RAW_PRCTL_PATCH_RANGE = 45,
    R0LAB_EVENT_RAW_PRCTL_RELEASE_RANGE = 46,
    R0LAB_EVENT_RAW_ABORT_READ_CYCLE_BEGIN = 47,
};

struct r0lab_event {
    uint64_t seq;
    uint64_t monotonic_ticks;
    uint32_t uid;
    uint32_t tid;
    int32_t cpu;
    int32_t result;
    uint16_t op;
    uint16_t reserved;
    uint64_t pc;
    uint64_t x0;
    uint64_t x30;
};

struct r0lab_session {
    uid_t lab_uid;
    pid_t owner_tgid;
    uint64_t token;
    pid_t last_closed_tgid;
    uint64_t last_closed_token;
    uint64_t last_summary_seq;
    bool active;
};

/*
 * The target uses the Linux v5.10 perf_event_attr UAPI (PERF_ATTR_SIZE_VER6).
 * KernelPatch's compact KPM headers do not ship this UAPI definition, so keep
 * a local ABI-exact copy rather than relying on a host platform header.
 */
struct r0lab_perf_event_attr {
    uint32_t type;
    uint32_t size;
    uint64_t config;
    uint64_t sample_period;
    uint64_t sample_type;
    uint64_t read_format;
    uint64_t disabled : 1,
             inherit : 1,
             pinned : 1,
             exclusive : 1,
             exclude_user : 1,
             exclude_kernel : 1,
             exclude_hv : 1,
             exclude_idle : 1,
             mmap : 1,
             comm : 1,
             freq : 1,
             inherit_stat : 1,
             enable_on_exec : 1,
             task : 1,
             watermark : 1,
             precise_ip : 2,
             mmap_data : 1,
             sample_id_all : 1,
             exclude_host : 1,
             exclude_guest : 1,
             exclude_callchain_kernel : 1,
             exclude_callchain_user : 1,
             mmap2 : 1,
             comm_exec : 1,
             use_clockid : 1,
             context_switch : 1,
             write_backward : 1,
             namespaces : 1,
             ksymbol : 1,
             bpf_event : 1,
             aux_output : 1,
             cgroup : 1,
             text_poke : 1,
             reserved_flags : 30;
    uint32_t wakeup_events;
    uint32_t bp_type;
    uint64_t bp_addr;
    uint64_t bp_len;
    uint64_t branch_sample_type;
    uint64_t sample_regs_user;
    uint32_t sample_stack_user;
    int32_t clockid;
    uint64_t sample_regs_intr;
    uint32_t aux_watermark;
    uint16_t sample_max_stack;
    uint16_t reserved_2;
    uint32_t aux_sample_size;
    uint32_t reserved_3;
};

typedef char r0lab_perf_event_attr_size_must_be_120[
    sizeof(struct r0lab_perf_event_attr) == 120 ? 1 : -1];

struct perf_event;
struct perf_sample_data;
struct mm_struct;

struct r0lab_hwbp_slot {
    pid_t tid;
    bool reserving;
    bool armed;
    bool clearing;
    bool hit[R0LAB_HWBP_EVENT_COUNT];
    struct task_struct *target_task;
    struct perf_event *events[R0LAB_HWBP_EVENT_COUNT];
    struct r0lab_perf_event_attr attrs[R0LAB_HWBP_EVENT_COUNT];
};

struct r0lab_m3_page {
    struct r0lab_raw_page raw;
    struct mm_struct *mm;
    unsigned long address;
    uint64_t generation;
    uint32_t fault_events;
    bool reserving;
    bool armed;
    bool clearing;
    bool hook_installed;
    bool target_exiting;
    bool monitor_running;
    bool transitioning;
};

enum r0lab_page_record_backend {
    R0LAB_PAGE_RECORD_NONE = 0,
    R0LAB_PAGE_RECORD_M4_VISIBLE_CLONE = 1,
    R0LAB_PAGE_RECORD_RAW_TWO_PFN = 2,
};

enum r0lab_page_record_state {
    R0LAB_PAGE_RECORD_EMPTY = 0,
    R0LAB_PAGE_RECORD_PREPARING = 1,
    R0LAB_PAGE_RECORD_SOURCE_UXN = 2,
    R0LAB_PAGE_RECORD_SHADOW_ACTIVE = 3,
    R0LAB_PAGE_RECORD_RESTORING = 4,
    R0LAB_PAGE_RECORD_ORIGINAL_STEP = 5,
    R0LAB_PAGE_RECORD_ORIGINAL_READ = 6,
};

enum r0lab_s4_state {
    R0LAB_S4_EMPTY = 0,
    R0LAB_S4_PREPARING = 1,
    R0LAB_S4_HOOKED = 2,
    R0LAB_S4_BRK_OBSERVED = 3,
    R0LAB_S4_STEP_ARMED = 4,
    R0LAB_S4_STEP_OBSERVED = 5,
    R0LAB_S4_RESTORING = 6,
};

enum r0lab_s4_descriptor_state {
    R0LAB_S4_DESCRIPTOR_EMPTY = 0,
    R0LAB_S4_DESCRIPTOR_ARMED_SHADOW = 1,
    R0LAB_S4_DESCRIPTOR_BRK_MATCHED_ORIGINAL_STEP = 2,
    R0LAB_S4_DESCRIPTOR_STEP_MATCHED_SHADOW = 3,
    R0LAB_S4_DESCRIPTOR_CLEARED = 4,
};

enum r0lab_s4_descriptor_mode {
    R0LAB_S4_DESCRIPTOR_MODE_NONE = 0,
    R0LAB_S4_DESCRIPTOR_MODE_RAW_STEP = 1,
    R0LAB_S4_DESCRIPTOR_MODE_RAW_REG = 2,
};

enum r0lab_raw_hook_kind {
    R0LAB_RAW_HOOK_NONE = 0,
    R0LAB_RAW_HOOK_ABORT = 1,
    R0LAB_RAW_HOOK_FAULT = 2,
    R0LAB_RAW_HOOK_GUP = 3,
    R0LAB_RAW_HOOK_FORK = 4,
    R0LAB_RAW_HOOK_SYSCALL = 5,
    R0LAB_RAW_HOOK_PRCTL = 6,
    R0LAB_RAW_HOOK_EXIT = 7,
};

enum r0lab_raw_hook_route_flags {
    R0LAB_RAW_HOOK_ROUTE_MUTATING = 1U << 0,
    R0LAB_RAW_HOOK_ROUTE_REQUIRE_SHADOW_RX = 1U << 1,
    R0LAB_RAW_HOOK_ROUTE_ALLOW_SOURCE_UXN = 1U << 2,
    R0LAB_RAW_HOOK_ROUTE_ALLOW_ORIGINAL_READ = 1U << 3,
    R0LAB_RAW_HOOK_ROUTE_ALLOW_SHADOW_XOM = 1U << 4,
};

struct r0lab_page_record {
    unsigned long source_address;
    unsigned long peer_address;
    unsigned long source_pfn;
    unsigned long shadow_pfn;
    uint64_t generation;
    uint32_t events;
    uint8_t backend;
    uint8_t state;
};

struct r0lab_patch_record {
    uint16_t offset;
    uint16_t length;
    uint8_t active;
    uint8_t reserved[3];
    uint64_t version;
    void *data;
};

struct r0lab_raw_hook_route_stats {
    uint32_t route_hits;
    uint32_t route_rejects;
    uint32_t route_wrong_mm;
    uint32_t route_outside_page;
    uint32_t route_stale_generation;
    uint32_t route_busy;
    uint32_t route_wrong_state;
    uint32_t route_identity_mismatch;
};

struct r0lab_raw_hook_page_token {
    uint16_t slot_id;
    uint64_t generation;
    struct r0lab_raw_shadow_page *page;
    enum r0lab_raw_hook_kind kind;
};

struct r0lab_s4_descriptor {
    struct mm_struct *mm;
    unsigned long page_address;
    uint64_t generation;
    uint32_t brk_offset;
    uint32_t step_offset;
    uint32_t brk_events;
    uint32_t step_events;
    uint32_t pte_begin_events;
    uint32_t pte_finish_events;
    uint32_t reject_events;
    uint32_t register_index;
    uint64_t register_value;
    pid_t step_tid;
    uint16_t slot_id;
    uint8_t state;
    uint8_t mode;
    bool present;
};

struct r0lab_prctl_patch_request {
    uint64_t address;
    uint32_t length;
    uint32_t flags;
};

/* M4 permits only offset zero of the fixed two-instruction Lab App sequence. */
struct r0lab_m4_page {
    struct r0lab_page_record record;
    struct r0lab_raw_page raw;
    struct mm_struct *mm;
    unsigned long source_address;
    unsigned long clone_address;
    uint64_t generation;
    uint32_t redirect_events;
    bool reserving;
    bool armed;
    bool clearing;
    bool hook_installed;
    bool target_exiting;
    bool monitor_running;
};

struct r0lab_raw_shadow_page {
    struct r0lab_page_record record;
    struct r0lab_raw_page raw;
    uint64_t generation;
    uint32_t activation_events;
    uint16_t slot_id;
    bool reserving;
    bool armed;
    bool clearing;
    bool hook_installed;
    bool abort_hook_suppressed;
    bool abort_hook_passthrough;
    bool abort_hook_mmget;
    bool abort_hook_lock;
    bool abort_hook_inflight;
    bool abort_hook_iabt_route;
    bool abort_hook_iabt_transition;
    bool mm_count_owned;
    bool mm_users_owned;
    bool target_exiting;
    bool monitor_running;
    bool transitioning;
    struct r0lab_s4_descriptor s4_descriptor;
    bool s4_shadow_brk_layout;
    bool s4_shadow_reg_layout;
    bool gup_hook_installed;
    bool gup_hook_uses_pte;
    bool fork_hook_installed;
    bool fault_hook_installed;
    bool exit_hook_installed;
    bool syscall_hook_installed;
    bool syscall_hook_read_cycle_mode;
    bool prctl_hook_installed;
    bool fault_probe_armed;
    bool abort_probe_armed;
    bool abort_write_release_armed;
    bool abort_read_cycle_armed;
    unsigned long fault_probe_address;
    pid_t fault_probe_reader_tgid;
    uint32_t gup_hook_begin_events;
    uint32_t gup_hook_finish_events;
    uint32_t gup_hook_failures;
    uint32_t fork_hook_begin_events;
    uint32_t fork_hook_finish_events;
    uint32_t fork_hook_failures;
    uint32_t fault_hook_read_events;
    uint32_t fault_hook_write_events;
    uint32_t fault_hook_exec_events;
    uint32_t fault_hook_failures;
    uint32_t exit_hook_events;
    uint32_t exit_hook_failures;
    uint32_t syscall_hook_events;
    uint32_t syscall_hook_read_cycle_events;
    uint32_t syscall_hook_failures;
    uint32_t prctl_hook_events;
    uint32_t prctl_hook_read_cycle_events;
    uint32_t prctl_hook_patch_events;
    uint32_t prctl_hook_release_events;
    uint32_t prctl_hook_reject_events;
    uint32_t prctl_hook_failures;
    struct r0lab_patch_record patch_records[R0LAB_PATCH_RECORD_CAPACITY];
    uint16_t patch_rebuild_order[R0LAB_PATCH_RECORD_CAPACITY];
    uint8_t patch_dirty[R0LAB_PATCH_DIRTY_BITMAP_SIZE];
    uint64_t patch_version;
    uint16_t patch_record_slots;
    uint16_t patch_active_count;
    uint16_t patch_dirty_bytes;
    uint32_t fault_probe_read_events;
    uint32_t fault_probe_write_events;
    uint32_t fault_probe_exec_events;
    uint32_t fault_probe_failures;
    uint32_t abort_probe_read_events;
    uint32_t abort_probe_write_events;
    uint32_t abort_probe_exec_events;
    uint32_t abort_probe_failures;
    uint32_t abort_probe_last_esr;
    unsigned long abort_probe_last_far;
    uint8_t abort_probe_source;
    uint32_t abort_write_release_events;
    uint32_t abort_write_release_failures;
    uint32_t abort_write_release_last_esr;
    unsigned long abort_write_release_last_far;
    int abort_write_release_last_result;
    uint32_t abort_read_cycle_events;
    uint32_t abort_read_cycle_failures;
    uint32_t abort_read_cycle_last_esr;
    unsigned long abort_read_cycle_last_far;
    int abort_read_cycle_last_result;
    struct r0lab_raw_hook_route_stats hook_route_stats;
};

struct r0lab_raw_exit_cleanup_slot {
    struct r0lab_raw_shadow_page *page;
    uint64_t generation;
    uint32_t exit_hook_events_before;
    int result;
    bool exit_hook_installed;
};

struct r0lab_raw_page_table {
    struct r0lab_raw_shadow_page slots[R0LAB_RAW_PAGE_SLOT_CAPACITY];
    uint16_t selected_slot;
    struct r0lab_raw_hook_route_stats hook_route_miss_stats;
};

struct r0lab_s4_breakpoint {
    struct mm_struct *mm;
    unsigned long target_address;
    uint64_t generation;
    uint32_t brk_events;
    uint32_t step_events;
    uint32_t step_enable_events;
    uint32_t step_disable_events;
    uint32_t pte_begin_events;
    uint32_t pte_finish_events;
    uint32_t reg_write_events;
    uint64_t reg_value;
    pid_t step_tid;
    uint8_t state;
    bool reserving;
    bool armed;
    bool clearing;
    bool hook_installed;
    bool step_hook_installed;
    bool step_mode;
    bool raw_step_mode;
    bool raw_reg_mode;
    bool target_exiting;
    bool monitor_running;
};

typedef uint64_t (*r0lab_clock_fn_t)(void);
typedef uint64_t (*r0lab_current_cpu_fn_t)(void);
typedef pid_t (*r0lab_task_pid_fn_t)(struct task_struct *task,
                                     enum pid_type type,
                                     struct pid_namespace *ns);
typedef unsigned long (*r0lab_lock_irqsave_fn_t)(raw_spinlock_t *lock);
typedef void (*r0lab_unlock_irqrestore_fn_t)(raw_spinlock_t *lock,
                                             unsigned long flags);
typedef void (*r0lab_hwbp_handler_t)(struct perf_event *event,
                                     struct perf_sample_data *data,
                                     struct pt_regs *regs);
typedef struct perf_event *(*r0lab_register_hwbp_fn_t)(
    struct r0lab_perf_event_attr *attr, r0lab_hwbp_handler_t handler,
    void *context, struct task_struct *task);
typedef void (*r0lab_disable_hwbp_inatomic_fn_t)(struct perf_event *event);
typedef void (*r0lab_disable_hwbp_local_fn_t)(struct perf_event *event);
typedef void (*r0lab_unregister_hwbp_fn_t)(struct perf_event *event);
typedef void (*r0lab_synchronize_rcu_fn_t)(void);
typedef struct task_struct *(*r0lab_find_get_task_fn_t)(pid_t tid);
typedef void (*r0lab_put_task_fn_t)(struct task_struct *task);
typedef struct task_struct *(*r0lab_kthread_create_fn_t)(
    int (*threadfn)(void *), void *data, int node, const char *namefmt, ...);
typedef int (*r0lab_wake_up_process_fn_t)(struct task_struct *task);
typedef int (*r0lab_kthread_stop_fn_t)(struct task_struct *task);
typedef bool (*r0lab_kthread_should_stop_fn_t)(void);
typedef void (*r0lab_msleep_fn_t)(unsigned int milliseconds);
typedef struct mm_struct *(*r0lab_get_task_mm_fn_t)(struct task_struct *task);
typedef void (*r0lab_mmput_fn_t)(struct mm_struct *mm);
typedef void (*r0lab_mmdrop_fn_t)(struct mm_struct *mm);
typedef unsigned long (*r0lab_get_free_pages_fn_t)(unsigned int gfp_mask,
                                                   unsigned int order);
typedef void (*r0lab_free_pages_fn_t)(unsigned long address,
                                      unsigned int order);
typedef void *(*r0lab_vmalloc_fn_t)(unsigned long size);
typedef void (*r0lab_vfree_fn_t)(const void *addr);
typedef void *(*r0lab_vmalloc_to_page_fn_t)(const void *addr);
typedef void (*r0lab_down_read_fn_t)(void *sem);
typedef void (*r0lab_up_read_fn_t)(void *sem);
typedef void (*r0lab_raw_spin_fn_t)(void *lock);
typedef void *(*r0lab_find_vma_fn_t)(void *mm, unsigned long addr);
typedef void (*r0lab_sync_icache_dcache_fn_t)(unsigned long pte);
typedef void (*r0lab_sync_icache_aliases_fn_t)(unsigned long start,
                                               unsigned long end);
typedef long (*r0lab_copy_from_user_nofault_fn_t)(void *dst,
                                                  const void __user *src,
                                                  size_t size);
typedef void (*r0lab_mte_sync_tags_fn_t)(unsigned long old_pte,
                                         unsigned long new_pte);
typedef void (*r0lab_user_step_fn_t)(struct task_struct *task);

KPM_NAME("r0lab-m1");
KPM_VERSION("0.9.0");
KPM_LICENSE("GPL v2");
KPM_AUTHOR("r0hook research");
KPM_DESCRIPTION("Controlled M0-M5 session, HWBP, UXN, clone, and lifecycle lab");

DEFINE_SPINLOCK(g_r0lab_lock);

static struct r0lab_event g_events[R0LAB_EVENT_CAPACITY];
static struct r0lab_session g_session;
static struct r0lab_hwbp_slot g_hwbp_slots[R0LAB_HWBP_SLOT_CAPACITY];
static struct r0lab_m3_page g_m3_page;
static struct r0lab_m4_page g_m4_page;
static struct r0lab_raw_page_table g_raw_page_table;
#define g_raw_page (g_raw_page_table.slots[R0LAB_RAW_PRIMARY_SLOT])
static struct r0lab_s4_breakpoint g_s4_brk;
static r0lab_clock_fn_t g_clock;
static r0lab_current_cpu_fn_t g_current_cpu;
static r0lab_task_pid_fn_t g_task_pid;
static r0lab_lock_irqsave_fn_t g_lock_irqsave;
static r0lab_unlock_irqrestore_fn_t g_unlock_irqrestore;
static r0lab_register_hwbp_fn_t g_register_hwbp;
static r0lab_disable_hwbp_inatomic_fn_t g_disable_hwbp_inatomic;
static r0lab_disable_hwbp_local_fn_t g_disable_hwbp_local;
static r0lab_unregister_hwbp_fn_t g_unregister_hwbp;
static r0lab_synchronize_rcu_fn_t g_synchronize_rcu;
static r0lab_find_get_task_fn_t g_find_get_task;
static r0lab_put_task_fn_t g_put_task;
static r0lab_kthread_create_fn_t g_kthread_create;
static r0lab_wake_up_process_fn_t g_wake_up_process;
static r0lab_kthread_stop_fn_t g_kthread_stop;
static r0lab_kthread_should_stop_fn_t g_kthread_should_stop;
static r0lab_msleep_fn_t g_msleep;
static r0lab_get_task_mm_fn_t g_get_task_mm;
static r0lab_mmput_fn_t g_mmput;
static r0lab_mmdrop_fn_t g_mmdrop;
static r0lab_get_free_pages_fn_t g_get_free_pages;
static r0lab_free_pages_fn_t g_free_pages;
static r0lab_vmalloc_fn_t g_vmalloc;
static r0lab_vfree_fn_t g_vfree;
static r0lab_vmalloc_to_page_fn_t g_vmalloc_to_page;
static r0lab_down_read_fn_t g_raw_down_read;
static r0lab_up_read_fn_t g_raw_up_read;
static r0lab_raw_spin_fn_t g_raw_spin_lock;
static r0lab_raw_spin_fn_t g_raw_spin_unlock;
static r0lab_find_vma_fn_t g_raw_find_vma;
static r0lab_sync_icache_dcache_fn_t g_sync_icache_dcache;
static r0lab_sync_icache_aliases_fn_t g_sync_icache_aliases;
static r0lab_copy_from_user_nofault_fn_t g_copy_from_user_nofault;
static r0lab_mte_sync_tags_fn_t g_mte_sync_tags;
static void *g_do_mem_abort;
static void *g_handle_mm_fault;
static void *g_dup_mmap;
static void *g_exit_mmap;
static void *g_sys_getpid;
static void *g_sys_prctl;
static void *g_follow_page_pte;
static void *g_follow_page_mask;
static void *g_s4_brk_handler;
static void *g_s4_single_step_handler;
static r0lab_user_step_fn_t g_s4_user_enable_single_step;
static r0lab_user_step_fn_t g_s4_user_disable_single_step;
static uint64_t g_next_seq;
static uint64_t g_m3_generation;
static uint64_t g_m4_generation;
static uint64_t g_raw_generation;
static uint64_t g_s4_generation;
static uint32_t g_hwbp_entry_events;
static uint32_t g_hwbp_return_events;
static unsigned int g_hwbp_callback_log_count;
static bool g_initialized;
static bool g_exit_probe_armed;
static unsigned int g_m3_inflight;
static unsigned int g_m4_inflight;
static unsigned int g_raw_inflight;
static unsigned int g_s4_inflight;
static bool g_m3_arm_delay_once;
static bool g_m3_arm_enomem_once;
static struct task_struct *g_hwbp_workers[R0LAB_HWBP_SLOT_CAPACITY];
static struct task_struct *g_m3_worker;
static struct task_struct *g_m3_monitor_worker_task;
static struct task_struct *g_m4_worker;
static struct task_struct *g_m4_monitor_worker_task;
static struct task_struct *g_raw_worker;
static struct task_struct *g_raw_monitor_worker_task;
static struct task_struct *g_s4_monitor_worker_task;
static struct task_struct *g_session_monitor_worker_task;
static bool g_workers_started;
static bool g_workers_shutdown_requested;
static unsigned int g_workers_live;
static bool g_raw_abort_hook_transitioning;
static hook_chain3_callback g_raw_abort_resident_callback;
static bool g_raw_exit_hook_resident;
static bool g_raw_exit_hook_transitioning;

int64_t memstart_addr;
unsigned long cpu_hwcaps[R0LAB_RAW_HWCAP_WORDS];
unsigned char arm64_const_caps_ready[R0LAB_RAW_STATIC_KEY_SIZE];
unsigned char cpu_hwcap_keys[R0LAB_RAW_ARM64_NCAPS * R0LAB_RAW_STATIC_KEY_SIZE];

void down_read(void *sem)
{
    if (g_raw_down_read)
        g_raw_down_read(sem);
}

void up_read(void *sem)
{
    if (g_raw_up_read)
        g_raw_up_read(sem);
}

void _raw_spin_lock(void *lock)
{
    if (g_raw_spin_lock)
        g_raw_spin_lock(lock);
}

void _raw_spin_unlock(void *lock)
{
    if (g_raw_spin_unlock)
        g_raw_spin_unlock(lock);
}

void *find_vma(void *mm, unsigned long addr)
{
    return g_raw_find_vma ? g_raw_find_vma(mm, addr) : NULL;
}

void *vmalloc_to_page(const void *addr)
{
    return g_vmalloc_to_page ? g_vmalloc_to_page(addr) : NULL;
}

void __sync_icache_dcache(unsigned long pte)
{
    if (g_sync_icache_dcache)
        g_sync_icache_dcache(pte);
}

void __mmdrop(struct mm_struct *mm)
{
    if (g_mmdrop)
        g_mmdrop(mm);
}

void r0lab_runtime_sync_icache_aliases(unsigned long start,
                                      unsigned long end)
{
    if (g_sync_icache_aliases)
        g_sync_icache_aliases(start, end);
}

void mte_sync_tags(unsigned long old_pte, unsigned long new_pte)
{
    if (g_mte_sync_tags)
        g_mte_sync_tags(old_pte, new_pte);
}

/* This FolkPatch KernelPatch-compatible runtime omits KPM kf_* wrappers for these symbols. */
static unsigned long r0lab_lock(void)
{
    return g_lock_irqsave(&g_r0lab_lock.rlock);
}

static void r0lab_unlock(unsigned long flags)
{
    g_unlock_irqrestore(&g_r0lab_lock.rlock, flags);
}

static pid_t r0lab_current_tgid(void)
{
    return g_task_pid(current, PIDTYPE_TGID, NULL);
}

static pid_t r0lab_current_tid(void)
{
    return g_task_pid(current, PIDTYPE_PID, NULL);
}

/*
 * KernelPatch marks chain uninstall/free as unsafe. Detach only this KPM's
 * callback and retain the KernelPatch-owned transit so concurrent callers
 * cannot execute freed hook memory.
 */
static void r0lab_hook_detach(void *func, void *before, void *after)
{
    hook_unwrap_remove(func, before, after, 0);
}

static void r0lab_close_exited_session(pid_t owner_tgid);
static bool r0lab_target_mm_live(pid_t owner_tgid,
                                 struct mm_struct *expected_mm);
static bool r0lab_target_task_live(pid_t owner_tgid);
static uint64_t r0lab_record_values(enum r0lab_event_op op, int result,
                                    uint64_t pc, uint64_t x0, uint64_t x30);
static int r0lab_raw_wait_for_callbacks(void);
static unsigned int r0lab_raw_syscall_hook_users_locked(void);
static unsigned int r0lab_raw_prctl_hook_users_locked(void);
static void r0lab_raw_reset(struct mm_struct *mm);
static void r0lab_raw_reset_final(struct mm_struct *mm, uint64_t generation);
static int r0lab_raw_prepare_shadow(struct r0lab_raw_shadow_page *page);
static void r0lab_raw_deactivate_patch_records_locked(
    struct r0lab_raw_shadow_page *page);

static uint64_t r0lab_now(void)
{
    return g_clock ? g_clock() : 0;
}

static int r0lab_s4_restore_descriptor_pages(bool final);

static bool r0lab_worker_should_stop(void)
{
    unsigned long flags;
    bool shutdown_requested;

    if (g_kthread_should_stop && g_kthread_should_stop())
        return true;
    flags = r0lab_lock();
    shutdown_requested = g_workers_shutdown_requested;
    r0lab_unlock(flags);
    return shutdown_requested;
}

static void r0lab_worker_finished(void)
{
    unsigned long flags = r0lab_lock();

    if (g_workers_live)
        --g_workers_live;
    r0lab_unlock(flags);
}

static uint64_t r0lab_record_regs(enum r0lab_event_op op, int result,
                                  const struct pt_regs *regs)
{
    return r0lab_record_values(op, result, regs ? regs->pc : 0,
                               regs ? regs->regs[0] : 0,
                               regs ? regs->regs[30] : 0);
}

static uint64_t r0lab_record_values(enum r0lab_event_op op, int result,
                                    uint64_t pc, uint64_t x0, uint64_t x30)
{
    struct r0lab_event event;
    unsigned long flags;

    event.monotonic_ticks = r0lab_now();
    event.uid = current_uid();
    event.tid = r0lab_current_tid();
    event.cpu = (int32_t)g_current_cpu();
    event.result = result;
    event.op = op;
    event.reserved = 0;
    event.pc = pc;
    event.x0 = x0;
    event.x30 = x30;

    flags = r0lab_lock();
    event.seq = g_next_seq++;
    g_events[event.seq % R0LAB_EVENT_CAPACITY] = event;
    r0lab_unlock(flags);
    return event.seq;
}

static uint64_t r0lab_record(enum r0lab_event_op op, int result)
{
    return r0lab_record_regs(op, result, NULL);
}

static int r0lab_parse_u64(const char *text, uint64_t *out)
{
    uint64_t value = 0;
    int base = 10;

    if (!text || !text[0] || !out)
        return R0LAB_EINVAL;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
        text += 2;
        if (!text[0])
            return R0LAB_EINVAL;
    }
    while (*text) {
        int digit;

        if (*text >= '0' && *text <= '9')
            digit = *text - '0';
        else if (base == 16 && *text >= 'a' && *text <= 'f')
            digit = *text - 'a' + 10;
        else if (base == 16 && *text >= 'A' && *text <= 'F')
            digit = *text - 'A' + 10;
        else
            return R0LAB_EINVAL;
        if (digit >= base || value > (~0ULL - digit) / base)
            return R0LAB_EINVAL;
        value = value * base + digit;
        ++text;
    }
    *out = value;
    return 0;
}

static int r0lab_parse_lab_uid(const char *args, uid_t *uid)
{
    const char *prefix = "lab_uid=";
    const char *value;
    const char *end;
    char uid_text[17];
    size_t length;
    uint64_t parsed;

    if (!args || !uid)
        return R0LAB_EINVAL;
    value = strstr(args, prefix);
    if (!value)
        return R0LAB_EINVAL;
    value += strlen(prefix);
    end = value;
    while (*end && *end != ' ' && *end != '\t' && *end != ',')
        ++end;
    length = (size_t)(end - value);
    if (!length || length >= sizeof(uid_text))
        return R0LAB_EINVAL;
    memcpy(uid_text, value, length);
    uid_text[length] = '\0';
    if (r0lab_parse_u64(uid_text, &parsed) || parsed > 0xffffffffULL)
        return R0LAB_EINVAL;
    *uid = (uid_t)parsed;
    return 0;
}

static bool r0lab_has_arg_flag(const char *args, const char *flag)
{
    const char *match;
    size_t flag_length;

    if (!args || !flag)
        return false;
    flag_length = strlen(flag);
    for (match = args; (match = strstr(match, flag)); ++match) {
        if ((match == args || match[-1] == ' ' || match[-1] == '\t' ||
             match[-1] == ',') &&
            (!match[flag_length] || match[flag_length] == ' ' ||
             match[flag_length] == '\t' || match[flag_length] == ','))
            return true;
    }
    return false;
}

static int r0lab_copy_reply(char __user *out_msg, int outlen, const char *reply)
{
    int length;

    if (!out_msg || outlen <= 1 || !reply)
        return R0LAB_EINVAL;
    length = strnlen(reply, outlen - 1);
    return compat_copy_to_user(out_msg, reply, length + 1);
}

static int r0lab_validate_lab_caller(void)
{
    if (!g_initialized || current_uid() != g_session.lab_uid)
        return R0LAB_EPERM;
    return 0;
}

static int r0lab_validate_owner(uint64_t token)
{
    unsigned long flags;
    int result = r0lab_validate_lab_caller();

    if (result)
        return result;
    flags = r0lab_lock();
    if (!g_session.active || g_session.owner_tgid != r0lab_current_tgid() || g_session.token != token) {
        r0lab_unlock(flags);
        return R0LAB_EPERM;
    }
    r0lab_unlock(flags);
    return 0;
}

static int r0lab_validate_reader(uint64_t token)
{
    unsigned long flags;
    int result = r0lab_validate_lab_caller();
    pid_t current_tgid;

    if (result)
        return result;
    current_tgid = r0lab_current_tgid();
    flags = r0lab_lock();
    if (g_session.active) {
        result = g_session.owner_tgid == current_tgid && g_session.token == token
                     ? 0 : R0LAB_EPERM;
    } else {
        result = g_session.last_closed_token == token ? 0 : R0LAB_EPERM;
    }
    r0lab_unlock(flags);
    return result;
}

static unsigned int r0lab_hwbp_slot_count_locked(void)
{
    unsigned int index;
    unsigned int count = 0;

    for (index = 0; index < R0LAB_HWBP_SLOT_CAPACITY; ++index) {
        if (g_hwbp_slots[index].reserving || g_hwbp_slots[index].armed ||
            g_hwbp_slots[index].clearing)
            ++count;
    }
    return count;
}

static unsigned int r0lab_m3_slot_count_locked(void)
{
    return g_m3_page.reserving || g_m3_page.armed || g_m3_page.clearing ? 1U : 0U;
}

static unsigned int r0lab_m4_slot_count_locked(void)
{
    return g_m4_page.reserving || g_m4_page.armed || g_m4_page.clearing ? 1U : 0U;
}

static void r0lab_raw_page_slot_reset_locked(
    struct r0lab_raw_shadow_page *page, uint16_t slot_id)
{
    if (!page)
        return;
    memset(page, 0, sizeof(*page));
    page->slot_id = slot_id;
}

static void r0lab_raw_page_table_reset_locked(void)
{
    unsigned int index;

    memset(&g_raw_page_table, 0, sizeof(g_raw_page_table));
    g_raw_page_table.selected_slot = R0LAB_RAW_PRIMARY_SLOT;
    for (index = 0; index < R0LAB_RAW_PAGE_SLOT_CAPACITY; ++index)
        g_raw_page_table.slots[index].slot_id = (uint16_t)index;
}

static bool r0lab_raw_page_slot_owned_locked(
    const struct r0lab_raw_shadow_page *page)
{
    return page && (page->reserving || page->armed || page->clearing);
}

static struct r0lab_raw_shadow_page *r0lab_raw_page_slot_locked(
    uint16_t slot_id)
{
    if (slot_id >= R0LAB_RAW_PAGE_SLOT_CAPACITY)
        return NULL;
    return &g_raw_page_table.slots[slot_id];
}

static struct r0lab_raw_shadow_page *r0lab_raw_selected_page_locked(void)
{
    return r0lab_raw_page_slot_locked(g_raw_page_table.selected_slot);
}

static unsigned int r0lab_raw_page_table_active_count_locked(void)
{
    unsigned int index;
    unsigned int count = 0;

    for (index = 0; index < R0LAB_RAW_PAGE_SLOT_CAPACITY; ++index) {
        if (r0lab_raw_page_slot_owned_locked(&g_raw_page_table.slots[index]))
            ++count;
    }
    return count;
}

static bool r0lab_s4_descriptor_owned_locked(
    const struct r0lab_s4_descriptor *descriptor)
{
    return descriptor && (descriptor->present ||
                          descriptor->state != R0LAB_S4_DESCRIPTOR_EMPTY);
}

static unsigned int r0lab_s4_descriptor_count_locked(void)
{
    unsigned int index;
    unsigned int count = 0;

    for (index = 0; index < R0LAB_RAW_PAGE_SLOT_CAPACITY; ++index) {
        if (r0lab_s4_descriptor_owned_locked(
                &g_raw_page_table.slots[index].s4_descriptor))
            ++count;
    }
    return count;
}

static const struct r0lab_s4_descriptor *
r0lab_s4_selected_descriptor_locked(void)
{
    struct r0lab_raw_shadow_page *page = r0lab_raw_selected_page_locked();

    if (!page)
        return NULL;
    return &page->s4_descriptor;
}

static void r0lab_s4_descriptor_prepare_locked(
    struct r0lab_raw_shadow_page *page, struct mm_struct *mm,
    bool raw_reg_mode)
{
    struct r0lab_s4_descriptor *descriptor;

    if (!page)
        return;
    descriptor = &page->s4_descriptor;
    memset(descriptor, 0, sizeof(*descriptor));
    descriptor->mm = mm;
    descriptor->page_address = page->raw.address;
    descriptor->generation = page->generation;
    descriptor->brk_offset = R0LAB_S4_DESCRIPTOR_BRK_OFFSET;
    descriptor->step_offset = R0LAB_S4_DESCRIPTOR_STEP_OFFSET;
    descriptor->register_index = R0LAB_S4_RAW_REG_INDEX;
    descriptor->register_value = raw_reg_mode ? R0LAB_S4_RAW_REG_VALUE : 0;
    descriptor->slot_id = page->slot_id;
    descriptor->state = R0LAB_S4_DESCRIPTOR_ARMED_SHADOW;
    descriptor->mode = raw_reg_mode ? R0LAB_S4_DESCRIPTOR_MODE_RAW_REG :
                       R0LAB_S4_DESCRIPTOR_MODE_RAW_STEP;
    descriptor->present = true;
}

static bool r0lab_s4_descriptor_raw_mode(uint8_t mode)
{
    return mode == R0LAB_S4_DESCRIPTOR_MODE_RAW_STEP ||
           mode == R0LAB_S4_DESCRIPTOR_MODE_RAW_REG;
}

static bool r0lab_s4_descriptor_register_allowed(
    const struct r0lab_s4_descriptor *descriptor)
{
    return descriptor &&
           descriptor->register_index == R0LAB_S4_RAW_REG_INDEX &&
           descriptor->register_value == R0LAB_S4_RAW_REG_VALUE;
}

static bool r0lab_s4_descriptor_same(
    const struct r0lab_s4_descriptor *left,
    const struct r0lab_s4_descriptor *right)
{
    return left && right && left->mm == right->mm &&
           left->page_address == right->page_address &&
           left->generation == right->generation &&
           left->brk_offset == right->brk_offset &&
           left->step_offset == right->step_offset &&
           left->brk_events == right->brk_events &&
           left->step_events == right->step_events &&
           left->pte_begin_events == right->pte_begin_events &&
           left->pte_finish_events == right->pte_finish_events &&
           left->reject_events == right->reject_events &&
           left->register_index == right->register_index &&
           left->register_value == right->register_value &&
           left->step_tid == right->step_tid &&
           left->slot_id == right->slot_id &&
           left->state == right->state && left->mode == right->mode &&
           left->present == right->present;
}

static bool r0lab_s4_descriptor_brk_matches_locked(
    const struct r0lab_raw_shadow_page *page, unsigned long pc, uint64_t esr)
{
    const struct r0lab_s4_descriptor *descriptor;

    if (!page || !page->armed || page->clearing || page->transitioning ||
        page->raw.state != R0LAB_RAW_SHADOW_RX ||
        (esr & 0xffffU) != R0LAB_S4_BRK_COMMENT)
        return false;
    descriptor = &page->s4_descriptor;
    if (!r0lab_s4_descriptor_owned_locked(descriptor) ||
        descriptor->state != R0LAB_S4_DESCRIPTOR_ARMED_SHADOW ||
        !r0lab_s4_descriptor_raw_mode(descriptor->mode) ||
        descriptor->mm != page->raw.mm ||
        descriptor->page_address != page->raw.address ||
        descriptor->generation != page->generation ||
        descriptor->slot_id != page->slot_id ||
        descriptor->brk_offset >= R0LAB_RAW_PAGE_SIZE ||
        descriptor->step_offset >= R0LAB_RAW_PAGE_SIZE ||
        (descriptor->brk_offset & 3U) ||
        (descriptor->step_offset & 3U))
        return false;
    if (descriptor->mode == R0LAB_S4_DESCRIPTOR_MODE_RAW_REG &&
        !r0lab_s4_descriptor_register_allowed(descriptor))
        return false;
    if (pc < descriptor->page_address ||
        pc >= descriptor->page_address + R0LAB_RAW_PAGE_SIZE)
        return false;
    return pc == descriptor->page_address + descriptor->brk_offset;
}

static bool r0lab_s4_descriptor_step_matches_locked(
    const struct r0lab_raw_shadow_page *page, unsigned long pc)
{
    const struct r0lab_s4_descriptor *descriptor;

    if (!page || !page->armed || page->clearing || page->transitioning ||
        page->raw.state != R0LAB_RAW_ORIGINAL_STEP)
        return false;
    descriptor = &page->s4_descriptor;
    if (!r0lab_s4_descriptor_owned_locked(descriptor) ||
        descriptor->state !=
            R0LAB_S4_DESCRIPTOR_BRK_MATCHED_ORIGINAL_STEP ||
        !r0lab_s4_descriptor_raw_mode(descriptor->mode) ||
        descriptor->mm != page->raw.mm ||
        descriptor->page_address != page->raw.address ||
        descriptor->generation != page->generation ||
        descriptor->slot_id != page->slot_id ||
        descriptor->step_tid != r0lab_current_tid() ||
        descriptor->step_offset >= R0LAB_RAW_PAGE_SIZE ||
        (descriptor->step_offset & 3U))
        return false;
    if (pc < descriptor->page_address ||
        pc >= descriptor->page_address + R0LAB_RAW_PAGE_SIZE)
        return false;
    return pc == descriptor->page_address + descriptor->step_offset;
}

static struct r0lab_raw_shadow_page *
r0lab_s4_descriptor_find_brk_locked(unsigned long pc, uint64_t esr)
{
    unsigned int index;

    for (index = 0; index < R0LAB_RAW_PAGE_SLOT_CAPACITY; ++index) {
        struct r0lab_raw_shadow_page *page =
            &g_raw_page_table.slots[index];

        if (r0lab_s4_descriptor_brk_matches_locked(page, pc, esr))
            return page;
    }
    return NULL;
}

static struct r0lab_raw_shadow_page *
r0lab_s4_descriptor_find_step_locked(unsigned long pc)
{
    unsigned int index;

    for (index = 0; index < R0LAB_RAW_PAGE_SLOT_CAPACITY; ++index) {
        struct r0lab_raw_shadow_page *page =
            &g_raw_page_table.slots[index];

        if (r0lab_s4_descriptor_step_matches_locked(page, pc))
            return page;
    }
    return NULL;
}

static bool r0lab_s4_descriptor_has_armed_locked(void)
{
    unsigned int index;

    for (index = 0; index < R0LAB_RAW_PAGE_SLOT_CAPACITY; ++index) {
        const struct r0lab_s4_descriptor *descriptor =
            &g_raw_page_table.slots[index].s4_descriptor;

        if (r0lab_s4_descriptor_owned_locked(descriptor) &&
            descriptor->state == R0LAB_S4_DESCRIPTOR_ARMED_SHADOW)
            return true;
    }
    return false;
}

static void r0lab_s4_descriptor_clear_step_tid_locked(pid_t tid)
{
    unsigned int index;

    if (!tid)
        return;
    for (index = 0; index < R0LAB_RAW_PAGE_SLOT_CAPACITY; ++index) {
        struct r0lab_s4_descriptor *descriptor =
            &g_raw_page_table.slots[index].s4_descriptor;

        if (descriptor->step_tid == tid)
            descriptor->step_tid = 0;
    }
}

static unsigned long r0lab_raw_page_base(unsigned long address)
{
    return address & ~(R0LAB_RAW_PAGE_SIZE - 1UL);
}

static const char *r0lab_raw_hook_kind_name(enum r0lab_raw_hook_kind kind)
{
    switch (kind) {
    case R0LAB_RAW_HOOK_ABORT:
        return "do_mem_abort";
    case R0LAB_RAW_HOOK_FAULT:
        return "handle_mm_fault";
    case R0LAB_RAW_HOOK_GUP:
        return "gup";
    case R0LAB_RAW_HOOK_FORK:
        return "dup_mmap";
    case R0LAB_RAW_HOOK_SYSCALL:
        return "syscall_getpid";
    case R0LAB_RAW_HOOK_PRCTL:
        return "prctl";
    case R0LAB_RAW_HOOK_EXIT:
        return "exit_mmap";
    default:
        return "none";
    }
}

static struct r0lab_raw_shadow_page *r0lab_raw_page_find_by_mm_addr_locked(
    void *mm, unsigned long address)
{
    unsigned long page_address = r0lab_raw_page_base(address);
    unsigned int index;

    if (!mm || !page_address)
        return NULL;
    for (index = 0; index < R0LAB_RAW_PAGE_SLOT_CAPACITY; ++index) {
        struct r0lab_raw_shadow_page *page =
            &g_raw_page_table.slots[index];

        if (!r0lab_raw_page_slot_owned_locked(page))
            continue;
        if (page->raw.mm == mm && page->raw.address == page_address)
            return page;
    }
    return NULL;
}

static struct r0lab_raw_shadow_page *r0lab_raw_page_find_by_fault_locked(
    void *mm, unsigned long far, unsigned int esr)
{
    (void)esr;
    return r0lab_raw_page_find_by_mm_addr_locked(mm, far);
}

static void r0lab_raw_hook_route_reject_locked(
    struct r0lab_raw_shadow_page *page, bool outside_page, bool wrong_mm,
    bool stale_generation, bool busy, bool wrong_state)
{
    struct r0lab_raw_hook_route_stats *stats =
        page ? &page->hook_route_stats :
               &g_raw_page_table.hook_route_miss_stats;

    ++stats->route_rejects;
    if (outside_page)
        ++stats->route_outside_page;
    if (wrong_mm)
        ++stats->route_wrong_mm;
    if (stale_generation)
        ++stats->route_stale_generation;
    if (busy)
        ++stats->route_busy;
    if (wrong_state)
        ++stats->route_wrong_state;
}

static void r0lab_raw_hook_route_identity_reject_locked(
    struct r0lab_raw_shadow_page *page)
{
    struct r0lab_raw_hook_route_stats *stats =
        page ? &page->hook_route_stats :
               &g_raw_page_table.hook_route_miss_stats;

    ++stats->route_rejects;
    ++stats->route_identity_mismatch;
}

static bool r0lab_raw_hook_route_state_allowed(
    const struct r0lab_raw_shadow_page *page, unsigned int route_flags)
{
    unsigned long state;

    if (!page)
        return false;
    state = page->raw.state;
    if (state == R0LAB_RAW_SHADOW_RX)
        return true;
    if ((route_flags & R0LAB_RAW_HOOK_ROUTE_ALLOW_SHADOW_XOM) &&
        state == R0LAB_RAW_SHADOW_XOM)
        return true;
    if ((route_flags & R0LAB_RAW_HOOK_ROUTE_ALLOW_SOURCE_UXN) &&
        state == R0LAB_RAW_SOURCE_UXN)
        return true;
    if ((route_flags & R0LAB_RAW_HOOK_ROUTE_ALLOW_ORIGINAL_READ) &&
        state == R0LAB_RAW_ORIGINAL_READ)
        return true;
    if (!(route_flags & R0LAB_RAW_HOOK_ROUTE_REQUIRE_SHADOW_RX) &&
        state != R0LAB_RAW_EMPTY && state != R0LAB_RAW_POISONED)
        return true;
    return false;
}

static int r0lab_raw_page_find_for_hook_locked(
    enum r0lab_raw_hook_kind kind, void *mm, unsigned long address,
    uint64_t generation, unsigned int route_flags,
    struct r0lab_raw_hook_page_token *token)
{
    struct r0lab_raw_shadow_page *page;
    bool wrong_state;
    bool busy;

    if (token) {
        token->slot_id = R0LAB_RAW_PRIMARY_SLOT;
        token->generation = 0;
        token->page = NULL;
        token->kind = R0LAB_RAW_HOOK_NONE;
    }
    page = r0lab_raw_page_find_by_mm_addr_locked(mm, address);
    if (!page) {
        r0lab_raw_hook_route_reject_locked(NULL, true, false, false,
                                           false, false);
        return R0LAB_ENOENT;
    }
    if (!mm || page->raw.mm != mm) {
        r0lab_raw_hook_route_reject_locked(page, false, true, false,
                                           false, false);
        return R0LAB_EPERM;
    }
    if (generation && page->generation != generation) {
        r0lab_raw_hook_route_reject_locked(page, false, false, true,
                                           false, false);
        return R0LAB_EAGAIN;
    }
    busy = page->transitioning ||
           ((route_flags & R0LAB_RAW_HOOK_ROUTE_MUTATING) &&
            (page->raw.gup_hide_active || page->raw.fork_hide_active ||
             page->raw.read_cycle_active));
    if (busy) {
        r0lab_raw_hook_route_reject_locked(page, false, false, false,
                                           true, false);
        return R0LAB_EBUSY;
    }
    wrong_state = !page->armed || page->reserving || page->clearing ||
                  !page->generation ||
                  !r0lab_raw_hook_route_state_allowed(page, route_flags);
    if (wrong_state) {
        r0lab_raw_hook_route_reject_locked(page, false, false, false,
                                           false, true);
        return R0LAB_EAGAIN;
    }
    if (!r0lab_raw_saved_identity_matches(&page->raw)) {
        r0lab_raw_hook_route_identity_reject_locked(page);
        return R0LAB_EAGAIN;
    }
    ++page->hook_route_stats.route_hits;
    if (route_flags & R0LAB_RAW_HOOK_ROUTE_MUTATING)
        page->transitioning = true;
    if (token) {
        token->slot_id = page->slot_id;
        token->generation = page->generation;
        token->page = page;
        token->kind = kind;
    }
    return 0;
}

static __maybe_unused int r0lab_raw_hook_page_token_acquire_locked(
    enum r0lab_raw_hook_kind kind, void *mm, unsigned long address,
    uint64_t generation, unsigned int route_flags,
    struct r0lab_raw_hook_page_token *token)
{
    if (!token)
        return R0LAB_EINVAL;
    return r0lab_raw_page_find_for_hook_locked(kind, mm, address, generation,
                                               route_flags, token);
}

static int r0lab_raw_hook_page_token_acquire_readonly_locked(
    enum r0lab_raw_hook_kind kind, void *mm, unsigned long address,
    unsigned int route_flags, struct r0lab_raw_hook_page_token *token)
{
    struct r0lab_raw_shadow_page *page;

    if (!token || kind == R0LAB_RAW_HOOK_NONE ||
        (route_flags & R0LAB_RAW_HOOK_ROUTE_MUTATING))
        return R0LAB_EINVAL;
    token->slot_id = R0LAB_RAW_PRIMARY_SLOT;
    token->generation = 0;
    token->page = NULL;
    token->kind = R0LAB_RAW_HOOK_NONE;
    page = r0lab_raw_page_find_by_mm_addr_locked(mm, address);
    if (!page)
        return R0LAB_ENOENT;
    if (!mm || page->raw.mm != mm)
        return R0LAB_EPERM;
    if (!page->abort_hook_iabt_route)
        return R0LAB_EPERM;
    if (page->transitioning)
        return R0LAB_EBUSY;
    if (!page->armed || page->reserving || page->clearing ||
        !page->generation ||
        !r0lab_raw_hook_route_state_allowed(page, route_flags))
        return R0LAB_EAGAIN;
    if (!r0lab_raw_saved_identity_matches(&page->raw))
        return R0LAB_EAGAIN;
    token->slot_id = page->slot_id;
    token->generation = page->generation;
    token->page = page;
    token->kind = kind;
    return 0;
}

static int r0lab_raw_hook_page_token_acquire_iabt_transition_locked(
    void *mm, unsigned long address, struct r0lab_raw_hook_page_token *token)
{
    struct r0lab_raw_shadow_page *page;

    if (!token)
        return R0LAB_EINVAL;
    token->slot_id = R0LAB_RAW_PRIMARY_SLOT;
    token->generation = 0;
    token->page = NULL;
    token->kind = R0LAB_RAW_HOOK_NONE;
    page = r0lab_raw_page_find_by_mm_addr_locked(mm, address);
    if (!page)
        return R0LAB_ENOENT;
    if (!mm || page->raw.mm != mm)
        return R0LAB_EPERM;
    if (!page->abort_hook_iabt_transition)
        return R0LAB_EPERM;
    if (page->transitioning)
        return R0LAB_EBUSY;
    if (!page->armed || page->reserving || page->clearing ||
        !page->generation || page->raw.gup_hide_active ||
        page->raw.fork_hide_active)
        return R0LAB_EAGAIN;
    if (page->raw.state != R0LAB_RAW_SOURCE_UXN &&
        (page->raw.state != R0LAB_RAW_ORIGINAL_READ ||
         !page->raw.read_cycle_active))
        return R0LAB_EAGAIN;
    if (!r0lab_raw_saved_identity_matches(&page->raw))
        return R0LAB_EAGAIN;
    token->slot_id = page->slot_id;
    token->generation = page->generation;
    token->page = page;
    token->kind = R0LAB_RAW_HOOK_ABORT;
    return 0;
}

static bool r0lab_raw_page_uses_full_abort_callback_locked(
    const struct r0lab_raw_shadow_page *page)
{
    return page && page->hook_installed && !page->abort_hook_suppressed &&
           !page->abort_hook_passthrough && !page->abort_hook_mmget &&
           !page->abort_hook_lock && !page->abort_hook_inflight &&
           !page->abort_hook_iabt_route &&
           !page->abort_hook_iabt_transition;
}

static int r0lab_raw_hook_page_token_acquire_full_abort_locked(
    void *mm, unsigned long address, unsigned int route_flags,
    struct r0lab_raw_hook_page_token *token)
{
    struct r0lab_raw_shadow_page *page;
    bool state_allowed;
    bool original_read_resume;

    if (!token)
        return R0LAB_EINVAL;
    token->slot_id = R0LAB_RAW_PRIMARY_SLOT;
    token->generation = 0;
    token->page = NULL;
    token->kind = R0LAB_RAW_HOOK_NONE;
    page = r0lab_raw_page_find_by_mm_addr_locked(mm, address);
    if (!page)
        return R0LAB_ENOENT;
    if (!mm || page->raw.mm != mm)
        return R0LAB_EPERM;
    if (!r0lab_raw_page_uses_full_abort_callback_locked(page))
        return R0LAB_EPERM;
    original_read_resume =
        (route_flags & R0LAB_RAW_HOOK_ROUTE_ALLOW_ORIGINAL_READ) &&
        page->raw.state == R0LAB_RAW_ORIGINAL_READ &&
        page->raw.read_cycle_active;
    if (route_flags & R0LAB_RAW_HOOK_ROUTE_ALLOW_SOURCE_UXN) {
        state_allowed = page->raw.state == R0LAB_RAW_SOURCE_UXN ||
                        original_read_resume;
    } else {
        state_allowed =
            r0lab_raw_hook_route_state_allowed(page, route_flags);
    }
    if (page->transitioning || page->raw.gup_hide_active ||
        page->raw.fork_hide_active ||
        ((route_flags & R0LAB_RAW_HOOK_ROUTE_MUTATING) &&
         page->raw.read_cycle_active && !original_read_resume))
        return R0LAB_EBUSY;
    if (!page->armed || page->reserving || page->clearing ||
        !page->generation || !state_allowed)
        return R0LAB_EAGAIN;
    if (!r0lab_raw_saved_identity_matches(&page->raw)) {
        r0lab_raw_hook_route_identity_reject_locked(page);
        return R0LAB_EAGAIN;
    }
    token->slot_id = page->slot_id;
    token->generation = page->generation;
    token->page = page;
    token->kind = R0LAB_RAW_HOOK_ABORT;
    return 0;
}

static __maybe_unused int r0lab_raw_hook_page_token_release_locked(
    struct r0lab_raw_hook_page_token *token, bool clear_transition)
{
    struct r0lab_raw_shadow_page *page;

    if (!token || !token->page ||
        token->slot_id >= R0LAB_RAW_PAGE_SLOT_CAPACITY)
        return R0LAB_EINVAL;
    page = r0lab_raw_page_slot_locked(token->slot_id);
    if (page != token->page || page->generation != token->generation)
        return R0LAB_EAGAIN;
    if (clear_transition)
        page->transitioning = false;
    token->page = NULL;
    token->generation = 0;
    token->kind = R0LAB_RAW_HOOK_NONE;
    return 0;
}

static bool r0lab_raw_page_has_aux_state_locked(
    const struct r0lab_raw_shadow_page *page)
{
    return page && (page->s4_shadow_brk_layout || page->s4_shadow_reg_layout ||
                    page->gup_hook_installed || page->gup_hook_uses_pte ||
                    page->fork_hook_installed || page->fault_hook_installed ||
                    page->syscall_hook_installed ||
                    page->syscall_hook_read_cycle_mode ||
                    page->prctl_hook_installed || page->fault_probe_armed ||
                    page->abort_probe_armed ||
                    page->abort_write_release_armed ||
                    page->abort_read_cycle_armed ||
                    page->raw.gup_hide_active ||
                    page->raw.fork_hide_active ||
                    page->raw.read_cycle_active);
}

static bool r0lab_raw_page_table_has_aux_state_locked(void)
{
    unsigned int index;

    for (index = 0; index < R0LAB_RAW_PAGE_SLOT_CAPACITY; ++index) {
        if (r0lab_raw_page_has_aux_state_locked(
                &g_raw_page_table.slots[index]))
            return true;
    }
    return false;
}

static unsigned int r0lab_raw_fault_hook_users_locked(void)
{
    unsigned int index;
    unsigned int count = 0;

    for (index = 0; index < R0LAB_RAW_PAGE_SLOT_CAPACITY; ++index) {
        if (g_raw_page_table.slots[index].fault_hook_installed)
            ++count;
    }
    return count;
}

static unsigned int r0lab_raw_slot_count_locked(void)
{
    return r0lab_raw_page_table_active_count_locked();
}

static unsigned int r0lab_s4_slot_count_locked(void)
{
    return g_s4_brk.reserving || g_s4_brk.armed || g_s4_brk.clearing ? 1U : 0U;
}

static bool r0lab_session_has_slots_locked(void)
{
    return r0lab_hwbp_slot_count_locked() || r0lab_m3_slot_count_locked() ||
           r0lab_m4_slot_count_locked() || r0lab_raw_slot_count_locked() ||
           r0lab_s4_slot_count_locked();
}

static const char *r0lab_page_record_backend_name(uint8_t backend)
{
    switch (backend) {
    case R0LAB_PAGE_RECORD_M4_VISIBLE_CLONE:
        return "visible_clone";
    case R0LAB_PAGE_RECORD_RAW_TWO_PFN:
        return "raw_two_pfn";
    default:
        return "none";
    }
}

static const char *r0lab_page_record_state_name(uint8_t state)
{
    switch (state) {
    case R0LAB_PAGE_RECORD_PREPARING:
        return "preparing";
    case R0LAB_PAGE_RECORD_SOURCE_UXN:
        return "source_uxn";
    case R0LAB_PAGE_RECORD_SHADOW_ACTIVE:
        return "shadow_active";
    case R0LAB_PAGE_RECORD_RESTORING:
        return "restoring";
    case R0LAB_PAGE_RECORD_ORIGINAL_STEP:
        return "original_step";
    case R0LAB_PAGE_RECORD_ORIGINAL_READ:
        return "original_read";
    default:
        return "empty";
    }
}

static const char *r0lab_s4_state_name(uint8_t state)
{
    switch (state) {
    case R0LAB_S4_PREPARING:
        return "preparing";
    case R0LAB_S4_HOOKED:
        return "hooked";
    case R0LAB_S4_BRK_OBSERVED:
        return "brk_observed";
    case R0LAB_S4_STEP_ARMED:
        return "step_armed";
    case R0LAB_S4_STEP_OBSERVED:
        return "step_observed";
    case R0LAB_S4_RESTORING:
        return "restoring";
    default:
        return "empty";
    }
}

static const char *r0lab_s4_descriptor_state_name(uint8_t state)
{
    switch (state) {
    case R0LAB_S4_DESCRIPTOR_ARMED_SHADOW:
        return "armed_shadow";
    case R0LAB_S4_DESCRIPTOR_BRK_MATCHED_ORIGINAL_STEP:
        return "brk_matched_original_step";
    case R0LAB_S4_DESCRIPTOR_STEP_MATCHED_SHADOW:
        return "step_matched_shadow";
    case R0LAB_S4_DESCRIPTOR_CLEARED:
        return "cleared";
    default:
        return "empty";
    }
}

static const char *r0lab_s4_descriptor_mode_name(uint8_t mode)
{
    switch (mode) {
    case R0LAB_S4_DESCRIPTOR_MODE_RAW_STEP:
        return "raw_step";
    case R0LAB_S4_DESCRIPTOR_MODE_RAW_REG:
        return "raw_reg";
    default:
        return "none";
    }
}

static unsigned int r0lab_page_record_count_locked(void)
{
    unsigned int index;
    unsigned int count = 0;

    if (g_m4_page.record.backend != R0LAB_PAGE_RECORD_NONE)
        ++count;
    for (index = 0; index < R0LAB_RAW_PAGE_SLOT_CAPACITY; ++index) {
        if (g_raw_page_table.slots[index].record.backend !=
            R0LAB_PAGE_RECORD_NONE)
            ++count;
    }
    return count;
}

static long r0lab_status(char __user *out_msg, int outlen)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    unsigned long flags;
    bool active;
    pid_t owner_tgid;
    uint64_t next_seq;
    uint64_t last_summary_seq;
    unsigned int hwbp_slots;
    unsigned int m3_slots;
    unsigned int m4_slots;
    unsigned int raw_slots;
    unsigned int s4_slots;
    unsigned int page_records;
    unsigned int raw_page_table_slots;
    unsigned int raw_page_table_active;
    unsigned int raw_inflight;
    uint16_t raw_selected_slot;
    uint8_t raw_slot_backend;
    uint8_t raw_slot_state;
    uint64_t raw_slot_source;
    uint64_t raw_slot_source_pfn;
    uint64_t raw_slot_shadow_pfn;
    uint8_t page_backend;
    uint8_t page_state;
    uint32_t m3_fault_events;
    uint32_t m4_redirect_events;
    uint32_t raw_activation_events;
    uint32_t s4_brk_events;
    uint32_t s4_step_events;
    uint8_t s4_state;
    unsigned int s4_descriptor_slots;
    unsigned int s4_descriptor_active;
    uint16_t s4_descriptor_selected_slot;
    uint8_t s4_descriptor_state;
    uint8_t s4_descriptor_mode;
    uint64_t s4_descriptor_page;
    uint64_t s4_descriptor_generation;
    uint32_t s4_descriptor_brk_offset;
    uint32_t s4_descriptor_step_offset;
    uint32_t hwbp_entry_events;
    uint32_t hwbp_return_events;
    unsigned int workers_live;
    bool workers_shutdown;
    bool raw_abort_resident;
    bool raw_exit_resident;
    int current_cpu;
    struct r0lab_raw_shadow_page *raw_selected;
    const struct r0lab_s4_descriptor *s4_descriptor;

    flags = r0lab_lock();
    active = g_session.active;
    owner_tgid = g_session.owner_tgid;
    next_seq = g_next_seq;
    last_summary_seq = g_session.last_summary_seq;
    hwbp_slots = r0lab_hwbp_slot_count_locked();
    m3_slots = r0lab_m3_slot_count_locked();
    m4_slots = r0lab_m4_slot_count_locked();
    raw_slots = r0lab_raw_slot_count_locked();
    s4_slots = r0lab_s4_slot_count_locked();
    page_records = r0lab_page_record_count_locked();
    raw_page_table_slots = R0LAB_RAW_PAGE_SLOT_CAPACITY;
    raw_page_table_active = r0lab_raw_page_table_active_count_locked();
    raw_inflight = g_raw_inflight;
    raw_selected = r0lab_raw_selected_page_locked();
    raw_selected_slot = raw_selected ? raw_selected->slot_id :
                        R0LAB_RAW_PRIMARY_SLOT;
    if (raw_selected &&
        raw_selected->record.backend != R0LAB_PAGE_RECORD_NONE) {
        raw_slot_backend = raw_selected->record.backend;
        raw_slot_state = raw_selected->record.state;
        raw_slot_source = raw_selected->raw.address;
        raw_slot_source_pfn = raw_selected->raw.source_pfn;
        raw_slot_shadow_pfn = raw_selected->raw.shadow_pfn;
    } else {
        raw_slot_backend = R0LAB_PAGE_RECORD_NONE;
        raw_slot_state = R0LAB_PAGE_RECORD_EMPTY;
        raw_slot_source = 0;
        raw_slot_source_pfn = 0;
        raw_slot_shadow_pfn = 0;
    }
    if (raw_slot_backend != R0LAB_PAGE_RECORD_NONE) {
        page_backend = raw_slot_backend;
        page_state = raw_slot_state;
    } else if (g_m4_page.record.backend != R0LAB_PAGE_RECORD_NONE) {
        page_backend = g_m4_page.record.backend;
        page_state = g_m4_page.record.state;
    } else {
        page_backend = R0LAB_PAGE_RECORD_NONE;
        page_state = R0LAB_PAGE_RECORD_EMPTY;
    }
    m3_fault_events = g_m3_page.fault_events;
    m4_redirect_events = g_m4_page.redirect_events;
    raw_activation_events = g_raw_page.activation_events;
    s4_brk_events = g_s4_brk.brk_events;
    s4_step_events = g_s4_brk.step_events;
    s4_state = g_s4_brk.state;
    s4_descriptor_slots = R0LAB_RAW_PAGE_SLOT_CAPACITY;
    s4_descriptor_active = r0lab_s4_descriptor_count_locked();
    s4_descriptor = r0lab_s4_selected_descriptor_locked();
    s4_descriptor_selected_slot = raw_selected_slot;
    if (r0lab_s4_descriptor_owned_locked(s4_descriptor)) {
        s4_descriptor_selected_slot = s4_descriptor->slot_id;
        s4_descriptor_state = s4_descriptor->state;
        s4_descriptor_mode = s4_descriptor->mode;
        s4_descriptor_page = s4_descriptor->page_address;
        s4_descriptor_generation = s4_descriptor->generation;
        s4_descriptor_brk_offset = s4_descriptor->brk_offset;
        s4_descriptor_step_offset = s4_descriptor->step_offset;
    } else {
        s4_descriptor_state = R0LAB_S4_DESCRIPTOR_EMPTY;
        s4_descriptor_mode = R0LAB_S4_DESCRIPTOR_MODE_NONE;
        s4_descriptor_page = 0;
        s4_descriptor_generation = 0;
        s4_descriptor_brk_offset = 0;
        s4_descriptor_step_offset = 0;
    }
    hwbp_entry_events = g_hwbp_entry_events;
    hwbp_return_events = g_hwbp_return_events;
    workers_live = g_workers_live;
    workers_shutdown = g_workers_shutdown_requested;
    raw_abort_resident = g_raw_abort_resident_callback != NULL;
    raw_exit_resident = g_raw_exit_hook_resident;
    r0lab_unlock(flags);
    current_cpu = (int)g_current_cpu();

    snprintf(reply, sizeof(reply),
             "version=5 lab_uid=%u active=%u owner_tgid=%d next_seq=%llu last_summary_seq=%llu hwbp_slots=%u hwbp_entry_events=%u hwbp_return_events=%u m3_slots=%u m3_fault_events=%u m4_slots=%u m4_redirect_events=%u raw_slots=%u raw_activation_events=%u raw_inflight=%u raw_abort_resident=%u raw_exit_resident=%u s4_slots=%u s4_brk_events=%u s4_step_events=%u s4_state=%s s4_descriptor_slots=%u s4_descriptor_active=%u s4_descriptor_selected_slot=%u s4_descriptor_state=%s s4_descriptor_mode=%s s4_descriptor_page=%llx s4_descriptor_generation=%llu s4_descriptor_brk_offset=%u s4_descriptor_step_offset=%u page_records=%u page_backend=%s page_state=%s raw_page_table_slots=%u raw_page_table_active=%u raw_selected_slot=%u raw_slot_backend=%s raw_slot_state=%s raw_slot_source=%llx raw_slot_source_pfn=%llx raw_slot_shadow_pfn=%llx workers_live=%u workers_shutdown=%u cpu_id=%d clock=sched_clock\n",
             g_session.lab_uid, active, owner_tgid, next_seq, last_summary_seq,
             hwbp_slots, hwbp_entry_events, hwbp_return_events, m3_slots,
             m3_fault_events, m4_slots, m4_redirect_events, raw_slots,
             raw_activation_events, raw_inflight,
             raw_abort_resident ? 1U : 0U,
             raw_exit_resident ? 1U : 0U, s4_slots, s4_brk_events,
             s4_step_events,
             r0lab_s4_state_name(s4_state), s4_descriptor_slots,
             s4_descriptor_active, (unsigned int)s4_descriptor_selected_slot,
             r0lab_s4_descriptor_state_name(s4_descriptor_state),
             r0lab_s4_descriptor_mode_name(s4_descriptor_mode),
             s4_descriptor_page, s4_descriptor_generation,
             s4_descriptor_brk_offset, s4_descriptor_step_offset,
             page_records,
             r0lab_page_record_backend_name(page_backend),
             r0lab_page_record_state_name(page_state), raw_page_table_slots,
             raw_page_table_active, (unsigned int)raw_selected_slot,
             r0lab_page_record_backend_name(raw_slot_backend),
             r0lab_page_record_state_name(raw_slot_state), raw_slot_source,
             raw_slot_source_pfn, raw_slot_shadow_pfn, workers_live,
             workers_shutdown, current_cpu);
    r0lab_record(R0LAB_EVENT_STATUS, 0);
    return r0lab_copy_reply(out_msg, outlen, reply);
}

static long r0lab_s4_abi_probe(char __user *out_msg, int outlen)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    bool ready;

    ready = g_s4_brk_handler && g_s4_single_step_handler &&
            g_s4_user_enable_single_step && g_s4_user_disable_single_step;
    snprintf(reply, sizeof(reply),
             "s4_abi ready=%s brk_handler=%s single_step_handler=%s user_enable_single_step=%s user_disable_single_step=%s brk_addr=%llx step_addr=%llx enable_addr=%llx disable_addr=%llx mode=probe_only next_state=%s\n",
             ready ? "proven" : "blocked",
             g_s4_brk_handler ? "present" : "absent",
             g_s4_single_step_handler ? "present" : "absent",
             g_s4_user_enable_single_step ? "present" : "absent",
             g_s4_user_disable_single_step ? "present" : "absent",
             (uint64_t)(uintptr_t)g_s4_brk_handler,
             (uint64_t)(uintptr_t)g_s4_single_step_handler,
             (uint64_t)(uintptr_t)g_s4_user_enable_single_step,
             (uint64_t)(uintptr_t)g_s4_user_disable_single_step,
             r0lab_s4_state_name(R0LAB_S4_HOOKED));
    return r0lab_copy_reply(out_msg, outlen, reply);
}

static void r0lab_s4_brk_before(hook_fargs3_t *args, void *udata)
{
    struct pt_regs *regs;
    unsigned long flags;
    uint64_t esr;
    uint64_t event_pc = 0;
    uint64_t event_x0 = 0;
    uint64_t event_x30 = 0;
    bool matched = false;
    bool enable_step = false;
    bool step_ready = false;
    bool raw_begin = false;
    bool reg_write = false;
    uint64_t raw_generation = 0;
    uint64_t reg_value = 0;
    struct r0lab_raw_shadow_page *raw_page = NULL;
    int raw_result = 0;

    (void)udata;
    if (!args)
        return;
    esr = args->arg1;
    regs = (struct pt_regs *)(uintptr_t)args->arg2;
    if (!regs)
        return;

    flags = r0lab_lock();
    ++g_s4_inflight;
    if (g_s4_brk.armed && !g_s4_brk.clearing && g_session.active &&
        g_session.owner_tgid == r0lab_current_tgid() &&
        (esr & 0xffffU) == R0LAB_S4_BRK_COMMENT) {
        step_ready = g_s4_brk.step_mode && g_s4_user_enable_single_step &&
                     g_s4_brk.step_hook_installed;
        event_pc = regs->pc;
        event_x0 = regs->regs[0];
        event_x30 = regs->regs[30];
        if (step_ready && g_s4_brk.raw_step_mode &&
            g_s4_brk.state == R0LAB_S4_HOOKED) {
            raw_page = r0lab_s4_descriptor_find_brk_locked(regs->pc, esr);
            if (raw_page) {
                struct r0lab_s4_descriptor *descriptor =
                    &raw_page->s4_descriptor;

                ++g_s4_brk.brk_events;
                ++descriptor->brk_events;
                raw_page->transitioning = true;
                raw_generation = descriptor->generation;
                raw_begin = true;
                matched = true;
            }
        } else if (g_s4_brk.target_address == regs->pc) {
            ++g_s4_brk.brk_events;
            if (step_ready) {
                g_s4_brk.state = R0LAB_S4_STEP_ARMED;
                g_s4_brk.step_tid = r0lab_current_tid();
                ++g_s4_brk.step_enable_events;
                /* Step-mode only: consume BRK and run the fixed Lab instruction. */
                regs->pc = g_s4_brk.target_address + 4;
                args->skip_origin = 1;
                args->ret = 0;
                enable_step = true;
            } else {
                g_s4_brk.state = R0LAB_S4_BRK_OBSERVED;
            }
            matched = true;
        }
    }
    r0lab_unlock(flags);

    if (raw_begin)
        raw_result = r0lab_raw_begin_stepping(&raw_page->raw);

    if (raw_begin) {
        flags = r0lab_lock();
        if (raw_page->generation == raw_generation) {
            struct r0lab_s4_descriptor *descriptor =
                &raw_page->s4_descriptor;

            raw_page->transitioning = false;
            if (!raw_result && g_s4_brk.armed && !g_s4_brk.clearing &&
                g_s4_brk.raw_step_mode &&
                g_s4_brk.state == R0LAB_S4_HOOKED &&
                r0lab_s4_descriptor_owned_locked(descriptor) &&
                descriptor->generation == raw_generation &&
                descriptor->state == R0LAB_S4_DESCRIPTOR_ARMED_SHADOW &&
                r0lab_s4_descriptor_raw_mode(descriptor->mode)) {
                g_s4_brk.state = R0LAB_S4_STEP_ARMED;
                g_s4_brk.step_tid = r0lab_current_tid();
                descriptor->state =
                    R0LAB_S4_DESCRIPTOR_BRK_MATCHED_ORIGINAL_STEP;
                descriptor->step_tid = r0lab_current_tid();
                ++g_s4_brk.step_enable_events;
                ++g_s4_brk.pte_begin_events;
                ++descriptor->pte_begin_events;
                raw_page->record.state = R0LAB_PAGE_RECORD_ORIGINAL_STEP;
                if (descriptor->mode == R0LAB_S4_DESCRIPTOR_MODE_RAW_REG) {
                    reg_value = descriptor->register_value;
                    regs->regs[descriptor->register_index] = reg_value;
                    ++g_s4_brk.reg_write_events;
                    reg_write = true;
                }
                args->skip_origin = 1;
                args->ret = 0;
                enable_step = true;
            }
        }
        r0lab_unlock(flags);
        if (raw_result)
            r0lab_record_values(R0LAB_EVENT_REJECT, raw_result, event_pc,
                                event_x0, event_x30);
    }

    if (enable_step)
        g_s4_user_enable_single_step(current);
    if (matched)
        r0lab_record_values(R0LAB_EVENT_S4_BRK_OBSERVED, 0, event_pc,
                            event_x0, event_x30);
    if (reg_write)
        r0lab_record_values(R0LAB_EVENT_S4_REG_WRITE, 0, event_pc,
                            R0LAB_S4_RAW_REG_INDEX, reg_value);

    flags = r0lab_lock();
    if (g_s4_inflight)
        --g_s4_inflight;
    r0lab_unlock(flags);
}

static void r0lab_s4_step_before(hook_fargs3_t *args, void *udata)
{
    struct pt_regs *regs;
    unsigned long flags;
    bool matched = false;
    bool disable_step = false;
    bool raw_finish = false;
    bool raw_step_mode = false;
    uint64_t raw_generation = 0;
    struct r0lab_raw_shadow_page *raw_page = NULL;
    int raw_result = 0;

    (void)udata;
    if (!args)
        return;
    regs = (struct pt_regs *)(uintptr_t)args->arg2;
    if (!regs)
        return;

    flags = r0lab_lock();
    ++g_s4_inflight;
    if (g_s4_brk.armed && !g_s4_brk.clearing && g_s4_brk.step_mode &&
        g_session.active && g_session.owner_tgid == r0lab_current_tgid()) {
        raw_step_mode = g_s4_brk.raw_step_mode;
        if (raw_step_mode) {
            if (g_s4_brk.state == R0LAB_S4_STEP_ARMED)
                raw_page = r0lab_s4_descriptor_find_step_locked(regs->pc);
            if (raw_page) {
                struct r0lab_s4_descriptor *descriptor =
                    &raw_page->s4_descriptor;

                ++g_s4_brk.step_disable_events;
                disable_step = true;
                raw_page->transitioning = true;
                raw_generation = descriptor->generation;
                raw_finish = true;
                args->skip_origin = 1;
                args->ret = 0;
            }
        } else if (g_s4_brk.state == R0LAB_S4_STEP_ARMED &&
                   g_s4_brk.step_tid == r0lab_current_tid()) {
            ++g_s4_brk.step_disable_events;
            disable_step = true;
            if (regs->pc == g_s4_brk.target_address + 8) {
                ++g_s4_brk.step_events;
                g_s4_brk.state = R0LAB_S4_STEP_OBSERVED;
                matched = true;
            }
            args->skip_origin = 1;
            args->ret = 0;
        }
    }
    r0lab_unlock(flags);

    if (raw_finish)
        raw_result = r0lab_raw_finish_stepping(&raw_page->raw);

    if (raw_finish) {
        flags = r0lab_lock();
        if (raw_page->generation == raw_generation) {
            struct r0lab_s4_descriptor *descriptor =
                &raw_page->s4_descriptor;

            raw_page->transitioning = false;
            if (!raw_result && g_s4_brk.armed && !g_s4_brk.clearing &&
                g_s4_brk.raw_step_mode &&
                r0lab_s4_descriptor_owned_locked(descriptor) &&
                descriptor->generation == raw_generation &&
                descriptor->state ==
                    R0LAB_S4_DESCRIPTOR_BRK_MATCHED_ORIGINAL_STEP &&
                regs->pc == descriptor->page_address +
                    descriptor->step_offset) {
                ++g_s4_brk.step_events;
                ++descriptor->step_events;
                ++g_s4_brk.pte_finish_events;
                ++descriptor->pte_finish_events;
                descriptor->state =
                    R0LAB_S4_DESCRIPTOR_STEP_MATCHED_SHADOW;
                descriptor->step_tid = 0;
                g_s4_brk.state = r0lab_s4_descriptor_has_armed_locked() ?
                    R0LAB_S4_HOOKED : R0LAB_S4_STEP_OBSERVED;
                raw_page->record.state = R0LAB_PAGE_RECORD_SHADOW_ACTIVE;
                matched = true;
            }
        }
        r0lab_unlock(flags);
        if (raw_result)
            r0lab_record_regs(R0LAB_EVENT_REJECT, raw_result, regs);
    }

    if (disable_step) {
        flags = r0lab_lock();
        if (g_s4_brk.step_tid == r0lab_current_tid())
            g_s4_brk.step_tid = 0;
        r0lab_s4_descriptor_clear_step_tid_locked(r0lab_current_tid());
        r0lab_unlock(flags);
    }

    if (disable_step && g_s4_user_disable_single_step)
        g_s4_user_disable_single_step(current);
    if (matched)
        r0lab_record_regs(R0LAB_EVENT_S4_STEP_OBSERVED, 0, regs);

    flags = r0lab_lock();
    if (g_s4_inflight)
        --g_s4_inflight;
    r0lab_unlock(flags);
}

static int r0lab_s4_wait_for_callbacks(void)
{
    unsigned int attempt;

    for (attempt = 0; attempt < R0LAB_M3_DRAIN_LIMIT; ++attempt) {
        unsigned long flags = r0lab_lock();
        bool drained = !g_s4_inflight;

        r0lab_unlock(flags);
        if (drained)
            return 0;
        asm volatile("yield" ::: "memory");
    }
    return R0LAB_EBUSY;
}

static void r0lab_s4_unhook(void)
{
    bool brk_installed;
    bool step_installed;
    unsigned long flags = r0lab_lock();

    brk_installed = g_s4_brk.hook_installed;
    step_installed = g_s4_brk.step_hook_installed;
    g_s4_brk.hook_installed = false;
    g_s4_brk.step_hook_installed = false;
    r0lab_unlock(flags);
    if (brk_installed || step_installed)
        (void)r0lab_s4_wait_for_callbacks();
    if (step_installed)
        r0lab_hook_detach(g_s4_single_step_handler, r0lab_s4_step_before,
                          NULL);
    if (brk_installed)
        r0lab_hook_detach(g_s4_brk_handler, r0lab_s4_brk_before, NULL);
    if (brk_installed || step_installed)
        (void)r0lab_s4_wait_for_callbacks();
}

static bool r0lab_s4_raw_state_needs_restore(unsigned long state)
{
    return state == R0LAB_RAW_SOURCE_UXN || state == R0LAB_RAW_SHADOW_RX ||
           state == R0LAB_RAW_SHADOW_XOM ||
           state == R0LAB_RAW_ORIGINAL_STEP ||
           state == R0LAB_RAW_ORIGINAL_READ;
}

static bool r0lab_s4_raw_page_owned_locked(
    const struct r0lab_raw_shadow_page *page)
{
    return r0lab_raw_page_slot_owned_locked(page) &&
           (r0lab_s4_descriptor_owned_locked(&page->s4_descriptor) ||
            page->s4_shadow_brk_layout || page->s4_shadow_reg_layout);
}

static void r0lab_s4_prepare_descriptor_slot_locked(
    struct r0lab_raw_shadow_page *slot, uint16_t slot_id,
    struct mm_struct *raw_mm, uint64_t page_address)
{
    r0lab_raw_page_slot_reset_locked(slot, slot_id);
    slot->raw.mm = raw_mm;
    slot->mm_users_owned = true;
    slot->raw.address = (unsigned long)page_address;
    slot->generation = ++g_raw_generation;
    slot->record.source_address = slot->raw.address;
    slot->record.generation = slot->generation;
    slot->record.backend = R0LAB_PAGE_RECORD_RAW_TWO_PFN;
    slot->record.state = R0LAB_PAGE_RECORD_PREPARING;
    slot->s4_shadow_brk_layout = true;
    slot->s4_shadow_reg_layout = false;
    r0lab_s4_descriptor_prepare_locked(slot, raw_mm, false);
    slot->reserving = true;
}

static int r0lab_s4_activate_descriptor_pages(void)
{
    unsigned int index;

    for (index = 0; index < R0LAB_RAW_PAGE_SLOT_CAPACITY; ++index) {
        struct r0lab_raw_shadow_page *page =
            &g_raw_page_table.slots[index];
        int result;

        result = r0lab_raw_prepare_shadow(page);
        if (!result)
            result = r0lab_raw_arm_source_uxn(&page->raw);
        if (!result)
            result = r0lab_raw_activate_shadow(&page->raw);
        if (result)
            return result;
    }
    return 0;
}

static void r0lab_s4_reset(struct mm_struct *mm)
{
    unsigned long flags = r0lab_lock();

    if (g_s4_brk.mm == mm) {
        bool monitor_running = g_s4_brk.monitor_running;
        uint64_t generation = g_s4_brk.generation;

        memset(&g_s4_brk, 0, sizeof(g_s4_brk));
        g_s4_brk.generation = generation;
        g_s4_brk.monitor_running = monitor_running;
    }
    r0lab_unlock(flags);
    if (mm)
        g_mmput(mm);
}

static void r0lab_s4_reset_final(struct mm_struct *mm, uint64_t generation)
{
    unsigned long flags = r0lab_lock();

    if (g_s4_brk.mm == mm && g_s4_brk.generation == generation)
        memset(&g_s4_brk, 0, sizeof(g_s4_brk));
    r0lab_unlock(flags);
    if (mm)
        g_mmput(mm);
}

static void r0lab_s4_monitor_done(uint64_t generation)
{
    unsigned long flags = r0lab_lock();

    if (g_s4_brk.generation == generation)
        g_s4_brk.monitor_running = false;
    r0lab_unlock(flags);
}

static int r0lab_s4_monitor_worker(void *opaque)
{
    struct r0lab_s4_breakpoint *slot = opaque;
    struct mm_struct *mm;
    unsigned long flags;
    uint64_t generation;
    pid_t owner_tgid;
    bool raw_step_mode;
    int result;

    flags = r0lab_lock();
    if (!slot || slot != &g_s4_brk || !slot->monitor_running ||
        !slot->generation) {
        r0lab_unlock(flags);
        return 0;
    }
    generation = slot->generation;
    raw_step_mode = slot->raw_step_mode;
    r0lab_unlock(flags);

    for (;;) {
        bool active;
        bool slot_owned;
        bool already_clearing;

        flags = r0lab_lock();
        if (g_s4_brk.generation != generation ||
            !g_s4_brk.monitor_running) {
            r0lab_unlock(flags);
            return 0;
        }
        mm = g_s4_brk.mm;
        owner_tgid = g_session.owner_tgid;
        active = g_session.active;
        slot_owned = g_s4_brk.reserving || g_s4_brk.armed ||
                     g_s4_brk.clearing;
        already_clearing = g_s4_brk.clearing && !g_s4_brk.target_exiting;
        if (!mm || !active || !slot_owned) {
            g_s4_brk.monitor_running = false;
            r0lab_unlock(flags);
            return 0;
        }
        r0lab_unlock(flags);

        if (already_clearing) {
            g_msleep(R0LAB_M5_MONITOR_INTERVAL_MS);
            continue;
        }
        if (!r0lab_target_mm_live(owner_tgid, mm))
            break;
        g_msleep(R0LAB_M5_MONITOR_INTERVAL_MS);
    }

    flags = r0lab_lock();
    if (g_s4_brk.generation != generation || g_s4_brk.mm != mm) {
        r0lab_unlock(flags);
        r0lab_s4_monitor_done(generation);
        return 0;
    }
    g_s4_brk.target_exiting = true;
    g_s4_brk.clearing = true;
    g_s4_brk.armed = false;
    g_s4_brk.state = R0LAB_S4_RESTORING;
    r0lab_unlock(flags);

    r0lab_record(R0LAB_EVENT_TARGET_EXIT, 0);
    r0lab_s4_unhook();
    result = r0lab_s4_wait_for_callbacks();
    if (!result && raw_step_mode)
        result = r0lab_s4_restore_descriptor_pages(true);
    r0lab_s4_reset_final(mm, generation);
    r0lab_close_exited_session(owner_tgid);
    r0lab_record(R0LAB_EVENT_S4_CLEAR, result);
    return 0;
}

static int r0lab_s4_start_monitor(void)
{
    unsigned long flags;

    flags = r0lab_lock();
    if (!g_s4_brk.armed || g_s4_brk.monitor_running) {
        r0lab_unlock(flags);
        return R0LAB_EBUSY;
    }
    g_s4_brk.monitor_running = true;
    r0lab_unlock(flags);

    if (!g_s4_monitor_worker_task)
        return R0LAB_ESRCH;
    g_wake_up_process(g_s4_monitor_worker_task);
    return 0;
}

static int r0lab_s4_monitor_loop(void *opaque)
{
    (void)opaque;
    while (!r0lab_worker_should_stop()) {
        bool monitor_pending;
        unsigned long flags = r0lab_lock();

        monitor_pending = g_s4_brk.monitor_running;
        r0lab_unlock(flags);
        if (monitor_pending) {
            r0lab_s4_monitor_worker(&g_s4_brk);
            continue;
        }
        g_msleep(R0LAB_M5_MONITOR_INTERVAL_MS);
    }
    r0lab_worker_finished();
    return 0;
}

static long r0lab_s4_brk_arm(uint64_t token, uint64_t target_address,
                             bool step_mode, bool raw_step_mode,
                             bool raw_reg_mode,
                             char __user *out_msg, int outlen)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    struct mm_struct *mm;
    struct mm_struct *raw_mm = NULL;
    unsigned long flags;
    hook_err_t brk_hook_result;
    hook_err_t step_hook_result = 0;
    int result = r0lab_validate_owner(token);

    if (result)
        goto record;
    if (!g_s4_brk_handler ||
        (step_mode && (!g_s4_single_step_handler ||
                       !g_s4_user_enable_single_step ||
                       !g_s4_user_disable_single_step))) {
        result = R0LAB_ENOSYS;
        goto record;
    }
    if (raw_reg_mode && !raw_step_mode) {
        result = R0LAB_EINVAL;
        goto record;
    }
    if (!target_address || (target_address & 3ULL) ||
        (raw_step_mode && (target_address & (R0LAB_RAW_PAGE_SIZE - 1UL)))) {
        result = R0LAB_EINVAL;
        goto record;
    }
    mm = g_get_task_mm(current);
    if (!mm) {
        result = R0LAB_ESRCH;
        goto record;
    }
    if (raw_step_mode) {
        raw_mm = g_get_task_mm(current);
        if (!raw_mm) {
            g_mmput(mm);
            result = R0LAB_ESRCH;
            goto record;
        }
    }

    flags = r0lab_lock();
    if (!g_session.active || g_session.owner_tgid != r0lab_current_tgid() ||
        g_session.token != token || r0lab_hwbp_slot_count_locked() ||
        r0lab_m3_slot_count_locked() || r0lab_m4_slot_count_locked() ||
        r0lab_raw_slot_count_locked() || r0lab_s4_slot_count_locked()) {
        r0lab_unlock(flags);
        g_mmput(mm);
        if (raw_mm)
            g_mmput(raw_mm);
        result = R0LAB_EBUSY;
        goto record;
    }
    memset(&g_s4_brk, 0, sizeof(g_s4_brk));
    if (raw_step_mode) {
        r0lab_raw_page_slot_reset_locked(&g_raw_page, R0LAB_RAW_PRIMARY_SLOT);
        g_raw_page.raw.mm = raw_mm;
        g_raw_page.mm_users_owned = true;
        g_raw_page.raw.address = (unsigned long)target_address;
        g_raw_page.generation = ++g_raw_generation;
        g_raw_page.record.source_address = g_raw_page.raw.address;
        g_raw_page.record.generation = g_raw_page.generation;
        g_raw_page.record.backend = R0LAB_PAGE_RECORD_RAW_TWO_PFN;
        g_raw_page.record.state = R0LAB_PAGE_RECORD_PREPARING;
        g_raw_page.s4_shadow_brk_layout = true;
        g_raw_page.s4_shadow_reg_layout = raw_reg_mode;
        r0lab_s4_descriptor_prepare_locked(&g_raw_page, raw_mm,
                                           raw_reg_mode);
        g_raw_page.reserving = true;
    }
    g_s4_brk.mm = mm;
    g_s4_brk.target_address = (unsigned long)target_address;
    g_s4_brk.generation = ++g_s4_generation;
    g_s4_brk.state = R0LAB_S4_PREPARING;
    g_s4_brk.step_mode = step_mode;
    g_s4_brk.raw_step_mode = raw_step_mode;
    g_s4_brk.raw_reg_mode = raw_reg_mode;
    g_s4_brk.reg_value = raw_reg_mode ? R0LAB_S4_RAW_REG_VALUE : 0;
    g_s4_brk.reserving = true;
    r0lab_unlock(flags);
    if (raw_step_mode) {
        result = r0lab_raw_prepare_shadow(&g_raw_page);
        if (!result)
            result = r0lab_raw_arm_source_uxn(&g_raw_page.raw);
        if (!result)
            result = r0lab_raw_activate_shadow(&g_raw_page.raw);
        if (result)
            goto fail_reserved;
    }

    brk_hook_result = hook_wrap3(g_s4_brk_handler, r0lab_s4_brk_before, NULL,
                                 NULL);
    result = (int)brk_hook_result;
    if (!result && step_mode) {
        step_hook_result = hook_wrap3(g_s4_single_step_handler,
                                      r0lab_s4_step_before, NULL, NULL);
        result = (int)step_hook_result;
        if (result)
            r0lab_hook_detach(g_s4_brk_handler, r0lab_s4_brk_before, NULL);
    }

    flags = r0lab_lock();
    if (!result) {
        g_s4_brk.reserving = false;
        g_s4_brk.armed = true;
        g_s4_brk.hook_installed = true;
        g_s4_brk.step_hook_installed = step_mode;
        g_s4_brk.state = R0LAB_S4_HOOKED;
        if (raw_step_mode) {
            g_raw_page.reserving = false;
            g_raw_page.armed = true;
            g_raw_page.record.state = R0LAB_PAGE_RECORD_SHADOW_ACTIVE;
        }
    } else {
        if (raw_step_mode)
            g_raw_page.record.state = R0LAB_PAGE_RECORD_RESTORING;
    }
    r0lab_unlock(flags);
    if (result) {
fail_reserved:
        if (raw_step_mode) {
            int restore_result = r0lab_s4_restore_descriptor_pages(false);

            if (restore_result)
                result = restore_result;
        }
        r0lab_s4_reset(mm);
        goto record;
    }

    result = r0lab_s4_start_monitor();
    if (result) {
        r0lab_s4_unhook();
        if (raw_step_mode) {
            int restore_result = r0lab_s4_restore_descriptor_pages(false);

            if (restore_result)
                result = restore_result;
        }
        r0lab_s4_reset(mm);
        goto record;
    }
    r0lab_record(R0LAB_EVENT_S4_ARM, 0);
    if (raw_reg_mode)
        snprintf(reply, sizeof(reply),
                 "s4_raw_reg_ready target=%llx state=%s mode=raw_reg brk_skip_origin=1 step_skip_origin=1 single_step=1 pte_switch=1 register_edit=1 register_index=%u register_value=%llu register_apply=brk_before_single_step raw_state=shadow_active\n",
                 target_address, r0lab_s4_state_name(R0LAB_S4_HOOKED),
                 R0LAB_S4_RAW_REG_INDEX, R0LAB_S4_RAW_REG_VALUE);
    else if (raw_step_mode)
        snprintf(reply, sizeof(reply),
                 "s4_raw_step_ready target=%llx state=%s mode=raw_step brk_skip_origin=1 step_skip_origin=1 single_step=1 pte_switch=1 raw_state=shadow_active\n",
                 target_address, r0lab_s4_state_name(R0LAB_S4_HOOKED));
    else if (step_mode)
        snprintf(reply, sizeof(reply),
                 "s4_step_ready target=%llx state=%s mode=step_only brk_skip_origin=1 step_skip_origin=1 single_step=1 pte_switch=0\n",
                 target_address, r0lab_s4_state_name(R0LAB_S4_HOOKED));
    else
        snprintf(reply, sizeof(reply),
                 "s4_brk_ready target=%llx state=%s mode=brk_only skip_origin=0 single_step=0 pte_switch=0\n",
                 target_address, r0lab_s4_state_name(R0LAB_S4_HOOKED));
    return r0lab_copy_reply(out_msg, outlen, reply);

record:
    r0lab_record(R0LAB_EVENT_REJECT, result);
    return result;
}

static long r0lab_s4_descriptor_routing_arm(uint64_t token, uint64_t page0,
                                            uint64_t page1,
                                            char __user *out_msg, int outlen)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    struct mm_struct *mm;
    struct mm_struct *raw_mms[R0LAB_RAW_PAGE_SLOT_CAPACITY] = {NULL, NULL};
    uint64_t pages[R0LAB_RAW_PAGE_SLOT_CAPACITY];
    unsigned long flags;
    hook_err_t brk_hook_result;
    hook_err_t step_hook_result;
    unsigned int index;
    int result = r0lab_validate_owner(token);

    if (result)
        goto record;
    if (!g_s4_brk_handler || !g_s4_single_step_handler ||
        !g_s4_user_enable_single_step || !g_s4_user_disable_single_step) {
        result = R0LAB_ENOSYS;
        goto record;
    }
    if (!page0 || !page1 || page0 == page1 ||
        (page0 & (R0LAB_RAW_PAGE_SIZE - 1UL)) ||
        (page1 & (R0LAB_RAW_PAGE_SIZE - 1UL))) {
        result = R0LAB_EINVAL;
        goto record;
    }
    mm = g_get_task_mm(current);
    if (!mm) {
        result = R0LAB_ESRCH;
        goto record;
    }
    for (index = 0; index < R0LAB_RAW_PAGE_SLOT_CAPACITY; ++index) {
        raw_mms[index] = g_get_task_mm(current);
        if (!raw_mms[index]) {
            while (index)
                g_mmput(raw_mms[--index]);
            g_mmput(mm);
            result = R0LAB_ESRCH;
            goto record;
        }
    }

    pages[0] = page0;
    pages[1] = page1;
    flags = r0lab_lock();
    if (!g_session.active || g_session.owner_tgid != r0lab_current_tgid() ||
        g_session.token != token || r0lab_hwbp_slot_count_locked() ||
        r0lab_m3_slot_count_locked() || r0lab_m4_slot_count_locked() ||
        r0lab_raw_slot_count_locked() || r0lab_s4_slot_count_locked() ||
        r0lab_raw_page_table_has_aux_state_locked()) {
        r0lab_unlock(flags);
        for (index = 0; index < R0LAB_RAW_PAGE_SLOT_CAPACITY; ++index)
            g_mmput(raw_mms[index]);
        g_mmput(mm);
        result = R0LAB_EBUSY;
        goto record;
    }

    memset(&g_s4_brk, 0, sizeof(g_s4_brk));
    for (index = 0; index < R0LAB_RAW_PAGE_SLOT_CAPACITY; ++index)
        r0lab_s4_prepare_descriptor_slot_locked(
            &g_raw_page_table.slots[index], (uint16_t)index, raw_mms[index],
            pages[index]);
    g_raw_page_table.selected_slot = R0LAB_RAW_PRIMARY_SLOT;
    g_s4_brk.mm = mm;
    g_s4_brk.target_address = (unsigned long)page0;
    g_s4_brk.generation = ++g_s4_generation;
    g_s4_brk.state = R0LAB_S4_PREPARING;
    g_s4_brk.step_mode = true;
    g_s4_brk.raw_step_mode = true;
    g_s4_brk.reserving = true;
    r0lab_unlock(flags);

    result = r0lab_s4_activate_descriptor_pages();
    if (result)
        goto fail_reserved;

    brk_hook_result = hook_wrap3(g_s4_brk_handler, r0lab_s4_brk_before, NULL,
                                 NULL);
    result = (int)brk_hook_result;
    if (!result) {
        step_hook_result = hook_wrap3(g_s4_single_step_handler,
                                      r0lab_s4_step_before, NULL, NULL);
        result = (int)step_hook_result;
        if (result)
            r0lab_hook_detach(g_s4_brk_handler, r0lab_s4_brk_before, NULL);
    }

    flags = r0lab_lock();
    if (!result) {
        g_s4_brk.reserving = false;
        g_s4_brk.armed = true;
        g_s4_brk.hook_installed = true;
        g_s4_brk.step_hook_installed = true;
        g_s4_brk.state = R0LAB_S4_HOOKED;
        for (index = 0; index < R0LAB_RAW_PAGE_SLOT_CAPACITY; ++index) {
            struct r0lab_raw_shadow_page *slot =
                &g_raw_page_table.slots[index];

            slot->reserving = false;
            slot->armed = true;
            slot->record.state = R0LAB_PAGE_RECORD_SHADOW_ACTIVE;
        }
    } else {
        for (index = 0; index < R0LAB_RAW_PAGE_SLOT_CAPACITY; ++index)
            g_raw_page_table.slots[index].record.state =
                R0LAB_PAGE_RECORD_RESTORING;
    }
    r0lab_unlock(flags);
    if (result)
        goto fail_reserved;

    result = r0lab_s4_start_monitor();
    if (result) {
        r0lab_s4_unhook();
        goto fail_reserved;
    }

    r0lab_record(R0LAB_EVENT_S4_ARM, 0);
    snprintf(reply, sizeof(reply),
             "s4_descriptor_routing_ready state=%s mode=raw_step slots=2 slot0_page=%llx slot1_page=%llx brk_skip_origin=1 step_skip_origin=1 single_step=1 pte_switch=1 raw_state=shadow_active s4_descriptor_active=2 raw_slots=2 raw_page_table_active=2\n",
             r0lab_s4_state_name(R0LAB_S4_HOOKED), page0, page1);
    return r0lab_copy_reply(out_msg, outlen, reply);

fail_reserved:
    {
        int restore_result = r0lab_s4_restore_descriptor_pages(false);

        if (restore_result)
            result = restore_result;
    }
    r0lab_s4_reset(mm);

record:
    r0lab_record(R0LAB_EVENT_REJECT, result);
    return result;
}

static long r0lab_s4_descriptor_routing_observed(uint64_t token,
                                                 char __user *out_msg,
                                                 int outlen)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    unsigned long flags;
    uint64_t pages[R0LAB_RAW_PAGE_SLOT_CAPACITY] = {0, 0};
    uint64_t generations[R0LAB_RAW_PAGE_SLOT_CAPACITY] = {0, 0};
    uint32_t brk_slot_events[R0LAB_RAW_PAGE_SLOT_CAPACITY] = {0, 0};
    uint32_t step_slot_events[R0LAB_RAW_PAGE_SLOT_CAPACITY] = {0, 0};
    uint32_t pte_begin_slot_events[R0LAB_RAW_PAGE_SLOT_CAPACITY] = {0, 0};
    uint32_t pte_finish_slot_events[R0LAB_RAW_PAGE_SLOT_CAPACITY] = {0, 0};
    uint8_t descriptor_states[R0LAB_RAW_PAGE_SLOT_CAPACITY] = {0, 0};
    uint8_t record_states[R0LAB_RAW_PAGE_SLOT_CAPACITY] = {0, 0};
    uint32_t brk_events;
    uint32_t step_events;
    uint32_t enable_events;
    uint32_t disable_events;
    uint32_t pte_begin_events;
    uint32_t pte_finish_events;
    unsigned int active_descriptors;
    uint8_t state;
    unsigned int index;
    int result = r0lab_validate_owner(token);

    if (result)
        return result;
    flags = r0lab_lock();
    if (!g_s4_brk.armed || g_s4_brk.clearing || !g_s4_brk.step_mode ||
        !g_s4_brk.raw_step_mode || g_s4_brk.raw_reg_mode) {
        r0lab_unlock(flags);
        return R0LAB_EAGAIN;
    }
    brk_events = g_s4_brk.brk_events;
    step_events = g_s4_brk.step_events;
    enable_events = g_s4_brk.step_enable_events;
    disable_events = g_s4_brk.step_disable_events;
    pte_begin_events = g_s4_brk.pte_begin_events;
    pte_finish_events = g_s4_brk.pte_finish_events;
    state = g_s4_brk.state;
    active_descriptors = r0lab_s4_descriptor_count_locked();
    for (index = 0; index < R0LAB_RAW_PAGE_SLOT_CAPACITY; ++index) {
        const struct r0lab_raw_shadow_page *page =
            &g_raw_page_table.slots[index];
        const struct r0lab_s4_descriptor *descriptor =
            &page->s4_descriptor;

        pages[index] = descriptor->page_address;
        generations[index] = descriptor->generation;
        brk_slot_events[index] = descriptor->brk_events;
        step_slot_events[index] = descriptor->step_events;
        pte_begin_slot_events[index] = descriptor->pte_begin_events;
        pte_finish_slot_events[index] = descriptor->pte_finish_events;
        descriptor_states[index] = descriptor->state;
        record_states[index] = page->record.state;
    }
    r0lab_unlock(flags);

    snprintf(reply, sizeof(reply),
             "s4_descriptor_routing_observed slots=2 active=%u brk_events=%u step_events=%u enable_events=%u disable_events=%u pte_begin_events=%u pte_finish_events=%u state=%s mode=raw_step pte_switch=1 slot0_page=%llx slot0_generation=%llu slot0_brk_events=%u slot0_step_events=%u slot0_pte_begin_events=%u slot0_pte_finish_events=%u slot0_state=%s slot0_record_state=%s slot1_page=%llx slot1_generation=%llu slot1_brk_events=%u slot1_step_events=%u slot1_pte_begin_events=%u slot1_pte_finish_events=%u slot1_state=%s slot1_record_state=%s\n",
             active_descriptors, brk_events, step_events, enable_events,
             disable_events, pte_begin_events, pte_finish_events,
             r0lab_s4_state_name(state), pages[0], generations[0],
             brk_slot_events[0], step_slot_events[0],
             pte_begin_slot_events[0], pte_finish_slot_events[0],
             r0lab_s4_descriptor_state_name(descriptor_states[0]),
             r0lab_page_record_state_name(record_states[0]), pages[1],
             generations[1], brk_slot_events[1], step_slot_events[1],
             pte_begin_slot_events[1], pte_finish_slot_events[1],
             r0lab_s4_descriptor_state_name(descriptor_states[1]),
             r0lab_page_record_state_name(record_states[1]));
    return r0lab_copy_reply(out_msg, outlen, reply);
}

static long r0lab_s4_descriptor_negative_probe(uint64_t token,
                                               char __user *out_msg,
                                               int outlen)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    struct r0lab_raw_shadow_page *slot0;
    struct r0lab_raw_shadow_page *slot1;
    struct r0lab_s4_descriptor saved0;
    struct r0lab_s4_descriptor saved1;
    unsigned long raw_state0;
    unsigned long raw_state1;
    bool transitioning0;
    bool transitioning1;
    unsigned long flags;
    pid_t tid;
    uint32_t brk_events;
    uint32_t step_events;
    uint32_t enable_events;
    uint32_t disable_events;
    uint32_t pte_begin_events;
    uint32_t pte_finish_events;
    unsigned int active_descriptors;
    unsigned int raw_slots;
    unsigned int raw_page_table_active;
    unsigned int baseline_brk_matches = 0;
    unsigned int baseline_step_matches = 0;
    unsigned int reject_checks = 0;
    bool bad_brk_offset_rejected = false;
    bool bad_step_offset_rejected = false;
    bool stale_brk_generation_rejected = false;
    bool stale_step_generation_rejected = false;
    bool wrong_brk_slot_rejected = false;
    bool wrong_step_slot_rejected = false;
    bool bad_register_index_rejected = false;
    bool bad_register_value_rejected = false;
    bool cross_slot_brk_rejected = false;
    bool cross_slot_step_rejected = false;
    bool wrong_step_tid_rejected = false;
    bool state_intact;
    int result = r0lab_validate_owner(token);

    if (result)
        return result;

    flags = r0lab_lock();
    slot0 = &g_raw_page_table.slots[0];
    slot1 = &g_raw_page_table.slots[1];
    if (!g_s4_brk.armed || g_s4_brk.clearing || !g_s4_brk.step_mode ||
        !g_s4_brk.raw_step_mode || g_s4_brk.raw_reg_mode ||
        g_s4_brk.state != R0LAB_S4_HOOKED ||
        !r0lab_raw_page_slot_owned_locked(slot0) ||
        !r0lab_raw_page_slot_owned_locked(slot1) ||
        !r0lab_s4_descriptor_owned_locked(&slot0->s4_descriptor) ||
        !r0lab_s4_descriptor_owned_locked(&slot1->s4_descriptor) ||
        slot0->raw.state != R0LAB_RAW_SHADOW_RX ||
        slot1->raw.state != R0LAB_RAW_SHADOW_RX) {
        r0lab_unlock(flags);
        return R0LAB_EAGAIN;
    }

    saved0 = slot0->s4_descriptor;
    saved1 = slot1->s4_descriptor;
    raw_state0 = slot0->raw.state;
    raw_state1 = slot1->raw.state;
    transitioning0 = slot0->transitioning;
    transitioning1 = slot1->transitioning;
    tid = r0lab_current_tid();

    if (r0lab_s4_descriptor_brk_matches_locked(
            slot0, saved0.page_address + saved0.brk_offset,
            R0LAB_S4_BRK_COMMENT))
        ++baseline_brk_matches;
    if (r0lab_s4_descriptor_brk_matches_locked(
            slot1, saved1.page_address + saved1.brk_offset,
            R0LAB_S4_BRK_COMMENT))
        ++baseline_brk_matches;

    slot0->raw.state = R0LAB_RAW_ORIGINAL_STEP;
    slot0->s4_descriptor.state =
        R0LAB_S4_DESCRIPTOR_BRK_MATCHED_ORIGINAL_STEP;
    slot0->s4_descriptor.step_tid = tid;
    slot1->raw.state = R0LAB_RAW_ORIGINAL_STEP;
    slot1->s4_descriptor.state =
        R0LAB_S4_DESCRIPTOR_BRK_MATCHED_ORIGINAL_STEP;
    slot1->s4_descriptor.step_tid = tid;
    if (r0lab_s4_descriptor_step_matches_locked(
            slot0, saved0.page_address + saved0.step_offset))
        ++baseline_step_matches;
    if (r0lab_s4_descriptor_step_matches_locked(
            slot1, saved1.page_address + saved1.step_offset))
        ++baseline_step_matches;

    slot0->s4_descriptor = saved0;
    slot1->s4_descriptor = saved1;
    slot0->raw.state = raw_state0;
    slot1->raw.state = raw_state1;

    slot0->s4_descriptor.brk_offset = R0LAB_RAW_PAGE_SIZE;
    if (!r0lab_s4_descriptor_brk_matches_locked(
            slot0, saved0.page_address + saved0.brk_offset,
            R0LAB_S4_BRK_COMMENT)) {
        bad_brk_offset_rejected = true;
        ++reject_checks;
    }
    slot0->s4_descriptor = saved0;

    slot0->raw.state = R0LAB_RAW_ORIGINAL_STEP;
    slot0->s4_descriptor.state =
        R0LAB_S4_DESCRIPTOR_BRK_MATCHED_ORIGINAL_STEP;
    slot0->s4_descriptor.step_tid = tid;
    slot0->s4_descriptor.step_offset = R0LAB_RAW_PAGE_SIZE;
    if (!r0lab_s4_descriptor_step_matches_locked(
            slot0, saved0.page_address + saved0.step_offset)) {
        bad_step_offset_rejected = true;
        ++reject_checks;
    }
    slot0->s4_descriptor = saved0;
    slot0->raw.state = raw_state0;

    slot0->s4_descriptor.generation = saved0.generation + 1ULL;
    if (!r0lab_s4_descriptor_brk_matches_locked(
            slot0, saved0.page_address + saved0.brk_offset,
            R0LAB_S4_BRK_COMMENT)) {
        stale_brk_generation_rejected = true;
        ++reject_checks;
    }
    slot0->s4_descriptor = saved0;

    slot0->raw.state = R0LAB_RAW_ORIGINAL_STEP;
    slot0->s4_descriptor.state =
        R0LAB_S4_DESCRIPTOR_BRK_MATCHED_ORIGINAL_STEP;
    slot0->s4_descriptor.step_tid = tid;
    slot0->s4_descriptor.generation = saved0.generation + 1ULL;
    if (!r0lab_s4_descriptor_step_matches_locked(
            slot0, saved0.page_address + saved0.step_offset)) {
        stale_step_generation_rejected = true;
        ++reject_checks;
    }
    slot0->s4_descriptor = saved0;
    slot0->raw.state = raw_state0;

    slot0->s4_descriptor.slot_id = slot1->slot_id;
    if (!r0lab_s4_descriptor_brk_matches_locked(
            slot0, saved0.page_address + saved0.brk_offset,
            R0LAB_S4_BRK_COMMENT)) {
        wrong_brk_slot_rejected = true;
        ++reject_checks;
    }
    slot0->s4_descriptor = saved0;

    slot0->raw.state = R0LAB_RAW_ORIGINAL_STEP;
    slot0->s4_descriptor.state =
        R0LAB_S4_DESCRIPTOR_BRK_MATCHED_ORIGINAL_STEP;
    slot0->s4_descriptor.step_tid = tid;
    slot0->s4_descriptor.slot_id = slot1->slot_id;
    if (!r0lab_s4_descriptor_step_matches_locked(
            slot0, saved0.page_address + saved0.step_offset)) {
        wrong_step_slot_rejected = true;
        ++reject_checks;
    }
    slot0->s4_descriptor = saved0;
    slot0->raw.state = raw_state0;

    slot0->s4_descriptor.mode = R0LAB_S4_DESCRIPTOR_MODE_RAW_REG;
    slot0->s4_descriptor.register_index = R0LAB_S4_RAW_REG_INDEX + 1U;
    slot0->s4_descriptor.register_value = R0LAB_S4_RAW_REG_VALUE;
    if (!r0lab_s4_descriptor_brk_matches_locked(
            slot0, saved0.page_address + saved0.brk_offset,
            R0LAB_S4_BRK_COMMENT)) {
        bad_register_index_rejected = true;
        ++reject_checks;
    }
    slot0->s4_descriptor = saved0;

    slot0->s4_descriptor.mode = R0LAB_S4_DESCRIPTOR_MODE_RAW_REG;
    slot0->s4_descriptor.register_index = R0LAB_S4_RAW_REG_INDEX;
    slot0->s4_descriptor.register_value = R0LAB_S4_RAW_REG_VALUE + 1ULL;
    if (!r0lab_s4_descriptor_brk_matches_locked(
            slot0, saved0.page_address + saved0.brk_offset,
            R0LAB_S4_BRK_COMMENT)) {
        bad_register_value_rejected = true;
        ++reject_checks;
    }
    slot0->s4_descriptor = saved0;

    if (!r0lab_s4_descriptor_brk_matches_locked(
            slot0, saved1.page_address + saved1.brk_offset,
            R0LAB_S4_BRK_COMMENT) &&
        !r0lab_s4_descriptor_brk_matches_locked(
            slot1, saved0.page_address + saved0.brk_offset,
            R0LAB_S4_BRK_COMMENT)) {
        cross_slot_brk_rejected = true;
        ++reject_checks;
    }

    slot0->raw.state = R0LAB_RAW_ORIGINAL_STEP;
    slot0->s4_descriptor.state =
        R0LAB_S4_DESCRIPTOR_BRK_MATCHED_ORIGINAL_STEP;
    slot0->s4_descriptor.step_tid = tid;
    slot1->raw.state = R0LAB_RAW_ORIGINAL_STEP;
    slot1->s4_descriptor.state =
        R0LAB_S4_DESCRIPTOR_BRK_MATCHED_ORIGINAL_STEP;
    slot1->s4_descriptor.step_tid = tid;
    if (!r0lab_s4_descriptor_step_matches_locked(
            slot0, saved1.page_address + saved1.step_offset) &&
        !r0lab_s4_descriptor_step_matches_locked(
            slot1, saved0.page_address + saved0.step_offset)) {
        cross_slot_step_rejected = true;
        ++reject_checks;
    }

    slot0->s4_descriptor = saved0;
    slot1->s4_descriptor = saved1;
    slot0->raw.state = raw_state0;
    slot1->raw.state = raw_state1;

    slot0->raw.state = R0LAB_RAW_ORIGINAL_STEP;
    slot0->s4_descriptor.state =
        R0LAB_S4_DESCRIPTOR_BRK_MATCHED_ORIGINAL_STEP;
    slot0->s4_descriptor.step_tid = tid + 1;
    if (!r0lab_s4_descriptor_step_matches_locked(
            slot0, saved0.page_address + saved0.step_offset)) {
        wrong_step_tid_rejected = true;
        ++reject_checks;
    }

    slot0->s4_descriptor = saved0;
    slot1->s4_descriptor = saved1;
    slot0->raw.state = raw_state0;
    slot1->raw.state = raw_state1;
    slot0->transitioning = transitioning0;
    slot1->transitioning = transitioning1;

    state_intact =
        slot0->raw.state == raw_state0 && slot1->raw.state == raw_state1 &&
        slot0->transitioning == transitioning0 &&
        slot1->transitioning == transitioning1 &&
        r0lab_s4_descriptor_same(&slot0->s4_descriptor, &saved0) &&
        r0lab_s4_descriptor_same(&slot1->s4_descriptor, &saved1);
    brk_events = g_s4_brk.brk_events;
    step_events = g_s4_brk.step_events;
    enable_events = g_s4_brk.step_enable_events;
    disable_events = g_s4_brk.step_disable_events;
    pte_begin_events = g_s4_brk.pte_begin_events;
    pte_finish_events = g_s4_brk.pte_finish_events;
    active_descriptors = r0lab_s4_descriptor_count_locked();
    raw_slots = r0lab_raw_slot_count_locked();
    raw_page_table_active = r0lab_raw_page_table_active_count_locked();
    r0lab_unlock(flags);

    snprintf(reply, sizeof(reply),
             "s4_descriptor_negative_observed slots=2 active=%u baseline_brk_matches=%u baseline_step_matches=%u reject_checks=%u state_intact=%u bad_brk_offset_rejected=%u bad_step_offset_rejected=%u stale_brk_generation_rejected=%u stale_step_generation_rejected=%u wrong_brk_slot_rejected=%u wrong_step_slot_rejected=%u bad_register_index_rejected=%u bad_register_value_rejected=%u cross_slot_brk_rejected=%u cross_slot_step_rejected=%u wrong_step_tid_rejected=%u brk_events=%u step_events=%u enable_events=%u disable_events=%u pte_begin_events=%u pte_finish_events=%u raw_slots=%u raw_page_table_active=%u\n",
             active_descriptors, baseline_brk_matches,
             baseline_step_matches, reject_checks, state_intact ? 1U : 0U,
             bad_brk_offset_rejected ? 1U : 0U,
             bad_step_offset_rejected ? 1U : 0U,
             stale_brk_generation_rejected ? 1U : 0U,
             stale_step_generation_rejected ? 1U : 0U,
             wrong_brk_slot_rejected ? 1U : 0U,
             wrong_step_slot_rejected ? 1U : 0U,
             bad_register_index_rejected ? 1U : 0U,
             bad_register_value_rejected ? 1U : 0U,
             cross_slot_brk_rejected ? 1U : 0U,
             cross_slot_step_rejected ? 1U : 0U,
             wrong_step_tid_rejected ? 1U : 0U, brk_events, step_events,
             enable_events, disable_events, pte_begin_events,
             pte_finish_events, raw_slots, raw_page_table_active);
    return r0lab_copy_reply(out_msg, outlen, reply);
}

static long r0lab_s4_brk_observed(uint64_t token, char __user *out_msg,
                                  int outlen)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    unsigned long flags;
    unsigned long target_address;
    uint32_t brk_events;
    uint8_t state;
    int result = r0lab_validate_owner(token);

    if (result)
        return result;
    flags = r0lab_lock();
    if (!g_s4_brk.armed || g_s4_brk.clearing) {
        r0lab_unlock(flags);
        return R0LAB_EAGAIN;
    }
    target_address = g_s4_brk.target_address;
    brk_events = g_s4_brk.brk_events;
    state = g_s4_brk.state;
    r0lab_unlock(flags);

    snprintf(reply, sizeof(reply),
             "s4_brk_observed target=%llx brk_events=%u state=%s mode=brk_only skip_origin=0 single_step=0 pte_switch=0\n",
             (uint64_t)target_address, brk_events, r0lab_s4_state_name(state));
    return r0lab_copy_reply(out_msg, outlen, reply);
}

static long r0lab_s4_step_observed(uint64_t token, char __user *out_msg,
                                   int outlen)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    unsigned long flags;
    unsigned long target_address;
    uint32_t brk_events;
    uint32_t step_events;
    uint32_t enable_events;
    uint32_t disable_events;
    uint32_t pte_begin_events;
    uint32_t pte_finish_events;
    uint32_t reg_write_events;
    uint64_t reg_value;
    uint8_t state;
    bool raw_step_mode;
    bool raw_reg_mode;
    int result = r0lab_validate_owner(token);

    if (result)
        return result;
    flags = r0lab_lock();
    if (!g_s4_brk.armed || g_s4_brk.clearing || !g_s4_brk.step_mode) {
        r0lab_unlock(flags);
        return R0LAB_EAGAIN;
    }
    target_address = g_s4_brk.target_address;
    brk_events = g_s4_brk.brk_events;
    step_events = g_s4_brk.step_events;
    enable_events = g_s4_brk.step_enable_events;
    disable_events = g_s4_brk.step_disable_events;
    pte_begin_events = g_s4_brk.pte_begin_events;
    pte_finish_events = g_s4_brk.pte_finish_events;
    reg_write_events = g_s4_brk.reg_write_events;
    reg_value = g_s4_brk.reg_value;
    state = g_s4_brk.state;
    raw_step_mode = g_s4_brk.raw_step_mode;
    raw_reg_mode = g_s4_brk.raw_reg_mode;
    r0lab_unlock(flags);

    if (raw_reg_mode)
        snprintf(reply, sizeof(reply),
                 "s4_step_observed target=%llx brk_events=%u step_events=%u enable_events=%u disable_events=%u pte_begin_events=%u pte_finish_events=%u reg_write_events=%u state=%s mode=raw_reg brk_skip_origin=1 step_skip_origin=1 single_step=1 pte_switch=1 register_edit=1 register_index=%u register_value=%llu register_apply=brk_before_single_step raw_state=shadow_active\n",
                 (uint64_t)target_address, brk_events, step_events,
                 enable_events, disable_events, pte_begin_events,
                 pte_finish_events, reg_write_events,
                 r0lab_s4_state_name(state), R0LAB_S4_RAW_REG_INDEX,
                 (unsigned long long)reg_value);
    else if (raw_step_mode)
        snprintf(reply, sizeof(reply),
                 "s4_step_observed target=%llx brk_events=%u step_events=%u enable_events=%u disable_events=%u pte_begin_events=%u pte_finish_events=%u state=%s mode=raw_step brk_skip_origin=1 step_skip_origin=1 single_step=1 pte_switch=1 raw_state=shadow_active\n",
                 (uint64_t)target_address, brk_events, step_events,
                 enable_events, disable_events, pte_begin_events,
                 pte_finish_events, r0lab_s4_state_name(state));
    else
        snprintf(reply, sizeof(reply),
                 "s4_step_observed target=%llx brk_events=%u step_events=%u enable_events=%u disable_events=%u state=%s mode=step_only brk_skip_origin=1 step_skip_origin=1 single_step=1 pte_switch=0\n",
                 (uint64_t)target_address, brk_events, step_events,
                 enable_events, disable_events, r0lab_s4_state_name(state));
    return r0lab_copy_reply(out_msg, outlen, reply);
}

static long r0lab_s4_brk_clear(uint64_t token, char __user *out_msg, int outlen)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    struct mm_struct *mm;
    unsigned long flags;
    bool brk_hook_installed;
    bool step_hook_installed;
    bool raw_step_mode;
    uint32_t brk_events;
    uint32_t step_events;
    int result = r0lab_validate_owner(token);

    if (result)
        goto record;
    flags = r0lab_lock();
    if ((!g_s4_brk.reserving && !g_s4_brk.armed) || g_s4_brk.clearing) {
        r0lab_unlock(flags);
        result = R0LAB_EAGAIN;
        goto record;
    }
    g_s4_brk.clearing = true;
    g_s4_brk.armed = false;
    g_s4_brk.state = R0LAB_S4_RESTORING;
    mm = g_s4_brk.mm;
    brk_hook_installed = g_s4_brk.hook_installed;
    step_hook_installed = g_s4_brk.step_hook_installed;
    raw_step_mode = g_s4_brk.raw_step_mode;
    brk_events = g_s4_brk.brk_events;
    step_events = g_s4_brk.step_events;
    r0lab_unlock(flags);

    r0lab_s4_unhook();
    result = r0lab_s4_wait_for_callbacks();

    if (result) {
        goto record;
    }
    if (raw_step_mode) {
        result = r0lab_s4_restore_descriptor_pages(false);
        if (result)
            goto record;
    }
    r0lab_s4_reset(mm);

    r0lab_record(R0LAB_EVENT_S4_CLEAR, 0);
    snprintf(reply, sizeof(reply),
             "s4_brk_cleared brk_events=%u step_events=%u brk_hook_unwrapped=%u step_hook_unwrapped=%u\n",
             brk_events, step_events, brk_hook_installed ? 1U : 0U,
             step_hook_installed ? 1U : 0U);
    return r0lab_copy_reply(out_msg, outlen, reply);

record:
    r0lab_record(R0LAB_EVENT_REJECT, result);
    return result;
}

static long r0lab_s4_descriptor_routing_clear(uint64_t token,
                                              char __user *out_msg,
                                              int outlen)
{
    return r0lab_s4_brk_clear(token, out_msg, outlen);
}

static long r0lab_s4_brk_cleared(uint64_t token, char __user *out_msg,
                                 int outlen)
{
    unsigned long flags;
    int result = r0lab_validate_owner(token);

    if (result)
        return result;
    flags = r0lab_lock();
    if (r0lab_s4_slot_count_locked() || r0lab_s4_descriptor_count_locked() ||
        r0lab_raw_slot_count_locked()) {
        r0lab_unlock(flags);
        return R0LAB_EAGAIN;
    }
    r0lab_unlock(flags);
    return r0lab_copy_reply(out_msg, outlen, "s4_brk_cleared\n");
}

static long r0lab_arm(uint64_t token, char __user *out_msg, int outlen)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    unsigned long flags;
    int result = 0;
    pid_t current_tgid = r0lab_current_tgid();

    flags = r0lab_lock();
    if (g_session.active || g_workers_shutdown_requested)
        result = R0LAB_EBUSY;
    else {
        g_session.owner_tgid = current_tgid;
        g_session.token = token;
        g_session.active = true;
    }
    r0lab_unlock(flags);

    r0lab_record(R0LAB_EVENT_ARM, result);
    if (result)
        return result;
    snprintf(reply, sizeof(reply), "armed uid=%u tgid=%d token=%llx\n", g_session.lab_uid, current_tgid, token);
    return r0lab_copy_reply(out_msg, outlen, reply);
}

static long r0lab_close(uint64_t token, char __user *out_msg, int outlen)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    unsigned long flags;
    int result = r0lab_validate_owner(token);
    uint64_t summary_seq;

    if (result) {
        r0lab_record(R0LAB_EVENT_REJECT, result);
        return result;
    }
    flags = r0lab_lock();
    if (!g_session.active || g_session.owner_tgid != r0lab_current_tgid() ||
        g_session.token != token) {
        r0lab_unlock(flags);
        r0lab_record(R0LAB_EVENT_REJECT, R0LAB_EPERM);
        return R0LAB_EPERM;
    }
    if (r0lab_hwbp_slot_count_locked() || r0lab_m3_slot_count_locked() ||
        r0lab_m4_slot_count_locked() || r0lab_raw_slot_count_locked() ||
        r0lab_s4_slot_count_locked()) {
        r0lab_unlock(flags);
        r0lab_record(R0LAB_EVENT_REJECT, R0LAB_EBUSY);
        return R0LAB_EBUSY;
    }
    g_session.last_closed_tgid = g_session.owner_tgid;
    g_session.last_closed_token = g_session.token;
    g_session.active = false;
    g_session.owner_tgid = 0;
    g_session.token = 0;
    r0lab_unlock(flags);

    r0lab_record(R0LAB_EVENT_CLOSE, 0);
    summary_seq = r0lab_record(R0LAB_EVENT_SUMMARY, 0);
    flags = r0lab_lock();
    g_session.last_summary_seq = summary_seq;
    r0lab_unlock(flags);
    snprintf(reply, sizeof(reply), "closed final_summary_seq=%llu\n", summary_seq);
    return r0lab_copy_reply(out_msg, outlen, reply);
}

static long r0lab_events(uint64_t token, char __user *out_msg, int outlen)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    unsigned long flags;
    uint64_t first_seq;
    uint64_t last_seq;
    uint64_t seq;
    struct r0lab_event snapshot[R0LAB_EVENT_SNAPSHOT_CAPACITY];
    unsigned int count = 0;
    int length = 0;
    int result = r0lab_validate_reader(token);

    if (result) {
        r0lab_record(R0LAB_EVENT_REJECT, result);
        return result;
    }
    flags = r0lab_lock();
    last_seq = g_next_seq;
    first_seq = last_seq > R0LAB_EVENT_SNAPSHOT_CAPACITY
                    ? last_seq - R0LAB_EVENT_SNAPSHOT_CAPACITY : 0;
    if (last_seq > R0LAB_EVENT_CAPACITY && first_seq < last_seq - R0LAB_EVENT_CAPACITY)
        first_seq = last_seq - R0LAB_EVENT_CAPACITY;
    for (seq = first_seq; seq < last_seq; ++seq) {
        const struct r0lab_event *event = &g_events[seq % R0LAB_EVENT_CAPACITY];

        if (event->seq != seq)
            continue;
        snapshot[count++] = *event;
    }
    r0lab_unlock(flags);

    for (seq = 0; seq < count; ++seq) {
        const struct r0lab_event *event = &snapshot[seq];

        if (length >= (int)sizeof(reply) - 168)
            break;
        length += snprintf(reply + length, sizeof(reply) - length,
                           "seq=%llu ticks=%llu cpu=%d tid=%u uid=%u op=%u result=%d pc=%llx x0=%llx x30=%llx\n",
                           event->seq, event->monotonic_ticks, event->cpu, event->tid,
                           event->uid, event->op, event->result, event->pc,
                           event->x0, event->x30);
    }

    r0lab_record(R0LAB_EVENT_STATUS, 0);
    if (!length)
        snprintf(reply, sizeof(reply), "events=empty\n");
    return r0lab_copy_reply(out_msg, outlen, reply);
}

static int r0lab_parse_u64_pair(const char *text, uint64_t *first,
                                uint64_t *second)
{
    const char *separator;
    char first_text[R0LAB_TOKEN_MAX + 1];
    size_t first_length;

    if (!text || !first || !second)
        return R0LAB_EINVAL;
    separator = strchr(text, ' ');
    if (!separator || !separator[1])
        return R0LAB_EINVAL;
    first_length = separator - text;
    if (!first_length || first_length > R0LAB_TOKEN_MAX ||
        strnlen(separator + 1, R0LAB_TOKEN_MAX + 1) > R0LAB_TOKEN_MAX)
        return R0LAB_EINVAL;
    memcpy(first_text, text, first_length);
    first_text[first_length] = 0;
    if (r0lab_parse_u64(first_text, first) ||
        r0lab_parse_u64(separator + 1, second))
        return R0LAB_EINVAL;
    return 0;
}

static int r0lab_parse_u64_triplet(const char *text, uint64_t *first,
                                   uint64_t *second, uint64_t *third)
{
    const char *separator;
    char first_text[R0LAB_TOKEN_MAX + 1];
    size_t first_length;

    if (!text || !first || !second || !third)
        return R0LAB_EINVAL;
    separator = strchr(text, ' ');
    if (!separator || !separator[1])
        return R0LAB_EINVAL;
    first_length = separator - text;
    if (!first_length || first_length > R0LAB_TOKEN_MAX)
        return R0LAB_EINVAL;
    memcpy(first_text, text, first_length);
    first_text[first_length] = 0;
    if (r0lab_parse_u64(first_text, first))
        return R0LAB_EINVAL;
    return r0lab_parse_u64_pair(separator + 1, second, third);
}

static int r0lab_parse_u64_quad(const char *text, uint64_t *first,
                                uint64_t *second, uint64_t *third,
                                uint64_t *fourth)
{
    const char *separator;
    char first_text[R0LAB_TOKEN_MAX + 1];
    size_t first_length;

    if (!text || !first || !second || !third || !fourth)
        return R0LAB_EINVAL;
    separator = strchr(text, ' ');
    if (!separator || !separator[1])
        return R0LAB_EINVAL;
    first_length = separator - text;
    if (!first_length || first_length > R0LAB_TOKEN_MAX)
        return R0LAB_EINVAL;
    memcpy(first_text, text, first_length);
    first_text[first_length] = 0;
    if (r0lab_parse_u64(first_text, first))
        return R0LAB_EINVAL;
    return r0lab_parse_u64_triplet(separator + 1, second, third, fourth);
}

static int r0lab_parse_u64_quintet(const char *text, uint64_t *first,
                                   uint64_t *second, uint64_t *third,
                                   uint64_t *fourth, uint64_t *fifth)
{
    const char *separator;
    char first_text[R0LAB_TOKEN_MAX + 1];
    size_t first_length;

    if (!text || !first || !second || !third || !fourth || !fifth)
        return R0LAB_EINVAL;
    separator = strchr(text, ' ');
    if (!separator || !separator[1])
        return R0LAB_EINVAL;
    first_length = separator - text;
    if (!first_length || first_length > R0LAB_TOKEN_MAX)
        return R0LAB_EINVAL;
    memcpy(first_text, text, first_length);
    first_text[first_length] = 0;
    if (r0lab_parse_u64(first_text, first))
        return R0LAB_EINVAL;
    return r0lab_parse_u64_quad(separator + 1, second, third, fourth, fifth);
}

static bool r0lab_is_error_pointer(const void *pointer)
{
    return (unsigned long)pointer >= (unsigned long)-4095;
}

static void r0lab_hwbp_attr_init(struct r0lab_perf_event_attr *attr,
                                  uint64_t address)
{
    memset(attr, 0, sizeof(*attr));
    attr->type = 5; /* PERF_TYPE_BREAKPOINT */
    attr->size = sizeof(*attr);
    attr->sample_period = 1;
    attr->pinned = 1;
    attr->exclude_kernel = 1;
    attr->bp_type = 4; /* HW_BREAKPOINT_X */
    attr->bp_addr = address;
    attr->bp_len = 4; /* HW_BREAKPOINT_LEN_4: mandatory for AArch64 execute */
}

static void r0lab_hwbp_callback(struct perf_event *event,
                                struct perf_sample_data *data,
                                struct pt_regs *regs)
{
    struct r0lab_hwbp_slot *slot = NULL;
    unsigned long flags;
    uint64_t expected_pc = 0;
    uint16_t event_op = R0LAB_EVENT_REJECT;
    unsigned int slot_index;
    unsigned int event_index = R0LAB_HWBP_EVENT_COUNT;
    unsigned int callback_log_index = 0;
    int result = R0LAB_EINVAL;

    (void)data;
    if (!regs || (!g_disable_hwbp_inatomic && !g_disable_hwbp_local))
        goto record;

    flags = r0lab_lock();
    for (slot_index = 0; slot_index < R0LAB_HWBP_SLOT_CAPACITY; ++slot_index) {
        unsigned int index;

        if (!g_hwbp_slots[slot_index].armed)
            continue;
        for (index = 0; index < R0LAB_HWBP_EVENT_COUNT; ++index) {
            if (g_hwbp_slots[slot_index].events[index] != event)
                continue;
            slot = &g_hwbp_slots[slot_index];
            event_index = index;
            expected_pc = slot->attrs[index].bp_addr;
            event_op = index == 0 ? R0LAB_EVENT_HWBP_ENTRY
                                  : R0LAB_EVENT_HWBP_RETURN;
            break;
        }
        if (slot)
            break;
    }
    r0lab_unlock(flags);
    if (!slot)
        goto record;

    flags = r0lab_lock();
    if (g_hwbp_callback_log_count < 8)
        callback_log_index = ++g_hwbp_callback_log_count;
    r0lab_unlock(flags);
    if (callback_log_index)
        pr_info("r0lab-m1: hwbp callback #%u tid=%d event=%u expected=%llx pc=%llx\n",
                callback_log_index, r0lab_current_tid(), event_index,
                expected_pc, regs->pc);

    /* Keep the exception path non-blocking; the worker owns final teardown. */
    if (g_disable_hwbp_inatomic)
        g_disable_hwbp_inatomic(event);
    else
        g_disable_hwbp_local(event);

    flags = r0lab_lock();
    if (!slot->armed || slot->tid != r0lab_current_tid() ||
        slot->events[event_index] != event || expected_pc != regs->pc) {
        result = R0LAB_EPERM;
    } else {
        slot->attrs[event_index].disabled = 1;
        slot->hit[event_index] = true;
        if (event_index == 0)
            ++g_hwbp_entry_events;
        else
            ++g_hwbp_return_events;
        result = 0;
    }
    r0lab_unlock(flags);

    if (callback_log_index)
        pr_info("r0lab-m1: hwbp callback #%u recorded result=%d\n",
                callback_log_index, result);

record:
    r0lab_record_regs(event_op, result, regs);
}

static void r0lab_hwbp_unregister_pair(struct perf_event *entry_event,
                                       struct perf_event *return_event)
{
    if (entry_event)
        g_unregister_hwbp(entry_event);
    if (return_event)
        g_unregister_hwbp(return_event);
    if (entry_event || return_event)
        g_synchronize_rcu();
}

/*
 * KPM control callbacks run inside FolkPatch's SuperCall RCU read-side
 * section. perf registration and release may wait for an RCU grace period,
 * so both operations must execute from this kthread. The thread remains
 * alive after registration until both one-shot observations or an explicit
 * clear request make teardown safe.
 */
static void r0lab_hwbp_run_slot(struct r0lab_hwbp_slot *slot)
{
    struct task_struct *target_task = NULL;
    struct perf_event *entry_event = NULL;
    struct perf_event *return_event = NULL;
    unsigned long flags;
    pid_t owner_tgid = 0;
    int result = 0;
    bool cancelled = false;

    flags = r0lab_lock();
    if (!slot) {
        r0lab_unlock(flags);
        r0lab_record(R0LAB_EVENT_REJECT, R0LAB_EINVAL);
        return;
    }
    if (!slot->target_task || !slot->reserving || slot->armed ||
        !g_session.active) {
        r0lab_unlock(flags);
        result = R0LAB_ESRCH;
        goto discard;
    } else if (slot->clearing) {
        target_task = slot->target_task;
        r0lab_unlock(flags);
        goto discard_cancelled;
    } else {
        target_task = slot->target_task;
        owner_tgid = g_session.owner_tgid;
    }
    r0lab_unlock(flags);

    if (g_task_pid(target_task, PIDTYPE_TGID, NULL) != owner_tgid) {
        result = R0LAB_ESRCH;
        goto discard;
    }

    entry_event = g_register_hwbp(&slot->attrs[0], r0lab_hwbp_callback,
                                  NULL, target_task);
    if (r0lab_is_error_pointer(entry_event)) {
        result = (long)(unsigned long)entry_event;
        entry_event = NULL;
        goto discard;
    }
    return_event = g_register_hwbp(&slot->attrs[1], r0lab_hwbp_callback,
                                   NULL, target_task);
    if (r0lab_is_error_pointer(return_event)) {
        result = (long)(unsigned long)return_event;
        return_event = NULL;
        goto rollback;
    }

    flags = r0lab_lock();
    cancelled = slot->clearing;
    if (!cancelled) {
        slot->events[0] = entry_event;
        slot->events[1] = return_event;
        slot->armed = true;
        slot->reserving = false;
    }
    r0lab_unlock(flags);
    if (cancelled)
        goto rollback_cancelled;
    r0lab_record(R0LAB_EVENT_HWBP_ARM, 0);
    pr_info("r0lab-m1: hwbp worker armed tid=%d entry=%llx return=%llx\n",
            slot->tid, slot->attrs[0].bp_addr, slot->attrs[1].bp_addr);

    for (;;) {
        bool cleanup = false;

        flags = r0lab_lock();
        if (slot->target_task == target_task && slot->armed) {
            cleanup = slot->clearing ||
                      (slot->hit[0] && slot->hit[1]);
        }
        r0lab_unlock(flags);

        if (r0lab_worker_should_stop())
            cleanup = true;
        if (!cleanup && g_task_pid(target_task, PIDTYPE_PID, NULL) == 0)
            cleanup = true;
        if (!cleanup) {
            g_msleep(1);
            continue;
        }

        pr_info("r0lab-m1: hwbp worker cleanup tid=%d hit=%u/%u clearing=%u\n",
                slot->tid, slot->hit[0], slot->hit[1], slot->clearing);

        flags = r0lab_lock();
        if (slot->target_task == target_task && slot->armed) {
            entry_event = slot->events[0];
            return_event = slot->events[1];
            slot->clearing = true;
        } else {
            entry_event = NULL;
            return_event = NULL;
        }
        r0lab_unlock(flags);

        r0lab_hwbp_unregister_pair(entry_event, return_event);
        flags = r0lab_lock();
        if (slot->target_task == target_task)
            memset(slot, 0, sizeof(*slot));
        r0lab_unlock(flags);
        g_put_task(target_task);
        r0lab_record(R0LAB_EVENT_HWBP_CLEAR, 0);
        return;
    }

rollback_cancelled:
    r0lab_hwbp_unregister_pair(entry_event, return_event);
discard_cancelled:
    flags = r0lab_lock();
    if (slot->target_task == target_task)
        memset(slot, 0, sizeof(*slot));
    r0lab_unlock(flags);
    g_put_task(target_task);
    r0lab_record(R0LAB_EVENT_HWBP_CLEAR, 0);
    return;

rollback:
    r0lab_hwbp_unregister_pair(entry_event, return_event);

discard:
    flags = r0lab_lock();
    if (slot->target_task == target_task)
        memset(slot, 0, sizeof(*slot));
    r0lab_unlock(flags);
    if (target_task)
        g_put_task(target_task);
    r0lab_record(R0LAB_EVENT_REJECT, result);
    return;
}

static int r0lab_hwbp_start_worker(struct r0lab_hwbp_slot *slot)
{
    long index;

    if (!slot)
        return R0LAB_EINVAL;
    index = slot - g_hwbp_slots;
    if (index < 0 || index >= R0LAB_HWBP_SLOT_CAPACITY ||
        !g_hwbp_workers[index])
        return R0LAB_ESRCH;
    g_wake_up_process(g_hwbp_workers[index]);
    return 0;
}

static int r0lab_hwbp_worker(void *opaque)
{
    struct r0lab_hwbp_slot *slot = opaque;

    while (!r0lab_worker_should_stop()) {
        bool pending;
        unsigned long flags = r0lab_lock();

        pending = slot && slot->target_task &&
                  (slot->reserving || slot->armed || slot->clearing);
        r0lab_unlock(flags);
        if (pending) {
            r0lab_hwbp_run_slot(slot);
            continue;
        }
        g_msleep(R0LAB_M5_MONITOR_INTERVAL_MS);
    }
    r0lab_worker_finished();
    return 0;
}

static long r0lab_hwbp_arm(uint64_t token, uint64_t entry_pc,
                           uint64_t return_pc, char __user *out_msg,
                           int outlen)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    struct r0lab_hwbp_slot *slot = NULL;
    struct task_struct *target_task;
    unsigned long flags;
    pid_t tid;
    unsigned int index;
    int result = r0lab_validate_owner(token);

    if (result)
        goto record;
    if (!entry_pc || !return_pc || (entry_pc & 3) || (return_pc & 3)) {
        result = R0LAB_EINVAL;
        goto record;
    }

    tid = r0lab_current_tid();
    target_task = g_find_get_task(tid);
    if (!target_task) {
        result = R0LAB_ESRCH;
        goto record;
    }
    flags = r0lab_lock();
    if (r0lab_raw_slot_count_locked() || r0lab_s4_slot_count_locked()) {
        r0lab_unlock(flags);
        g_put_task(target_task);
        result = R0LAB_EBUSY;
        goto record;
    }
    for (index = 0; index < R0LAB_HWBP_SLOT_CAPACITY; ++index) {
        if ((g_hwbp_slots[index].reserving || g_hwbp_slots[index].armed ||
             g_hwbp_slots[index].clearing) && g_hwbp_slots[index].tid == tid) {
            r0lab_unlock(flags);
            g_put_task(target_task);
            result = R0LAB_EBUSY;
            goto record;
        }
        if (!slot && !g_hwbp_slots[index].reserving &&
            !g_hwbp_slots[index].armed && !g_hwbp_slots[index].clearing)
            slot = &g_hwbp_slots[index];
    }
    if (!slot) {
        r0lab_unlock(flags);
        g_put_task(target_task);
        result = R0LAB_EBUSY;
        goto record;
    }
    memset(slot, 0, sizeof(*slot));
    slot->tid = tid;
    slot->target_task = target_task;
    slot->reserving = true;
    r0lab_hwbp_attr_init(&slot->attrs[0], entry_pc);
    r0lab_hwbp_attr_init(&slot->attrs[1], return_pc);
    r0lab_unlock(flags);

    result = r0lab_hwbp_start_worker(slot);
    if (result) {
        flags = r0lab_lock();
        if (slot->target_task == target_task)
            memset(slot, 0, sizeof(*slot));
        r0lab_unlock(flags);
        g_put_task(target_task);
        goto record;
    }
    snprintf(reply, sizeof(reply),
             "hwbp_pending tid=%d entry=%llx return=%llx slots=2\n",
             tid, entry_pc, return_pc);
    return r0lab_copy_reply(out_msg, outlen, reply);

record:
    r0lab_record(R0LAB_EVENT_REJECT, result);
    return result;
}

static long r0lab_hwbp_ready(uint64_t token, char __user *out_msg, int outlen)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    unsigned long flags;
    pid_t tid;
    unsigned int index;
    int result = r0lab_validate_owner(token);

    if (result)
        return result;
    tid = r0lab_current_tid();
    flags = r0lab_lock();
    for (index = 0; index < R0LAB_HWBP_SLOT_CAPACITY; ++index) {
        struct r0lab_hwbp_slot *slot = &g_hwbp_slots[index];

        if (slot->tid != tid)
            continue;
        if (slot->armed && !slot->clearing) {
            r0lab_unlock(flags);
            snprintf(reply, sizeof(reply), "hwbp_ready tid=%d\n", tid);
            return r0lab_copy_reply(out_msg, outlen, reply);
        }
        r0lab_unlock(flags);
        return R0LAB_EAGAIN;
    }
    r0lab_unlock(flags);
    return R0LAB_ENOENT;
}

static long r0lab_hwbp_clear(uint64_t token, char __user *out_msg, int outlen)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    struct r0lab_hwbp_slot *slot = NULL;
    unsigned long flags;
    pid_t tid;
    unsigned int index;
    int result = r0lab_validate_owner(token);

    if (result)
        goto record;
    tid = r0lab_current_tid();
    flags = r0lab_lock();
    for (index = 0; index < R0LAB_HWBP_SLOT_CAPACITY; ++index) {
        if ((g_hwbp_slots[index].reserving || g_hwbp_slots[index].armed) &&
            !g_hwbp_slots[index].clearing && g_hwbp_slots[index].tid == tid) {
            slot = &g_hwbp_slots[index];
            break;
        }
    }
    if (!slot) {
        r0lab_unlock(flags);
        result = R0LAB_EAGAIN;
        goto record;
    }
    slot->clearing = true;
    if (slot->reserving) {
        r0lab_unlock(flags);
        snprintf(reply, sizeof(reply), "hwbp_cancel_pending tid=%d\n", tid);
        return r0lab_copy_reply(out_msg, outlen, reply);
    }
    r0lab_unlock(flags);
    snprintf(reply, sizeof(reply), "hwbp_clear_pending tid=%d\n", tid);
    return r0lab_copy_reply(out_msg, outlen, reply);

record:
    r0lab_record(R0LAB_EVENT_REJECT, result);
    return result;
}

static long r0lab_hwbp_cleared(uint64_t token, char __user *out_msg, int outlen)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    unsigned long flags;
    pid_t tid;
    unsigned int index;
    int result = r0lab_validate_owner(token);

    if (result)
        return result;
    tid = r0lab_current_tid();
    flags = r0lab_lock();
    for (index = 0; index < R0LAB_HWBP_SLOT_CAPACITY; ++index) {
        if (g_hwbp_slots[index].tid == tid &&
            (g_hwbp_slots[index].reserving || g_hwbp_slots[index].armed ||
             g_hwbp_slots[index].clearing)) {
            r0lab_unlock(flags);
            return R0LAB_EAGAIN;
        }
    }
    r0lab_unlock(flags);
    snprintf(reply, sizeof(reply), "hwbp_cleared tid=%d\n", tid);
    return r0lab_copy_reply(out_msg, outlen, reply);
}

static void r0lab_close_exited_session(pid_t owner_tgid)
{
    unsigned long flags;
    uint64_t summary_seq;
    bool closed = false;

    flags = r0lab_lock();
    if (g_session.active && g_session.owner_tgid == owner_tgid &&
        !r0lab_session_has_slots_locked()) {
        g_session.last_closed_tgid = g_session.owner_tgid;
        g_session.last_closed_token = g_session.token;
        g_session.active = false;
        g_session.owner_tgid = 0;
        g_session.token = 0;
        closed = true;
    }
    r0lab_unlock(flags);
    if (!closed)
        return;
    r0lab_record(R0LAB_EVENT_CLOSE, 0);
    summary_seq = r0lab_record(R0LAB_EVENT_SUMMARY, 0);
    flags = r0lab_lock();
    g_session.last_summary_seq = summary_seq;
    r0lab_unlock(flags);
}

static bool r0lab_target_mm_live(pid_t owner_tgid, struct mm_struct *expected_mm)
{
    struct task_struct *task;
    struct mm_struct *task_mm;
    bool live;

    if (!owner_tgid || !expected_mm)
        return false;
    task = g_find_get_task(owner_tgid);
    if (!task)
        return false;
    task_mm = g_get_task_mm(task);
    g_put_task(task);
    if (!task_mm)
        return false;
    live = task_mm == expected_mm;
    g_mmput(task_mm);
    return live;
}

static bool r0lab_target_task_live(pid_t owner_tgid)
{
    struct task_struct *task;

    if (!owner_tgid)
        return false;
    task = g_find_get_task(owner_tgid);
    if (!task)
        return false;
    g_put_task(task);
    return true;
}

static int r0lab_session_monitor_loop(void *opaque)
{
    (void)opaque;
    while (!r0lab_worker_should_stop()) {
        unsigned long flags;
        pid_t owner_tgid = 0;
        bool should_check = false;

        flags = r0lab_lock();
        if (g_session.active && !r0lab_session_has_slots_locked()) {
            owner_tgid = g_session.owner_tgid;
            should_check = true;
        }
        r0lab_unlock(flags);

        if (should_check && !r0lab_target_task_live(owner_tgid))
            r0lab_close_exited_session(owner_tgid);
        g_msleep(R0LAB_M5_MONITOR_INTERVAL_MS);
    }
    r0lab_worker_finished();
    return 0;
}

static int r0lab_m3_wait_for_callbacks(void)
{
    unsigned int attempt;

    for (attempt = 0; attempt < R0LAB_M3_DRAIN_LIMIT; ++attempt) {
        unsigned long flags = r0lab_lock();
        bool drained = !g_m3_inflight;

        r0lab_unlock(flags);
        if (drained)
            return 0;
        g_msleep(1);
    }
    return R0LAB_EBUSY;
}

static void r0lab_m3_before_abort(hook_fargs3_t *args, void *udata)
{
    const unsigned long far = (unsigned long)args->arg0;
    const unsigned int esr = (unsigned int)args->arg1;
    const struct pt_regs *regs = (const struct pt_regs *)(unsigned long)args->arg2;
    unsigned long flags;
    uint64_t generation = 0;
    bool should_handle = false;
    bool handled = false;
    int result = R0LAB_EINVAL;

    (void)udata;
    flags = r0lab_lock();
    ++g_m3_inflight;
    if (regs && (esr >> R0LAB_M3_ESR_EC_SHIFT) == R0LAB_M3_ESR_EC_IABT_LOW &&
        (esr & R0LAB_M3_ESR_FSC_TYPE) == R0LAB_M3_ESR_FSC_PERM) {
        if (g_session.active && g_m3_page.armed && !g_m3_page.clearing &&
            !g_m3_page.transitioning &&
            g_m3_page.raw.state == R0LAB_RAW_SOURCE_UXN &&
            g_session.owner_tgid == r0lab_current_tgid() &&
            (far & ~(R0LAB_M3_PAGE_SIZE - 1UL)) == g_m3_page.address &&
            regs->pc >= g_m3_page.address &&
            regs->pc < g_m3_page.address + R0LAB_M3_PAGE_SIZE) {
            g_m3_page.transitioning = true;
            generation = g_m3_page.generation;
            should_handle = true;
        }
    }
    r0lab_unlock(flags);

    if (should_handle)
        result = r0lab_raw_restore_original(&g_m3_page.raw);

    flags = r0lab_lock();
    if (should_handle && g_m3_page.generation == generation) {
        g_m3_page.transitioning = false;
        if (!result && g_m3_page.raw.state == R0LAB_RAW_RESTORED) {
            ++g_m3_page.fault_events;
            args->skip_origin = 1;
            handled = true;
        }
    }
    --g_m3_inflight;
    r0lab_unlock(flags);
    if (handled)
        r0lab_record_regs(R0LAB_EVENT_M3_FAULT, 0, regs);
    else if (should_handle)
        r0lab_record_regs(R0LAB_EVENT_REJECT, result, regs);
}

static void r0lab_m3_reset(struct mm_struct *mm)
{
    unsigned long flags = r0lab_lock();

    if (g_m3_page.mm == mm) {
        bool monitor_running = g_m3_page.monitor_running;
        uint64_t generation = g_m3_page.generation;

        memset(&g_m3_page, 0, sizeof(g_m3_page));
        g_m3_page.generation = generation;
        g_m3_page.monitor_running = monitor_running;
    }
    r0lab_unlock(flags);
    g_mmput(mm);
}

static void r0lab_m3_reset_final(struct mm_struct *mm, uint64_t generation)
{
    unsigned long flags = r0lab_lock();

    if (g_m3_page.mm == mm && g_m3_page.generation == generation)
        memset(&g_m3_page, 0, sizeof(g_m3_page));
    r0lab_unlock(flags);
    g_mmput(mm);
}

static void r0lab_m3_monitor_done(uint64_t generation)
{
    unsigned long flags = r0lab_lock();

    if (g_m3_page.generation == generation)
        g_m3_page.monitor_running = false;
    r0lab_unlock(flags);
}

static void r0lab_m3_unhook(void)
{
    bool installed;
    unsigned long flags = r0lab_lock();

    installed = g_m3_page.hook_installed;
    g_m3_page.hook_installed = false;
    r0lab_unlock(flags);
    if (installed)
        (void)r0lab_m3_wait_for_callbacks();
    if (installed)
        r0lab_hook_detach(g_do_mem_abort, r0lab_m3_before_abort, NULL);
    if (installed)
        (void)r0lab_m3_wait_for_callbacks();
}

static int r0lab_m3_monitor_worker(void *opaque)
{
    struct r0lab_m3_page *page = opaque;
    struct mm_struct *mm;
    unsigned long flags;
    unsigned long address;
    uint64_t generation;
    pid_t owner_tgid;
    int result;

    flags = r0lab_lock();
    if (!page || page != &g_m3_page || !page->monitor_running ||
        !page->generation) {
        r0lab_unlock(flags);
        return 0;
    }
    generation = page->generation;
    r0lab_unlock(flags);

    for (;;) {
        bool active;
        bool page_owned;
        bool already_clearing;

        flags = r0lab_lock();
        if (g_m3_page.generation != generation || !g_m3_page.monitor_running) {
            r0lab_unlock(flags);
            return 0;
        }
        mm = g_m3_page.mm;
        address = g_m3_page.address;
        owner_tgid = g_session.owner_tgid;
        active = g_session.active;
        page_owned = g_m3_page.reserving || g_m3_page.armed || g_m3_page.clearing;
        already_clearing = g_m3_page.clearing && !g_m3_page.target_exiting;
        if (!mm || !active || !page_owned) {
            g_m3_page.monitor_running = false;
            r0lab_unlock(flags);
            return 0;
        }
        r0lab_unlock(flags);

        if (already_clearing) {
            g_msleep(R0LAB_M5_MONITOR_INTERVAL_MS);
            continue;
        }
        if (!r0lab_target_mm_live(owner_tgid, mm))
            break;
        g_msleep(R0LAB_M5_MONITOR_INTERVAL_MS);
    }

    flags = r0lab_lock();
    if (g_m3_page.generation != generation || g_m3_page.mm != mm) {
        r0lab_unlock(flags);
        r0lab_m3_monitor_done(generation);
        return 0;
    }
    g_m3_page.target_exiting = true;
    g_m3_page.clearing = true;
    r0lab_unlock(flags);

    (void)address;
    r0lab_record(R0LAB_EVENT_TARGET_EXIT, 0);
    r0lab_m3_unhook();
    result = r0lab_m3_wait_for_callbacks();
    r0lab_m3_reset_final(mm, generation);
    r0lab_close_exited_session(owner_tgid);
    r0lab_record(R0LAB_EVENT_M3_CLEAR, result);
    return 0;
}

static int r0lab_m3_start_monitor(void)
{
    unsigned long flags;

    flags = r0lab_lock();
    if (!g_m3_page.armed || g_m3_page.monitor_running) {
        r0lab_unlock(flags);
        return R0LAB_EBUSY;
    }
    g_m3_page.monitor_running = true;
    r0lab_unlock(flags);

    if (!g_m3_monitor_worker_task)
        return R0LAB_ESRCH;
    g_wake_up_process(g_m3_monitor_worker_task);
    return 0;
}

static int r0lab_m3_arm_worker(void *opaque)
{
    struct r0lab_m3_page *page = opaque;
    struct mm_struct *mm = NULL;
    unsigned long flags;
    uint64_t generation;
    pid_t owner_tgid;
    bool cancelled;
    bool delay_arm;
    bool target_gone;
    int cleanup_result;
    int result;

    flags = r0lab_lock();
    if (!page || page != &g_m3_page || !page->reserving || !page->mm ||
        !g_session.active) {
        if (page == &g_m3_page && page->mm) {
            mm = page->mm;
            memset(&g_m3_page, 0, sizeof(g_m3_page));
        }
        r0lab_unlock(flags);
        if (mm)
            g_mmput(mm);
        r0lab_record(R0LAB_EVENT_REJECT, R0LAB_ESRCH);
        return 0;
    }
    mm = page->mm;
    generation = page->generation;
    owner_tgid = g_session.owner_tgid;
    delay_arm = g_m3_arm_delay_once;
    g_m3_arm_delay_once = false;
    r0lab_unlock(flags);

    if (delay_arm) {
        g_msleep(R0LAB_M5_TEST_ARM_DELAY_MS);
        flags = r0lab_lock();
        cancelled = page->clearing;
        r0lab_unlock(flags);
        if (cancelled) {
            r0lab_m3_reset(mm);
            r0lab_record(R0LAB_EVENT_M3_CLEAR, 0);
            return 0;
        }
    }

    result = hook_wrap3(g_do_mem_abort, r0lab_m3_before_abort, NULL, NULL);
    if (result)
        goto fail;
    flags = r0lab_lock();
    if (page->mm == mm)
        page->hook_installed = true;
    r0lab_unlock(flags);

    if (!r0lab_target_mm_live(owner_tgid, mm))
        goto target_exit;

    result = r0lab_raw_capture(&page->raw);
    if (result)
        goto fail_unhook;
    result = r0lab_raw_arm_source_uxn_only(&page->raw);
    if (result)
        goto fail_unhook;

    target_gone = !r0lab_target_mm_live(owner_tgid, mm);
    flags = r0lab_lock();
    cancelled = page->clearing;
    if (target_gone && page->mm == mm) {
        page->target_exiting = true;
        page->clearing = true;
    }
    if (!cancelled && !target_gone && page->mm == mm) {
        page->reserving = false;
        page->armed = true;
    }
    r0lab_unlock(flags);
    if (target_gone)
        goto target_exit;
    if (!cancelled) {
        result = r0lab_m3_start_monitor();
        if (result)
            goto fail_restore;
        r0lab_record(R0LAB_EVENT_M3_ARM, 0);
        return 0;
    }

fail_restore:
    cleanup_result = result;
    result = r0lab_raw_restore_original(&page->raw);
    r0lab_m3_unhook();
    if (!result)
        result = r0lab_m3_wait_for_callbacks();
    if (!result)
        result = cleanup_result;
    r0lab_m3_reset(mm);
    r0lab_record(R0LAB_EVENT_M3_CLEAR, result);
    return 0;

fail_unhook:
    r0lab_m3_unhook();
    (void)r0lab_m3_wait_for_callbacks();
fail:
    r0lab_m3_reset(mm);
    r0lab_record(R0LAB_EVENT_REJECT, result);
    return 0;

target_exit:
    r0lab_record(R0LAB_EVENT_TARGET_EXIT, 0);
    r0lab_m3_unhook();
    result = r0lab_m3_wait_for_callbacks();
    r0lab_m3_reset_final(mm, generation);
    r0lab_close_exited_session(owner_tgid);
    r0lab_record(R0LAB_EVENT_M3_CLEAR, result);
    return 0;
}

static int r0lab_m3_clear_worker(void *opaque)
{
    struct r0lab_m3_page *page = opaque;
    struct mm_struct *mm;
    unsigned long flags;
    int result;

    flags = r0lab_lock();
    if (!page || page != &g_m3_page || !page->armed || !page->clearing ||
        !page->mm) {
        r0lab_unlock(flags);
        r0lab_record(R0LAB_EVENT_REJECT, R0LAB_ESRCH);
        return 0;
    }
    mm = page->mm;
    r0lab_unlock(flags);

    result = page->raw.state == R0LAB_RAW_RESTORED ?
             0 : r0lab_raw_restore_original(&page->raw);
    r0lab_m3_unhook();
    if (!result)
        result = r0lab_m3_wait_for_callbacks();
    r0lab_m3_reset(mm);
    r0lab_record(R0LAB_EVENT_M3_CLEAR, result);
    return 0;
}

static int r0lab_m3_start_worker(int (*worker_fn)(void *))
{
    unsigned long flags;
    bool inject_enomem = false;

    if (worker_fn == r0lab_m3_arm_worker) {
        flags = r0lab_lock();
        inject_enomem = g_m3_arm_enomem_once;
        g_m3_arm_enomem_once = false;
        r0lab_unlock(flags);
        if (inject_enomem)
            return R0LAB_ENOMEM;
    }
    (void)worker_fn;

    if (!g_m3_worker)
        return R0LAB_ESRCH;
    g_wake_up_process(g_m3_worker);
    return 0;
}

static int r0lab_m3_worker_loop(void *opaque)
{
    (void)opaque;
    while (!r0lab_worker_should_stop()) {
        bool arm_pending;
        bool clear_pending;
        unsigned long flags = r0lab_lock();

        arm_pending = g_m3_page.reserving;
        clear_pending = !arm_pending && g_m3_page.armed &&
                        g_m3_page.clearing;
        r0lab_unlock(flags);
        if (arm_pending) {
            r0lab_m3_arm_worker(&g_m3_page);
            continue;
        }
        if (clear_pending) {
            r0lab_m3_clear_worker(&g_m3_page);
            continue;
        }
        g_msleep(R0LAB_M5_MONITOR_INTERVAL_MS);
    }
    r0lab_worker_finished();
    return 0;
}

static int r0lab_m3_monitor_loop(void *opaque)
{
    (void)opaque;
    while (!r0lab_worker_should_stop()) {
        bool monitor_pending;
        unsigned long flags = r0lab_lock();

        monitor_pending = g_m3_page.monitor_running;
        r0lab_unlock(flags);
        if (monitor_pending) {
            r0lab_m3_monitor_worker(&g_m3_page);
            continue;
        }
        g_msleep(R0LAB_M5_MONITOR_INTERVAL_MS);
    }
    r0lab_worker_finished();
    return 0;
}

static long r0lab_m3_arm(uint64_t token, uint64_t page_address,
                          char __user *out_msg, int outlen)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    struct mm_struct *mm;
    unsigned long flags;
    int result = r0lab_validate_owner(token);

    if (result)
        goto record;
    if (!page_address || (page_address & (R0LAB_M3_PAGE_SIZE - 1UL))) {
        result = R0LAB_EINVAL;
        goto record;
    }
    mm = g_get_task_mm(current);
    if (!mm) {
        result = R0LAB_ESRCH;
        goto record;
    }
    flags = r0lab_lock();
    if (!g_session.active || g_session.owner_tgid != r0lab_current_tgid() ||
        g_session.token != token || r0lab_m3_slot_count_locked() ||
        r0lab_m4_slot_count_locked() || r0lab_raw_slot_count_locked() ||
        r0lab_s4_slot_count_locked()) {
        r0lab_unlock(flags);
        g_mmput(mm);
        result = R0LAB_EBUSY;
        goto record;
    }
    memset(&g_m3_page, 0, sizeof(g_m3_page));
    g_m3_page.mm = mm;
    g_m3_page.address = (unsigned long)page_address;
    g_m3_page.raw.mm = mm;
    g_m3_page.raw.address = (unsigned long)page_address;
    g_m3_page.generation = ++g_m3_generation;
    g_m3_page.reserving = true;
    r0lab_unlock(flags);

    result = r0lab_m3_start_worker(r0lab_m3_arm_worker);
    if (result) {
        r0lab_m3_reset(mm);
        goto record;
    }
    snprintf(reply, sizeof(reply), "m3_pending page=%llx\n", page_address);
    return r0lab_copy_reply(out_msg, outlen, reply);

record:
    r0lab_record(R0LAB_EVENT_REJECT, result);
    return result;
}

static long r0lab_m3_ready(uint64_t token, char __user *out_msg, int outlen)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    unsigned long flags;
    unsigned long address;
    int result = r0lab_validate_owner(token);

    if (result)
        return result;
    flags = r0lab_lock();
    if (g_m3_page.reserving || g_m3_page.clearing) {
        r0lab_unlock(flags);
        return R0LAB_EAGAIN;
    }
    if (!g_m3_page.armed) {
        r0lab_unlock(flags);
        return R0LAB_ENOENT;
    }
    address = g_m3_page.address;
    r0lab_unlock(flags);
    snprintf(reply, sizeof(reply), "m3_ready page=%llx\n", address);
    return r0lab_copy_reply(out_msg, outlen, reply);
}

static long r0lab_m3_observed(uint64_t token, char __user *out_msg, int outlen)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    unsigned long flags;
    uint32_t fault_events;
    int result = r0lab_validate_owner(token);

    if (result)
        return result;
    flags = r0lab_lock();
    if (!g_m3_page.armed || g_m3_page.clearing) {
        r0lab_unlock(flags);
        return R0LAB_EAGAIN;
    }
    fault_events = g_m3_page.fault_events;
    r0lab_unlock(flags);
    snprintf(reply, sizeof(reply), "m3_observed faults=%u\n", fault_events);
    return r0lab_copy_reply(out_msg, outlen, reply);
}

static long r0lab_m3_clear(uint64_t token, char __user *out_msg, int outlen)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    unsigned long flags;
    int result = r0lab_validate_owner(token);

    if (result)
        goto record;
    flags = r0lab_lock();
    if ((!g_m3_page.reserving && !g_m3_page.armed) || g_m3_page.clearing) {
        r0lab_unlock(flags);
        result = R0LAB_EAGAIN;
        goto record;
    }
    g_m3_page.clearing = true;
    if (g_m3_page.reserving) {
        r0lab_unlock(flags);
        return r0lab_copy_reply(out_msg, outlen, "m3_cancel_pending\n");
    }
    r0lab_unlock(flags);
    result = r0lab_m3_start_worker(r0lab_m3_clear_worker);
    if (result) {
        flags = r0lab_lock();
        if (g_m3_page.armed)
            g_m3_page.clearing = false;
        r0lab_unlock(flags);
        goto record;
    }
    snprintf(reply, sizeof(reply), "m3_clear_pending\n");
    return r0lab_copy_reply(out_msg, outlen, reply);

record:
    r0lab_record(R0LAB_EVENT_REJECT, result);
    return result;
}

static long r0lab_m3_cleared(uint64_t token, char __user *out_msg, int outlen)
{
    unsigned long flags;
    int result = r0lab_validate_owner(token);

    if (result)
        return result;
    flags = r0lab_lock();
    if (r0lab_m3_slot_count_locked() || r0lab_s4_slot_count_locked()) {
        r0lab_unlock(flags);
        return R0LAB_EAGAIN;
    }
    r0lab_unlock(flags);
    return r0lab_copy_reply(out_msg, outlen, "m3_cleared\n");
}

static long r0lab_m3_set_test_fault(uint64_t token, bool delay_arm,
                                    char __user *out_msg, int outlen)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    unsigned long flags;
    int result = r0lab_validate_owner(token);

    if (result)
        goto record;
    flags = r0lab_lock();
    if (r0lab_m3_slot_count_locked() || r0lab_m4_slot_count_locked() ||
        r0lab_raw_slot_count_locked() || r0lab_s4_slot_count_locked()) {
        r0lab_unlock(flags);
        result = R0LAB_EBUSY;
        goto record;
    }
    if (delay_arm)
        g_m3_arm_delay_once = true;
    else
        g_m3_arm_enomem_once = true;
    r0lab_unlock(flags);

    r0lab_record(R0LAB_EVENT_TEST_FAULT, 0);
    snprintf(reply, sizeof(reply), "m3_test_fault=%s\n",
             delay_arm ? "arm-delay" : "arm-enomem");
    return r0lab_copy_reply(out_msg, outlen, reply);

record:
    r0lab_record(R0LAB_EVENT_REJECT, result);
    return result;
}

static bool r0lab_m4_page_address_valid(uint64_t address)
{
    return address && !(address & (R0LAB_M3_PAGE_SIZE - 1UL)) &&
           address < (uint64_t)((unsigned long)-R0LAB_M3_PAGE_SIZE);
}

static int r0lab_m4_wait_for_callbacks(void)
{
    unsigned int attempt;

    for (attempt = 0; attempt < R0LAB_M3_DRAIN_LIMIT; ++attempt) {
        unsigned long flags = r0lab_lock();
        bool drained = !g_m4_inflight;

        r0lab_unlock(flags);
        if (drained)
            return 0;
        g_msleep(1);
    }
    return R0LAB_EBUSY;
}

static void r0lab_m4_before_abort(hook_fargs3_t *args, void *udata)
{
    const unsigned long far = (unsigned long)args->arg0;
    const unsigned int esr = (unsigned int)args->arg1;
    struct pt_regs *regs = (struct pt_regs *)(unsigned long)args->arg2;
    unsigned long flags;
    bool redirected = false;

    (void)udata;
    flags = r0lab_lock();
    ++g_m4_inflight;
    if (regs && (esr >> R0LAB_M3_ESR_EC_SHIFT) == R0LAB_M3_ESR_EC_IABT_LOW &&
        (esr & R0LAB_M3_ESR_FSC_TYPE) == R0LAB_M3_ESR_FSC_PERM &&
        g_session.active && g_m4_page.armed && !g_m4_page.clearing &&
        g_session.owner_tgid == r0lab_current_tgid() &&
        far == g_m4_page.source_address && regs->pc == g_m4_page.source_address) {
        /* The fixed table contains the only admitted mapping: source+0 -> clone+0. */
        regs->pc = g_m4_page.clone_address;
        args->skip_origin = 1;
        ++g_m4_page.redirect_events;
        g_m4_page.record.events = g_m4_page.redirect_events;
        redirected = true;
    }
    --g_m4_inflight;
    r0lab_unlock(flags);
    if (redirected)
        r0lab_record_regs(R0LAB_EVENT_M4_REDIRECT, 0, regs);
}

static void r0lab_m4_reset(struct mm_struct *mm)
{
    unsigned long flags = r0lab_lock();

    if (g_m4_page.mm == mm) {
        bool monitor_running = g_m4_page.monitor_running;
        uint64_t generation = g_m4_page.generation;

        memset(&g_m4_page, 0, sizeof(g_m4_page));
        g_m4_page.generation = generation;
        g_m4_page.monitor_running = monitor_running;
    }
    r0lab_unlock(flags);
    g_mmput(mm);
}

static void r0lab_m4_reset_final(struct mm_struct *mm, uint64_t generation)
{
    unsigned long flags = r0lab_lock();

    if (g_m4_page.mm == mm && g_m4_page.generation == generation)
        memset(&g_m4_page, 0, sizeof(g_m4_page));
    r0lab_unlock(flags);
    g_mmput(mm);
}

static void r0lab_m4_monitor_done(uint64_t generation)
{
    unsigned long flags = r0lab_lock();

    if (g_m4_page.generation == generation)
        g_m4_page.monitor_running = false;
    r0lab_unlock(flags);
}

static void r0lab_m4_unhook(void)
{
    bool installed;
    unsigned long flags = r0lab_lock();

    installed = g_m4_page.hook_installed;
    g_m4_page.hook_installed = false;
    r0lab_unlock(flags);
    if (installed)
        (void)r0lab_m4_wait_for_callbacks();
    if (installed)
        r0lab_hook_detach(g_do_mem_abort, r0lab_m4_before_abort, NULL);
    if (installed)
        (void)r0lab_m4_wait_for_callbacks();
}

static int r0lab_m4_monitor_worker(void *opaque)
{
    struct r0lab_m4_page *page = opaque;
    struct mm_struct *mm;
    unsigned long flags;
    uint64_t generation;
    pid_t owner_tgid;
    int result;

    flags = r0lab_lock();
    if (!page || page != &g_m4_page || !page->monitor_running ||
        !page->generation) {
        r0lab_unlock(flags);
        return 0;
    }
    generation = page->generation;
    r0lab_unlock(flags);

    for (;;) {
        bool active;
        bool page_owned;
        bool already_clearing;

        flags = r0lab_lock();
        if (g_m4_page.generation != generation || !g_m4_page.monitor_running) {
            r0lab_unlock(flags);
            return 0;
        }
        mm = g_m4_page.mm;
        owner_tgid = g_session.owner_tgid;
        active = g_session.active;
        page_owned = g_m4_page.reserving || g_m4_page.armed || g_m4_page.clearing;
        already_clearing = g_m4_page.clearing && !g_m4_page.target_exiting;
        if (!mm || !active || !page_owned) {
            g_m4_page.monitor_running = false;
            r0lab_unlock(flags);
            return 0;
        }
        r0lab_unlock(flags);

        if (already_clearing) {
            g_msleep(R0LAB_M5_MONITOR_INTERVAL_MS);
            continue;
        }
        if (!r0lab_target_mm_live(owner_tgid, mm))
            break;
        g_msleep(R0LAB_M5_MONITOR_INTERVAL_MS);
    }

    flags = r0lab_lock();
    if (g_m4_page.generation != generation || g_m4_page.mm != mm) {
        r0lab_unlock(flags);
        r0lab_m4_monitor_done(generation);
        return 0;
    }
    g_m4_page.target_exiting = true;
    g_m4_page.clearing = true;
    g_m4_page.record.state = R0LAB_PAGE_RECORD_RESTORING;
    r0lab_unlock(flags);

    r0lab_record(R0LAB_EVENT_TARGET_EXIT, 0);
    r0lab_m4_unhook();
    result = r0lab_m4_wait_for_callbacks();
    r0lab_m4_reset_final(mm, generation);
    r0lab_close_exited_session(owner_tgid);
    r0lab_record(R0LAB_EVENT_M4_CLEAR, result);
    return 0;
}

static int r0lab_m4_start_monitor(void)
{
    unsigned long flags;

    flags = r0lab_lock();
    if (!g_m4_page.armed || g_m4_page.monitor_running) {
        r0lab_unlock(flags);
        return R0LAB_EBUSY;
    }
    g_m4_page.monitor_running = true;
    r0lab_unlock(flags);

    if (!g_m4_monitor_worker_task)
        return R0LAB_ESRCH;
    g_wake_up_process(g_m4_monitor_worker_task);
    return 0;
}

static int r0lab_m4_arm_worker(void *opaque)
{
    struct r0lab_m4_page *page = opaque;
    struct mm_struct *mm = NULL;
    unsigned long flags;
    uint64_t generation;
    pid_t owner_tgid;
    bool cancelled;
    bool target_gone;
    int cleanup_result;
    int result;

    flags = r0lab_lock();
    if (!page || page != &g_m4_page || !page->reserving || !page->mm ||
        !g_session.active) {
        if (page == &g_m4_page && page->mm) {
            mm = page->mm;
            memset(&g_m4_page, 0, sizeof(g_m4_page));
        }
        r0lab_unlock(flags);
        if (mm)
            g_mmput(mm);
        r0lab_record(R0LAB_EVENT_REJECT, R0LAB_ESRCH);
        return 0;
    }
    mm = page->mm;
    generation = page->generation;
    owner_tgid = g_session.owner_tgid;
    r0lab_unlock(flags);

    result = hook_wrap3(g_do_mem_abort, r0lab_m4_before_abort, NULL, NULL);
    if (result)
        goto fail;
    flags = r0lab_lock();
    if (page->mm == mm)
        page->hook_installed = true;
    r0lab_unlock(flags);

    if (!r0lab_target_mm_live(owner_tgid, mm))
        goto target_exit;

    result = r0lab_raw_capture(&page->raw);
    if (result)
        goto fail_unhook;
    result = r0lab_raw_arm_source_uxn_only(&page->raw);
    if (result)
        goto fail_unhook;

    target_gone = !r0lab_target_mm_live(owner_tgid, mm);
    flags = r0lab_lock();
    cancelled = page->clearing;
    if (target_gone && page->mm == mm) {
        page->target_exiting = true;
        page->clearing = true;
    }
    if (!cancelled && !target_gone && page->mm == mm) {
        page->reserving = false;
        page->armed = true;
        page->record.state = R0LAB_PAGE_RECORD_SOURCE_UXN;
    }
    r0lab_unlock(flags);
    if (target_gone)
        goto target_exit;
    if (!cancelled) {
        result = r0lab_m4_start_monitor();
        if (result)
            goto fail_restore;
        r0lab_record(R0LAB_EVENT_M4_ARM, 0);
        return 0;
    }

fail_restore:
    cleanup_result = result;
    flags = r0lab_lock();
    if (page == &g_m4_page && page->mm == mm)
        page->record.state = R0LAB_PAGE_RECORD_RESTORING;
    r0lab_unlock(flags);
    result = r0lab_raw_restore_original(&page->raw);
    flags = r0lab_lock();
    if (page == &g_m4_page && page->mm == mm)
        page->record.state = R0LAB_PAGE_RECORD_RESTORING;
    r0lab_unlock(flags);
    r0lab_m4_unhook();
    if (!result)
        result = r0lab_m4_wait_for_callbacks();
    if (!result)
        result = cleanup_result;
    r0lab_m4_reset(mm);
    r0lab_record(R0LAB_EVENT_M4_CLEAR, result);
    return 0;

fail_unhook:
    r0lab_m4_unhook();
    (void)r0lab_m4_wait_for_callbacks();
fail:
    r0lab_m4_reset(mm);
    r0lab_record(R0LAB_EVENT_REJECT, result);
    return 0;

target_exit:
    r0lab_record(R0LAB_EVENT_TARGET_EXIT, 0);
    flags = r0lab_lock();
    if (page == &g_m4_page && page->mm == mm)
        page->record.state = R0LAB_PAGE_RECORD_RESTORING;
    r0lab_unlock(flags);
    r0lab_m4_unhook();
    result = r0lab_m4_wait_for_callbacks();
    r0lab_m4_reset_final(mm, generation);
    r0lab_close_exited_session(owner_tgid);
    r0lab_record(R0LAB_EVENT_M4_CLEAR, result);
    return 0;
}

static int r0lab_m4_clear_worker(void *opaque)
{
    struct r0lab_m4_page *page = opaque;
    struct mm_struct *mm;
    unsigned long flags;
    int result;

    flags = r0lab_lock();
    if (!page || page != &g_m4_page || !page->armed || !page->clearing ||
        !page->mm) {
        r0lab_unlock(flags);
        r0lab_record(R0LAB_EVENT_REJECT, R0LAB_ESRCH);
        return 0;
    }
    mm = page->mm;
    r0lab_unlock(flags);

    result = r0lab_raw_restore_original(&page->raw);
    r0lab_m4_unhook();
    if (!result)
        result = r0lab_m4_wait_for_callbacks();
    r0lab_m4_reset(mm);
    r0lab_record(R0LAB_EVENT_M4_CLEAR, result);
    return 0;
}

static int r0lab_m4_start_worker(int (*worker_fn)(void *))
{
    (void)worker_fn;

    if (!g_m4_worker)
        return R0LAB_ESRCH;
    g_wake_up_process(g_m4_worker);
    return 0;
}

static int r0lab_m4_worker_loop(void *opaque)
{
    (void)opaque;
    while (!r0lab_worker_should_stop()) {
        bool arm_pending;
        bool clear_pending;
        unsigned long flags = r0lab_lock();

        arm_pending = g_m4_page.reserving;
        clear_pending = !arm_pending && g_m4_page.armed &&
                        g_m4_page.clearing;
        r0lab_unlock(flags);
        if (arm_pending) {
            r0lab_m4_arm_worker(&g_m4_page);
            continue;
        }
        if (clear_pending) {
            r0lab_m4_clear_worker(&g_m4_page);
            continue;
        }
        g_msleep(R0LAB_M5_MONITOR_INTERVAL_MS);
    }
    r0lab_worker_finished();
    return 0;
}

static int r0lab_m4_monitor_loop(void *opaque)
{
    (void)opaque;
    while (!r0lab_worker_should_stop()) {
        bool monitor_pending;
        unsigned long flags = r0lab_lock();

        monitor_pending = g_m4_page.monitor_running;
        r0lab_unlock(flags);
        if (monitor_pending) {
            r0lab_m4_monitor_worker(&g_m4_page);
            continue;
        }
        g_msleep(R0LAB_M5_MONITOR_INTERVAL_MS);
    }
    r0lab_worker_finished();
    return 0;
}

static long r0lab_m4_arm(uint64_t token, uint64_t source_address,
                          uint64_t clone_address, char __user *out_msg,
                          int outlen)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    struct mm_struct *mm;
    unsigned long flags;
    int result = r0lab_validate_owner(token);

    if (result)
        goto record;
    if (!r0lab_m4_page_address_valid(source_address) ||
        !r0lab_m4_page_address_valid(clone_address) ||
        source_address == clone_address) {
        result = R0LAB_EINVAL;
        goto record;
    }
    mm = g_get_task_mm(current);
    if (!mm) {
        result = R0LAB_ESRCH;
        goto record;
    }
    flags = r0lab_lock();
    if (!g_session.active || g_session.owner_tgid != r0lab_current_tgid() ||
        g_session.token != token || r0lab_m3_slot_count_locked() ||
        r0lab_m4_slot_count_locked() || r0lab_raw_slot_count_locked() ||
        r0lab_s4_slot_count_locked()) {
        r0lab_unlock(flags);
        g_mmput(mm);
        result = R0LAB_EBUSY;
        goto record;
    }
    memset(&g_m4_page, 0, sizeof(g_m4_page));
    g_m4_page.mm = mm;
    g_m4_page.source_address = (unsigned long)source_address;
    g_m4_page.clone_address = (unsigned long)clone_address;
    g_m4_page.raw.mm = mm;
    g_m4_page.raw.address = (unsigned long)source_address;
    g_m4_page.generation = ++g_m4_generation;
    g_m4_page.record.source_address = g_m4_page.source_address;
    g_m4_page.record.peer_address = g_m4_page.clone_address;
    g_m4_page.record.generation = g_m4_page.generation;
    g_m4_page.record.backend = R0LAB_PAGE_RECORD_M4_VISIBLE_CLONE;
    g_m4_page.record.state = R0LAB_PAGE_RECORD_PREPARING;
    g_m4_page.reserving = true;
    r0lab_unlock(flags);

    result = r0lab_m4_start_worker(r0lab_m4_arm_worker);
    if (result) {
        r0lab_m4_reset(mm);
        goto record;
    }
    snprintf(reply, sizeof(reply), "m4_pending source=%llx clone=%llx bytes=%lu\n",
             source_address, clone_address, R0LAB_M4_SEQUENCE_BYTES);
    return r0lab_copy_reply(out_msg, outlen, reply);

record:
    r0lab_record(R0LAB_EVENT_REJECT, result);
    return result;
}

static long r0lab_m4_ready(uint64_t token, char __user *out_msg, int outlen)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    unsigned long flags;
    unsigned long source_address;
    unsigned long clone_address;
    uint8_t record_backend;
    uint8_t record_state;
    int result = r0lab_validate_owner(token);

    if (result)
        return result;
    flags = r0lab_lock();
    if (g_m4_page.reserving || g_m4_page.clearing) {
        r0lab_unlock(flags);
        return R0LAB_EAGAIN;
    }
    if (!g_m4_page.armed) {
        r0lab_unlock(flags);
        return R0LAB_ENOENT;
    }
    source_address = g_m4_page.source_address;
    clone_address = g_m4_page.clone_address;
    record_backend = g_m4_page.record.backend;
    record_state = g_m4_page.record.state;
    r0lab_unlock(flags);
    snprintf(reply, sizeof(reply),
             "m4_ready source=%llx clone=%llx offset=0 record_backend=%s record_state=%s source_transition=pte_uxn pte_switch=1\n",
             source_address, clone_address,
             r0lab_page_record_backend_name(record_backend),
             r0lab_page_record_state_name(record_state));
    return r0lab_copy_reply(out_msg, outlen, reply);
}

static long r0lab_m4_observed(uint64_t token, char __user *out_msg, int outlen)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    unsigned long flags;
    uint32_t redirect_events;
    uint8_t record_backend;
    uint8_t record_state;
    int result = r0lab_validate_owner(token);

    if (result)
        return result;
    flags = r0lab_lock();
    if (!g_m4_page.armed || g_m4_page.clearing) {
        r0lab_unlock(flags);
        return R0LAB_EAGAIN;
    }
    redirect_events = g_m4_page.redirect_events;
    record_backend = g_m4_page.record.backend;
    record_state = g_m4_page.record.state;
    r0lab_unlock(flags);
    snprintf(reply, sizeof(reply),
             "m4_observed redirects=%u record_backend=%s record_state=%s source_transition=pte_uxn pte_switch=1\n",
             redirect_events, r0lab_page_record_backend_name(record_backend),
             r0lab_page_record_state_name(record_state));
    return r0lab_copy_reply(out_msg, outlen, reply);
}

static long r0lab_m4_clear(uint64_t token, char __user *out_msg, int outlen)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    unsigned long flags;
    int result = r0lab_validate_owner(token);

    if (result)
        goto record;
    flags = r0lab_lock();
    if ((!g_m4_page.reserving && !g_m4_page.armed) || g_m4_page.clearing) {
        r0lab_unlock(flags);
        result = R0LAB_EAGAIN;
        goto record;
    }
    g_m4_page.record.state = R0LAB_PAGE_RECORD_RESTORING;
    g_m4_page.clearing = true;
    if (g_m4_page.reserving) {
        r0lab_unlock(flags);
        return r0lab_copy_reply(out_msg, outlen, "m4_cancel_pending\n");
    }
    r0lab_unlock(flags);
    result = r0lab_m4_start_worker(r0lab_m4_clear_worker);
    if (result) {
        flags = r0lab_lock();
        if (g_m4_page.armed)
            g_m4_page.clearing = false;
        r0lab_unlock(flags);
        goto record;
    }
    snprintf(reply, sizeof(reply), "m4_clear_pending\n");
    return r0lab_copy_reply(out_msg, outlen, reply);

record:
    r0lab_record(R0LAB_EVENT_REJECT, result);
    return result;
}

static long r0lab_m4_cleared(uint64_t token, char __user *out_msg, int outlen)
{
    unsigned long flags;
    int result = r0lab_validate_owner(token);

    if (result)
        return result;
    flags = r0lab_lock();
    if (r0lab_m4_slot_count_locked() || r0lab_s4_slot_count_locked()) {
        r0lab_unlock(flags);
        return R0LAB_EAGAIN;
    }
    r0lab_unlock(flags);
    return r0lab_copy_reply(out_msg, outlen, "m4_cleared\n");
}

static int r0lab_raw_wait_for_callbacks(void)
{
    unsigned int attempt;

    for (attempt = 0; attempt < R0LAB_M3_DRAIN_LIMIT; ++attempt) {
        unsigned long flags = r0lab_lock();
        bool drained = !g_raw_inflight;

        r0lab_unlock(flags);
        if (drained)
            return 0;
        g_msleep(1);
    }
    return R0LAB_EBUSY;
}

static void r0lab_raw_before_abort_passthrough(hook_fargs3_t *args, void *udata)
{
    (void)args;
    (void)udata;
}

static void r0lab_raw_before_abort_mmget_passthrough(hook_fargs3_t *args,
                                                     void *udata)
{
    struct mm_struct *current_mm;

    (void)args;
    (void)udata;
    if (!g_get_task_mm || !g_mmput)
        return;
    current_mm = g_get_task_mm(current);
    if (current_mm)
        g_mmput(current_mm);
}

static void r0lab_raw_before_abort_lock_passthrough(hook_fargs3_t *args,
                                                    void *udata)
{
    struct mm_struct *current_mm;
    unsigned long flags;

    (void)args;
    (void)udata;
    if (!g_get_task_mm || !g_mmput)
        return;
    current_mm = g_get_task_mm(current);
    if (!current_mm)
        return;
    flags = r0lab_lock();
    r0lab_unlock(flags);
    g_mmput(current_mm);
}

static void r0lab_raw_before_abort_inflight_passthrough(
    hook_fargs3_t *args, void *udata)
{
    struct mm_struct *current_mm;
    unsigned long flags;

    (void)args;
    (void)udata;
    if (!g_get_task_mm || !g_mmput)
        return;
    current_mm = g_get_task_mm(current);
    if (!current_mm)
        return;
    flags = r0lab_lock();
    ++g_raw_inflight;
    r0lab_unlock(flags);
    g_mmput(current_mm);
    flags = r0lab_lock();
    --g_raw_inflight;
    r0lab_unlock(flags);
}

static void r0lab_raw_iabt_route_compiler_witness(
    const struct r0lab_raw_shadow_page *page, uint64_t generation,
    unsigned long pc)
{
    asm volatile("" : : "r"(page), "r"(generation), "r"(pc) : "memory");
}

static void r0lab_raw_before_abort_iabt_route(hook_fargs3_t *args,
                                              void *udata)
{
    unsigned long far;
    unsigned int esr;
    struct pt_regs *regs;
    struct mm_struct *current_mm;
    struct r0lab_raw_hook_page_token route_token = {0};
    unsigned long flags;

    (void)udata;
    if (!g_initialized || !args || !g_get_task_mm || !g_mmput)
        return;
    far = (unsigned long)args->arg0;
    esr = (unsigned int)args->arg1;
    regs = (struct pt_regs *)(unsigned long)args->arg2;
    current_mm = g_get_task_mm(current);
    if (!current_mm)
        return;

    flags = r0lab_lock();
    ++g_raw_inflight;
    if (regs &&
        (esr >> R0LAB_M3_ESR_EC_SHIFT) == R0LAB_M3_ESR_EC_IABT_LOW &&
        (esr & R0LAB_M3_ESR_FSC_TYPE) == R0LAB_M3_ESR_FSC_PERM &&
        g_session.active &&
        g_session.owner_tgid == r0lab_current_tgid()) {
        int route_result =
            r0lab_raw_hook_page_token_acquire_readonly_locked(
                R0LAB_RAW_HOOK_ABORT, current_mm, far,
                R0LAB_RAW_HOOK_ROUTE_ALLOW_SOURCE_UXN |
                    R0LAB_RAW_HOOK_ROUTE_ALLOW_ORIGINAL_READ,
                &route_token);

        if (!route_result) {
            struct r0lab_raw_shadow_page *page = route_token.page;
            unsigned long page_address = page->raw.address;
            bool state_allowed =
                page->raw.state == R0LAB_RAW_SOURCE_UXN ||
                (page->raw.state == R0LAB_RAW_ORIGINAL_READ &&
                 page->raw.read_cycle_active);

            if (page->abort_hook_iabt_route &&
                page->generation == route_token.generation &&
                regs->pc >= page_address &&
                regs->pc < page_address + R0LAB_RAW_PAGE_SIZE &&
                state_allowed)
                r0lab_raw_iabt_route_compiler_witness(
                    page, route_token.generation, regs->pc);
            (void)r0lab_raw_hook_page_token_release_locked(
                &route_token, false);
        }
    }
    r0lab_unlock(flags);
    g_mmput(current_mm);
    flags = r0lab_lock();
    --g_raw_inflight;
    r0lab_unlock(flags);
}

static void r0lab_raw_before_abort_iabt_transition(hook_fargs3_t *args,
                                                   void *udata)
{
    unsigned long far;
    unsigned int esr;
    struct pt_regs *regs;
    struct mm_struct *current_mm;
    struct r0lab_raw_hook_page_token route_token = {0};
    struct r0lab_raw_shadow_page *fault_page = NULL;
    unsigned long flags;
    uint64_t generation = 0;
    bool read_cycle_resume = false;
    bool should_handle = false;
    int result = R0LAB_EINVAL;

    (void)udata;
    if (!args || !g_get_task_mm || !g_mmput)
        return;
    far = (unsigned long)args->arg0;
    esr = (unsigned int)args->arg1;
    regs = (struct pt_regs *)(unsigned long)args->arg2;
    current_mm = g_get_task_mm(current);
    if (!current_mm)
        return;

    flags = r0lab_lock();
    ++g_raw_inflight;
    if (regs &&
        (esr >> R0LAB_M3_ESR_EC_SHIFT) == R0LAB_M3_ESR_EC_IABT_LOW &&
        (esr & R0LAB_M3_ESR_FSC_TYPE) == R0LAB_M3_ESR_FSC_PERM &&
        g_session.active &&
        g_session.owner_tgid == r0lab_current_tgid() &&
        !r0lab_raw_hook_page_token_acquire_iabt_transition_locked(
            current_mm, far, &route_token)) {
        struct r0lab_raw_shadow_page *page = route_token.page;
        unsigned long page_address = page->raw.address;

        if (page->abort_hook_iabt_transition &&
            page->generation == route_token.generation &&
            regs->pc >= page_address &&
            regs->pc < page_address + R0LAB_RAW_PAGE_SIZE &&
            (page->raw.state == R0LAB_RAW_SOURCE_UXN ||
             (page->raw.state == R0LAB_RAW_ORIGINAL_READ &&
              page->raw.read_cycle_active))) {
            read_cycle_resume =
                page->raw.state == R0LAB_RAW_ORIGINAL_READ;
            page->transitioning = true;
            fault_page = page;
            generation = route_token.generation;
            should_handle = true;
        } else {
            (void)r0lab_raw_hook_page_token_release_locked(
                &route_token, false);
        }
    }
    r0lab_unlock(flags);

    if (should_handle) {
        if (read_cycle_resume)
            result = r0lab_raw_finish_read_cycle(&fault_page->raw);
        else
            result = r0lab_raw_activate_shadow(&fault_page->raw);
    }

    flags = r0lab_lock();
    if (should_handle && route_token.page == fault_page &&
        fault_page->generation == generation &&
        fault_page->transitioning &&
        !r0lab_raw_hook_page_token_release_locked(&route_token, true) &&
        !result &&
        (fault_page->raw.state == R0LAB_RAW_SHADOW_RX ||
         fault_page->raw.state == R0LAB_RAW_SHADOW_XOM)) {
        if (!read_cycle_resume) {
            ++fault_page->activation_events;
            fault_page->record.events = fault_page->activation_events;
            fault_page->record.source_pfn = fault_page->raw.source_pfn;
            fault_page->record.shadow_pfn = fault_page->raw.shadow_pfn;
        }
        fault_page->record.state = R0LAB_PAGE_RECORD_SHADOW_ACTIVE;
        args->skip_origin = 1;
        args->ret = 0;
    }
    r0lab_unlock(flags);
    g_mmput(current_mm);
    flags = r0lab_lock();
    --g_raw_inflight;
    r0lab_unlock(flags);
}

static void r0lab_raw_before_abort(hook_fargs3_t *args, void *udata)
{
    unsigned long far;
    unsigned int esr;
    unsigned int ec;
    struct pt_regs *regs;
    struct mm_struct *current_mm;
    struct r0lab_raw_hook_page_token route_token = {0};
    struct r0lab_raw_shadow_page *fault_page = NULL;
    uint64_t generation = 0;
    unsigned long flags;
    bool iabt_hit = false;
    bool iabt_committed = false;
    bool read_cycle_resume = false;
    bool abort_probe_hit = false;
    bool read_cycle_hit = false;
    bool write_release_hit = false;
    uint32_t abort_probe_kind = 0;
    uint32_t abort_probe_events = 0;
    uint32_t write_release_events = 0;
    uint32_t write_release_failures = 0;
    uint32_t abort_read_cycle_events = 0;
    uint32_t abort_read_cycle_failures = 0;
    enum {
        R0LAB_RAW_ABORT_BRANCH_NONE = 0,
        R0LAB_RAW_ABORT_BRANCH_IABT,
        R0LAB_RAW_ABORT_BRANCH_READ,
        R0LAB_RAW_ABORT_BRANCH_WRITE,
        R0LAB_RAW_ABORT_BRANCH_XOM_READ,
    } branch = R0LAB_RAW_ABORT_BRANCH_NONE;
    int branch_result = R0LAB_EINVAL;
    int write_release_result = R0LAB_EINVAL;
    int abort_read_cycle_result = R0LAB_EINVAL;

    (void)udata;
    if (!g_initialized || !args || !g_get_task_mm || !g_mmput)
        return;
    far = (unsigned long)args->arg0;
    esr = (unsigned int)args->arg1;
    regs = (struct pt_regs *)(unsigned long)args->arg2;
    current_mm = g_get_task_mm(current);
    if (!current_mm)
        return;
    ec = esr >> R0LAB_M3_ESR_EC_SHIFT;

    flags = r0lab_lock();
    ++g_raw_inflight;
    if (regs && ec == R0LAB_M3_ESR_EC_IABT_LOW &&
        (esr & R0LAB_M3_ESR_FSC_TYPE) == R0LAB_M3_ESR_FSC_PERM &&
        g_session.active &&
        g_session.owner_tgid == r0lab_current_tgid()) {
        int route_result =
            r0lab_raw_hook_page_token_acquire_full_abort_locked(
            current_mm, far,
            R0LAB_RAW_HOOK_ROUTE_MUTATING |
            R0LAB_RAW_HOOK_ROUTE_ALLOW_SOURCE_UXN |
                R0LAB_RAW_HOOK_ROUTE_ALLOW_ORIGINAL_READ,
            &route_token);

        if (!route_result) {
            fault_page = route_token.page;
            if (regs->pc >= fault_page->raw.address &&
                regs->pc <
                    fault_page->raw.address + R0LAB_RAW_PAGE_SIZE &&
                (fault_page->raw.state == R0LAB_RAW_SOURCE_UXN ||
                 (fault_page->raw.state == R0LAB_RAW_ORIGINAL_READ &&
                  fault_page->raw.read_cycle_active))) {
                read_cycle_resume =
                    fault_page->raw.state == R0LAB_RAW_ORIGINAL_READ;
                fault_page->transitioning = true;
                generation = route_token.generation;
                branch = R0LAB_RAW_ABORT_BRANCH_IABT;
            } else {
                (void)r0lab_raw_hook_page_token_release_locked(
                    &route_token, false);
                fault_page = NULL;
            }
        }
    } else if (regs && ec == R0LAB_M3_ESR_EC_DABT_LOW &&
               g_session.active &&
               g_session.owner_tgid == r0lab_current_tgid()) {
        bool translation_read =
            (esr & R0LAB_M3_ESR_FSC_TYPE) ==
                R0LAB_M3_ESR_FSC_TRANSLATION &&
            !(esr & R0LAB_M3_ESR_WNR);
        bool permission_write =
            (esr & R0LAB_M3_ESR_FSC_TYPE) == R0LAB_M3_ESR_FSC_PERM &&
            (esr & R0LAB_M3_ESR_WNR);
        bool permission_read =
            (esr & R0LAB_M3_ESR_FSC_TYPE) == R0LAB_M3_ESR_FSC_PERM &&
            !(esr & R0LAB_M3_ESR_WNR);
        int route_result =
            r0lab_raw_hook_page_token_acquire_full_abort_locked(
                current_mm, far,
                R0LAB_RAW_HOOK_ROUTE_REQUIRE_SHADOW_RX |
                    R0LAB_RAW_HOOK_ROUTE_ALLOW_SHADOW_XOM,
                &route_token);

        if (!route_result) {
            bool shadow_rx_state;
            bool shadow_xom_state;

            fault_page = route_token.page;
            shadow_rx_state =
                fault_page->raw.state == R0LAB_RAW_SHADOW_RX;
            shadow_xom_state =
                fault_page->raw.state == R0LAB_RAW_SHADOW_XOM;
            if (translation_read && shadow_rx_state &&
                fault_page->abort_read_cycle_armed) {
                if (!fault_page->raw.gup_hide_active &&
                    !fault_page->raw.fork_hide_active &&
                    !fault_page->raw.read_cycle_active) {
                    fault_page->transitioning = true;
                    generation = route_token.generation;
                    branch = R0LAB_RAW_ABORT_BRANCH_READ;
                } else {
                    fault_page = NULL;
                }
            } else if (permission_write &&
                       shadow_rx_state &&
                       fault_page->abort_write_release_armed) {
                fault_page->transitioning = true;
                generation = route_token.generation;
                branch = R0LAB_RAW_ABORT_BRANCH_WRITE;
            } else if (fault_page->abort_probe_armed) {
                bool abort_probe_match =
                    (fault_page->abort_probe_source ==
                         R0LAB_ABORT_PROBE_SOURCE_READ_TRANSLATION &&
                     translation_read && shadow_rx_state) ||
                    (fault_page->abort_probe_source ==
                         R0LAB_ABORT_PROBE_SOURCE_WRITE_PERMISSION &&
                     permission_write && shadow_rx_state) ||
                    (fault_page->abort_probe_source ==
                         R0LAB_ABORT_PROBE_SOURCE_READ_PERMISSION &&
                     permission_read && shadow_xom_state);

                if (abort_probe_match) {
                    abort_probe_kind = (esr & R0LAB_M3_ESR_WNR) ?
                                       R0LAB_FAULT_KIND_WRITE :
                                       R0LAB_FAULT_KIND_READ;
                    if (abort_probe_kind == R0LAB_FAULT_KIND_WRITE)
                        ++fault_page->abort_probe_write_events;
                    else
                        ++fault_page->abort_probe_read_events;
                    abort_probe_events =
                        fault_page->abort_probe_read_events +
                        fault_page->abort_probe_write_events +
                        fault_page->abort_probe_exec_events;
                    fault_page->abort_probe_last_esr = esr;
                    fault_page->abort_probe_last_far = far;
                    abort_probe_hit = true;
                    if (fault_page->abort_probe_source ==
                            R0LAB_ABORT_PROBE_SOURCE_READ_PERMISSION &&
                        permission_read &&
                        shadow_xom_state &&
                        !fault_page->raw.gup_hide_active &&
                        !fault_page->raw.fork_hide_active &&
                        !fault_page->raw.read_cycle_active) {
                        fault_page->transitioning = true;
                        generation = route_token.generation;
                        branch = R0LAB_RAW_ABORT_BRANCH_XOM_READ;
                    }
                }
                if (branch == R0LAB_RAW_ABORT_BRANCH_NONE)
                    fault_page = NULL;
            } else {
                fault_page = NULL;
            }
            if (branch == R0LAB_RAW_ABORT_BRANCH_NONE)
                (void)r0lab_raw_hook_page_token_release_locked(
                    &route_token, false);
        }
    }
    r0lab_unlock(flags);

    if (branch == R0LAB_RAW_ABORT_BRANCH_IABT && fault_page) {
        if (read_cycle_resume)
            branch_result =
                r0lab_raw_finish_read_cycle(&fault_page->raw);
        else
            branch_result =
                r0lab_raw_activate_shadow(&fault_page->raw);
    } else if (branch == R0LAB_RAW_ABORT_BRANCH_READ && fault_page) {
        abort_read_cycle_result =
            r0lab_raw_begin_fault_read_cycle(&fault_page->raw);
    } else if (branch == R0LAB_RAW_ABORT_BRANCH_WRITE && fault_page) {
        write_release_result =
            r0lab_raw_restore_original(&fault_page->raw);
    } else if (branch == R0LAB_RAW_ABORT_BRANCH_XOM_READ && fault_page) {
        abort_read_cycle_result =
            r0lab_raw_begin_xom_read_cycle(&fault_page->raw);
    }

    flags = r0lab_lock();
    if (branch == R0LAB_RAW_ABORT_BRANCH_IABT && fault_page &&
        fault_page->generation == generation &&
        fault_page->transitioning) {
        iabt_hit = true;
        if (!r0lab_raw_hook_page_token_release_locked(
                &route_token, true) &&
            !branch_result &&
            (fault_page->raw.state == R0LAB_RAW_SHADOW_RX ||
             fault_page->raw.state == R0LAB_RAW_SHADOW_XOM)) {
            if (!read_cycle_resume) {
                ++fault_page->activation_events;
                fault_page->record.events = fault_page->activation_events;
                fault_page->record.source_pfn =
                    fault_page->raw.source_pfn;
                fault_page->record.shadow_pfn =
                    fault_page->raw.shadow_pfn;
            }
            fault_page->record.state = R0LAB_PAGE_RECORD_SHADOW_ACTIVE;
            args->skip_origin = 1;
            args->ret = 0;
            iabt_committed = true;
        }
    }
    if (branch == R0LAB_RAW_ABORT_BRANCH_READ && fault_page &&
        fault_page->generation == generation &&
        fault_page->transitioning &&
        !r0lab_raw_hook_page_token_release_locked(&route_token, true)) {
        fault_page->abort_read_cycle_armed = false;
        ++fault_page->abort_read_cycle_events;
        if (abort_read_cycle_result)
            ++fault_page->abort_read_cycle_failures;
        fault_page->abort_read_cycle_last_esr = esr;
        fault_page->abort_read_cycle_last_far = far;
        fault_page->abort_read_cycle_last_result = abort_read_cycle_result;
        abort_read_cycle_events = fault_page->abort_read_cycle_events;
        abort_read_cycle_failures = fault_page->abort_read_cycle_failures;
        if (!abort_read_cycle_result &&
            fault_page->raw.state == R0LAB_RAW_ORIGINAL_READ) {
            fault_page->record.state = R0LAB_PAGE_RECORD_ORIGINAL_READ;
            args->skip_origin = 1;
            args->ret = 0;
        }
        read_cycle_hit = true;
    }
    if (branch == R0LAB_RAW_ABORT_BRANCH_XOM_READ && fault_page &&
        fault_page->generation == generation &&
        fault_page->transitioning &&
        !r0lab_raw_hook_page_token_release_locked(&route_token, true)) {
        fault_page->abort_probe_armed = false;
        if (abort_read_cycle_result)
            ++fault_page->abort_probe_failures;
        if (!abort_read_cycle_result &&
            fault_page->raw.state == R0LAB_RAW_ORIGINAL_READ) {
            fault_page->record.state = R0LAB_PAGE_RECORD_ORIGINAL_READ;
            args->skip_origin = 1;
            args->ret = 0;
        }
        read_cycle_hit = true;
    }
    if (branch == R0LAB_RAW_ABORT_BRANCH_WRITE && fault_page &&
        fault_page->generation == generation &&
        fault_page->transitioning &&
        !r0lab_raw_hook_page_token_release_locked(&route_token, true)) {
        fault_page->abort_write_release_armed = false;
        ++fault_page->abort_write_release_events;
        if (write_release_result)
            ++fault_page->abort_write_release_failures;
        fault_page->abort_write_release_last_esr = esr;
        fault_page->abort_write_release_last_far = far;
        fault_page->abort_write_release_last_result = write_release_result;
        fault_page->record.state = R0LAB_PAGE_RECORD_RESTORING;
        if (!write_release_result)
            r0lab_raw_deactivate_patch_records_locked(fault_page);
        write_release_events = fault_page->abort_write_release_events;
        write_release_failures =
            fault_page->abort_write_release_failures;
        write_release_hit = true;
    }
    r0lab_unlock(flags);

    if (iabt_committed)
        r0lab_record_regs(read_cycle_resume ?
                          R0LAB_EVENT_RAW_READ_CYCLE_FINISH :
                          R0LAB_EVENT_RAW_ACTIVATE, 0, regs);
    else if (iabt_hit)
        r0lab_record_regs(R0LAB_EVENT_REJECT, branch_result, regs);
    if (abort_probe_hit)
        r0lab_record_values(R0LAB_EVENT_RAW_ABORT_PROBE_HIT, 0, far, esr,
                            abort_probe_kind | ((uint64_t)abort_probe_events
                                                << 32));
    if (read_cycle_hit)
        r0lab_record_values(R0LAB_EVENT_RAW_ABORT_READ_CYCLE_BEGIN,
                            abort_read_cycle_result, far, esr,
                            abort_read_cycle_events |
                                ((uint64_t)abort_read_cycle_failures << 32));
    if (write_release_hit)
        r0lab_record_values(R0LAB_EVENT_RAW_ABORT_WRITE_RELEASE,
                            write_release_result, far, esr,
                            write_release_events |
                                ((uint64_t)write_release_failures << 32));
    g_mmput(current_mm);
    flags = r0lab_lock();
    --g_raw_inflight;
    r0lab_unlock(flags);
}

static __attribute__((noinline)) void
r0lab_raw_before_abort_full_iabt(hook_fargs3_t *args, void *udata)
{
    unsigned long far;
    unsigned int esr;
    struct pt_regs *regs;
    struct mm_struct *current_mm;
    struct r0lab_raw_hook_page_token route_token = {0};
    struct r0lab_raw_shadow_page *fault_page = NULL;
    unsigned long flags;
    uint64_t generation = 0;
    uint16_t slot_id = R0LAB_RAW_PRIMARY_SLOT;
    unsigned long page_address = 0;
    unsigned long final_state = R0LAB_RAW_EMPTY;
    bool read_cycle_resume = false;
    bool should_handle = false;
    bool committed = false;
    int result = R0LAB_EINVAL;

    (void)udata;
    if (!args || !g_get_task_mm || !g_mmput)
        return;
    far = (unsigned long)args->arg0;
    esr = (unsigned int)args->arg1;
    regs = (struct pt_regs *)(unsigned long)args->arg2;
    current_mm = g_get_task_mm(current);
    if (!current_mm)
        return;

    flags = r0lab_lock();
    ++g_raw_inflight;
    if (regs &&
        (esr >> R0LAB_M3_ESR_EC_SHIFT) == R0LAB_M3_ESR_EC_IABT_LOW &&
        (esr & R0LAB_M3_ESR_FSC_TYPE) == R0LAB_M3_ESR_FSC_PERM &&
        g_session.active &&
        g_session.owner_tgid == r0lab_current_tgid() &&
        !r0lab_raw_hook_page_token_acquire_full_abort_locked(
            current_mm, far,
            R0LAB_RAW_HOOK_ROUTE_MUTATING |
                R0LAB_RAW_HOOK_ROUTE_ALLOW_SOURCE_UXN |
                R0LAB_RAW_HOOK_ROUTE_ALLOW_ORIGINAL_READ,
            &route_token)) {
        struct r0lab_raw_shadow_page *page = route_token.page;

        page_address = page->raw.address;
        if (r0lab_raw_page_uses_full_abort_callback_locked(page) &&
            page->generation == route_token.generation &&
            regs->pc >= page_address &&
            regs->pc < page_address + R0LAB_RAW_PAGE_SIZE &&
            (page->raw.state == R0LAB_RAW_SOURCE_UXN ||
             (page->raw.state == R0LAB_RAW_ORIGINAL_READ &&
              page->raw.read_cycle_active))) {
            read_cycle_resume =
                page->raw.state == R0LAB_RAW_ORIGINAL_READ;
            page->transitioning = true;
            fault_page = page;
            generation = route_token.generation;
            slot_id = route_token.slot_id;
            should_handle = true;
        } else {
            (void)r0lab_raw_hook_page_token_release_locked(
                &route_token, false);
        }
    }
    r0lab_unlock(flags);

    if (should_handle) {
        pr_info("r0lab-r3o: iabt_transition_begin slot=%u generation=%llu mm=%px va=%lx far=%lx pc=%lx resume=%u\n",
                slot_id, generation, current_mm, page_address, far,
                regs->pc, read_cycle_resume ? 1U : 0U);
        if (read_cycle_resume)
            result = r0lab_raw_finish_read_cycle(&fault_page->raw);
        else
            result = r0lab_raw_activate_shadow(&fault_page->raw);
    }

    flags = r0lab_lock();
    if (should_handle && route_token.page == fault_page &&
        fault_page->generation == generation &&
        fault_page->transitioning &&
        !r0lab_raw_hook_page_token_release_locked(&route_token, true) &&
        !result &&
        (fault_page->raw.state == R0LAB_RAW_SHADOW_RX ||
         fault_page->raw.state == R0LAB_RAW_SHADOW_XOM)) {
        if (!read_cycle_resume) {
            ++fault_page->activation_events;
            fault_page->record.events = fault_page->activation_events;
            fault_page->record.source_pfn = fault_page->raw.source_pfn;
            fault_page->record.shadow_pfn = fault_page->raw.shadow_pfn;
        }
        fault_page->record.state = R0LAB_PAGE_RECORD_SHADOW_ACTIVE;
        args->skip_origin = 1;
        args->ret = 0;
        committed = true;
    }
    if (should_handle && fault_page->generation == generation)
        final_state = fault_page->raw.state;
    r0lab_unlock(flags);

    if (should_handle)
        pr_info("r0lab-r3o: iabt_transition_end slot=%u generation=%llu result=%d state=%lu committed=%u skip_origin=%llu\n",
                slot_id, generation, result, final_state,
                committed ? 1U : 0U,
                (unsigned long long)args->skip_origin);
    if (committed)
        r0lab_record_regs(read_cycle_resume ?
                          R0LAB_EVENT_RAW_READ_CYCLE_FINISH :
                          R0LAB_EVENT_RAW_ACTIVATE, 0, regs);
    else if (should_handle)
        r0lab_record_regs(R0LAB_EVENT_REJECT, result, regs);
    g_mmput(current_mm);
    flags = r0lab_lock();
    --g_raw_inflight;
    r0lab_unlock(flags);
}

static bool r0lab_raw_dabt_route_armed_unlocked(void)
{
    unsigned int index;

    for (index = 0; index < R0LAB_RAW_PAGE_SLOT_CAPACITY; ++index) {
        const struct r0lab_raw_shadow_page *page =
            &g_raw_page_table.slots[index];

        if (page->hook_installed &&
            (page->abort_read_cycle_armed ||
             page->abort_write_release_armed ||
             page->abort_probe_armed))
            return true;
    }
    return false;
}

static void r0lab_raw_before_abort_compact(hook_fargs3_t *args, void *udata)
{
    unsigned int esr;
    unsigned int ec;

    if (!args)
        return;
    esr = (unsigned int)args->arg1;
    ec = esr >> R0LAB_M3_ESR_EC_SHIFT;
    if (ec == R0LAB_M3_ESR_EC_IABT_LOW &&
        (esr & R0LAB_M3_ESR_FSC_TYPE) == R0LAB_M3_ESR_FSC_PERM) {
        r0lab_raw_before_abort_full_iabt(args, udata);
        return;
    }
    if (ec == R0LAB_M3_ESR_EC_DABT_LOW &&
        g_session.active &&
        g_session.owner_tgid == r0lab_current_tgid() &&
        r0lab_raw_dabt_route_armed_unlocked())
        r0lab_raw_before_abort(args, udata);
}

static void r0lab_raw_gup_before_common(void *vma, unsigned long address,
                                        hook_local_t *local)
{
    void *vma_mm = r0lab_raw_vma_mm(vma);
    struct r0lab_raw_hook_page_token route_token;
    struct r0lab_raw_shadow_page *page = NULL;
    uint64_t generation = 0;
    uint16_t slot_id = R0LAB_RAW_PRIMARY_SLOT;
    unsigned long flags;
    bool should_hide = false;
    int result = R0LAB_EINVAL;

    local->data0 = 0;
    local->data1 = 0;
    local->data2 = 0;
    local->data7 = 1;

    flags = r0lab_lock();
    ++g_raw_inflight;
    page = r0lab_raw_page_find_by_mm_addr_locked(vma_mm, address);
    if (g_session.active && page && page->gup_hook_installed) {
        result = r0lab_raw_page_find_for_hook_locked(
            R0LAB_RAW_HOOK_GUP, vma_mm, address, 0,
            R0LAB_RAW_HOOK_ROUTE_MUTATING |
                R0LAB_RAW_HOOK_ROUTE_REQUIRE_SHADOW_RX,
            &route_token);
        if (!result && route_token.page &&
            route_token.page->gup_hook_installed &&
            r0lab_raw_vma_matches(&route_token.page->raw, vma, address)) {
            page = route_token.page;
            generation = route_token.generation;
            slot_id = route_token.slot_id;
            should_hide = true;
        } else if (!result) {
            (void)r0lab_raw_hook_page_token_release_locked(&route_token,
                                                           true);
            result = R0LAB_EAGAIN;
        }
    }
    r0lab_unlock(flags);

    if (should_hide)
        result = r0lab_raw_begin_gup_hide(&page->raw);

    flags = r0lab_lock();
    if (should_hide && page->generation == generation &&
        page->slot_id == slot_id) {
        page->transitioning = false;
        if (!result) {
            ++page->gup_hook_begin_events;
            local->data0 = 1;
            local->data1 = generation;
            local->data2 = slot_id;
        } else {
            ++page->gup_hook_failures;
        }
    }
    r0lab_unlock(flags);

    if (should_hide)
        r0lab_record(R0LAB_EVENT_RAW_GUP_HOOK_BEGIN, result);
}

static void r0lab_raw_gup_after_common(hook_local_t *local)
{
    uint64_t generation = local->data1;
    uint16_t slot_id = (uint16_t)local->data2;
    struct r0lab_raw_shadow_page *page = NULL;
    unsigned long flags;
    bool should_finish = local->data0 == 1;
    int result = R0LAB_EINVAL;

    if (should_finish) {
        flags = r0lab_lock();
        page = r0lab_raw_page_slot_locked(slot_id);
        if (page && page->generation == generation &&
            page->raw.gup_hide_active && !page->transitioning) {
            page->transitioning = true;
        } else {
            should_finish = false;
        }
        r0lab_unlock(flags);
    }

    if (should_finish)
        result = r0lab_raw_finish_gup_hide(&page->raw);

    flags = r0lab_lock();
    if (should_finish && page->generation == generation &&
        page->slot_id == slot_id) {
        page->transitioning = false;
        if (!result)
            ++page->gup_hook_finish_events;
        else
            ++page->gup_hook_failures;
    }
    if (local->data7 == 1)
        --g_raw_inflight;
    r0lab_unlock(flags);

    if (should_finish)
        r0lab_record(R0LAB_EVENT_RAW_GUP_HOOK_FINISH, result);
}

static void r0lab_raw_gup_pte_before(hook_fargs5_t *args, void *udata)
{
    (void)udata;
    r0lab_raw_gup_before_common((void *)args->arg0,
                                (unsigned long)args->arg1, &args->local);
}

static void r0lab_raw_gup_pte_after(hook_fargs5_t *args, void *udata)
{
    (void)udata;
    r0lab_raw_gup_after_common(&args->local);
}

static void r0lab_raw_gup_mask_before(hook_fargs4_t *args, void *udata)
{
    (void)udata;
    r0lab_raw_gup_before_common((void *)args->arg0,
                                (unsigned long)args->arg1, &args->local);
}

static void r0lab_raw_gup_mask_after(hook_fargs4_t *args, void *udata)
{
    (void)udata;
    r0lab_raw_gup_after_common(&args->local);
}

static void r0lab_raw_fault_before(hook_fargs4_t *args, void *udata)
{
    void *vma = (void *)(unsigned long)args->arg0;
    void *vma_mm = r0lab_raw_vma_mm(vma);
    unsigned long address = (unsigned long)args->arg1;
    unsigned int fault_flags = (unsigned int)args->arg2;
    unsigned long flags;
    struct r0lab_raw_page fault_probe_page;
    struct r0lab_raw_hook_page_token route_token;
    struct r0lab_raw_shadow_page *fault_page;
    uint32_t kind = R0LAB_FAULT_KIND_READ;
    bool hit = false;

    (void)udata;
    if (fault_flags & R0LAB_FAULT_FLAG_INSTRUCTION)
        kind = R0LAB_FAULT_KIND_EXEC;
    else if (fault_flags & R0LAB_FAULT_FLAG_WRITE)
        kind = R0LAB_FAULT_KIND_WRITE;

    flags = r0lab_lock();
    if (!r0lab_raw_fault_hook_users_locked()) {
        r0lab_unlock(flags);
        return;
    }
    ++g_raw_inflight;
    if (g_session.active && g_session.owner_tgid == r0lab_current_tgid() &&
        vma_mm && (fault_flags & R0LAB_FAULT_FLAG_USER) &&
        !r0lab_raw_hook_page_token_acquire_locked(
            R0LAB_RAW_HOOK_FAULT, vma_mm, address, 0,
            R0LAB_RAW_HOOK_ROUTE_REQUIRE_SHADOW_RX, &route_token)) {
        fault_page = route_token.page;
        if (fault_page && fault_page->fault_hook_installed &&
            r0lab_raw_vma_matches(&fault_page->raw, vma, address)) {
            if (kind == R0LAB_FAULT_KIND_EXEC)
                ++fault_page->fault_hook_exec_events;
            else if (kind == R0LAB_FAULT_KIND_WRITE)
                ++fault_page->fault_hook_write_events;
            else
                ++fault_page->fault_hook_read_events;
            hit = true;
        }
        (void)r0lab_raw_hook_page_token_release_locked(&route_token, false);
    }
    if (!hit && g_session.active && g_raw_page.armed &&
        !g_raw_page.clearing && !g_raw_page.transitioning &&
        g_raw_page.raw.state == R0LAB_RAW_SHADOW_RX &&
        g_raw_page.fault_probe_armed && g_raw_page.fault_probe_address &&
        g_raw_page.fault_probe_reader_tgid == r0lab_current_tgid() &&
        (fault_flags & R0LAB_FAULT_FLAG_REMOTE)) {
        fault_probe_page = g_raw_page.raw;
        fault_probe_page.address = g_raw_page.fault_probe_address;
        if (r0lab_raw_vma_matches(&fault_probe_page, vma, address)) {
            if (kind == R0LAB_FAULT_KIND_EXEC)
                ++g_raw_page.fault_probe_exec_events;
            else if (kind == R0LAB_FAULT_KIND_WRITE)
                ++g_raw_page.fault_probe_write_events;
            else
                ++g_raw_page.fault_probe_read_events;
            hit = true;
        }
    }
    r0lab_unlock(flags);

    if (hit)
        r0lab_record_values(R0LAB_EVENT_RAW_FAULT_HOOK_HIT, 0, address,
                            fault_flags, kind);

    flags = r0lab_lock();
    --g_raw_inflight;
    r0lab_unlock(flags);
}

static void r0lab_raw_syscall_before(hook_fargs1_t *args, void *udata)
{
    struct mm_struct *current_mm;
    struct r0lab_raw_hook_page_token route_token = {0};
    struct r0lab_raw_shadow_page *page = NULL;
    unsigned long flags;
    unsigned long address = 0;
    unsigned long state = 0;
    uint64_t generation = 0;
    uint16_t slot_id = R0LAB_RAW_PRIMARY_SLOT;
    uint32_t events = 0;
    uint32_t read_cycle_events = 0;
    bool hit = false;
    bool read_cycle_mode = false;
    bool should_begin_read_cycle = false;
    int read_cycle_result = R0LAB_EINVAL;

    (void)args;
    (void)udata;
    current_mm = g_get_task_mm ? g_get_task_mm(current) : NULL;
    flags = r0lab_lock();
    if (!r0lab_raw_syscall_hook_users_locked()) {
        r0lab_unlock(flags);
        if (current_mm)
            g_mmput(current_mm);
        return;
    }
    ++g_raw_inflight;
    page = r0lab_raw_selected_page_locked();
    if (g_session.active && page && page->syscall_hook_installed &&
        current_mm && g_session.owner_tgid == r0lab_current_tgid()) {
        unsigned int route_flags = R0LAB_RAW_HOOK_ROUTE_REQUIRE_SHADOW_RX;
        int route_result;

        read_cycle_mode = page->syscall_hook_read_cycle_mode;
        if (read_cycle_mode)
            route_flags |= R0LAB_RAW_HOOK_ROUTE_MUTATING;
        route_result = r0lab_raw_hook_page_token_acquire_locked(
            R0LAB_RAW_HOOK_SYSCALL, current_mm, page->raw.address,
            page->generation, route_flags, &route_token);
        if (!route_result && route_token.page == page &&
            page->raw.mm == current_mm && page->syscall_hook_installed) {
            ++page->syscall_hook_events;
            events = page->syscall_hook_events;
            address = page->raw.address;
            state = page->raw.state;
            generation = route_token.generation;
            slot_id = route_token.slot_id;
            if (read_cycle_mode)
                should_begin_read_cycle = true;
            else
                (void)r0lab_raw_hook_page_token_release_locked(
                    &route_token, false);
            hit = true;
        } else {
            if (!route_result)
                (void)r0lab_raw_hook_page_token_release_locked(
                    &route_token, read_cycle_mode);
            if (page)
                ++page->syscall_hook_failures;
            read_cycle_result = route_result ? route_result : R0LAB_EAGAIN;
        }
    }
    r0lab_unlock(flags);

    if (should_begin_read_cycle)
        read_cycle_result = r0lab_raw_begin_read_cycle(&page->raw);

    if (should_begin_read_cycle) {
        flags = r0lab_lock();
        page = r0lab_raw_page_slot_locked(slot_id);
        if (page && page->generation == generation &&
            page->syscall_hook_installed) {
            page->transitioning = false;
            if (!read_cycle_result &&
                page->raw.state == R0LAB_RAW_ORIGINAL_READ) {
                page->record.state = R0LAB_PAGE_RECORD_ORIGINAL_READ;
                ++page->syscall_hook_read_cycle_events;
                read_cycle_events =
                    page->syscall_hook_read_cycle_events;
                state = page->raw.state;
            } else {
                ++page->syscall_hook_failures;
            }
        } else {
            read_cycle_result = R0LAB_EAGAIN;
        }
        r0lab_unlock(flags);
    }

    if (current_mm)
        g_mmput(current_mm);

    if (hit)
        r0lab_record_values(R0LAB_EVENT_RAW_SYSCALL_HOOK_HIT, 0, address,
                            state, events);
    if (read_cycle_mode && (should_begin_read_cycle || read_cycle_result))
        r0lab_record_values(R0LAB_EVENT_RAW_SYSCALL_READ_CYCLE_BEGIN,
                            read_cycle_result, address, state,
                            read_cycle_events);

    flags = r0lab_lock();
    --g_raw_inflight;
    r0lab_unlock(flags);
}

static void r0lab_raw_apply_seed_range(struct r0lab_raw_shadow_page *page,
                                       unsigned int offset,
                                       unsigned int length)
{
    uint32_t seed_words[3];
    unsigned int seed_length;
    unsigned int range_end = offset + length;
    unsigned int copy_start;
    unsigned int copy_end;

    if (page->s4_shadow_brk_layout) {
        seed_words[0] = R0LAB_S4_BRK_COMMENT << 5 | 0xd4200000U;
        seed_words[1] = page->s4_shadow_reg_layout ?
                        R0LAB_RAW_CODE_MOV_X0_X1 :
                        R0LAB_RAW_CODE_MOV_W0_99;
        seed_words[2] = 0xd65f03c0U;
        seed_length = sizeof(seed_words);
    } else {
        seed_words[0] = R0LAB_RAW_CODE_MOV_W0_99;
        seed_length = sizeof(seed_words[0]);
    }

    copy_start = offset;
    if (copy_start > seed_length)
        copy_start = seed_length;
    copy_end = range_end;
    if (copy_end > seed_length)
        copy_end = seed_length;
    if (copy_end <= copy_start)
        return;
    memcpy((char *)page->raw.shadow_kaddr + copy_start,
           (char *)seed_words + copy_start, copy_end - copy_start);
}

static bool r0lab_patch_overlaps(const struct r0lab_patch_record *record,
                                 unsigned int offset, unsigned int length)
{
    unsigned int record_end;
    unsigned int range_end;

    if (!record->active || !record->data || !record->length)
        return false;
    record_end = (unsigned int)record->offset + record->length;
    range_end = offset + length;
    return range_end > record->offset && offset < record_end;
}

static void r0lab_raw_sync_patch_metadata_locked(
    struct r0lab_raw_shadow_page *page)
{
    unsigned int active = 0;
    unsigned int dirty = 0;
    unsigned int index;

    memset(page->patch_dirty, 0, sizeof(page->patch_dirty));
    for (index = 0; index < page->patch_record_slots; ++index) {
        const struct r0lab_patch_record *record = &page->patch_records[index];
        unsigned int start_byte;
        unsigned int end_byte;
        unsigned int range_end;
        unsigned int byte_index;

        if (!record->active || !record->data || !record->length)
            continue;
        ++active;
        range_end = (unsigned int)record->offset + record->length;
        start_byte = record->offset >> 3;
        end_byte = (range_end - 1U) >> 3;
        if (start_byte == end_byte) {
            unsigned int first_bit = record->offset & 7U;
            unsigned int last_bit = (range_end - 1U) & 7U;
            unsigned int mask = ((1U << (last_bit - first_bit + 1U)) - 1U)
                                << first_bit;

            page->patch_dirty[start_byte] |= (uint8_t)mask;
            continue;
        }
        page->patch_dirty[start_byte] |=
            (uint8_t)(0xffU << (record->offset & 7U));
        for (byte_index = start_byte + 1U; byte_index < end_byte; ++byte_index)
            page->patch_dirty[byte_index] = 0xffU;
        page->patch_dirty[end_byte] |=
            (uint8_t)((1U << (((range_end - 1U) & 7U) + 1U)) - 1U);
    }
    for (index = 0; index < sizeof(page->patch_dirty); ++index) {
        uint8_t value = page->patch_dirty[index];

        while (value) {
            dirty += value & 1U;
            value >>= 1;
        }
    }
    page->patch_active_count = (uint16_t)active;
    page->patch_dirty_bytes = (uint16_t)dirty;
}

static int r0lab_raw_rebuild_patch_range(struct r0lab_raw_shadow_page *page,
                                         unsigned int offset,
                                         unsigned int length)
{
    uint16_t *order;
    void *source_kaddr;
    unsigned int order_count = 0;
    unsigned int range_end;
    unsigned int index;

    if (!page || !length || offset >= R0LAB_RAW_PAGE_SIZE ||
        length > R0LAB_RAW_PAGE_SIZE - offset)
        return R0LAB_EINVAL;
    if (!page->raw.shadow_kaddr || !g_sync_icache_aliases)
        return R0LAB_ENOSYS;
    source_kaddr = r0lab_raw_source_kernel_address(&page->raw);
    if (!source_kaddr)
        return R0LAB_EFAULT;

    order = page->patch_rebuild_order;
    range_end = offset + length;
    for (index = 0; index < page->patch_record_slots; ++index) {
        unsigned int insert_at;

        if (!r0lab_patch_overlaps(&page->patch_records[index], offset, length))
            continue;
        insert_at = order_count;
        while (insert_at &&
               page->patch_records[order[insert_at - 1U]].version >
               page->patch_records[index].version) {
            order[insert_at] = order[insert_at - 1U];
            --insert_at;
        }
        order[insert_at] = (uint16_t)index;
        ++order_count;
    }

    memcpy((char *)page->raw.shadow_kaddr + offset,
           (char *)source_kaddr + offset, length);
    r0lab_raw_apply_seed_range(page, offset, length);
    for (index = 0; index < order_count; ++index) {
        const struct r0lab_patch_record *record =
            &page->patch_records[order[index]];
        unsigned int record_end = (unsigned int)record->offset + record->length;
        unsigned int copy_start = offset > record->offset ?
                                  offset : record->offset;
        unsigned int copy_end = range_end < record_end ?
                                range_end : record_end;

        if (copy_end <= copy_start)
            continue;
        memcpy((char *)page->raw.shadow_kaddr + copy_start,
               (char *)record->data + copy_start - record->offset,
               copy_end - copy_start);
    }
    g_sync_icache_aliases((unsigned long)page->raw.shadow_kaddr + offset,
                          (unsigned long)page->raw.shadow_kaddr + range_end);
    return 0;
}

static int r0lab_raw_upsert_patch_locked(
    struct r0lab_raw_shadow_page *page, unsigned int offset,
    unsigned int length, void *data, unsigned int *record_index,
    struct r0lab_patch_record *old_record, unsigned int *rebuild_length)
{
    int free_index = -1;
    unsigned int index;

    for (index = 0; index < page->patch_record_slots; ++index) {
        struct r0lab_patch_record *record = &page->patch_records[index];

        if (record->active && record->offset == offset) {
            free_index = (int)index;
            break;
        }
        if (free_index < 0 && !record->active && !record->data)
            free_index = (int)index;
    }
    if (free_index < 0) {
        if (page->patch_record_slots == R0LAB_PATCH_RECORD_CAPACITY)
            return R0LAB_ENOSPC;
        free_index = page->patch_record_slots++;
    }

    *record_index = (unsigned int)free_index;
    *old_record = page->patch_records[*record_index];
    *rebuild_length = old_record->active && old_record->length > length ?
                      old_record->length : length;
    ++page->patch_version;
    if (!page->patch_version)
        page->patch_version = 1;
    page->patch_records[*record_index].offset = (uint16_t)offset;
    page->patch_records[*record_index].length = (uint16_t)length;
    page->patch_records[*record_index].active = 1;
    page->patch_records[*record_index].version = page->patch_version;
    page->patch_records[*record_index].data = data;
    r0lab_raw_sync_patch_metadata_locked(page);
    return 0;
}

static int r0lab_raw_release_patch_locked(
    struct r0lab_raw_shadow_page *page, unsigned int offset,
    unsigned int *record_index, struct r0lab_patch_record *old_record)
{
    unsigned int index;

    for (index = 0; index < page->patch_record_slots; ++index) {
        struct r0lab_patch_record *record = &page->patch_records[index];

        if (!record->active || record->offset != offset)
            continue;
        *record_index = index;
        *old_record = *record;
        record->length = 0;
        record->active = 0;
        record->version = 0;
        record->data = NULL;
        r0lab_raw_sync_patch_metadata_locked(page);
        return 0;
    }
    return R0LAB_ENOENT;
}

static void *r0lab_raw_detach_one_patch_buffer_locked(
    struct r0lab_raw_shadow_page *page)
{
    unsigned int index;

    for (index = 0; index < page->patch_record_slots; ++index) {
        struct r0lab_patch_record *record = &page->patch_records[index];
        void *data = record->data;

        if (!data)
            continue;
        record->length = 0;
        record->active = 0;
        record->version = 0;
        record->data = NULL;
        r0lab_raw_sync_patch_metadata_locked(page);
        return data;
    }
    return NULL;
}

static void r0lab_raw_deactivate_patch_records_locked(
    struct r0lab_raw_shadow_page *page)
{
    unsigned int index;

    for (index = 0; index < page->patch_record_slots; ++index)
        page->patch_records[index].active = 0;
    r0lab_raw_sync_patch_metadata_locked(page);
}

static void r0lab_raw_drain_patch_buffers_page(
    struct r0lab_raw_shadow_page *page, struct mm_struct *mm,
    uint64_t generation, bool match_generation)
{
    if (!g_vfree)
        return;
    for (;;) {
        void *buffer;
        unsigned long flags = r0lab_lock();

        if (!page || page->raw.mm != mm ||
            (match_generation && page->generation != generation)) {
            r0lab_unlock(flags);
            return;
        }
        buffer = r0lab_raw_detach_one_patch_buffer_locked(page);
        r0lab_unlock(flags);
        if (!buffer)
            return;
        g_vfree(buffer);
    }
}

static void r0lab_raw_prctl_before(hook_fargs1_t *args, void *udata)
{
    struct r0lab_patch_record old_record = {0};
    struct r0lab_prctl_patch_request request = {0};
    struct r0lab_raw_hook_page_token route_token = {0};
    struct r0lab_raw_shadow_page *event_page = NULL;
    struct r0lab_raw_shadow_page *page = NULL;
    struct pt_regs *syscall_regs;
    struct mm_struct *current_mm = NULL;
    void *patch_kaddr = NULL;
    void *patch_data = NULL;
    void *new_record_data = NULL;
    void *old_data_to_free = NULL;
    unsigned long flags;
    unsigned long address = 0;
    unsigned long page_address = 0;
    unsigned long state = 0;
    unsigned long user_request = 0;
    unsigned long user_request_size = 0;
    uint64_t token;
    uint64_t operation;
    uint64_t generation = 0;
    uint32_t events = 0;
    uint32_t read_cycle_events = 0;
    uint32_t patch_events = 0;
    uint32_t release_events = 0;
    uint32_t reject_events = 0;
    uint16_t active_patch_count = 0;
    uint32_t patch_before = 0;
    uint32_t patch_after = 0;
    unsigned int range_offset = 0;
    unsigned int range_length = 0;
    unsigned int rebuild_length = 0;
    unsigned int record_index = 0;
    bool admitted = false;
    bool session_valid = false;
    bool installed;
    bool should_begin_read_cycle = false;
    bool should_patch = false;
    bool should_release = false;
    bool range_operation = false;
    bool record_changed = false;
    int result = R0LAB_EPERM;

    (void)udata;
    flags = r0lab_lock();
    ++g_raw_inflight;
    installed = r0lab_raw_prctl_hook_users_locked() != 0;
    r0lab_unlock(flags);

    syscall_regs = (struct pt_regs *)(unsigned long)args->arg0;
    if (!installed || !syscall_regs ||
        (uint32_t)syscall_regs->regs[0] != R0LAB_PRCTL_MAGIC)
        goto out;

    token = syscall_regs->regs[1];
    operation = syscall_regs->regs[2];
    current_mm = g_get_task_mm ? g_get_task_mm(current) : NULL;
    session_valid = current_uid() == g_session.lab_uid && g_session.active &&
                    token == g_session.token &&
                    g_session.owner_tgid == r0lab_current_tgid() &&
                    current_mm;

    if (operation == R0LAB_PRCTL_OP_READ_CYCLE) {
        if (syscall_regs->regs[3] || syscall_regs->regs[4]) {
            result = R0LAB_EINVAL;
        } else {
            should_begin_read_cycle = true;
        }
    } else if (operation == R0LAB_PRCTL_OP_PATCH_WORD) {
        address = syscall_regs->regs[3];
        if (!g_sync_icache_aliases || !g_vmalloc || !g_vfree) {
            result = R0LAB_ENOSYS;
        } else if (syscall_regs->regs[4] >> 32 ||
                   (address & (sizeof(uint32_t) - 1UL))) {
            result = R0LAB_EINVAL;
        } else {
            patch_after = (uint32_t)syscall_regs->regs[4];
            range_length = sizeof(uint32_t);
            should_patch = true;
        }
    } else if (operation == R0LAB_PRCTL_OP_RELEASE_PATCH) {
        address = syscall_regs->regs[3];
        if (syscall_regs->regs[4] ||
            (address & (sizeof(uint32_t) - 1UL))) {
            result = R0LAB_EINVAL;
        } else if (!g_sync_icache_aliases) {
            result = R0LAB_ENOSYS;
        } else {
            range_length = sizeof(uint32_t);
            should_release = true;
        }
    } else if (operation == R0LAB_PRCTL_OP_PATCH_RANGE) {
        user_request = syscall_regs->regs[3];
        user_request_size = syscall_regs->regs[4];
        range_operation = true;
        if (!g_copy_from_user_nofault || !g_sync_icache_aliases ||
            !g_vmalloc || !g_vfree) {
            result = R0LAB_ENOSYS;
        } else if (!user_request ||
                   user_request_size <= sizeof(request) ||
                   user_request_size >
                       sizeof(request) + R0LAB_RAW_PAGE_SIZE) {
            result = R0LAB_EINVAL;
        } else if (g_copy_from_user_nofault(
                       &request, (const void __user *)user_request,
                       sizeof(request))) {
            result = R0LAB_EFAULT;
        } else if (!request.length ||
                   request.length > R0LAB_RAW_PAGE_SIZE ||
                   request.flags ||
                   user_request_size != sizeof(request) + request.length) {
            result = R0LAB_EINVAL;
        } else {
            address = (unsigned long)request.address;
            range_length = request.length;
            should_patch = true;
        }
    } else if (operation == R0LAB_PRCTL_OP_RELEASE_RANGE) {
        address = syscall_regs->regs[3];
        range_operation = true;
        if (syscall_regs->regs[4]) {
            result = R0LAB_EINVAL;
        } else if (!g_sync_icache_aliases) {
            result = R0LAB_ENOSYS;
        } else {
            range_length = 1U;
            should_release = true;
        }
    } else {
        result = R0LAB_EINVAL;
    }

    flags = r0lab_lock();
    if (should_begin_read_cycle) {
        event_page = r0lab_raw_selected_page_locked();
        if (event_page) {
            page_address = event_page->raw.address;
            address = page_address;
            state = event_page->raw.state;
        }
    } else if ((should_patch || should_release || range_operation) &&
               current_mm && address) {
        event_page = r0lab_raw_page_find_by_fault_locked(current_mm, address,
                                                         0);
        if (event_page) {
            page_address = event_page->raw.address;
            state = event_page->raw.state;
        }
    }
    if (!session_valid) {
        result = R0LAB_EPERM;
    } else if (result == R0LAB_EPERM) {
        if (should_begin_read_cycle) {
            if (!event_page || !event_page->prctl_hook_installed) {
                result = R0LAB_EAGAIN;
            } else {
                result = r0lab_raw_hook_page_token_acquire_locked(
                    R0LAB_RAW_HOOK_PRCTL, current_mm, event_page->raw.address,
                    event_page->generation,
                    R0LAB_RAW_HOOK_ROUTE_MUTATING |
                    R0LAB_RAW_HOOK_ROUTE_REQUIRE_SHADOW_RX, &route_token);
            }
        } else if (should_patch || should_release) {
            if (!event_page || !event_page->prctl_hook_installed) {
                result = R0LAB_EINVAL;
            } else if (operation == R0LAB_PRCTL_OP_PATCH_WORD &&
                       (address < page_address ||
                        address > page_address + R0LAB_RAW_PAGE_SIZE -
                                  sizeof(uint32_t))) {
                result = R0LAB_EINVAL;
            } else if (operation == R0LAB_PRCTL_OP_RELEASE_PATCH &&
                       (address < page_address ||
                        address > page_address + R0LAB_RAW_PAGE_SIZE -
                                  sizeof(uint32_t))) {
                result = R0LAB_EINVAL;
            } else if (operation == R0LAB_PRCTL_OP_PATCH_RANGE &&
                       (address < page_address ||
                        address - page_address >
                            R0LAB_RAW_PAGE_SIZE - range_length)) {
                result = R0LAB_EINVAL;
            } else if (operation == R0LAB_PRCTL_OP_RELEASE_RANGE &&
                       (address < page_address ||
                        address >= page_address + R0LAB_RAW_PAGE_SIZE)) {
                result = R0LAB_EINVAL;
            } else {
                result = r0lab_raw_hook_page_token_acquire_locked(
                    R0LAB_RAW_HOOK_PRCTL, current_mm, address, 0,
                    R0LAB_RAW_HOOK_ROUTE_MUTATING |
                    R0LAB_RAW_HOOK_ROUTE_REQUIRE_SHADOW_RX, &route_token);
            }
        }
        if (!result && route_token.page &&
            !route_token.page->prctl_hook_installed) {
            (void)r0lab_raw_hook_page_token_release_locked(&route_token,
                                                           true);
            result = R0LAB_EAGAIN;
        }
        if (!result && route_token.page) {
            page = route_token.page;
            page_address = page->raw.address;
            state = page->raw.state;
            generation = route_token.generation;
            if (should_begin_read_cycle)
                address = page_address;
            else
                range_offset = (unsigned int)(address - page_address);
            admitted = true;
        }
    }
    r0lab_unlock(flags);

    if (admitted)
        result = 0;
    if (should_patch && !range_operation && !result) {
        patch_data = g_vmalloc(range_length);
        if (!patch_data) {
            result = R0LAB_ENOMEM;
        } else {
            memcpy(patch_data, &patch_after, sizeof(patch_after));
        }
    } else if (should_patch && range_operation && !result) {
        range_offset = (unsigned int)(address - page_address);
        patch_data = g_vmalloc(range_length);
        if (!patch_data) {
            result = R0LAB_ENOMEM;
        } else if (g_copy_from_user_nofault(
                       patch_data,
                       (const char __user *)user_request + sizeof(request),
                       range_length)) {
            result = R0LAB_EFAULT;
        } else if (range_length >= sizeof(patch_after)) {
            memcpy(&patch_after, patch_data, sizeof(patch_after));
        }
    }

    if (should_begin_read_cycle && !result)
        result = r0lab_raw_begin_read_cycle(&page->raw);
    else if ((should_patch || should_release) && !result) {
        flags = r0lab_lock();
        if (!page || page->generation != generation ||
            !page->transitioning || !page->raw.shadow_kaddr ||
            page->raw.address != page_address ||
            page->raw.state != R0LAB_RAW_SHADOW_RX) {
            result = R0LAB_EAGAIN;
        } else {
            patch_kaddr = (char *)page->raw.shadow_kaddr + range_offset;
            if (R0LAB_RAW_PAGE_SIZE - range_offset >= sizeof(patch_before))
                memcpy(&patch_before, patch_kaddr, sizeof(patch_before));
            if (should_patch) {
                result = r0lab_raw_upsert_patch_locked(
                    page, range_offset, range_length, patch_data,
                    &record_index, &old_record, &rebuild_length);
                if (!result) {
                    new_record_data = patch_data;
                    patch_data = NULL;
                    record_changed = true;
                }
            } else {
                result = r0lab_raw_release_patch_locked(
                    page, range_offset, &record_index, &old_record);
                if (!result) {
                    range_length = old_record.length;
                    rebuild_length = old_record.length;
                    record_changed = true;
                }
            }
        }
        r0lab_unlock(flags);

        if (record_changed)
            result = r0lab_raw_rebuild_patch_range(page, range_offset,
                                                   rebuild_length);
        if (record_changed && result) {
            flags = r0lab_lock();
            if (page && page->generation == generation) {
                page->patch_records[record_index] = old_record;
                r0lab_raw_sync_patch_metadata_locked(page);
            }
            r0lab_unlock(flags);
            if (should_patch) {
                patch_data = new_record_data;
                new_record_data = NULL;
            }
        } else if (record_changed) {
            old_data_to_free = old_record.data;
            if (page && R0LAB_RAW_PAGE_SIZE - range_offset >=
                    sizeof(patch_after))
                memcpy(&patch_after,
                       (char *)page->raw.shadow_kaddr + range_offset,
                       sizeof(patch_after));
        }
    }

    flags = r0lab_lock();
    if (admitted && page && page->generation == generation) {
        page->transitioning = false;
        route_token.page = NULL;
        if (!result && should_begin_read_cycle &&
            page->raw.state == R0LAB_RAW_ORIGINAL_READ) {
            ++page->prctl_hook_events;
            ++page->prctl_hook_read_cycle_events;
            page->record.state = R0LAB_PAGE_RECORD_ORIGINAL_READ;
        } else if (!result && should_patch &&
                   page->raw.state == R0LAB_RAW_SHADOW_RX) {
            ++page->prctl_hook_events;
            ++page->prctl_hook_patch_events;
        } else if (!result && should_release &&
                   page->raw.state == R0LAB_RAW_SHADOW_RX) {
            ++page->prctl_hook_events;
            ++page->prctl_hook_release_events;
        } else {
            if (!result)
                result = R0LAB_EAGAIN;
            ++page->prctl_hook_failures;
        }
        if (!result) {
            events = page->prctl_hook_events;
            read_cycle_events = page->prctl_hook_read_cycle_events;
            patch_events = page->prctl_hook_patch_events;
            release_events = page->prctl_hook_release_events;
            state = page->raw.state;
        }
        reject_events = page->prctl_hook_reject_events;
        active_patch_count = page->patch_active_count;
    } else if (admitted && page) {
        result = R0LAB_EAGAIN;
        ++page->prctl_hook_failures;
        reject_events = page->prctl_hook_reject_events;
        active_patch_count = page->patch_active_count;
    } else if (event_page && event_page->prctl_hook_installed) {
        if (session_valid && operation == R0LAB_PRCTL_OP_PATCH_RANGE)
            ++event_page->prctl_hook_failures;
        else
            ++event_page->prctl_hook_reject_events;
        reject_events = event_page->prctl_hook_reject_events;
        active_patch_count = event_page->patch_active_count;
    }
    r0lab_unlock(flags);

    args->skip_origin = 1;
    args->ret = (uint64_t)(int64_t)result;
    r0lab_record_values(R0LAB_EVENT_RAW_PRCTL_TRIGGER, result, address,
                        operation, events | ((uint64_t)reject_events << 32));
    if (should_begin_read_cycle)
        r0lab_record_values(R0LAB_EVENT_RAW_SYSCALL_READ_CYCLE_BEGIN,
                            result, address, state, read_cycle_events);
    else if (should_patch && range_operation)
        r0lab_record_values(R0LAB_EVENT_RAW_PRCTL_PATCH_RANGE, result,
                            address, range_length,
                            active_patch_count |
                                ((uint64_t)patch_events << 32));
    else if (should_patch)
        r0lab_record_values(R0LAB_EVENT_RAW_PRCTL_PATCH, result, address,
                            patch_before,
                            patch_after | ((uint64_t)patch_events << 32));
    else if (should_release && range_operation)
        r0lab_record_values(R0LAB_EVENT_RAW_PRCTL_RELEASE_RANGE, result,
                            address, range_length,
                            active_patch_count |
                                ((uint64_t)release_events << 32));
    else if (should_release)
        r0lab_record_values(R0LAB_EVENT_RAW_PRCTL_RELEASE, result, address,
                            patch_before,
                            patch_after | ((uint64_t)release_events << 32));

out:
    if (route_token.page) {
        flags = r0lab_lock();
        (void)r0lab_raw_hook_page_token_release_locked(&route_token, true);
        r0lab_unlock(flags);
    }
    if (old_data_to_free && g_vfree)
        g_vfree(old_data_to_free);
    if (patch_data && g_vfree)
        g_vfree(patch_data);
    if (current_mm)
        g_mmput(current_mm);
    flags = r0lab_lock();
    --g_raw_inflight;
    r0lab_unlock(flags);
}

static void r0lab_raw_exit_mmap_before(hook_fargs1_t *args, void *udata)
{
    struct r0lab_raw_hook_page_token tokens[R0LAB_RAW_PAGE_SLOT_CAPACITY] = {0};
    int results[R0LAB_RAW_PAGE_SLOT_CAPACITY] = {0};
    void *mm;
    unsigned long flags;
    unsigned int index;

    (void)udata;
    if (!args || !g_initialized)
        return;
    mm = (void *)(unsigned long)args->arg0;
    flags = r0lab_lock();
    ++g_raw_inflight;
    for (index = 0; index < R0LAB_RAW_PAGE_SLOT_CAPACITY; ++index) {
        struct r0lab_raw_shadow_page *page =
            &g_raw_page_table.slots[index];
        struct r0lab_raw_hook_page_token *token = &tokens[index];

        if (!mm || page->raw.mm != mm || !page->raw.address ||
            !page->generation || !page->exit_hook_installed ||
            !r0lab_raw_page_slot_owned_locked(page))
            continue;
        if (page->transitioning) {
            ++page->exit_hook_failures;
            continue;
        }
        token->slot_id = page->slot_id;
        token->generation = page->generation;
        token->page = page;
        token->kind = R0LAB_RAW_HOOK_EXIT;
        page->target_exiting = true;
        page->clearing = true;
        page->transitioning = true;
        page->record.state = R0LAB_PAGE_RECORD_RESTORING;
    }
    r0lab_unlock(flags);

    for (index = 0; index < R0LAB_RAW_PAGE_SLOT_CAPACITY; ++index) {
        struct r0lab_raw_shadow_page *page = tokens[index].page;

        if (!page)
            continue;
        pr_info("r0lab-r3o: exit_restore_begin slot=%u generation=%llu mm=%px va=%lx state=%u\n",
                (unsigned int)page->slot_id,
                (unsigned long long)tokens[index].generation, mm,
                page->raw.address, (unsigned int)page->raw.state);
        results[index] =
            page->raw.state == R0LAB_RAW_RESTORED ?
                0 : r0lab_raw_restore_original(&page->raw);
        pr_info("r0lab-r3o: exit_restore_end slot=%u generation=%llu result=%d state=%u\n",
                (unsigned int)page->slot_id,
                (unsigned long long)tokens[index].generation,
                results[index], (unsigned int)page->raw.state);
    }

    flags = r0lab_lock();
    for (index = 0; index < R0LAB_RAW_PAGE_SLOT_CAPACITY; ++index) {
        struct r0lab_raw_hook_page_token *token = &tokens[index];
        struct r0lab_raw_shadow_page *page = token->page;

        if (!page || page->generation != token->generation ||
            !page->transitioning)
            continue;
        ++page->exit_hook_events;
        if (results[index])
            ++page->exit_hook_failures;
        (void)r0lab_raw_hook_page_token_release_locked(token, true);
    }
    r0lab_unlock(flags);

    for (index = 0; index < R0LAB_RAW_PAGE_SLOT_CAPACITY; ++index) {
        struct r0lab_raw_shadow_page *page =
            &g_raw_page_table.slots[index];

        if (!page->target_exiting || page->raw.mm != mm)
            continue;
        r0lab_record_values(R0LAB_EVENT_RAW_EXIT_MMAP_HIT,
                            results[index], page->raw.address,
                            page->raw.state, page->exit_hook_events);
    }

    flags = r0lab_lock();
    --g_raw_inflight;
    r0lab_unlock(flags);
}

static uint64_t r0lab_raw_fork_slot_mask(uint16_t slot_id)
{
    return 1ULL << slot_id;
}

static void r0lab_raw_fork_local_set_generation(hook_local_t *local,
                                                uint16_t slot_id,
                                                uint64_t generation)
{
    if (slot_id == 0)
        local->data2 = generation;
    else if (slot_id == 1)
        local->data3 = generation;
}

static uint64_t r0lab_raw_fork_local_generation(const hook_local_t *local,
                                                uint16_t slot_id)
{
    if (slot_id == 0)
        return local->data2;
    if (slot_id == 1)
        return local->data3;
    return 0;
}

static bool r0lab_raw_fork_page_eligible_locked(
    const struct r0lab_raw_shadow_page *page, void *oldmm)
{
    return page && page->armed && page->fork_hook_installed &&
           !page->reserving && !page->clearing && !page->transitioning &&
           page->raw.state == R0LAB_RAW_SHADOW_RX &&
           page->raw.mm == oldmm && !page->raw.gup_hide_active &&
           !page->raw.fork_hide_active && !page->raw.read_cycle_active;
}

static unsigned int r0lab_raw_fork_hook_users_locked(void)
{
    unsigned int index;
    unsigned int count = 0;

    for (index = 0; index < R0LAB_RAW_PAGE_SLOT_CAPACITY; ++index) {
        if (g_raw_page_table.slots[index].fork_hook_installed)
            ++count;
    }
    return count;
}

static void r0lab_raw_fork_before(hook_fargs2_t *args, void *udata)
{
    void *oldmm = (void *)(unsigned long)args->arg1;
    uint64_t generations[R0LAB_RAW_PAGE_SLOT_CAPACITY] = {0};
    uint64_t planned_mask = 0;
    uint64_t paused_mask = 0;
    unsigned long flags;
    unsigned int index;
    int result = 0;

    (void)udata;
    args->local.data0 = 0;
    args->local.data1 = 0;
    args->local.data2 = 0;
    args->local.data3 = 0;
    args->local.data7 = 1;

    flags = r0lab_lock();
    ++g_raw_inflight;
    if (oldmm && g_session.active &&
        g_session.owner_tgid == r0lab_current_tgid()) {
        for (index = 0; index < R0LAB_RAW_PAGE_SLOT_CAPACITY; ++index) {
            struct r0lab_raw_shadow_page *page =
                &g_raw_page_table.slots[index];

            if (!r0lab_raw_fork_page_eligible_locked(page, oldmm))
                continue;
            page->transitioning = true;
            generations[index] = page->generation;
            planned_mask |= r0lab_raw_fork_slot_mask((uint16_t)index);
        }
    }
    r0lab_unlock(flags);

    for (index = 0; index < R0LAB_RAW_PAGE_SLOT_CAPACITY; ++index) {
        struct r0lab_raw_shadow_page *page =
            &g_raw_page_table.slots[index];
        uint64_t bit = r0lab_raw_fork_slot_mask((uint16_t)index);
        int begin_result;

        if (!(planned_mask & bit))
            continue;
        begin_result = r0lab_raw_begin_fork_hide(&page->raw, oldmm);

        flags = r0lab_lock();
        if (page->generation == generations[index] &&
            page->slot_id == index) {
            if (!begin_result && page->raw.fork_hide_active) {
                ++page->fork_hook_begin_events;
                paused_mask |= bit;
            } else {
                page->transitioning = false;
                ++page->fork_hook_failures;
                if (!begin_result)
                    begin_result = R0LAB_EAGAIN;
            }
        } else {
            if (!begin_result)
                paused_mask |= bit;
            begin_result = R0LAB_EAGAIN;
        }
        r0lab_unlock(flags);

        r0lab_record(R0LAB_EVENT_RAW_FORK_HOOK_BEGIN, begin_result);
        if (begin_result) {
            result = begin_result;
            break;
        }
    }

    if (result) {
        for (index = 0; index < R0LAB_RAW_PAGE_SLOT_CAPACITY; ++index) {
            struct r0lab_raw_shadow_page *page =
                &g_raw_page_table.slots[index];
            uint64_t bit = r0lab_raw_fork_slot_mask((uint16_t)index);
            int finish_result = 0;

            if (paused_mask & bit) {
                finish_result = r0lab_raw_finish_fork_hide(&page->raw, oldmm);
                r0lab_record(R0LAB_EVENT_RAW_FORK_HOOK_FINISH,
                             finish_result);
            }
            flags = r0lab_lock();
            if ((planned_mask & bit) &&
                page->generation == generations[index] &&
                page->slot_id == index) {
                page->transitioning = false;
                if ((paused_mask & bit) && !finish_result)
                    ++page->fork_hook_finish_events;
                else if (paused_mask & bit)
                    ++page->fork_hook_failures;
            }
            r0lab_unlock(flags);
        }
        paused_mask = 0;
    }

    flags = r0lab_lock();
    if (paused_mask) {
        args->local.data0 = paused_mask;
        args->local.data1 = (uint64_t)(unsigned long)oldmm;
        for (index = 0; index < R0LAB_RAW_PAGE_SLOT_CAPACITY; ++index) {
            if (paused_mask & r0lab_raw_fork_slot_mask((uint16_t)index))
                r0lab_raw_fork_local_set_generation(
                    &args->local, (uint16_t)index, generations[index]);
        }
    } else {
        args->local.data7 = 0;
        --g_raw_inflight;
    }
    r0lab_unlock(flags);
}

static void r0lab_raw_fork_after(hook_fargs2_t *args, void *udata)
{
    uint64_t paused_mask = args->local.data0;
    void *oldmm = (void *)(unsigned long)args->local.data1;
    unsigned long flags;
    unsigned int index;

    (void)udata;
    for (index = 0; index < R0LAB_RAW_PAGE_SLOT_CAPACITY; ++index) {
        struct r0lab_raw_shadow_page *page;
        uint64_t bit = r0lab_raw_fork_slot_mask((uint16_t)index);
        uint64_t generation;
        bool should_finish = false;
        int result = R0LAB_EINVAL;

        if (!(paused_mask & bit))
            continue;
        generation = r0lab_raw_fork_local_generation(&args->local,
                                                     (uint16_t)index);
        flags = r0lab_lock();
        page = r0lab_raw_page_slot_locked((uint16_t)index);
        if (page && page->generation == generation &&
            page->raw.fork_hide_active && page->transitioning &&
            page->raw.mm == oldmm) {
            should_finish = true;
        } else if (page && page->generation == generation) {
            page->transitioning = false;
            ++page->fork_hook_failures;
        }
        r0lab_unlock(flags);

        if (should_finish)
            result = r0lab_raw_finish_fork_hide(&page->raw, oldmm);

        flags = r0lab_lock();
        if (should_finish && page->generation == generation &&
            page->slot_id == index) {
            page->transitioning = false;
            if (!result)
                ++page->fork_hook_finish_events;
            else
                ++page->fork_hook_failures;
        }
        r0lab_unlock(flags);

        if (should_finish)
            r0lab_record(R0LAB_EVENT_RAW_FORK_HOOK_FINISH, result);
    }

    flags = r0lab_lock();
    if (args->local.data7 == 1)
        --g_raw_inflight;
    r0lab_unlock(flags);
}

static void r0lab_raw_fork_hook_release(struct r0lab_raw_shadow_page *page)
{
    bool installed = false;
    bool should_detach = false;
    unsigned long flags;

    if (!page)
        return;
    flags = r0lab_lock();
    if (page->fork_hook_installed) {
        page->fork_hook_installed = false;
        installed = true;
        should_detach = r0lab_raw_fork_hook_users_locked() == 0;
    }
    r0lab_unlock(flags);

    if (installed)
        (void)r0lab_raw_wait_for_callbacks();
    if (should_detach)
        r0lab_hook_detach(g_dup_mmap, r0lab_raw_fork_before,
                          r0lab_raw_fork_after);
    if (installed)
        (void)r0lab_raw_wait_for_callbacks();
}

static void r0lab_raw_fault_hook_release(struct r0lab_raw_shadow_page *page)
{
    bool installed = false;
    bool should_detach = false;
    unsigned long flags;

    if (!page)
        return;
    flags = r0lab_lock();
    if (page->fault_hook_installed) {
        page->fault_hook_installed = false;
        page->fault_probe_armed = false;
        page->fault_probe_address = 0;
        page->fault_probe_reader_tgid = 0;
        installed = true;
        should_detach = r0lab_raw_fault_hook_users_locked() == 0;
    }
    r0lab_unlock(flags);

    if (installed)
        (void)r0lab_raw_wait_for_callbacks();
    if (should_detach)
        r0lab_hook_detach(g_handle_mm_fault, r0lab_raw_fault_before, NULL);
    if (installed)
        (void)r0lab_raw_wait_for_callbacks();
}

static unsigned int r0lab_raw_syscall_hook_users_locked(void)
{
    unsigned int index;
    unsigned int count = 0;

    for (index = 0; index < R0LAB_RAW_PAGE_SLOT_CAPACITY; ++index) {
        if (g_raw_page_table.slots[index].syscall_hook_installed)
            ++count;
    }
    return count;
}

static void r0lab_raw_syscall_hook_release(
    struct r0lab_raw_shadow_page *page)
{
    bool installed = false;
    bool should_detach = false;
    unsigned long flags;

    if (!page)
        return;
    flags = r0lab_lock();
    if (page->syscall_hook_installed) {
        page->syscall_hook_installed = false;
        page->syscall_hook_read_cycle_mode = false;
        installed = true;
        should_detach = r0lab_raw_syscall_hook_users_locked() == 0;
    }
    r0lab_unlock(flags);

    if (installed)
        (void)r0lab_raw_wait_for_callbacks();
    if (should_detach)
        r0lab_hook_detach(g_sys_getpid, r0lab_raw_syscall_before, NULL);
    if (installed)
        (void)r0lab_raw_wait_for_callbacks();
}

static void r0lab_raw_syscall_unhook(void)
{
    bool installed;
    unsigned int index;
    unsigned long flags = r0lab_lock();

    installed = r0lab_raw_syscall_hook_users_locked() != 0;
    for (index = 0; index < R0LAB_RAW_PAGE_SLOT_CAPACITY; ++index) {
        g_raw_page_table.slots[index].syscall_hook_installed = false;
        g_raw_page_table.slots[index].syscall_hook_read_cycle_mode = false;
    }
    r0lab_unlock(flags);
    if (installed)
        (void)r0lab_raw_wait_for_callbacks();
    if (installed)
        r0lab_hook_detach(g_sys_getpid, r0lab_raw_syscall_before, NULL);
    if (installed)
        (void)r0lab_raw_wait_for_callbacks();
}

static unsigned int r0lab_raw_prctl_hook_users_locked(void)
{
    unsigned int index;
    unsigned int count = 0;

    for (index = 0; index < R0LAB_RAW_PAGE_SLOT_CAPACITY; ++index) {
        if (g_raw_page_table.slots[index].prctl_hook_installed)
            ++count;
    }
    return count;
}

static void r0lab_raw_prctl_hook_release(
    struct r0lab_raw_shadow_page *page)
{
    bool installed = false;
    bool should_detach = false;
    unsigned long flags;

    if (!page)
        return;
    flags = r0lab_lock();
    if (page->prctl_hook_installed) {
        page->prctl_hook_installed = false;
        installed = true;
        should_detach = r0lab_raw_prctl_hook_users_locked() == 0;
    }
    r0lab_unlock(flags);

    if (installed)
        (void)r0lab_raw_wait_for_callbacks();
    if (should_detach)
        r0lab_hook_detach(g_sys_prctl, r0lab_raw_prctl_before, NULL);
    if (installed)
        (void)r0lab_raw_wait_for_callbacks();
}

static void r0lab_raw_prctl_unhook(void)
{
    bool installed;
    unsigned int index;
    unsigned long flags = r0lab_lock();

    installed = r0lab_raw_prctl_hook_users_locked() != 0;
    for (index = 0; index < R0LAB_RAW_PAGE_SLOT_CAPACITY; ++index)
        g_raw_page_table.slots[index].prctl_hook_installed = false;
    r0lab_unlock(flags);
    if (installed)
        (void)r0lab_raw_wait_for_callbacks();
    if (installed)
        r0lab_hook_detach(g_sys_prctl, r0lab_raw_prctl_before, NULL);
    if (installed)
        (void)r0lab_raw_wait_for_callbacks();
}

static int r0lab_raw_exit_hook_acquire(
    struct r0lab_raw_shadow_page *page)
{
    bool attached = false;
    unsigned long flags;
    int result;

    if (!page || !g_exit_mmap)
        return R0LAB_ENOSYS;
    flags = r0lab_lock();
    if (page->exit_hook_installed) {
        r0lab_unlock(flags);
        return 0;
    }
    if (!page->raw.mm || !page->mm_count_owned ||
        !r0lab_raw_page_slot_owned_locked(page)) {
        r0lab_unlock(flags);
        return R0LAB_ESRCH;
    }
    if (g_raw_exit_hook_resident) {
        page->exit_hook_installed = true;
        page->exit_hook_events = 0;
        page->exit_hook_failures = 0;
        r0lab_unlock(flags);
        return 0;
    }
    if (g_raw_exit_hook_transitioning) {
        r0lab_unlock(flags);
        return R0LAB_EBUSY;
    }
    g_raw_exit_hook_transitioning = true;
    r0lab_unlock(flags);

    result = hook_wrap1(g_exit_mmap, r0lab_raw_exit_mmap_before,
                        NULL, NULL);
    attached = result == 0;
    flags = r0lab_lock();
    if (!result && page->raw.mm && page->mm_count_owned &&
        r0lab_raw_page_slot_owned_locked(page)) {
        g_raw_exit_hook_resident = true;
        page->exit_hook_installed = true;
        page->exit_hook_events = 0;
        page->exit_hook_failures = 0;
    } else if (!result) {
        result = R0LAB_EAGAIN;
    }
    g_raw_exit_hook_transitioning = false;
    r0lab_unlock(flags);
    if (result && attached)
        r0lab_hook_detach(g_exit_mmap, r0lab_raw_exit_mmap_before, NULL);
    return result;
}

static void r0lab_raw_exit_hook_release(
    struct r0lab_raw_shadow_page *page)
{
    bool installed = false;
    unsigned long flags;

    if (!page)
        return;
    flags = r0lab_lock();
    if (page->exit_hook_installed) {
        page->exit_hook_installed = false;
        installed = true;
    }
    r0lab_unlock(flags);

    if (installed)
        (void)r0lab_raw_wait_for_callbacks();
}

static void r0lab_raw_exit_unhook(void)
{
    r0lab_raw_exit_hook_release(&g_raw_page);
}

static void r0lab_raw_exit_unhook_primary_legacy(void)
{
    r0lab_raw_exit_hook_release(&g_raw_page);
}

static unsigned int r0lab_raw_gup_hook_users_locked(void)
{
    unsigned int index;
    unsigned int count = 0;

    for (index = 0; index < R0LAB_RAW_PAGE_SLOT_CAPACITY; ++index) {
        if (g_raw_page_table.slots[index].gup_hook_installed)
            ++count;
    }
    return count;
}

static void r0lab_raw_gup_hook_release(struct r0lab_raw_shadow_page *page)
{
    bool installed = false;
    bool uses_pte = false;
    bool should_detach = false;
    unsigned long flags;

    if (!page)
        return;
    flags = r0lab_lock();
    if (page->gup_hook_installed) {
        uses_pte = page->gup_hook_uses_pte;
        page->gup_hook_installed = false;
        page->gup_hook_uses_pte = false;
        installed = true;
        should_detach = r0lab_raw_gup_hook_users_locked() == 0;
    }
    r0lab_unlock(flags);

    if (installed)
        (void)r0lab_raw_wait_for_callbacks();
    if (should_detach && uses_pte)
        r0lab_hook_detach(g_follow_page_pte, r0lab_raw_gup_pte_before,
                          r0lab_raw_gup_pte_after);
    else if (should_detach)
        r0lab_hook_detach(g_follow_page_mask, r0lab_raw_gup_mask_before,
                          r0lab_raw_gup_mask_after);
    if (installed)
        (void)r0lab_raw_wait_for_callbacks();
}

static unsigned int r0lab_raw_abort_hook_users_locked(void)
{
    unsigned int index;
    unsigned int count = 0;

    for (index = 0; index < R0LAB_RAW_PAGE_SLOT_CAPACITY; ++index) {
        if (g_raw_page_table.slots[index].hook_installed)
            ++count;
    }
    return count;
}

static hook_chain3_callback
r0lab_raw_abort_hook_callback(bool passthrough, bool mmget, bool lock,
                              bool inflight, bool iabt_route,
                              bool iabt_transition)
{
    if (iabt_transition)
        return r0lab_raw_before_abort_iabt_transition;
    if (iabt_route)
        return r0lab_raw_before_abort_iabt_route;
    if (inflight)
        return r0lab_raw_before_abort_inflight_passthrough;
    if (lock)
        return r0lab_raw_before_abort_lock_passthrough;
    if (mmget)
        return r0lab_raw_before_abort_mmget_passthrough;
    if (passthrough)
        return r0lab_raw_before_abort_passthrough;
    return r0lab_raw_before_abort_compact;
}

static hook_chain3_callback
r0lab_raw_abort_hook_installed_callback_locked(void)
{
    unsigned int index;

    for (index = 0; index < R0LAB_RAW_PAGE_SLOT_CAPACITY; ++index) {
        struct r0lab_raw_shadow_page *page =
            &g_raw_page_table.slots[index];

        if (page->hook_installed)
            return r0lab_raw_abort_hook_callback(
                page->abort_hook_passthrough, page->abort_hook_mmget,
                page->abort_hook_lock, page->abort_hook_inflight,
                page->abort_hook_iabt_route,
                page->abort_hook_iabt_transition);
    }
    return NULL;
}

static int r0lab_raw_abort_hook_acquire(struct r0lab_raw_shadow_page *page)
{
    hook_chain3_callback callback;
    hook_chain3_callback installed_callback;
    unsigned long flags;
    int result = 0;

    if (!page)
        return R0LAB_EINVAL;
    callback = r0lab_raw_abort_hook_callback(
        page->abort_hook_passthrough, page->abort_hook_mmget,
        page->abort_hook_lock, page->abort_hook_inflight,
        page->abort_hook_iabt_route, page->abort_hook_iabt_transition);
    flags = r0lab_lock();
    if (page->hook_installed) {
        r0lab_unlock(flags);
        return 0;
    }
    if (g_raw_abort_hook_transitioning) {
        r0lab_unlock(flags);
        return R0LAB_EBUSY;
    }
    installed_callback = r0lab_raw_abort_hook_installed_callback_locked();
    if (!installed_callback)
        installed_callback = g_raw_abort_resident_callback;
    if (installed_callback && installed_callback != callback) {
        r0lab_unlock(flags);
        return R0LAB_EBUSY;
    }
    if (installed_callback) {
        if (!page->raw.mm) {
            r0lab_unlock(flags);
            return R0LAB_ESRCH;
        }
        page->hook_installed = true;
        r0lab_unlock(flags);
        return 0;
    }
    g_raw_abort_hook_transitioning = true;
    r0lab_unlock(flags);

    result = hook_wrap3(g_do_mem_abort, callback, NULL, NULL);
    if (result) {
        flags = r0lab_lock();
        g_raw_abort_hook_transitioning = false;
        r0lab_unlock(flags);
        return result;
    }

    flags = r0lab_lock();
    if (page->raw.mm) {
        page->hook_installed = true;
        g_raw_abort_resident_callback = callback;
        g_raw_abort_hook_transitioning = false;
    } else {
        result = R0LAB_ESRCH;
    }
    r0lab_unlock(flags);
    if (result) {
        (void)r0lab_raw_wait_for_callbacks();
        r0lab_hook_detach(g_do_mem_abort, callback, NULL);
        (void)r0lab_raw_wait_for_callbacks();
        flags = r0lab_lock();
        g_raw_abort_hook_transitioning = false;
        r0lab_unlock(flags);
    }
    return result;
}

static void r0lab_raw_abort_hook_release(struct r0lab_raw_shadow_page *page)
{
    hook_chain3_callback callback = NULL;
    bool iabt_transition = false;
    bool iabt_route = false;
    bool inflight = false;
    bool lock = false;
    bool mmget = false;
    bool passthrough = false;
    bool installed = false;
    bool should_detach = false;
    unsigned long flags;

    if (!page)
        return;
    flags = r0lab_lock();
    if (page->hook_installed) {
        iabt_transition = page->abort_hook_iabt_transition;
        iabt_route = page->abort_hook_iabt_route;
        inflight = page->abort_hook_inflight;
        lock = page->abort_hook_lock;
        mmget = page->abort_hook_mmget;
        passthrough = page->abort_hook_passthrough;
        callback = r0lab_raw_abort_hook_callback(
            passthrough, mmget, lock, inflight, iabt_route,
            iabt_transition);
        page->hook_installed = false;
        installed = true;
        should_detach =
            r0lab_raw_abort_hook_users_locked() == 0 &&
            callback != r0lab_raw_before_abort_compact;
        if (should_detach)
            g_raw_abort_hook_transitioning = true;
    }
    r0lab_unlock(flags);

    if (installed)
        (void)r0lab_raw_wait_for_callbacks();
    if (should_detach)
        r0lab_hook_detach(g_do_mem_abort, callback, NULL);
    if (installed)
        (void)r0lab_raw_wait_for_callbacks();
    if (should_detach) {
        flags = r0lab_lock();
        if (g_raw_abort_resident_callback == callback)
            g_raw_abort_resident_callback = NULL;
        g_raw_abort_hook_transitioning = false;
        r0lab_unlock(flags);
    }
}

static void r0lab_raw_unhook_page(struct r0lab_raw_shadow_page *page,
                                  bool include_exit)
{
    if (!page)
        return;
    r0lab_raw_fork_hook_release(page);
    r0lab_raw_gup_hook_release(page);
    r0lab_raw_fault_hook_release(page);
    r0lab_raw_abort_hook_release(page);
    r0lab_raw_syscall_hook_release(page);
    r0lab_raw_prctl_hook_release(page);
    if (include_exit)
        r0lab_raw_exit_hook_release(page);
}

static void r0lab_raw_free_shadow_page(void *shadow_kaddr)
{
    if (shadow_kaddr && g_free_pages) {
        pr_info("r0lab-r3o: shadow_free kaddr=%px order=0\n",
                shadow_kaddr);
        g_free_pages((unsigned long)shadow_kaddr, 0);
    }
}

static void r0lab_raw_release_mm_ref(struct mm_struct *mm,
                                     bool mm_count_owned,
                                     bool mm_users_owned)
{
    if (!mm)
        return;
    if (mm_count_owned) {
        pr_info("r0lab-r3o: mm_release mm=%px ref=mm_count op=mmdrop\n",
                mm);
        r0lab_raw_mmdrop(mm);
    } else if (mm_users_owned) {
        pr_info("r0lab-r3o: mm_release mm=%px ref=mm_users op=mmput\n",
                mm);
        g_mmput(mm);
    }
}

static void r0lab_raw_reset_page(struct r0lab_raw_shadow_page *page,
                                 struct mm_struct *mm)
{
    void *shadow_kaddr = NULL;
    bool mm_count_owned = false;
    bool mm_users_owned = false;
    bool should_reset = false;
    uint16_t slot_id = 0;
    unsigned long flags;

    if (!page || !mm)
        return;
    flags = r0lab_lock();
    if (page->raw.mm == mm) {
        page->clearing = true;
        page->armed = false;
        slot_id = page->slot_id;
        should_reset = true;
    }
    r0lab_unlock(flags);

    if (should_reset)
        r0lab_raw_drain_patch_buffers_page(page, mm, 0, false);

    flags = r0lab_lock();
    if (page->raw.mm == mm) {
        bool monitor_running = page->monitor_running;
        uint64_t generation = page->generation;

        shadow_kaddr = page->raw.shadow_kaddr;
        mm_count_owned = page->mm_count_owned;
        mm_users_owned = page->mm_users_owned;
        r0lab_raw_page_slot_reset_locked(page, slot_id);
        page->generation = generation;
        page->monitor_running = monitor_running;
    }
    r0lab_unlock(flags);
    r0lab_raw_free_shadow_page(shadow_kaddr);
    r0lab_raw_release_mm_ref(mm, mm_count_owned, mm_users_owned);
}

static void __attribute__((unused)) r0lab_raw_reset(struct mm_struct *mm)
{
    r0lab_raw_reset_page(&g_raw_page, mm);
}

static void r0lab_raw_reset_final_page(struct r0lab_raw_shadow_page *page,
                                       struct mm_struct *mm,
                                       uint64_t generation)
{
    void *shadow_kaddr = NULL;
    bool mm_count_owned = false;
    bool mm_users_owned = false;
    bool should_reset = false;
    uint16_t slot_id = 0;
    unsigned long flags;

    if (!page || !mm)
        return;
    flags = r0lab_lock();
    if (page->raw.mm == mm && page->generation == generation) {
        slot_id = page->slot_id;
        page->clearing = true;
        page->armed = false;
        should_reset = true;
    }
    r0lab_unlock(flags);

    if (!should_reset)
        return;
    r0lab_raw_drain_patch_buffers_page(page, mm, generation, true);
    flags = r0lab_lock();
    if (page->raw.mm == mm && page->generation == generation) {
        shadow_kaddr = page->raw.shadow_kaddr;
        mm_count_owned = page->mm_count_owned;
        mm_users_owned = page->mm_users_owned;
        r0lab_raw_page_slot_reset_locked(page, slot_id);
    }
    r0lab_unlock(flags);
    r0lab_raw_free_shadow_page(shadow_kaddr);
    r0lab_raw_release_mm_ref(mm, mm_count_owned, mm_users_owned);
}

static void __attribute__((unused)) r0lab_raw_reset_final(
    struct mm_struct *mm, uint64_t generation)
{
    r0lab_raw_reset_final_page(&g_raw_page, mm, generation);
}

static int r0lab_s4_restore_descriptor_pages(bool final)
{
    unsigned int index;

    for (index = 0; index < R0LAB_RAW_PAGE_SLOT_CAPACITY; ++index) {
        struct r0lab_raw_shadow_page *page =
            &g_raw_page_table.slots[index];
        struct mm_struct *mm = NULL;
        unsigned long raw_state = R0LAB_RAW_EMPTY;
        uint64_t generation = 0;
        unsigned long flags;
        int result;

        flags = r0lab_lock();
        if (r0lab_s4_raw_page_owned_locked(page) && page->raw.mm) {
            mm = (struct mm_struct *)page->raw.mm;
            generation = page->generation;
            raw_state = page->raw.state;
            page->record.state = R0LAB_PAGE_RECORD_RESTORING;
        }
        r0lab_unlock(flags);
        if (!mm)
            continue;

        if (r0lab_s4_raw_state_needs_restore(raw_state)) {
            result = r0lab_raw_restore_original(&page->raw);
            if (result)
                return result;
            result = r0lab_raw_wait_for_callbacks();
            if (result)
                return result;
        }
        if (final)
            r0lab_raw_reset_final_page(page, mm, generation);
        else
            r0lab_raw_reset_page(page, mm);
    }
    return 0;
}

static void r0lab_raw_reset_exited_page(
    struct r0lab_raw_exit_cleanup_slot *cleanup)
{
    struct r0lab_raw_shadow_page *page;
    struct mm_struct *mm = NULL;
    void *shadow_kaddr = NULL;
    bool mm_count_owned = false;
    bool mm_users_owned = false;
    unsigned long flags;
    uint16_t slot_id;

    if (!cleanup || !cleanup->page)
        return;
    page = cleanup->page;
    flags = r0lab_lock();
    if (page->generation == cleanup->generation &&
        page->target_exiting && page->clearing) {
        slot_id = page->slot_id;
        mm = (struct mm_struct *)page->raw.mm;
        shadow_kaddr = page->raw.shadow_kaddr;
        mm_count_owned = page->mm_count_owned;
        mm_users_owned = page->mm_users_owned;
        r0lab_raw_page_slot_reset_locked(page, slot_id);
    } else if (!cleanup->result) {
        cleanup->result = R0LAB_EAGAIN;
    }
    r0lab_unlock(flags);
    r0lab_raw_free_shadow_page(shadow_kaddr);
    r0lab_raw_release_mm_ref(mm, mm_count_owned, mm_users_owned);
}

static int r0lab_raw_wait_for_owner_task_exit(
    struct r0lab_raw_shadow_page *page, struct mm_struct *mm,
    uint64_t generation, pid_t owner_tgid)
{
    for (;;) {
        bool valid;
        unsigned long flags;

        if (!r0lab_target_task_live(owner_tgid))
            return 0;
        flags = r0lab_lock();
        valid = page && page->generation == generation &&
                page->raw.mm == mm && page->monitor_running &&
                g_session.active &&
                g_session.owner_tgid == owner_tgid;
        r0lab_unlock(flags);
        if (!valid || r0lab_worker_should_stop())
            return R0LAB_EAGAIN;
        g_msleep(R0LAB_M5_MONITOR_INTERVAL_MS);
    }
}

static int r0lab_raw_cleanup_exited_mm(struct mm_struct *mm,
                                       pid_t owner_tgid)
{
    struct r0lab_raw_exit_cleanup_slot
        cleanup[R0LAB_RAW_PAGE_SLOT_CAPACITY] = {0};
    unsigned int count = 0;
    unsigned int index;
    int wait_result;
    unsigned long flags;

    if (!mm)
        return R0LAB_EINVAL;
    flags = r0lab_lock();
    for (index = 0; index < R0LAB_RAW_PAGE_SLOT_CAPACITY; ++index) {
        struct r0lab_raw_shadow_page *page =
            &g_raw_page_table.slots[index];
        struct r0lab_raw_exit_cleanup_slot *slot;

        if (page->raw.mm != mm || !page->generation ||
            !r0lab_raw_page_slot_owned_locked(page))
            continue;
        slot = &cleanup[count++];
        slot->page = page;
        slot->generation = page->generation;
        slot->exit_hook_events_before = page->exit_hook_events;
        slot->exit_hook_installed = page->exit_hook_installed;
        page->target_exiting = true;
        page->clearing = true;
        page->record.state = R0LAB_PAGE_RECORD_RESTORING;
    }
    r0lab_unlock(flags);
    if (!count)
        return R0LAB_ESRCH;

    for (index = 0; index < count; ++index) {
        struct r0lab_raw_exit_cleanup_slot *slot = &cleanup[index];

        r0lab_record(R0LAB_EVENT_TARGET_EXIT, 0);
        if (!slot->exit_hook_installed) {
            slot->result =
                slot->page->raw.state == R0LAB_RAW_RESTORED ?
                    0 : r0lab_raw_restore_original(&slot->page->raw);
        }
        r0lab_raw_unhook_page(slot->page, false);
    }

    wait_result = r0lab_raw_wait_for_callbacks();
    if (wait_result)
        return wait_result;
    for (index = 0; index < count; ++index) {
        struct r0lab_raw_exit_cleanup_slot *slot = &cleanup[index];

        r0lab_raw_drain_patch_buffers_page(
            slot->page, mm, slot->generation, true);
    }

    flags = r0lab_lock();
    for (index = 0; index < count; ++index) {
        struct r0lab_raw_exit_cleanup_slot *slot = &cleanup[index];
        struct r0lab_raw_shadow_page *page = slot->page;

        if (!slot->exit_hook_installed)
            continue;
        if (page->generation != slot->generation ||
            page->raw.state != R0LAB_RAW_RESTORED ||
            !slot->exit_hook_events_before ||
            page->exit_hook_events != slot->exit_hook_events_before) {
            if (!slot->result)
                slot->result = R0LAB_EFAULT;
        }
    }
    r0lab_unlock(flags);

    for (index = 0; index < count; ++index) {
        struct r0lab_raw_exit_cleanup_slot *slot = &cleanup[index];

        if (slot->exit_hook_installed)
            r0lab_raw_exit_hook_release(slot->page);
    }
    while ((wait_result = r0lab_raw_wait_for_callbacks()) != 0)
        g_msleep(R0LAB_M5_MONITOR_INTERVAL_MS);
    for (index = 0; index < count; ++index) {
        struct r0lab_raw_exit_cleanup_slot *slot = &cleanup[index];

        r0lab_raw_reset_exited_page(slot);
        r0lab_record(R0LAB_EVENT_RAW_CLEAR, slot->result);
    }
    r0lab_close_exited_session(owner_tgid);
    return 0;
}

static void r0lab_raw_monitor_done(struct r0lab_raw_shadow_page *page,
                                   uint64_t generation)
{
    unsigned long flags = r0lab_lock();

    if (page && page->generation == generation)
        page->monitor_running = false;
    r0lab_unlock(flags);
}

static int r0lab_raw_monitor_worker(void *opaque)
{
    struct r0lab_raw_shadow_page *page = opaque;
    struct mm_struct *mm;
    unsigned long flags;
    uint64_t generation;
    pid_t owner_tgid;
    int result;

    flags = r0lab_lock();
    if (!page || !page->monitor_running || !page->generation) {
        r0lab_unlock(flags);
        return 0;
    }
    generation = page->generation;
    r0lab_unlock(flags);

    for (;;) {
        bool active;
        bool page_owned;
        bool already_clearing;

        flags = r0lab_lock();
        if (page->generation != generation || !page->monitor_running) {
            r0lab_unlock(flags);
            return 0;
        }
        mm = (struct mm_struct *)page->raw.mm;
        owner_tgid = g_session.owner_tgid;
        active = g_session.active;
        page_owned = page->reserving || page->armed || page->clearing;
        already_clearing = page->clearing && !page->target_exiting;
        if (!mm || !active || !page_owned) {
            page->monitor_running = false;
            r0lab_unlock(flags);
            return 0;
        }
        r0lab_unlock(flags);

        if (already_clearing) {
            g_msleep(R0LAB_M5_MONITOR_INTERVAL_MS);
            continue;
        }
        if (!r0lab_target_mm_live(owner_tgid, mm))
            break;
        g_msleep(R0LAB_M5_MONITOR_INTERVAL_MS);
    }

    result = r0lab_raw_wait_for_owner_task_exit(
        page, mm, generation, owner_tgid);
    if (result) {
        r0lab_raw_monitor_done(page, generation);
        return 0;
    }
    flags = r0lab_lock();
    if (page->generation != generation || page->raw.mm != mm) {
        r0lab_unlock(flags);
        r0lab_raw_monitor_done(page, generation);
        return 0;
    }
    r0lab_unlock(flags);

    (void)r0lab_raw_cleanup_exited_mm(mm, owner_tgid);
    return 0;
}

static int r0lab_raw_start_monitor(struct r0lab_raw_shadow_page *page)
{
    unsigned long flags;

    flags = r0lab_lock();
    if (!page || !page->armed || page->monitor_running) {
        r0lab_unlock(flags);
        return R0LAB_EBUSY;
    }
    page->monitor_running = true;
    r0lab_unlock(flags);

    if (!g_raw_monitor_worker_task)
        return R0LAB_ESRCH;
    g_wake_up_process(g_raw_monitor_worker_task);
    return 0;
}

static int r0lab_raw_prepare_shadow(struct r0lab_raw_shadow_page *page)
{
    void *source_kaddr;
    unsigned long flags;
    int result;

    result = r0lab_raw_capture(&page->raw);
    if (result)
        return result;
    source_kaddr = r0lab_raw_source_kernel_address(&page->raw);
    if (!source_kaddr)
        return R0LAB_ENOENT;
    page->raw.shadow_kaddr = (void *)g_get_free_pages(
        (unsigned int)r0lab_raw_abi_gfp_kernel(), 0);
    if (!page->raw.shadow_kaddr)
        return R0LAB_ENOMEM;
    memcpy(page->raw.shadow_kaddr, source_kaddr, R0LAB_RAW_PAGE_SIZE);
    r0lab_raw_apply_seed_range(page, 0, R0LAB_RAW_PAGE_SIZE);
    r0lab_runtime_sync_icache_aliases(
        (unsigned long)page->raw.shadow_kaddr,
        (unsigned long)page->raw.shadow_kaddr + R0LAB_RAW_PAGE_SIZE);
    result = r0lab_raw_shadow_pfn_from_kaddr(&page->raw);
    if (result) {
        r0lab_raw_free_shadow_page(page->raw.shadow_kaddr);
        page->raw.shadow_kaddr = NULL;
        return result;
    }
    flags = r0lab_lock();
    if (page) {
        page->record.source_pfn = page->raw.source_pfn;
        page->record.shadow_pfn = page->raw.shadow_pfn;
    }
    r0lab_unlock(flags);
    pr_info("r0lab-r3o: shadow_ready slot=%u generation=%llu mm=%px va=%lx source_pfn=%lx shadow_pfn=%lx cache_sync=full\n",
            (unsigned int)page->slot_id,
            (unsigned long long)page->generation, page->raw.mm,
            page->raw.address, page->raw.source_pfn,
            page->raw.shadow_pfn);
    return 0;
}

static int r0lab_raw_arm_worker(void *opaque)
{
    struct r0lab_raw_shadow_page *page = opaque;
    struct mm_struct *mm = NULL;
    unsigned long flags;
    uint64_t generation;
    pid_t owner_tgid;
    bool cancelled;
    bool target_gone;
    int cleanup_result;
    int result;

    flags = r0lab_lock();
    if (!page || !page->reserving || !page->raw.mm || !g_session.active) {
        bool mm_count_owned = false;
        bool mm_users_owned = false;

        if (page && page->raw.mm) {
            mm = (struct mm_struct *)page->raw.mm;
            mm_count_owned = page->mm_count_owned;
            mm_users_owned = page->mm_users_owned;
            r0lab_raw_page_slot_reset_locked(page, page->slot_id);
        }
        r0lab_unlock(flags);
        r0lab_raw_release_mm_ref(mm, mm_count_owned, mm_users_owned);
        r0lab_record(R0LAB_EVENT_REJECT, R0LAB_ESRCH);
        return 0;
    }
    mm = (struct mm_struct *)page->raw.mm;
    generation = page->generation;
    owner_tgid = g_session.owner_tgid;
    r0lab_unlock(flags);

    result = r0lab_raw_prepare_shadow(page);
    if (result)
        goto fail;

    result = r0lab_raw_exit_hook_acquire(page);
    if (result)
        goto fail_free_shadow;
    pr_info("r0lab-r3o: exit_protection_ready slot=%u generation=%llu resident=%u before_source_uxn=1\n",
            (unsigned int)page->slot_id,
            (unsigned long long)generation, 1U);

    if (!page->abort_hook_suppressed) {
        result = r0lab_raw_abort_hook_acquire(page);
        if (result)
            goto fail_unhook;
    }

    if (!r0lab_target_mm_live(owner_tgid, mm))
        goto target_exit;

    result = r0lab_raw_arm_source_uxn(&page->raw);
    if (result)
        goto fail_unhook;
    pr_info("r0lab-r3o: source_uxn_installed slot=%u generation=%llu mm=%px va=%lx state=%u\n",
            (unsigned int)page->slot_id,
            (unsigned long long)generation, mm, page->raw.address,
            (unsigned int)page->raw.state);

    target_gone = !r0lab_target_mm_live(owner_tgid, mm);
    flags = r0lab_lock();
    cancelled = page->clearing;
    if (target_gone && page->raw.mm == mm) {
        page->target_exiting = true;
        page->clearing = true;
    }
    if (!cancelled && !target_gone && page->raw.mm == mm) {
        page->reserving = false;
        page->armed = true;
        page->record.source_pfn = page->raw.source_pfn;
        page->record.shadow_pfn = page->raw.shadow_pfn;
        page->record.state = R0LAB_PAGE_RECORD_SOURCE_UXN;
    }
    r0lab_unlock(flags);
    if (target_gone)
        goto target_exit;
    if (!cancelled) {
        result = r0lab_raw_start_monitor(page);
        if (result)
            goto fail_restore;
        pr_info("r0lab-r3o: arm_complete slot=%u generation=%llu owner_tgid=%d mm=%px ref=mm_count monitor=1\n",
                (unsigned int)page->slot_id,
                (unsigned long long)generation, owner_tgid, mm);
        r0lab_record(R0LAB_EVENT_RAW_ARM, 0);
        return 0;
    }

fail_restore:
    cleanup_result = result;
    flags = r0lab_lock();
    if (page->raw.mm == mm)
        page->record.state = R0LAB_PAGE_RECORD_RESTORING;
    r0lab_unlock(flags);
    result = r0lab_raw_restore_original(&page->raw);
    r0lab_raw_unhook_page(page, true);
    if (!result)
        result = r0lab_raw_wait_for_callbacks();
    if (!result)
        result = cleanup_result;
    r0lab_raw_reset_page(page, mm);
    r0lab_record(R0LAB_EVENT_RAW_CLEAR, result);
    return 0;

fail_unhook:
    r0lab_raw_unhook_page(page, true);
    (void)r0lab_raw_wait_for_callbacks();
fail_free_shadow:
    if (page->raw.shadow_kaddr) {
        r0lab_raw_free_shadow_page(page->raw.shadow_kaddr);
        page->raw.shadow_kaddr = NULL;
    }
fail:
    r0lab_raw_reset_page(page, mm);
    r0lab_record(R0LAB_EVENT_REJECT, result);
    return 0;

target_exit:
    r0lab_record(R0LAB_EVENT_TARGET_EXIT, 0);
    flags = r0lab_lock();
    if (page->raw.mm == mm)
        page->record.state = R0LAB_PAGE_RECORD_RESTORING;
    r0lab_unlock(flags);
    result = r0lab_raw_restore_original(&page->raw);
    r0lab_raw_unhook_page(page, true);
    if (!result)
        result = r0lab_raw_wait_for_callbacks();
    r0lab_raw_reset_final_page(page, mm, generation);
    r0lab_close_exited_session(owner_tgid);
    r0lab_record(R0LAB_EVENT_RAW_CLEAR, result);
    return 0;
}

static int r0lab_raw_clear_worker(void *opaque)
{
    struct r0lab_raw_shadow_page *page = opaque;
    struct mm_struct *mm;
    unsigned long flags;
    uint64_t generation;
    uint16_t slot_id;
    int result;

    flags = r0lab_lock();
    if (!page || !page->armed || !page->clearing || !page->raw.mm) {
        r0lab_unlock(flags);
        r0lab_record(R0LAB_EVENT_REJECT, R0LAB_ESRCH);
        return 0;
    }
    mm = (struct mm_struct *)page->raw.mm;
    generation = page->generation;
    slot_id = page->slot_id;
    page->record.state = R0LAB_PAGE_RECORD_RESTORING;
    r0lab_unlock(flags);

    pr_info("r0lab-r3o: clear_begin slot=%u generation=%llu mm=%px va=%lx state=%u\n",
            (unsigned int)slot_id, (unsigned long long)generation, mm,
            page->raw.address, (unsigned int)page->raw.state);
    result = page->raw.state == R0LAB_RAW_RESTORED ?
             0 : r0lab_raw_restore_original(&page->raw);
    r0lab_raw_unhook_page(page, true);
    if (!result)
        result = r0lab_raw_wait_for_callbacks();
    r0lab_raw_reset_page(page, mm);
    pr_info("r0lab-r3o: clear_end slot=%u generation=%llu result=%d pte=original shadow_freed=1 mm_ref_released=1\n",
            (unsigned int)slot_id, (unsigned long long)generation, result);
    r0lab_record(R0LAB_EVENT_RAW_CLEAR, result);
    return 0;
}

static int r0lab_raw_start_worker(int (*worker_fn)(void *))
{
    (void)worker_fn;

    if (!g_raw_worker)
        return R0LAB_ESRCH;
    g_wake_up_process(g_raw_worker);
    return 0;
}

static int r0lab_raw_worker_loop(void *opaque)
{
    (void)opaque;
    while (!r0lab_worker_should_stop()) {
        struct r0lab_raw_shadow_page *pending = NULL;
        bool arm_pending = false;
        bool clear_pending = false;
        unsigned int index;
        unsigned long flags = r0lab_lock();

        for (index = 0; index < R0LAB_RAW_PAGE_SLOT_CAPACITY; ++index) {
            struct r0lab_raw_shadow_page *page =
                &g_raw_page_table.slots[index];

            if (page->reserving) {
                pending = page;
                arm_pending = true;
                break;
            }
        }
        if (!pending) {
            for (index = 0; index < R0LAB_RAW_PAGE_SLOT_CAPACITY; ++index) {
                struct r0lab_raw_shadow_page *page =
                    &g_raw_page_table.slots[index];

                if (page->armed && page->clearing && !page->target_exiting) {
                    pending = page;
                    clear_pending = true;
                    break;
                }
            }
        }
        r0lab_unlock(flags);
        if (arm_pending) {
            r0lab_raw_arm_worker(pending);
            continue;
        }
        if (clear_pending) {
            r0lab_raw_clear_worker(pending);
            continue;
        }
        g_msleep(R0LAB_M5_MONITOR_INTERVAL_MS);
    }
    r0lab_worker_finished();
    return 0;
}

static int r0lab_raw_monitor_loop(void *opaque)
{
    (void)opaque;
    while (!r0lab_worker_should_stop()) {
        struct r0lab_raw_shadow_page *pending = NULL;
        unsigned int index;
        unsigned long flags = r0lab_lock();

        for (index = 0; index < R0LAB_RAW_PAGE_SLOT_CAPACITY; ++index) {
            if (g_raw_page_table.slots[index].monitor_running) {
                pending = &g_raw_page_table.slots[index];
                break;
            }
        }
        r0lab_unlock(flags);
        if (pending) {
            r0lab_raw_monitor_worker(pending);
            continue;
        }
        g_msleep(R0LAB_M5_MONITOR_INTERVAL_MS);
    }
    r0lab_worker_finished();
    return 0;
}

static int r0lab_raw_parse_slot_id(uint64_t slot_value, uint16_t *slot_id)
{
    if (!slot_id || slot_value >= R0LAB_RAW_PAGE_SLOT_CAPACITY)
        return R0LAB_EINVAL;
    *slot_id = (uint16_t)slot_value;
    return 0;
}

static long r0lab_raw_slot_arm(uint64_t token, uint16_t slot_id,
                               uint64_t page_address,
                               bool suppress_abort_hook,
                               bool passthrough_abort_hook,
                               bool mmget_abort_hook,
                               bool lock_abort_hook,
                               bool inflight_abort_hook,
                               bool iabt_route_abort_hook,
                               bool iabt_transition_abort_hook,
                               bool legacy_reply,
                               char __user *out_msg, int outlen)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    struct r0lab_raw_shadow_page *slot;
    struct mm_struct *mm;
    unsigned long flags;
    unsigned int index;
    int result = r0lab_validate_owner(token);

    if (result)
        goto record;
    if ((suppress_abort_hook ? 1U : 0U) +
            (passthrough_abort_hook ? 1U : 0U) +
            (mmget_abort_hook ? 1U : 0U) +
            (lock_abort_hook ? 1U : 0U) +
            (inflight_abort_hook ? 1U : 0U) +
            (iabt_route_abort_hook ? 1U : 0U) +
            (iabt_transition_abort_hook ? 1U : 0U) >
        1U) {
        result = R0LAB_EINVAL;
        goto record;
    }
    if (!page_address || (page_address & (R0LAB_RAW_PAGE_SIZE - 1UL))) {
        result = R0LAB_EINVAL;
        goto record;
    }
    if (slot_id >= R0LAB_RAW_PAGE_SLOT_CAPACITY) {
        result = R0LAB_EINVAL;
        goto record;
    }
    mm = g_get_task_mm(current);
    if (!mm) {
        result = R0LAB_ESRCH;
        goto record;
    }
    flags = r0lab_lock();
    slot = r0lab_raw_page_slot_locked(slot_id);
    if (!slot || !g_session.active ||
        g_session.owner_tgid != r0lab_current_tgid() ||
        g_session.token != token || r0lab_hwbp_slot_count_locked() ||
        r0lab_m3_slot_count_locked() || r0lab_m4_slot_count_locked() ||
        r0lab_s4_slot_count_locked() ||
        (legacy_reply && r0lab_raw_slot_count_locked()) ||
        (!legacy_reply && r0lab_raw_page_slot_owned_locked(slot)) ||
        r0lab_raw_page_table_has_aux_state_locked()) {
        r0lab_unlock(flags);
        g_mmput(mm);
        result = R0LAB_EBUSY;
        goto record;
    }
    for (index = 0; index < R0LAB_RAW_PAGE_SLOT_CAPACITY; ++index) {
        struct r0lab_raw_shadow_page *active =
            &g_raw_page_table.slots[index];

        if (!r0lab_raw_page_slot_owned_locked(active))
            continue;
        if (suppress_abort_hook || passthrough_abort_hook ||
            mmget_abort_hook || lock_abort_hook || inflight_abort_hook ||
            iabt_route_abort_hook || iabt_transition_abort_hook ||
            active->abort_hook_suppressed ||
            active->abort_hook_passthrough ||
            active->abort_hook_mmget ||
            active->abort_hook_lock ||
            active->abort_hook_inflight ||
            active->abort_hook_iabt_route ||
            active->abort_hook_iabt_transition) {
            r0lab_unlock(flags);
            g_mmput(mm);
            result = R0LAB_EBUSY;
            goto record;
        }
        if (active->raw.mm != mm) {
            r0lab_unlock(flags);
            g_mmput(mm);
            result = R0LAB_EBUSY;
            goto record;
        }
        if (active->raw.address == (unsigned long)page_address) {
            r0lab_unlock(flags);
            g_mmput(mm);
            result = R0LAB_EINVAL;
            goto record;
        }
    }
    r0lab_raw_page_slot_reset_locked(slot, slot_id);
    r0lab_raw_mmgrab(mm);
    slot->raw.mm = mm;
    slot->mm_count_owned = true;
    slot->raw.address = (unsigned long)page_address;
    slot->generation = ++g_raw_generation;
    slot->record.source_address = slot->raw.address;
    slot->record.generation = slot->generation;
    slot->record.backend = R0LAB_PAGE_RECORD_RAW_TWO_PFN;
    slot->record.state = R0LAB_PAGE_RECORD_PREPARING;
    slot->abort_hook_suppressed = suppress_abort_hook;
    slot->abort_hook_passthrough = passthrough_abort_hook;
    slot->abort_hook_mmget = mmget_abort_hook;
    slot->abort_hook_lock = lock_abort_hook;
    slot->abort_hook_inflight = inflight_abort_hook;
    slot->abort_hook_iabt_route = iabt_route_abort_hook;
    slot->abort_hook_iabt_transition = iabt_transition_abort_hook;
    slot->reserving = true;
    g_raw_page_table.selected_slot = slot_id;
    r0lab_unlock(flags);
    g_mmput(mm);
    pr_info("r0lab-r3o: slot_owned slot=%u generation=%llu owner_tgid=%d mm=%px va=%llx mm_ref=mm_count temp_mm_users_released=1\n",
            (unsigned int)slot_id, (unsigned long long)slot->generation,
            r0lab_current_tgid(), mm,
            (unsigned long long)page_address);

    result = r0lab_raw_start_worker(r0lab_raw_arm_worker);
    if (result) {
        r0lab_raw_reset_page(slot, mm);
        goto record;
    }
    if (legacy_reply)
        snprintf(reply, sizeof(reply), "raw_pending page=%llx\n",
                 page_address);
    else
        snprintf(reply, sizeof(reply),
                 "raw_slot_pending slot=%u page=%llx\n",
                 (unsigned int)slot_id, page_address);
    return r0lab_copy_reply(out_msg, outlen, reply);

record:
    r0lab_record(R0LAB_EVENT_REJECT, result);
    return result;
}

static long r0lab_raw_arm(uint64_t token, uint64_t page_address,
                          char __user *out_msg, int outlen)
{
    return r0lab_raw_slot_arm(token, R0LAB_RAW_PRIMARY_SLOT, page_address,
                              false, false, false, false, false, false, false,
                              true, out_msg, outlen);
}

static long r0lab_raw_ready(uint64_t token, char __user *out_msg, int outlen)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    unsigned long flags;
    unsigned long address;
    unsigned long source_pfn;
    unsigned long shadow_pfn;
    uint8_t record_backend;
    uint8_t record_state;
    bool exit_hook_installed;
    bool mm_count_owned;
    int result = r0lab_validate_owner(token);

    if (result)
        return result;
    flags = r0lab_lock();
    if (g_raw_page.reserving || g_raw_page.clearing) {
        r0lab_unlock(flags);
        return R0LAB_EAGAIN;
    }
    if (!g_raw_page.armed) {
        r0lab_unlock(flags);
        return R0LAB_ENOENT;
    }
    address = g_raw_page.raw.address;
    source_pfn = g_raw_page.raw.source_pfn;
    shadow_pfn = g_raw_page.raw.shadow_pfn;
    record_backend = g_raw_page.record.backend;
    record_state = g_raw_page.record.state;
    exit_hook_installed = g_raw_page.exit_hook_installed;
    mm_count_owned = g_raw_page.mm_count_owned;
    r0lab_unlock(flags);
    snprintf(reply, sizeof(reply),
             "raw_ready page=%llx source_pfn_low=%lx shadow_pfn_low=%lx state=source_uxn record_backend=%s record_state=%s mm_ref=%s exit_hook_installed=%u\n",
             (uint64_t)address, source_pfn & 0xffffUL, shadow_pfn & 0xffffUL,
             r0lab_page_record_backend_name(record_backend),
             r0lab_page_record_state_name(record_state),
             mm_count_owned ? "mm_count" : "unexpected",
             exit_hook_installed ? 1U : 0U);
    return r0lab_copy_reply(out_msg, outlen, reply);
}

static long r0lab_raw_observed(uint64_t token, char __user *out_msg, int outlen)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    unsigned long flags;
    uint32_t activation_events;
    unsigned long state;
    uint8_t record_backend;
    uint8_t record_state;
    int result = r0lab_validate_owner(token);

    if (result)
        return result;
    flags = r0lab_lock();
    if (!g_raw_page.armed || g_raw_page.clearing) {
        r0lab_unlock(flags);
        return R0LAB_EAGAIN;
    }
    activation_events = g_raw_page.activation_events;
    state = g_raw_page.raw.state;
    record_backend = g_raw_page.record.backend;
    record_state = g_raw_page.record.state;
    r0lab_unlock(flags);
    snprintf(reply, sizeof(reply),
             "raw_observed activations=%u state=%lu record_backend=%s record_state=%s\n",
             activation_events, state,
             r0lab_page_record_backend_name(record_backend),
             r0lab_page_record_state_name(record_state));
    return r0lab_copy_reply(out_msg, outlen, reply);
}

static const char *r0lab_raw_active_kind(unsigned long state,
                                         unsigned long active_pte,
                                         unsigned long original_pte,
                                         unsigned long source_uxn_pte,
                                         unsigned long shadow_rx_pte,
                                         unsigned long shadow_xom_pte)
{
    if (state == R0LAB_RAW_ORIGINAL_READ)
        return "original_read";
    if (active_pte == original_pte)
        return "original";
    if (active_pte == source_uxn_pte)
        return "source_uxn";
    if (shadow_xom_pte && active_pte == shadow_xom_pte)
        return "shadow_xom";
    if (active_pte == shadow_rx_pte)
        return "shadow_rx";
    if (state == R0LAB_RAW_ORIGINAL_STEP)
        return "original_step";
    return "none";
}

static const char *r0lab_raw_state_name(unsigned long state)
{
    switch (state) {
    case R0LAB_RAW_EMPTY:
        return "empty";
    case R0LAB_RAW_CAPTURED:
        return "captured";
    case R0LAB_RAW_SOURCE_UXN:
        return "source_uxn";
    case R0LAB_RAW_SHADOW_RX:
        return "shadow_rx";
    case R0LAB_RAW_ORIGINAL_STEP:
        return "original_step";
    case R0LAB_RAW_RESTORED:
        return "restored";
    case R0LAB_RAW_POISONED:
        return "poisoned";
    case R0LAB_RAW_ORIGINAL_READ:
        return "original_read";
    case R0LAB_RAW_SHADOW_XOM:
        return "shadow_xom";
    default:
        return "unknown";
    }
}

static long r0lab_raw_slot_ready(uint64_t token, uint16_t slot_id,
                                 char __user *out_msg, int outlen)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    struct r0lab_raw_shadow_page *page;
    unsigned long flags;
    unsigned long address;
    unsigned long source_pfn;
    unsigned long shadow_pfn;
    uint64_t generation;
    uint8_t record_backend;
    uint8_t record_state;
    bool abort_hook_installed;
    bool abort_hook_iabt_transition;
    bool abort_hook_iabt_route;
    bool abort_hook_inflight;
    bool abort_hook_lock;
    bool abort_hook_mmget;
    bool abort_hook_suppressed;
    bool abort_hook_passthrough;
    bool abort_hook_full;
    bool exit_hook_installed;
    bool mm_count_owned;
    int result = r0lab_validate_owner(token);

    if (result)
        return result;
    if (slot_id >= R0LAB_RAW_PAGE_SLOT_CAPACITY)
        return R0LAB_EINVAL;
    (void)r0lab_raw_wait_for_callbacks();
    flags = r0lab_lock();
    page = r0lab_raw_page_slot_locked(slot_id);
    if (!page) {
        r0lab_unlock(flags);
        return R0LAB_EINVAL;
    }
    if (page->reserving || page->clearing) {
        r0lab_unlock(flags);
        return R0LAB_EAGAIN;
    }
    if (!page->armed) {
        r0lab_unlock(flags);
        return R0LAB_ENOENT;
    }
    address = page->raw.address;
    source_pfn = page->raw.source_pfn;
    shadow_pfn = page->raw.shadow_pfn;
    generation = page->generation;
    record_backend = page->record.backend;
    record_state = page->record.state;
    abort_hook_installed = page->hook_installed;
    abort_hook_iabt_transition = page->abort_hook_iabt_transition;
    abort_hook_iabt_route = page->abort_hook_iabt_route;
    abort_hook_inflight = page->abort_hook_inflight;
    abort_hook_lock = page->abort_hook_lock;
    abort_hook_mmget = page->abort_hook_mmget;
    abort_hook_suppressed = page->abort_hook_suppressed;
    abort_hook_passthrough = page->abort_hook_passthrough;
    abort_hook_full =
        r0lab_raw_page_uses_full_abort_callback_locked(page);
    exit_hook_installed = page->exit_hook_installed;
    mm_count_owned = page->mm_count_owned;
    r0lab_unlock(flags);
    snprintf(reply, sizeof(reply),
             "raw_slot_ready slot=%u page=%llx generation=%llu source_pfn_low=%lx shadow_pfn_low=%lx state=source_uxn record_backend=%s record_state=%s mm_ref=%s exit_hook_installed=%u abort_hook_installed=%u abort_hook_suppressed=%u abort_hook_passthrough=%u abort_hook_mmget=%u abort_hook_lock=%u abort_hook_inflight=%u abort_hook_iabt_route=%u abort_hook_iabt_transition=%u abort_hook_full=%u\n",
             (unsigned int)slot_id, (uint64_t)address,
             (unsigned long long)generation, source_pfn & 0xffffUL,
             shadow_pfn & 0xffffUL,
             r0lab_page_record_backend_name(record_backend),
             r0lab_page_record_state_name(record_state),
             mm_count_owned ? "mm_count" : "unexpected",
             exit_hook_installed ? 1U : 0U,
             abort_hook_installed ? 1U : 0U,
             abort_hook_suppressed ? 1U : 0U,
             abort_hook_passthrough ? 1U : 0U,
             abort_hook_mmget ? 1U : 0U,
             abort_hook_lock ? 1U : 0U,
             abort_hook_inflight ? 1U : 0U,
             abort_hook_iabt_route ? 1U : 0U,
             abort_hook_iabt_transition ? 1U : 0U,
             abort_hook_full ? 1U : 0U);
    return r0lab_copy_reply(out_msg, outlen, reply);
}

static long r0lab_raw_slot_observed(uint64_t token, uint16_t slot_id,
                                    char __user *out_msg, int outlen)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    struct r0lab_raw_shadow_page *page;
    unsigned long flags;
    uint32_t activation_events;
    unsigned long state;
    uint64_t generation;
    uint8_t record_backend;
    uint8_t record_state;
    int result = r0lab_validate_owner(token);

    if (result)
        return result;
    if (slot_id >= R0LAB_RAW_PAGE_SLOT_CAPACITY)
        return R0LAB_EINVAL;
    flags = r0lab_lock();
    page = r0lab_raw_page_slot_locked(slot_id);
    if (!page) {
        r0lab_unlock(flags);
        return R0LAB_EINVAL;
    }
    if (!page->armed || page->clearing) {
        r0lab_unlock(flags);
        return R0LAB_EAGAIN;
    }
    activation_events = page->activation_events;
    state = page->raw.state;
    generation = page->generation;
    record_backend = page->record.backend;
    record_state = page->record.state;
    r0lab_unlock(flags);
    snprintf(reply, sizeof(reply),
             "raw_slot_observed slot=%u activations=%u state=%lu generation=%llu record_backend=%s record_state=%s\n",
             (unsigned int)slot_id, activation_events, state,
             (unsigned long long)generation,
             r0lab_page_record_backend_name(record_backend),
             r0lab_page_record_state_name(record_state));
    return r0lab_copy_reply(out_msg, outlen, reply);
}

static long r0lab_raw_slot_inspect(uint64_t token, uint16_t slot_id,
                                   char __user *out_msg, int outlen)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    struct r0lab_raw_shadow_page *page;
    unsigned long flags;
    unsigned long address;
    unsigned long state;
    unsigned long active_pte;
    unsigned long original_pte;
    unsigned long source_uxn_pte;
    unsigned long shadow_rx_pte;
    unsigned long shadow_xom_pte;
    unsigned long source_pfn;
    unsigned long shadow_pfn;
    uint64_t generation;
    uint32_t activation_events;
    uint32_t gup_hook_begin_events;
    uint32_t gup_hook_finish_events;
    uint32_t gup_hook_failures;
    unsigned long gup_begin_events;
    unsigned long gup_finish_events;
    bool gup_hook_installed;
    uint8_t record_backend;
    uint8_t record_state;
    const char *active_kind;
    int result = r0lab_validate_owner(token);

    if (result)
        return result;
    if (slot_id >= R0LAB_RAW_PAGE_SLOT_CAPACITY)
        return R0LAB_EINVAL;
    flags = r0lab_lock();
    page = r0lab_raw_page_slot_locked(slot_id);
    if (!page) {
        r0lab_unlock(flags);
        return R0LAB_EINVAL;
    }
    if (!page->armed || page->clearing) {
        r0lab_unlock(flags);
        return R0LAB_EAGAIN;
    }
    address = page->raw.address;
    state = page->raw.state;
    active_pte = page->raw.active_pte;
    original_pte = page->raw.original_pte;
    source_uxn_pte = page->raw.source_uxn_pte;
    shadow_rx_pte = page->raw.shadow_rx_pte;
    shadow_xom_pte = page->raw.shadow_xom_pte;
    source_pfn = page->raw.source_pfn;
    shadow_pfn = page->raw.shadow_pfn;
    generation = page->generation;
    activation_events = page->activation_events;
    gup_hook_installed = page->gup_hook_installed;
    gup_hook_begin_events = page->gup_hook_begin_events;
    gup_hook_finish_events = page->gup_hook_finish_events;
    gup_hook_failures = page->gup_hook_failures;
    gup_begin_events = page->raw.gup_begin_events;
    gup_finish_events = page->raw.gup_finish_events;
    record_backend = page->record.backend;
    record_state = page->record.state;
    r0lab_unlock(flags);

    active_kind = r0lab_raw_active_kind(state, active_pte, original_pte,
                                        source_uxn_pte, shadow_rx_pte,
                                        shadow_xom_pte);
    snprintf(reply, sizeof(reply),
             "raw_slot_inspect slot=%u page=%llx generation=%llu state=%lu activations=%u active_kind=%s source_pfn_low=%lx shadow_pfn_low=%lx gup_hook_installed=%u gup_hook_begin_events=%u gup_hook_finish_events=%u gup_hook_failures=%u gup_begin_events=%lu gup_finish_events=%lu record_backend=%s record_state=%s read_cycle=absent original_view_after_shadow=absent\n",
             (unsigned int)slot_id, (uint64_t)address,
             (unsigned long long)generation, state, activation_events,
             active_kind, source_pfn & 0xffffUL, shadow_pfn & 0xffffUL,
             gup_hook_installed ? 1 : 0, gup_hook_begin_events,
             gup_hook_finish_events, gup_hook_failures, gup_begin_events,
             gup_finish_events,
             r0lab_page_record_backend_name(record_backend),
             r0lab_page_record_state_name(record_state));
    return r0lab_copy_reply(out_msg, outlen, reply);
}

static long r0lab_raw_slot_live_pte(uint64_t token, uint16_t slot_id,
                                    char __user *out_msg, int outlen)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    struct r0lab_raw_live_pte_snapshot snapshot = {0};
    struct r0lab_raw_page raw = {0};
    struct r0lab_raw_shadow_page *page;
    struct mm_struct *current_mm;
    unsigned long flags;
    uint64_t generation;
    uint8_t record_backend;
    uint8_t record_state;
    bool record_match;
    int walk_rc;
    int result = r0lab_validate_owner(token);

    if (result)
        return result;
    if (slot_id >= R0LAB_RAW_PAGE_SLOT_CAPACITY)
        return R0LAB_EINVAL;
    if (!g_get_task_mm || !g_mmput)
        return R0LAB_ENOSYS;

    current_mm = g_get_task_mm(current);
    if (!current_mm)
        return R0LAB_ESRCH;

    flags = r0lab_lock();
    page = r0lab_raw_page_slot_locked(slot_id);
    if (!page || !page->armed || page->reserving || page->clearing ||
        page->transitioning) {
        r0lab_unlock(flags);
        g_mmput(current_mm);
        return R0LAB_EAGAIN;
    }
    if (page->raw.mm != current_mm) {
        r0lab_unlock(flags);
        g_mmput(current_mm);
        return R0LAB_EPERM;
    }
    raw = page->raw;
    raw.mm = current_mm;
    generation = page->generation;
    record_backend = page->record.backend;
    record_state = page->record.state;
    r0lab_unlock(flags);

    walk_rc = r0lab_raw_snapshot_live_pte(&raw, &snapshot);

    flags = r0lab_lock();
    page = r0lab_raw_page_slot_locked(slot_id);
    record_match = page && page->armed && !page->reserving &&
                   !page->clearing && page->generation == generation &&
                   page->raw.mm == current_mm &&
                   page->raw.address == raw.address &&
                   page->raw.active_pte == raw.active_pte &&
                   page->raw.state == raw.state &&
                   page->record.backend == record_backend &&
                   page->record.state == record_state;
    r0lab_unlock(flags);
    g_mmput(current_mm);

    snprintf(reply, sizeof(reply),
             "raw_slot_live_pte slot=%u page=%llx generation=%llu snapshot_stage=after_arm walk_rc=%d live_pte=%lx expected_pte=%lx live_match=%lu pte_match=%lu pfn_match=%lu live_pfn=%lx expected_pfn=%lx live_state=%s stored_state=%s live_state_id=%lu stored_state_id=%lu vma_match=%lu vma_flags=%lx source_identity_match=%lu shadow_identity_match=%lu identity_match=%lu pte_lock=%s mmap_lock=read record_backend=%s record_state=%s record_match=%u\n",
             (unsigned int)slot_id, (uint64_t)raw.address,
             (unsigned long long)generation, walk_rc, snapshot.live_pte,
             snapshot.expected_pte, snapshot.live_match,
             snapshot.pte_match, snapshot.pfn_match, snapshot.live_pfn,
             snapshot.expected_pfn,
             r0lab_raw_state_name(snapshot.live_state),
             r0lab_raw_state_name(snapshot.stored_state),
             snapshot.live_state, snapshot.stored_state, snapshot.vma_match,
             snapshot.vma_flags, snapshot.source_identity_match,
             snapshot.shadow_identity_match, snapshot.identity_match,
             walk_rc ? "not_acquired" : "held",
             r0lab_page_record_backend_name(record_backend),
             r0lab_page_record_state_name(record_state),
             record_match ? 1U : 0U);
    return r0lab_copy_reply(out_msg, outlen, reply);
}

static long r0lab_raw_slot_patch_check(uint64_t token, uint16_t slot_id,
                                       uint64_t generation, uint64_t offset,
                                       uint64_t length, char __user *out_msg,
                                       int outlen)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    struct r0lab_raw_shadow_page *page;
    unsigned long flags;
    int result = r0lab_validate_owner(token);

    if (result)
        return result;
    if (slot_id >= R0LAB_RAW_PAGE_SLOT_CAPACITY || !length ||
        offset >= R0LAB_RAW_PAGE_SIZE ||
        length > R0LAB_RAW_PAGE_SIZE ||
        offset + length > R0LAB_RAW_PAGE_SIZE ||
        offset + length < offset)
        return R0LAB_EINVAL;
    flags = r0lab_lock();
    page = r0lab_raw_page_slot_locked(slot_id);
    if (!page || !page->armed || page->clearing ||
        page->generation != generation) {
        r0lab_unlock(flags);
        return R0LAB_EINVAL;
    }
    r0lab_unlock(flags);
    snprintf(reply, sizeof(reply),
             "raw_slot_patch_check_ok slot=%u generation=%llu offset=%llu length=%llu\n",
             (unsigned int)slot_id, (unsigned long long)generation,
             (unsigned long long)offset, (unsigned long long)length);
    return r0lab_copy_reply(out_msg, outlen, reply);
}

static int r0lab_raw_slot_patch_admit_locked(
    uint64_t token, uint16_t slot_id, uint64_t generation,
    unsigned int offset, unsigned int length, struct mm_struct *current_mm,
    struct r0lab_raw_shadow_page **out_page)
{
    struct r0lab_raw_shadow_page *page;

    if (!out_page || slot_id >= R0LAB_RAW_PAGE_SLOT_CAPACITY || !length ||
        offset >= R0LAB_RAW_PAGE_SIZE ||
        length > R0LAB_RAW_PAGE_SIZE - offset)
        return R0LAB_EINVAL;
    page = r0lab_raw_page_slot_locked(slot_id);
    if (!page)
        return R0LAB_EINVAL;
    if (!g_session.active || g_session.token != token ||
        g_session.owner_tgid != r0lab_current_tgid() || !current_mm ||
        page->raw.mm != current_mm)
        return R0LAB_EPERM;
    if (!page->armed || page->reserving || page->clearing ||
        page->transitioning || page->generation != generation ||
        page->raw.state != R0LAB_RAW_SHADOW_RX || !page->raw.shadow_kaddr ||
        page->raw.gup_hide_active || page->raw.fork_hide_active ||
        page->raw.read_cycle_active ||
        r0lab_raw_page_has_aux_state_locked(page))
        return R0LAB_EAGAIN;
    page->transitioning = true;
    *out_page = page;
    return 0;
}

static long r0lab_raw_slot_patch_status(uint64_t token, uint16_t slot_id,
                                        char __user *out_msg, int outlen)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    struct r0lab_raw_shadow_page *page;
    unsigned long flags;
    unsigned long address;
    unsigned long state;
    uint64_t generation;
    uint64_t patch_version;
    uint16_t patch_record_slots;
    uint16_t patch_active_count;
    uint16_t patch_dirty_bytes;
    int result = r0lab_validate_owner(token);

    if (result)
        return result;
    if (slot_id >= R0LAB_RAW_PAGE_SLOT_CAPACITY)
        return R0LAB_EINVAL;
    flags = r0lab_lock();
    page = r0lab_raw_page_slot_locked(slot_id);
    if (!page || !page->armed || page->clearing) {
        r0lab_unlock(flags);
        return R0LAB_EAGAIN;
    }
    address = page->raw.address;
    state = page->raw.state;
    generation = page->generation;
    patch_version = page->patch_version;
    patch_record_slots = page->patch_record_slots;
    patch_active_count = page->patch_active_count;
    patch_dirty_bytes = page->patch_dirty_bytes;
    r0lab_unlock(flags);

    snprintf(reply, sizeof(reply),
             "raw_slot_patch_status slot=%u page=%llx generation=%llu state=%lu patch_record_slots=%u patch_active_count=%u patch_dirty_bytes=%u patch_version=%llu patch_capacity=%u patch_scope=page_slot_ranges\n",
             (unsigned int)slot_id, (uint64_t)address,
             (unsigned long long)generation, state,
             (unsigned int)patch_record_slots,
             (unsigned int)patch_active_count,
             (unsigned int)patch_dirty_bytes,
             (unsigned long long)patch_version,
             (unsigned int)R0LAB_PATCH_RECORD_CAPACITY);
    return r0lab_copy_reply(out_msg, outlen, reply);
}

static long r0lab_raw_hook_route_status(uint64_t token, uint16_t slot_id,
                                        char __user *out_msg, int outlen)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    struct r0lab_raw_shadow_page *page;
    struct r0lab_raw_hook_route_stats stats = {0};
    struct r0lab_raw_hook_route_stats miss_stats = {0};
    unsigned long flags;
    unsigned long address = 0;
    unsigned long state = R0LAB_RAW_EMPTY;
    uint64_t generation = 0;
    bool owned = false;
    bool armed = false;
    bool lookup_self = false;
    bool fault_lookup_self = false;
    int result = r0lab_validate_owner(token);

    if (result)
        return result;
    if (slot_id >= R0LAB_RAW_PAGE_SLOT_CAPACITY)
        return R0LAB_EINVAL;
    flags = r0lab_lock();
    page = r0lab_raw_page_slot_locked(slot_id);
    if (page) {
        owned = r0lab_raw_page_slot_owned_locked(page);
        armed = page->armed;
        generation = page->generation;
        address = page->raw.address;
        state = page->raw.state;
        stats = page->hook_route_stats;
        if (page->raw.mm && page->raw.address) {
            lookup_self =
                r0lab_raw_page_find_by_mm_addr_locked(page->raw.mm,
                                                      page->raw.address) ==
                page;
            fault_lookup_self =
                r0lab_raw_page_find_by_fault_locked(page->raw.mm,
                                                    page->raw.address,
                                                    0) == page;
        }
    }
    miss_stats = g_raw_page_table.hook_route_miss_stats;
    r0lab_unlock(flags);

    snprintf(reply, sizeof(reply),
             "raw_hook_route_status slot=%u generation=%llu hook=all owned=%u armed=%u page=%llx state=%lu route_helper_ready=1 page_record_routed=0 lookup_self=%u fault_lookup_self=%u target_mm_scoped=1 route_hits=%u route_rejects=%u route_wrong_mm=%u route_outside_page=%u route_stale_generation=%u route_busy=%u route_wrong_state=%u route_identity_mismatch=%u table_route_rejects=%u table_route_outside_page=%u table_route_identity_mismatch=%u migration=callbacks_slot0_compat route_kinds=%s,%s,%s,%s,%s,%s,%s\n",
             (unsigned int)slot_id, (unsigned long long)generation,
             owned ? 1U : 0U, armed ? 1U : 0U, (uint64_t)address, state,
             lookup_self ? 1U : 0U, fault_lookup_self ? 1U : 0U,
             stats.route_hits, stats.route_rejects, stats.route_wrong_mm,
             stats.route_outside_page, stats.route_stale_generation,
             stats.route_busy, stats.route_wrong_state,
             stats.route_identity_mismatch, miss_stats.route_rejects,
             miss_stats.route_outside_page,
             miss_stats.route_identity_mismatch,
             r0lab_raw_hook_kind_name(R0LAB_RAW_HOOK_ABORT),
             r0lab_raw_hook_kind_name(R0LAB_RAW_HOOK_FAULT),
             r0lab_raw_hook_kind_name(R0LAB_RAW_HOOK_GUP),
             r0lab_raw_hook_kind_name(R0LAB_RAW_HOOK_FORK),
             r0lab_raw_hook_kind_name(R0LAB_RAW_HOOK_SYSCALL),
             r0lab_raw_hook_kind_name(R0LAB_RAW_HOOK_PRCTL),
             r0lab_raw_hook_kind_name(R0LAB_RAW_HOOK_EXIT));
    return r0lab_copy_reply(out_msg, outlen, reply);
}

static long r0lab_raw_slot_patch_apply(uint64_t token, uint16_t slot_id,
                                       uint64_t generation, uint64_t offset,
                                       uint64_t value, unsigned int length,
                                       char __user *out_msg, int outlen)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    struct r0lab_patch_record old_record = {0};
    struct r0lab_raw_shadow_page *page = NULL;
    struct mm_struct *current_mm = NULL;
    void *patch_data = NULL;
    void *new_record_data = NULL;
    void *old_data_to_free = NULL;
    unsigned long flags;
    unsigned long address = 0;
    uint64_t patch_version = 0;
    uint16_t patch_record_slots = 0;
    uint16_t patch_active_count = 0;
    uint16_t patch_dirty_bytes = 0;
    unsigned int record_index = 0;
    unsigned int rebuild_length = 0;
    unsigned int range_offset;
    bool record_changed = false;
    int result = r0lab_validate_owner(token);

    if (result)
        goto out;
    if (!g_vmalloc || !g_vfree || !g_sync_icache_aliases) {
        result = R0LAB_ENOSYS;
        goto out;
    }
    if ((length != 1U && length != sizeof(uint32_t)) ||
        offset >= R0LAB_RAW_PAGE_SIZE ||
        length > R0LAB_RAW_PAGE_SIZE - offset ||
        (length == sizeof(uint32_t) && (offset & (sizeof(uint32_t) - 1U))) ||
        (length == 1U && value > 0xffULL) ||
        (length == sizeof(uint32_t) && value > 0xffffffffULL)) {
        result = R0LAB_EINVAL;
        goto out;
    }
    patch_data = g_vmalloc(length);
    if (!patch_data) {
        result = R0LAB_ENOMEM;
        goto out;
    }
    if (length == 1U) {
        uint8_t byte_value = (uint8_t)value;

        memcpy(patch_data, &byte_value, sizeof(byte_value));
    } else {
        uint32_t word_value = (uint32_t)value;

        memcpy(patch_data, &word_value, sizeof(word_value));
    }
    current_mm = g_get_task_mm ? g_get_task_mm(current) : NULL;
    if (!current_mm) {
        result = R0LAB_ESRCH;
        goto out;
    }

    range_offset = (unsigned int)offset;
    flags = r0lab_lock();
    result = r0lab_raw_slot_patch_admit_locked(
        token, slot_id, generation, range_offset, length, current_mm, &page);
    if (!result) {
        address = page->raw.address + range_offset;
        result = r0lab_raw_upsert_patch_locked(
            page, range_offset, length, patch_data, &record_index,
            &old_record, &rebuild_length);
        if (!result) {
            new_record_data = patch_data;
            patch_data = NULL;
            record_changed = true;
        } else {
            page->transitioning = false;
        }
    }
    r0lab_unlock(flags);

    if (record_changed)
        result = r0lab_raw_rebuild_patch_range(page, range_offset,
                                               rebuild_length);

    if (record_changed) {
        flags = r0lab_lock();
        if (page->generation == generation) {
            if (result) {
                page->patch_records[record_index] = old_record;
                r0lab_raw_sync_patch_metadata_locked(page);
                patch_data = new_record_data;
                new_record_data = NULL;
            } else {
                old_data_to_free = old_record.data;
                patch_version = page->patch_version;
                patch_record_slots = page->patch_record_slots;
                patch_active_count = page->patch_active_count;
                patch_dirty_bytes = page->patch_dirty_bytes;
            }
            page->transitioning = false;
        } else {
            old_data_to_free = old_record.data;
            result = R0LAB_EAGAIN;
        }
        r0lab_unlock(flags);
    }

out:
    if (current_mm)
        g_mmput(current_mm);
    if (old_data_to_free)
        g_vfree(old_data_to_free);
    if (patch_data)
        g_vfree(patch_data);
    if (result) {
        r0lab_record(R0LAB_EVENT_REJECT, result);
        return result;
    }
    snprintf(reply, sizeof(reply),
             "raw_slot_patch_ok slot=%u generation=%llu offset=%llu length=%u address=%llx patch_record_slots=%u patch_active_count=%u patch_dirty_bytes=%u patch_version=%llu patch_capacity=%u patch_scope=page_slot_ranges\n",
             (unsigned int)slot_id, (unsigned long long)generation,
             (unsigned long long)offset, length, (uint64_t)address,
             (unsigned int)patch_record_slots,
             (unsigned int)patch_active_count,
             (unsigned int)patch_dirty_bytes,
             (unsigned long long)patch_version,
             (unsigned int)R0LAB_PATCH_RECORD_CAPACITY);
    return r0lab_copy_reply(out_msg, outlen, reply);
}

static long r0lab_raw_slot_patch_release(uint64_t token, uint16_t slot_id,
                                         uint64_t generation, uint64_t offset,
                                         char __user *out_msg, int outlen)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    struct r0lab_patch_record old_record = {0};
    struct r0lab_raw_shadow_page *page = NULL;
    struct mm_struct *current_mm = NULL;
    void *old_data_to_free = NULL;
    unsigned long flags;
    unsigned long address = 0;
    uint64_t patch_version = 0;
    uint16_t patch_record_slots = 0;
    uint16_t patch_active_count = 0;
    uint16_t patch_dirty_bytes = 0;
    unsigned int record_index = 0;
    unsigned int range_offset;
    bool record_changed = false;
    int result = r0lab_validate_owner(token);

    if (result)
        goto out;
    if (!g_vfree || !g_sync_icache_aliases) {
        result = R0LAB_ENOSYS;
        goto out;
    }
    if (offset >= R0LAB_RAW_PAGE_SIZE) {
        result = R0LAB_EINVAL;
        goto out;
    }
    current_mm = g_get_task_mm ? g_get_task_mm(current) : NULL;
    if (!current_mm) {
        result = R0LAB_ESRCH;
        goto out;
    }

    range_offset = (unsigned int)offset;
    flags = r0lab_lock();
    result = r0lab_raw_slot_patch_admit_locked(
        token, slot_id, generation, range_offset, 1U, current_mm, &page);
    if (!result) {
        address = page->raw.address + range_offset;
        result = r0lab_raw_release_patch_locked(
            page, range_offset, &record_index, &old_record);
        if (!result) {
            record_changed = true;
        } else {
            page->transitioning = false;
        }
    }
    r0lab_unlock(flags);

    if (record_changed)
        result = r0lab_raw_rebuild_patch_range(page, range_offset,
                                               old_record.length);

    if (record_changed) {
        flags = r0lab_lock();
        if (page->generation == generation) {
            if (result) {
                page->patch_records[record_index] = old_record;
                r0lab_raw_sync_patch_metadata_locked(page);
            } else {
                old_data_to_free = old_record.data;
                patch_version = page->patch_version;
                patch_record_slots = page->patch_record_slots;
                patch_active_count = page->patch_active_count;
                patch_dirty_bytes = page->patch_dirty_bytes;
            }
            page->transitioning = false;
        } else {
            old_data_to_free = old_record.data;
            result = R0LAB_EAGAIN;
        }
        r0lab_unlock(flags);
    }

out:
    if (current_mm)
        g_mmput(current_mm);
    if (old_data_to_free)
        g_vfree(old_data_to_free);
    if (result) {
        r0lab_record(R0LAB_EVENT_REJECT, result);
        return result;
    }
    snprintf(reply, sizeof(reply),
             "raw_slot_patch_release_ok slot=%u generation=%llu offset=%llu address=%llx patch_record_slots=%u patch_active_count=%u patch_dirty_bytes=%u patch_version=%llu patch_capacity=%u patch_scope=page_slot_ranges\n",
             (unsigned int)slot_id, (unsigned long long)generation,
             (unsigned long long)offset, (uint64_t)address,
             (unsigned int)patch_record_slots,
             (unsigned int)patch_active_count,
             (unsigned int)patch_dirty_bytes,
             (unsigned long long)patch_version,
             (unsigned int)R0LAB_PATCH_RECORD_CAPACITY);
    return r0lab_copy_reply(out_msg, outlen, reply);
}

static long r0lab_raw_inspect(uint64_t token, char __user *out_msg, int outlen)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    unsigned long flags;
    unsigned long address;
    unsigned long state;
    unsigned long active_pte;
    unsigned long original_pte;
    unsigned long source_uxn_pte;
    unsigned long shadow_rx_pte;
    unsigned long shadow_xom_pte;
    unsigned long source_pfn;
    unsigned long shadow_pfn;
    unsigned long gup_hide_active;
    unsigned long gup_begin_events;
    unsigned long gup_finish_events;
    unsigned long fork_hide_active;
    unsigned long fork_begin_events;
    unsigned long fork_finish_events;
    unsigned long read_cycle_active;
    unsigned long read_cycle_begin_events;
    unsigned long read_cycle_finish_events;
    bool gup_hook_installed;
    uint32_t gup_hook_begin_events;
    uint32_t gup_hook_finish_events;
    uint32_t gup_hook_failures;
    bool fork_hook_installed;
    uint32_t fork_hook_begin_events;
    uint32_t fork_hook_finish_events;
    uint32_t fork_hook_failures;
    bool fault_hook_installed;
    uint32_t fault_hook_read_events;
    uint32_t fault_hook_write_events;
    uint32_t fault_hook_exec_events;
    uint32_t fault_hook_failures;
    bool exit_hook_installed;
    uint32_t exit_hook_events;
    uint32_t exit_hook_failures;
    bool fault_probe_armed;
    unsigned long fault_probe_address;
    pid_t fault_probe_reader_tgid;
    uint32_t fault_probe_read_events;
    uint32_t fault_probe_write_events;
    uint32_t fault_probe_exec_events;
    uint32_t fault_probe_failures;
    uint32_t abort_read_cycle_events;
    uint32_t activation_events;
    uint8_t record_backend;
    uint8_t record_state;
    const char *active_kind = "none";
    const char *read_cycle_mode = "absent";
    const char *read_cycle_data_fault = "absent";
    unsigned int read_cycle_pte_switch = 0;
    int result = r0lab_validate_owner(token);

    if (result)
        return result;
    flags = r0lab_lock();
    if (!g_raw_page.armed || g_raw_page.clearing) {
        r0lab_unlock(flags);
        return R0LAB_EAGAIN;
    }
    address = g_raw_page.raw.address;
    state = g_raw_page.raw.state;
    active_pte = g_raw_page.raw.active_pte;
    original_pte = g_raw_page.raw.original_pte;
    source_uxn_pte = g_raw_page.raw.source_uxn_pte;
    shadow_rx_pte = g_raw_page.raw.shadow_rx_pte;
    shadow_xom_pte = g_raw_page.raw.shadow_xom_pte;
    source_pfn = g_raw_page.raw.source_pfn;
    shadow_pfn = g_raw_page.raw.shadow_pfn;
    gup_hide_active = g_raw_page.raw.gup_hide_active;
    gup_begin_events = g_raw_page.raw.gup_begin_events;
    gup_finish_events = g_raw_page.raw.gup_finish_events;
    fork_hide_active = g_raw_page.raw.fork_hide_active;
    fork_begin_events = g_raw_page.raw.fork_begin_events;
    fork_finish_events = g_raw_page.raw.fork_finish_events;
    read_cycle_active = g_raw_page.raw.read_cycle_active;
    read_cycle_begin_events = g_raw_page.raw.read_cycle_begin_events;
    read_cycle_finish_events = g_raw_page.raw.read_cycle_finish_events;
    gup_hook_installed = g_raw_page.gup_hook_installed;
    gup_hook_begin_events = g_raw_page.gup_hook_begin_events;
    gup_hook_finish_events = g_raw_page.gup_hook_finish_events;
    gup_hook_failures = g_raw_page.gup_hook_failures;
    fork_hook_installed = g_raw_page.fork_hook_installed;
    fork_hook_begin_events = g_raw_page.fork_hook_begin_events;
    fork_hook_finish_events = g_raw_page.fork_hook_finish_events;
    fork_hook_failures = g_raw_page.fork_hook_failures;
    fault_hook_installed = g_raw_page.fault_hook_installed;
    fault_hook_read_events = g_raw_page.fault_hook_read_events;
    fault_hook_write_events = g_raw_page.fault_hook_write_events;
    fault_hook_exec_events = g_raw_page.fault_hook_exec_events;
    fault_hook_failures = g_raw_page.fault_hook_failures;
    exit_hook_installed = g_raw_page.exit_hook_installed;
    exit_hook_events = g_raw_page.exit_hook_events;
    exit_hook_failures = g_raw_page.exit_hook_failures;
    fault_probe_armed = g_raw_page.fault_probe_armed;
    fault_probe_address = g_raw_page.fault_probe_address;
    fault_probe_reader_tgid = g_raw_page.fault_probe_reader_tgid;
    fault_probe_read_events = g_raw_page.fault_probe_read_events;
    fault_probe_write_events = g_raw_page.fault_probe_write_events;
    fault_probe_exec_events = g_raw_page.fault_probe_exec_events;
    fault_probe_failures = g_raw_page.fault_probe_failures;
    abort_read_cycle_events = g_raw_page.abort_read_cycle_events;
    activation_events = g_raw_page.activation_events;
    record_backend = g_raw_page.record.backend;
    record_state = g_raw_page.record.state;
    r0lab_unlock(flags);

    active_kind = r0lab_raw_active_kind(state, active_pte, original_pte,
                                        source_uxn_pte, shadow_rx_pte,
                                        shadow_xom_pte);

    if (read_cycle_active || read_cycle_begin_events ||
        read_cycle_finish_events)
        read_cycle_mode = "uxn_original_exec_resume";
    if (read_cycle_begin_events)
        read_cycle_pte_switch = 1;
    if (abort_read_cycle_events)
        read_cycle_data_fault = "sync_el0_dabt";

    snprintf(reply, sizeof(reply),
             "raw_inspect page=%llx state=%lu activations=%u active_kind=%s source_pfn_low=%lx shadow_pfn_low=%lx gup_hide_active=%lu gup_begin_events=%lu gup_finish_events=%lu gup_hook_installed=%u gup_hook_begin_events=%u gup_hook_finish_events=%u gup_hook_failures=%u fork_hide_active=%lu fork_begin_events=%lu fork_finish_events=%lu read_cycle_active=%lu read_cycle_begin_events=%lu read_cycle_finish_events=%lu fork_hook_installed=%u fork_hook_begin_events=%u fork_hook_finish_events=%u fork_hook_failures=%u fault_hook_installed=%u fault_hook_read_events=%u fault_hook_write_events=%u fault_hook_exec_events=%u fault_hook_failures=%u exit_hook_installed=%u exit_hook_events=%u exit_hook_failures=%u fault_probe_armed=%u fault_probe_page=%llx fault_probe_reader_tgid=%d fault_probe_read_events=%u fault_probe_write_events=%u fault_probe_exec_events=%u fault_probe_failures=%u abort_read_cycle_events=%u record_backend=%s record_state=%s read_cycle=%s read_cycle_pte_switch=%u read_cycle_data_fault=%s read_cycle_exec_resume=%s fault_hook=observe_only exit_hook=exit_mmap_observe fault_probe=normal_anon_remote_gup original_view_after_shadow=%s gup_hide_primitive=%s fork_hide_primitive=%s\n",
             (uint64_t)address, state, activation_events, active_kind,
             source_pfn & 0xffffUL, shadow_pfn & 0xffffUL, gup_hide_active,
             gup_begin_events, gup_finish_events, gup_hook_installed ? 1 : 0,
             gup_hook_begin_events, gup_hook_finish_events, gup_hook_failures,
             fork_hide_active, fork_begin_events, fork_finish_events,
             read_cycle_active, read_cycle_begin_events,
             read_cycle_finish_events,
             fork_hook_installed ? 1 : 0, fork_hook_begin_events,
             fork_hook_finish_events, fork_hook_failures,
             fault_hook_installed ? 1 : 0, fault_hook_read_events,
             fault_hook_write_events, fault_hook_exec_events,
             fault_hook_failures,
             exit_hook_installed ? 1 : 0, exit_hook_events,
             exit_hook_failures,
             fault_probe_armed ? 1 : 0, (uint64_t)fault_probe_address,
             fault_probe_reader_tgid,
             fault_probe_read_events, fault_probe_write_events,
             fault_probe_exec_events, fault_probe_failures,
             abort_read_cycle_events,
             r0lab_page_record_backend_name(record_backend),
             r0lab_page_record_state_name(record_state), read_cycle_mode,
             read_cycle_pte_switch, read_cycle_data_fault,
             read_cycle_begin_events && read_cycle_finish_events ?
             "proven" : "absent",
             gup_begin_events && gup_finish_events ? "proven" : "absent",
             gup_begin_events && gup_finish_events ? "proven" : "absent",
             fork_begin_events && fork_finish_events ? "proven" : "absent");
    return r0lab_copy_reply(out_msg, outlen, reply);
}

static long r0lab_raw_gup_hide_transition(uint64_t token, bool begin,
                                          char __user *out_msg, int outlen)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    unsigned long flags;
    uint64_t generation = 0;
    int result = r0lab_validate_owner(token);
    unsigned long state = 0;
    unsigned long active_pte = 0;
    unsigned long original_pte = 0;
    unsigned long source_uxn_pte = 0;
    unsigned long shadow_rx_pte = 0;
    unsigned long gup_hide_active = 0;
    unsigned long gup_begin_events = 0;
    unsigned long gup_finish_events = 0;
    const char *active_kind = "none";

    if (result)
        goto record;
    flags = r0lab_lock();
    if (!g_raw_page.armed || g_raw_page.clearing ||
        g_raw_page.transitioning ||
        g_raw_page.raw.state != R0LAB_RAW_SHADOW_RX ||
        (!!g_raw_page.raw.gup_hide_active == begin)) {
        r0lab_unlock(flags);
        result = R0LAB_EAGAIN;
        goto record;
    }
    g_raw_page.transitioning = true;
    generation = g_raw_page.generation;
    r0lab_unlock(flags);

    if (begin)
        result = r0lab_raw_begin_gup_hide(&g_raw_page.raw);
    else
        result = r0lab_raw_finish_gup_hide(&g_raw_page.raw);

    flags = r0lab_lock();
    if (g_raw_page.generation == generation) {
        g_raw_page.transitioning = false;
        state = g_raw_page.raw.state;
        active_pte = g_raw_page.raw.active_pte;
        original_pte = g_raw_page.raw.original_pte;
        source_uxn_pte = g_raw_page.raw.source_uxn_pte;
        shadow_rx_pte = g_raw_page.raw.shadow_rx_pte;
        gup_hide_active = g_raw_page.raw.gup_hide_active;
        gup_begin_events = g_raw_page.raw.gup_begin_events;
        gup_finish_events = g_raw_page.raw.gup_finish_events;
    } else {
        result = R0LAB_EAGAIN;
    }
    r0lab_unlock(flags);

    if (active_pte == original_pte)
        active_kind = "original";
    else if (active_pte == source_uxn_pte)
        active_kind = "source_uxn";
    else if (active_pte == shadow_rx_pte)
        active_kind = "shadow_rx";

    snprintf(reply, sizeof(reply),
             "raw_gup_%s result=%d state=%lu active_kind=%s gup_hide_active=%lu gup_begin_events=%lu gup_finish_events=%lu gup_hide_primitive=proven\n",
             begin ? "begin" : "finish", result, state, active_kind,
             gup_hide_active, gup_begin_events, gup_finish_events);
    if (!result) {
        r0lab_record(begin ? R0LAB_EVENT_RAW_GUP_BEGIN :
                             R0LAB_EVENT_RAW_GUP_FINISH,
                     0);
        return r0lab_copy_reply(out_msg, outlen, reply);
    }

record:
    r0lab_record(begin ? R0LAB_EVENT_RAW_GUP_BEGIN :
                         R0LAB_EVENT_RAW_GUP_FINISH,
                 result);
    return result;
}

static long r0lab_raw_read_cycle_begin_cmd(uint64_t token, char __user *out_msg,
                                           int outlen)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    unsigned long flags;
    uint64_t generation = 0;
    int result = r0lab_validate_owner(token);
    unsigned long state = 0;
    unsigned long active_pte = 0;
    unsigned long original_pte = 0;
    unsigned long source_uxn_pte = 0;
    unsigned long shadow_rx_pte = 0;
    unsigned long read_cycle_active = 0;
    unsigned long read_cycle_begin_events = 0;
    unsigned long read_cycle_finish_events = 0;
    const char *active_kind = "none";

    if (result)
        goto record;
    flags = r0lab_lock();
    if (!g_raw_page.armed || g_raw_page.clearing ||
        g_raw_page.transitioning ||
        g_raw_page.raw.state != R0LAB_RAW_SHADOW_RX ||
        g_raw_page.raw.gup_hide_active ||
        g_raw_page.raw.fork_hide_active ||
        g_raw_page.raw.read_cycle_active) {
        r0lab_unlock(flags);
        result = R0LAB_EAGAIN;
        goto record;
    }
    g_raw_page.transitioning = true;
    generation = g_raw_page.generation;
    r0lab_unlock(flags);

    result = r0lab_raw_begin_read_cycle(&g_raw_page.raw);

    flags = r0lab_lock();
    if (g_raw_page.generation == generation) {
        g_raw_page.transitioning = false;
        if (!result)
            g_raw_page.record.state = R0LAB_PAGE_RECORD_ORIGINAL_READ;
        state = g_raw_page.raw.state;
        active_pte = g_raw_page.raw.active_pte;
        original_pte = g_raw_page.raw.original_pte;
        source_uxn_pte = g_raw_page.raw.source_uxn_pte;
        shadow_rx_pte = g_raw_page.raw.shadow_rx_pte;
        read_cycle_active = g_raw_page.raw.read_cycle_active;
        read_cycle_begin_events = g_raw_page.raw.read_cycle_begin_events;
        read_cycle_finish_events = g_raw_page.raw.read_cycle_finish_events;
    } else {
        result = R0LAB_EAGAIN;
    }
    r0lab_unlock(flags);

    if (state == R0LAB_RAW_ORIGINAL_READ)
        active_kind = "original_read";
    else if (active_pte == original_pte)
        active_kind = "original";
    else if (active_pte == source_uxn_pte)
        active_kind = "source_uxn";
    else if (active_pte == shadow_rx_pte)
        active_kind = "shadow_rx";

    snprintf(reply, sizeof(reply),
             "raw_read_cycle_begin result=%d state=%lu active_kind=%s read_cycle_active=%lu read_cycle_begin_events=%lu read_cycle_finish_events=%lu read_cycle=uxn_original_exec_resume trigger=supercall pte_switch=1 data_fault=absent exec_resume=pending\n",
             result, state, active_kind, read_cycle_active,
             read_cycle_begin_events, read_cycle_finish_events);
    if (!result) {
        r0lab_record(R0LAB_EVENT_RAW_READ_CYCLE_BEGIN, 0);
        return r0lab_copy_reply(out_msg, outlen, reply);
    }

record:
    r0lab_record(R0LAB_EVENT_RAW_READ_CYCLE_BEGIN, result);
    return result;
}

static long r0lab_raw_read_cycle_status(uint64_t token, char __user *out_msg,
                                        int outlen)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    unsigned long flags;
    unsigned long state;
    unsigned long active_pte;
    unsigned long original_pte;
    unsigned long source_uxn_pte;
    unsigned long shadow_rx_pte;
    unsigned long read_cycle_active;
    unsigned long read_cycle_begin_events;
    unsigned long read_cycle_finish_events;
    const char *active_kind = "none";
    int result = r0lab_validate_owner(token);

    if (result)
        return result;
    flags = r0lab_lock();
    if (!g_raw_page.armed || g_raw_page.clearing) {
        r0lab_unlock(flags);
        return R0LAB_EAGAIN;
    }
    state = g_raw_page.raw.state;
    active_pte = g_raw_page.raw.active_pte;
    original_pte = g_raw_page.raw.original_pte;
    source_uxn_pte = g_raw_page.raw.source_uxn_pte;
    shadow_rx_pte = g_raw_page.raw.shadow_rx_pte;
    read_cycle_active = g_raw_page.raw.read_cycle_active;
    read_cycle_begin_events = g_raw_page.raw.read_cycle_begin_events;
    read_cycle_finish_events = g_raw_page.raw.read_cycle_finish_events;
    r0lab_unlock(flags);

    if (state == R0LAB_RAW_ORIGINAL_READ)
        active_kind = "original_read";
    else if (active_pte == original_pte)
        active_kind = "original";
    else if (active_pte == source_uxn_pte)
        active_kind = "source_uxn";
    else if (active_pte == shadow_rx_pte)
        active_kind = "shadow_rx";

    snprintf(reply, sizeof(reply),
             "raw_read_cycle_status state=%lu active_kind=%s read_cycle_active=%lu read_cycle_begin_events=%lu read_cycle_finish_events=%lu read_cycle=uxn_original_exec_resume trigger=supercall pte_switch=%u data_fault=absent exec_resume=%s\n",
             state, active_kind, read_cycle_active, read_cycle_begin_events,
             read_cycle_finish_events, read_cycle_begin_events ? 1 : 0,
             read_cycle_begin_events && read_cycle_finish_events ?
             "proven" : "pending");
    return r0lab_copy_reply(out_msg, outlen, reply);
}

static long r0lab_raw_gup_hook_arm_common(uint64_t token, uint16_t slot_id,
                                          char __user *out_msg, int outlen,
                                          bool legacy_reply)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    const char *hook_name;
    void *hook_target;
    struct r0lab_raw_shadow_page *page;
    unsigned long flags;
    bool installed;
    bool should_install;
    bool uses_pte;
    uint64_t generation;
    int result = r0lab_validate_owner(token);

    if (result)
        goto record;
    if (slot_id >= R0LAB_RAW_PAGE_SLOT_CAPACITY) {
        result = R0LAB_EINVAL;
        goto record;
    }
    hook_target = g_follow_page_pte ? g_follow_page_pte : g_follow_page_mask;
    hook_name = g_follow_page_pte ? "follow_page_pte" : "follow_page_mask";
    uses_pte = !!g_follow_page_pte;
    if (!hook_target) {
        result = R0LAB_ENOSYS;
        goto record;
    }

    flags = r0lab_lock();
    page = r0lab_raw_page_slot_locked(slot_id);
    if (!page || !page->armed || page->clearing ||
        page->transitioning || page->raw.state != R0LAB_RAW_SHADOW_RX ||
        g_session.owner_tgid != r0lab_current_tgid()) {
        r0lab_unlock(flags);
        result = R0LAB_EAGAIN;
        goto record;
    }
    installed = page->gup_hook_installed;
    should_install = !installed && r0lab_raw_gup_hook_users_locked() == 0;
    generation = page->generation;
    if (!installed) {
        page->gup_hook_begin_events = 0;
        page->gup_hook_finish_events = 0;
        page->gup_hook_failures = 0;
    }
    r0lab_unlock(flags);

    if (should_install) {
        if (uses_pte)
            result = hook_wrap5(hook_target, r0lab_raw_gup_pte_before,
                                r0lab_raw_gup_pte_after, NULL);
        else
            result = hook_wrap4(hook_target, r0lab_raw_gup_mask_before,
                                r0lab_raw_gup_mask_after, NULL);
        if (result)
            goto record;
    }
    if (!installed) {
        flags = r0lab_lock();
        page = r0lab_raw_page_slot_locked(slot_id);
        if (page && page->armed && !page->clearing &&
            page->generation == generation &&
            page->raw.state == R0LAB_RAW_SHADOW_RX) {
            page->gup_hook_installed = true;
            page->gup_hook_uses_pte = uses_pte;
        } else {
            result = R0LAB_EAGAIN;
        }
        r0lab_unlock(flags);
        if (result && should_install) {
            if (uses_pte)
                r0lab_hook_detach(hook_target, r0lab_raw_gup_pte_before,
                                  r0lab_raw_gup_pte_after);
            else
                r0lab_hook_detach(hook_target, r0lab_raw_gup_mask_before,
                                  r0lab_raw_gup_mask_after);
            goto record;
        }
    }

    if (legacy_reply)
        snprintf(reply, sizeof(reply),
                 "raw_gup_hook_ready symbol=%s follow_page_pte=%s follow_page_mask=%s installed=1 mode=%s target_mm_scoped=1 external_reader=1\n",
                 hook_name, g_follow_page_pte ? "present" : "absent",
                 g_follow_page_mask ? "present" : "absent", hook_name);
    else
        snprintf(reply, sizeof(reply),
                 "raw_slot_gup_hook_ready slot=%u generation=%llu symbol=%s follow_page_pte=%s follow_page_mask=%s installed=1 mode=%s target_mm_scoped=1 page_record_routed=1 external_reader=1\n",
                 (unsigned int)slot_id, (unsigned long long)generation,
                 hook_name, g_follow_page_pte ? "present" : "absent",
                 g_follow_page_mask ? "present" : "absent", hook_name);
    return r0lab_copy_reply(out_msg, outlen, reply);

record:
    r0lab_record(R0LAB_EVENT_REJECT, result);
    return result;
}

static long r0lab_raw_gup_hook_arm(uint64_t token, char __user *out_msg,
                                   int outlen)
{
    return r0lab_raw_gup_hook_arm_common(
        token, R0LAB_RAW_PRIMARY_SLOT, out_msg, outlen, true);
}

static long r0lab_raw_fault_hook_arm_common(uint64_t token, uint16_t slot_id,
                                            char __user *out_msg, int outlen,
                                            bool legacy_reply)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    struct r0lab_raw_shadow_page *page;
    unsigned long flags;
    bool installed;
    bool should_install;
    uint64_t generation;
    int result = r0lab_validate_owner(token);

    if (result)
        goto record;
    if (!g_handle_mm_fault) {
        result = R0LAB_ENOSYS;
        goto record;
    }
    if (slot_id >= R0LAB_RAW_PAGE_SLOT_CAPACITY) {
        result = R0LAB_EINVAL;
        goto record;
    }

    flags = r0lab_lock();
    page = r0lab_raw_page_slot_locked(slot_id);
    if (!page || !page->armed || page->clearing ||
        page->transitioning || page->raw.state != R0LAB_RAW_SHADOW_RX ||
        g_session.owner_tgid != r0lab_current_tgid()) {
        r0lab_unlock(flags);
        result = R0LAB_EAGAIN;
        goto record;
    }
    installed = page->fault_hook_installed;
    should_install = !installed && r0lab_raw_fault_hook_users_locked() == 0;
    generation = page->generation;
    if (!installed) {
        page->fault_hook_read_events = 0;
        page->fault_hook_write_events = 0;
        page->fault_hook_exec_events = 0;
        page->fault_hook_failures = 0;
    }
    r0lab_unlock(flags);

    if (should_install) {
        result = hook_wrap4(g_handle_mm_fault, r0lab_raw_fault_before,
                            NULL, NULL);
        if (result)
            goto record;
    }
    if (!installed) {
        flags = r0lab_lock();
        page = r0lab_raw_page_slot_locked(slot_id);
        if (page && page->armed && !page->clearing &&
            page->generation == generation &&
            page->raw.state == R0LAB_RAW_SHADOW_RX) {
            page->fault_hook_installed = true;
        } else {
            result = R0LAB_EAGAIN;
        }
        r0lab_unlock(flags);
        if (result && should_install) {
            r0lab_hook_detach(g_handle_mm_fault, r0lab_raw_fault_before,
                              NULL);
            goto record;
        }
    }

    if (legacy_reply)
        snprintf(reply, sizeof(reply),
                 "raw_fault_hook_ready symbol=handle_mm_fault installed=1 target_mm_scoped=1 observe_only=1 pte_switch=0\n");
    else
        snprintf(reply, sizeof(reply),
                 "raw_slot_fault_hook_ready slot=%u generation=%llu symbol=handle_mm_fault installed=1 target_mm_scoped=1 page_record_routed=1 observe_only=1 pte_switch=0\n",
                 (unsigned int)slot_id, (unsigned long long)generation);
    return r0lab_copy_reply(out_msg, outlen, reply);

record:
    r0lab_record(R0LAB_EVENT_REJECT, result);
    return result;
}

static long r0lab_raw_fault_hook_arm(uint64_t token, char __user *out_msg,
                                     int outlen)
{
    return r0lab_raw_fault_hook_arm_common(
        token, R0LAB_RAW_PRIMARY_SLOT, out_msg, outlen, true);
}

static long r0lab_raw_fault_hook_status_common(uint64_t token,
                                               uint16_t slot_id,
                                               char __user *out_msg,
                                               int outlen,
                                               bool legacy_reply)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    struct r0lab_raw_shadow_page *page;
    unsigned long flags;
    bool installed;
    uint64_t generation;
    uint32_t read_events;
    uint32_t write_events;
    uint32_t exec_events;
    uint32_t failures;
    int result = r0lab_validate_owner(token);

    if (result)
        return result;
    if (slot_id >= R0LAB_RAW_PAGE_SLOT_CAPACITY)
        return R0LAB_EINVAL;
    flags = r0lab_lock();
    page = r0lab_raw_page_slot_locked(slot_id);
    if (!page) {
        r0lab_unlock(flags);
        return R0LAB_EINVAL;
    }
    installed = page->fault_hook_installed;
    generation = page->generation;
    read_events = page->fault_hook_read_events;
    write_events = page->fault_hook_write_events;
    exec_events = page->fault_hook_exec_events;
    failures = page->fault_hook_failures;
    r0lab_unlock(flags);

    if (legacy_reply)
        snprintf(reply, sizeof(reply),
                 "raw_fault_hook_status symbol=%s installed=%u read_events=%u write_events=%u exec_events=%u hit_events=%u failures=%u target_mm_scoped=1 observe_only=1 pte_switch=0\n",
                 g_handle_mm_fault ? "handle_mm_fault" : "absent",
                 installed ? 1 : 0, read_events, write_events, exec_events,
                 read_events + write_events + exec_events, failures);
    else
        snprintf(reply, sizeof(reply),
                 "raw_slot_fault_hook_status slot=%u generation=%llu symbol=%s installed=%u read_events=%u write_events=%u exec_events=%u hit_events=%u failures=%u target_mm_scoped=1 page_record_routed=1 observe_only=1 pte_switch=0\n",
                 (unsigned int)slot_id, (unsigned long long)generation,
                 g_handle_mm_fault ? "handle_mm_fault" : "absent",
                 installed ? 1 : 0, read_events, write_events, exec_events,
                 read_events + write_events + exec_events, failures);
    return r0lab_copy_reply(out_msg, outlen, reply);
}

static long r0lab_raw_fault_hook_status(uint64_t token, char __user *out_msg,
                                        int outlen)
{
    return r0lab_raw_fault_hook_status_common(
        token, R0LAB_RAW_PRIMARY_SLOT, out_msg, outlen, true);
}

static long r0lab_raw_fault_af_clear(uint64_t token, uint16_t slot_id,
                                     uint64_t generation,
                                     char __user *out_msg, int outlen)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    struct r0lab_raw_shadow_page *page = NULL;
    struct mm_struct *current_mm = NULL;
    unsigned long flags;
    unsigned long address = 0;
    unsigned long state = R0LAB_RAW_EMPTY;
    int result = r0lab_validate_owner(token);

    if (result)
        goto record;
    if (slot_id >= R0LAB_RAW_PAGE_SLOT_CAPACITY) {
        result = R0LAB_EINVAL;
        goto record;
    }
    current_mm = g_get_task_mm ? g_get_task_mm(current) : NULL;
    if (!current_mm) {
        result = R0LAB_ESRCH;
        goto record;
    }

    flags = r0lab_lock();
    page = r0lab_raw_page_slot_locked(slot_id);
    if (!page || !page->armed || page->reserving || page->clearing ||
        page->transitioning || page->generation != generation ||
        page->raw.mm != current_mm ||
        page->raw.state != R0LAB_RAW_SHADOW_RX ||
        !page->raw.shadow_rx_pte || !page->raw.shadow_pfn ||
        page->raw.gup_hide_active || page->raw.fork_hide_active ||
        page->raw.read_cycle_active || !page->fault_hook_installed ||
        g_session.owner_tgid != r0lab_current_tgid()) {
        r0lab_unlock(flags);
        result = R0LAB_EAGAIN;
        goto record;
    }
    page->transitioning = true;
    address = page->raw.address;
    r0lab_unlock(flags);

    result = r0lab_raw_clear_shadow_access_flag(&page->raw);

    flags = r0lab_lock();
    if (page->generation == generation) {
        state = page->raw.state;
        page->transitioning = false;
    } else {
        result = result ? result : R0LAB_EAGAIN;
    }
    r0lab_unlock(flags);

record:
    if (current_mm)
        g_mmput(current_mm);
    if (result) {
        r0lab_record(R0LAB_EVENT_REJECT, result);
        return result;
    }
    snprintf(reply, sizeof(reply),
             "raw_slot_fault_af_cleared slot=%u generation=%llu page=%llx result=0 pte_af=cleared state=%lu target_mm_scoped=1 page_record_routed=1 pte_switch=0 source=file_backed_rx\n",
             (unsigned int)slot_id, (unsigned long long)generation,
             (uint64_t)address, state);
    return r0lab_copy_reply(out_msg, outlen, reply);
}

static const char *r0lab_raw_abort_probe_source_name(uint8_t source)
{
    if (source == R0LAB_ABORT_PROBE_SOURCE_READ_TRANSLATION)
        return "raw_va_prot_none";
    if (source == R0LAB_ABORT_PROBE_SOURCE_WRITE_PERMISSION)
        return "raw_va_rx_write";
    if (source == R0LAB_ABORT_PROBE_SOURCE_READ_PERMISSION)
        return "raw_shadow_xom";
    return "none";
}

static long r0lab_raw_xom_read_fault_arm_common(uint64_t token,
                                                char __user *out_msg,
                                                int outlen,
                                                bool preflight_reply)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    struct mm_struct *current_mm = NULL;
    unsigned long flags;
    uint64_t generation = 0;
    unsigned long state = R0LAB_RAW_EMPTY;
    unsigned long active_pte = 0;
    unsigned long original_pte = 0;
    unsigned long source_uxn_pte = 0;
    unsigned long shadow_rx_pte = 0;
    unsigned long shadow_xom_pte = 0;
    const char *active_kind = "none";
    int result = r0lab_validate_owner(token);

    if (result)
        goto record;
    current_mm = g_get_task_mm ? g_get_task_mm(current) : NULL;
    if (!current_mm) {
        result = R0LAB_ESRCH;
        goto record;
    }

    flags = r0lab_lock();
    if (!g_raw_page.armed || g_raw_page.clearing ||
        g_raw_page.transitioning || g_raw_page.raw.mm != current_mm ||
        g_raw_page.raw.state != R0LAB_RAW_SHADOW_RX ||
        !g_raw_page.raw.shadow_rx_pte || !g_raw_page.raw.shadow_pfn ||
        !g_raw_page.hook_installed ||
        g_raw_page.raw.gup_hide_active || g_raw_page.raw.fork_hide_active ||
        g_raw_page.raw.read_cycle_active || g_raw_page.abort_probe_armed ||
        g_raw_page.abort_read_cycle_armed ||
        g_session.owner_tgid != r0lab_current_tgid()) {
        r0lab_unlock(flags);
        result = R0LAB_EAGAIN;
        goto record;
    }
    g_raw_page.transitioning = true;
    generation = g_raw_page.generation;
    r0lab_unlock(flags);

    result = r0lab_raw_activate_shadow_xom(&g_raw_page.raw);

    flags = r0lab_lock();
    if (g_raw_page.generation == generation) {
        g_raw_page.transitioning = false;
        if (!result) {
            g_raw_page.abort_probe_armed = true;
            g_raw_page.abort_probe_read_events = 0;
            g_raw_page.abort_probe_write_events = 0;
            g_raw_page.abort_probe_exec_events = 0;
            g_raw_page.abort_probe_failures = 0;
            g_raw_page.abort_probe_last_esr = 0;
            g_raw_page.abort_probe_last_far = 0;
            g_raw_page.abort_probe_source =
                R0LAB_ABORT_PROBE_SOURCE_READ_PERMISSION;
            g_raw_page.record.state = R0LAB_PAGE_RECORD_SHADOW_ACTIVE;
        } else {
            result = result ? result : R0LAB_EAGAIN;
        }
        state = g_raw_page.raw.state;
        active_pte = g_raw_page.raw.active_pte;
        original_pte = g_raw_page.raw.original_pte;
        source_uxn_pte = g_raw_page.raw.source_uxn_pte;
        shadow_rx_pte = g_raw_page.raw.shadow_rx_pte;
        shadow_xom_pte = g_raw_page.raw.shadow_xom_pte;
    } else {
        result = result ? result : R0LAB_EAGAIN;
    }
    r0lab_unlock(flags);

record:
    if (current_mm)
        g_mmput(current_mm);
    if (result) {
        r0lab_record(R0LAB_EVENT_REJECT, result);
        return result;
    }
    active_kind = r0lab_raw_active_kind(state, active_pte, original_pte,
                                        source_uxn_pte, shadow_rx_pte,
                                        shadow_xom_pte);
    snprintf(reply, sizeof(reply),
             "%s symbol=do_mem_abort installed=1 armed=1 generation=%llu state=%lu active_kind=%s source=raw_shadow_xom action=begin_xom_read_cycle observe_only=0 pte_switch=1 read_fault=permission read_cycle=uxn_original_exec_resume expected_wnr=0 skip_origin=1 shadow_xom_pte=%lx capability=raw_xom_read_fault\n",
             preflight_reply ? "raw_xom_preflight_ready" :
                               "raw_xom_read_fault_ready",
             (unsigned long long)generation, state, active_kind,
             shadow_xom_pte);
    return r0lab_copy_reply(out_msg, outlen, reply);
}

static long r0lab_raw_xom_read_fault_arm(uint64_t token, char __user *out_msg,
                                         int outlen)
{
    return r0lab_raw_xom_read_fault_arm_common(token, out_msg, outlen, false);
}

static long r0lab_raw_xom_preflight_arm(uint64_t token, char __user *out_msg,
                                        int outlen)
{
    return r0lab_raw_xom_read_fault_arm_common(token, out_msg, outlen, true);
}

static long r0lab_raw_abort_probe_arm_common(uint64_t token, uint16_t slot_id,
                                             char __user *out_msg, int outlen,
                                             uint8_t source,
                                             bool legacy_reply)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    struct r0lab_raw_shadow_page *page;
    unsigned long flags;
    uint64_t generation;
    int result = r0lab_validate_owner(token);

    if (result)
        goto record;
    if (source != R0LAB_ABORT_PROBE_SOURCE_READ_TRANSLATION &&
        source != R0LAB_ABORT_PROBE_SOURCE_WRITE_PERMISSION &&
        source != R0LAB_ABORT_PROBE_SOURCE_READ_PERMISSION) {
        result = R0LAB_EINVAL;
        goto record;
    }
    if (slot_id >= R0LAB_RAW_PAGE_SLOT_CAPACITY) {
        result = R0LAB_EINVAL;
        goto record;
    }

    flags = r0lab_lock();
    page = r0lab_raw_page_slot_locked(slot_id);
    if (!page || !page->armed || !page->hook_installed ||
        page->clearing || page->transitioning ||
        page->raw.state != R0LAB_RAW_SHADOW_RX ||
        g_session.owner_tgid != r0lab_current_tgid() ||
        page->abort_probe_armed ||
        page->abort_read_cycle_armed) {
        r0lab_unlock(flags);
        result = R0LAB_EAGAIN;
        goto record;
    }
    generation = page->generation;
    page->abort_probe_armed = true;
    page->abort_probe_read_events = 0;
    page->abort_probe_write_events = 0;
    page->abort_probe_exec_events = 0;
    page->abort_probe_failures = 0;
    page->abort_probe_last_esr = 0;
    page->abort_probe_last_far = 0;
    page->abort_probe_source = source;
    r0lab_unlock(flags);

    if (legacy_reply)
        snprintf(reply, sizeof(reply),
                 "raw_abort_probe_ready symbol=do_mem_abort installed=1 armed=1 target_mm_scoped=1 source=%s observe_only=1 pte_switch=0 data_fault=sync_el0_dabt read_cycle=absent\n",
                 r0lab_raw_abort_probe_source_name(source));
    else
        snprintf(reply, sizeof(reply),
                 "raw_slot_abort_probe_ready slot=%u generation=%llu symbol=do_mem_abort installed=1 armed=1 target_mm_scoped=1 page_record_routed=1 source=%s observe_only=1 pte_switch=0 data_fault=sync_el0_dabt read_cycle=absent\n",
                 (unsigned int)slot_id, (unsigned long long)generation,
                 r0lab_raw_abort_probe_source_name(source));
    return r0lab_copy_reply(out_msg, outlen, reply);

record:
    r0lab_record(R0LAB_EVENT_REJECT, result);
    return result;
}

static long r0lab_raw_abort_probe_arm(uint64_t token, char __user *out_msg,
                                      int outlen)
{
    return r0lab_raw_abort_probe_arm_common(
        token, R0LAB_RAW_PRIMARY_SLOT, out_msg, outlen,
        R0LAB_ABORT_PROBE_SOURCE_READ_TRANSLATION, true);
}

static long r0lab_raw_abort_write_probe_arm(uint64_t token,
                                            char __user *out_msg, int outlen)
{
    return r0lab_raw_abort_probe_arm_common(
        token, R0LAB_RAW_PRIMARY_SLOT, out_msg, outlen,
        R0LAB_ABORT_PROBE_SOURCE_WRITE_PERMISSION, true);
}

static long r0lab_raw_abort_probe_status_common(uint64_t token,
                                                uint16_t slot_id,
                                                char __user *out_msg,
                                                int outlen,
                                                bool legacy_reply)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    struct r0lab_raw_shadow_page *page;
    unsigned long flags;
    bool installed;
    bool armed;
    uint64_t generation;
    uint32_t read_events;
    uint32_t write_events;
    uint32_t exec_events;
    uint32_t failures;
    uint32_t last_esr;
    unsigned long last_far;
    uint32_t last_ec;
    uint32_t last_fsc_type;
    uint32_t last_wnr;
    uint32_t permission_fault;
    uint32_t translation_fault;
    uint8_t source;
    bool xom_read_cycle_source;
    int result = r0lab_validate_owner(token);

    if (result)
        return result;
    if (slot_id >= R0LAB_RAW_PAGE_SLOT_CAPACITY)
        return R0LAB_EINVAL;
    flags = r0lab_lock();
    page = r0lab_raw_page_slot_locked(slot_id);
    if (!page) {
        r0lab_unlock(flags);
        return R0LAB_EINVAL;
    }
    installed = page->hook_installed;
    armed = page->abort_probe_armed;
    generation = page->generation;
    read_events = page->abort_probe_read_events;
    write_events = page->abort_probe_write_events;
    exec_events = page->abort_probe_exec_events;
    failures = page->abort_probe_failures;
    last_esr = page->abort_probe_last_esr;
    last_far = page->abort_probe_last_far;
    source = page->abort_probe_source;
    r0lab_unlock(flags);

    last_ec = last_esr >> R0LAB_M3_ESR_EC_SHIFT;
    last_fsc_type = last_esr & R0LAB_M3_ESR_FSC_TYPE;
    last_wnr = (last_esr & R0LAB_M3_ESR_WNR) ? 1 : 0;
    permission_fault = last_fsc_type == R0LAB_M3_ESR_FSC_PERM ? 1 : 0;
    translation_fault =
        last_fsc_type == R0LAB_M3_ESR_FSC_TRANSLATION ? 1 : 0;
    xom_read_cycle_source =
        source == R0LAB_ABORT_PROBE_SOURCE_READ_PERMISSION;
    if (legacy_reply)
        snprintf(reply, sizeof(reply),
                 "raw_abort_probe_status symbol=%s installed=%u armed=%u read_events=%u write_events=%u exec_events=%u hit_events=%u failures=%u last_far=%llx last_esr=%x last_ec=%u last_fsc_type=%x last_wnr=%u permission_fault=%u translation_fault=%u target_mm_scoped=1 source=%s action=%s observe_only=%u pte_switch=%u data_fault=sync_el0_dabt read_cycle=%s skip_origin=%u\n",
                 g_do_mem_abort ? "do_mem_abort" : "absent",
                 installed ? 1 : 0, armed ? 1 : 0, read_events,
                 write_events, exec_events,
                 read_events + write_events + exec_events, failures,
                 (uint64_t)last_far, last_esr, last_ec, last_fsc_type,
                 last_wnr, permission_fault, translation_fault,
                 r0lab_raw_abort_probe_source_name(source),
                 xom_read_cycle_source ? "begin_xom_read_cycle" : "observe",
                 xom_read_cycle_source ? 0U : 1U,
                 xom_read_cycle_source ? 1U : 0U,
                 xom_read_cycle_source ? "uxn_original_exec_resume" :
                                         "absent",
                 xom_read_cycle_source ? 1U : 0U);
    else
        snprintf(reply, sizeof(reply),
                 "raw_slot_abort_probe_status slot=%u generation=%llu symbol=%s installed=%u armed=%u read_events=%u write_events=%u exec_events=%u hit_events=%u failures=%u last_far=%llx last_esr=%x last_ec=%u last_fsc_type=%x last_wnr=%u permission_fault=%u translation_fault=%u target_mm_scoped=1 page_record_routed=1 source=%s action=%s observe_only=%u pte_switch=%u data_fault=sync_el0_dabt read_cycle=%s skip_origin=%u\n",
                 (unsigned int)slot_id, (unsigned long long)generation,
                 g_do_mem_abort ? "do_mem_abort" : "absent",
                 installed ? 1 : 0, armed ? 1 : 0, read_events,
                 write_events, exec_events,
                 read_events + write_events + exec_events, failures,
                 (uint64_t)last_far, last_esr, last_ec, last_fsc_type,
                 last_wnr, permission_fault, translation_fault,
                 r0lab_raw_abort_probe_source_name(source),
                 xom_read_cycle_source ? "begin_xom_read_cycle" : "observe",
                 xom_read_cycle_source ? 0U : 1U,
                 xom_read_cycle_source ? 1U : 0U,
                 xom_read_cycle_source ? "uxn_original_exec_resume" :
                                         "absent",
                 xom_read_cycle_source ? 1U : 0U);
    return r0lab_copy_reply(out_msg, outlen, reply);
}

static long r0lab_raw_abort_probe_status(uint64_t token, char __user *out_msg,
                                         int outlen)
{
    return r0lab_raw_abort_probe_status_common(
        token, R0LAB_RAW_PRIMARY_SLOT, out_msg, outlen, true);
}

static long r0lab_raw_abort_probe_clear_common(uint64_t token,
                                              uint16_t slot_id,
                                              char __user *out_msg,
                                              int outlen,
                                              bool legacy_reply)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    struct r0lab_raw_shadow_page *page;
    unsigned long flags;
    bool armed;
    uint64_t generation;
    uint32_t read_events;
    uint32_t write_events;
    uint32_t exec_events;
    uint32_t failures;
    uint32_t last_esr;
    unsigned long last_far;
    uint8_t source;
    int result = r0lab_validate_owner(token);

    if (result)
        return result;
    if (slot_id >= R0LAB_RAW_PAGE_SLOT_CAPACITY)
        return R0LAB_EINVAL;
    flags = r0lab_lock();
    page = r0lab_raw_page_slot_locked(slot_id);
    if (!page) {
        r0lab_unlock(flags);
        return R0LAB_EINVAL;
    }
    armed = page->abort_probe_armed;
    page->abort_probe_armed = false;
    generation = page->generation;
    read_events = page->abort_probe_read_events;
    write_events = page->abort_probe_write_events;
    exec_events = page->abort_probe_exec_events;
    failures = page->abort_probe_failures;
    last_esr = page->abort_probe_last_esr;
    last_far = page->abort_probe_last_far;
    source = page->abort_probe_source;
    page->abort_probe_source = R0LAB_ABORT_PROBE_SOURCE_NONE;
    r0lab_unlock(flags);

    if (armed) {
        result = r0lab_raw_wait_for_callbacks();
        if (result)
            goto record;
    }
    if (legacy_reply)
        snprintf(reply, sizeof(reply),
                 "raw_abort_probe_cleared armed=0 read_events=%u write_events=%u exec_events=%u hit_events=%u failures=%u last_far=%llx last_esr=%x source=%s\n",
                 read_events, write_events, exec_events,
                 read_events + write_events + exec_events, failures,
                 (uint64_t)last_far, last_esr,
                 r0lab_raw_abort_probe_source_name(source));
    else
        snprintf(reply, sizeof(reply),
                 "raw_slot_abort_probe_cleared slot=%u generation=%llu armed=0 read_events=%u write_events=%u exec_events=%u hit_events=%u failures=%u last_far=%llx last_esr=%x source=%s page_record_routed=1\n",
                 (unsigned int)slot_id, (unsigned long long)generation,
                 read_events, write_events, exec_events,
                 read_events + write_events + exec_events, failures,
                 (uint64_t)last_far, last_esr,
                 r0lab_raw_abort_probe_source_name(source));
    return r0lab_copy_reply(out_msg, outlen, reply);

record:
    r0lab_record(R0LAB_EVENT_REJECT, result);
    return result;
}

static long r0lab_raw_abort_probe_clear(uint64_t token, char __user *out_msg,
                                        int outlen)
{
    return r0lab_raw_abort_probe_clear_common(
        token, R0LAB_RAW_PRIMARY_SLOT, out_msg, outlen, true);
}

static long r0lab_raw_abort_read_cycle_arm_common(uint64_t token,
                                                  uint16_t slot_id,
                                                  char __user *out_msg,
                                                  int outlen,
                                                  bool legacy_reply)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    struct r0lab_raw_shadow_page *page;
    unsigned long flags;
    uint64_t generation;
    int result = r0lab_validate_owner(token);

    if (result)
        goto record;
    if (slot_id >= R0LAB_RAW_PAGE_SLOT_CAPACITY) {
        result = R0LAB_EINVAL;
        goto record;
    }

    flags = r0lab_lock();
    page = r0lab_raw_page_slot_locked(slot_id);
    if (!page || !page->armed || !page->hook_installed ||
        page->clearing || page->transitioning ||
        page->raw.state != R0LAB_RAW_SHADOW_RX ||
        page->raw.gup_hide_active || page->raw.fork_hide_active ||
        page->raw.read_cycle_active ||
        g_session.owner_tgid != r0lab_current_tgid() ||
        page->abort_probe_armed ||
        page->abort_write_release_armed ||
        page->abort_read_cycle_armed) {
        r0lab_unlock(flags);
        result = R0LAB_EAGAIN;
        goto record;
    }
    generation = page->generation;
    page->abort_read_cycle_armed = true;
    page->abort_read_cycle_events = 0;
    page->abort_read_cycle_failures = 0;
    page->abort_read_cycle_last_esr = 0;
    page->abort_read_cycle_last_far = 0;
    page->abort_read_cycle_last_result = R0LAB_EINVAL;
    r0lab_unlock(flags);

    if (legacy_reply)
        snprintf(reply, sizeof(reply),
                 "raw_abort_read_cycle_ready symbol=do_mem_abort installed=1 armed=1 target_mm_scoped=1 source=raw_va_prot_none action=begin_read_cycle observe_only=0 pte_switch=1 data_fault=sync_el0_dabt read_cycle=uxn_original_exec_resume skip_origin=1 exec_resume=pending\n");
    else
        snprintf(reply, sizeof(reply),
                 "raw_slot_abort_read_cycle_ready slot=%u generation=%llu symbol=do_mem_abort installed=1 armed=1 target_mm_scoped=1 page_record_routed=1 source=raw_va_prot_none action=begin_read_cycle observe_only=0 pte_switch=1 data_fault=sync_el0_dabt read_cycle=uxn_original_exec_resume skip_origin=1 exec_resume=pending\n",
                 (unsigned int)slot_id, (unsigned long long)generation);
    return r0lab_copy_reply(out_msg, outlen, reply);

record:
    r0lab_record(R0LAB_EVENT_REJECT, result);
    return result;
}

static long r0lab_raw_abort_read_cycle_arm(uint64_t token,
                                           char __user *out_msg, int outlen)
{
    return r0lab_raw_abort_read_cycle_arm_common(
        token, R0LAB_RAW_PRIMARY_SLOT, out_msg, outlen, true);
}

static long r0lab_raw_abort_read_cycle_status_common(uint64_t token,
                                                     uint16_t slot_id,
                                                     char __user *out_msg,
                                                     int outlen,
                                                     bool legacy_reply)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    struct r0lab_raw_shadow_page *page;
    unsigned long flags;
    bool installed;
    bool armed;
    uint64_t generation;
    uint32_t events;
    uint32_t failures;
    uint32_t last_esr;
    unsigned long last_far;
    uint32_t last_ec;
    uint32_t last_fsc_type;
    uint32_t last_wnr;
    uint32_t permission_fault;
    uint32_t translation_fault;
    int begin_result;
    unsigned long read_cycle_active;
    unsigned long read_cycle_begin_events;
    unsigned long read_cycle_finish_events;
    const char *exec_resume;
    int result = r0lab_validate_owner(token);

    if (result)
        return result;
    if (slot_id >= R0LAB_RAW_PAGE_SLOT_CAPACITY)
        return R0LAB_EINVAL;
    flags = r0lab_lock();
    page = r0lab_raw_page_slot_locked(slot_id);
    if (!page) {
        r0lab_unlock(flags);
        return R0LAB_EINVAL;
    }
    installed = page->hook_installed;
    armed = page->abort_read_cycle_armed;
    generation = page->generation;
    events = page->abort_read_cycle_events;
    failures = page->abort_read_cycle_failures;
    last_esr = page->abort_read_cycle_last_esr;
    last_far = page->abort_read_cycle_last_far;
    begin_result = page->abort_read_cycle_last_result;
    read_cycle_active = page->raw.read_cycle_active;
    read_cycle_begin_events = page->raw.read_cycle_begin_events;
    read_cycle_finish_events = page->raw.read_cycle_finish_events;
    r0lab_unlock(flags);

    last_ec = last_esr >> R0LAB_M3_ESR_EC_SHIFT;
    last_fsc_type = last_esr & R0LAB_M3_ESR_FSC_TYPE;
    last_wnr = (last_esr & R0LAB_M3_ESR_WNR) ? 1 : 0;
    permission_fault = last_fsc_type == R0LAB_M3_ESR_FSC_PERM ? 1 : 0;
    translation_fault =
        last_fsc_type == R0LAB_M3_ESR_FSC_TRANSLATION ? 1 : 0;
    exec_resume = read_cycle_begin_events && read_cycle_finish_events ?
                  "proven" :
                  (read_cycle_begin_events ? "pending" : "absent");
    if (legacy_reply)
        snprintf(reply, sizeof(reply),
                 "raw_abort_read_cycle_status symbol=%s installed=%u armed=%u read_events=%u failures=%u last_far=%llx last_esr=%x last_ec=%u last_fsc_type=%x last_wnr=%u permission_fault=%u translation_fault=%u begin_result=%d target_mm_scoped=1 source=raw_va_prot_none action=begin_read_cycle observe_only=0 pte_switch=%u data_fault=sync_el0_dabt read_cycle=uxn_original_exec_resume read_cycle_active=%lu read_cycle_begin_events=%lu read_cycle_finish_events=%lu skip_origin=1 exec_resume=%s\n",
                 g_do_mem_abort ? "do_mem_abort" : "absent",
                 installed ? 1 : 0, armed ? 1 : 0, events, failures,
                 (uint64_t)last_far, last_esr, last_ec, last_fsc_type,
                 last_wnr, permission_fault, translation_fault, begin_result,
                 events && !begin_result ? 1 : 0, read_cycle_active,
                 read_cycle_begin_events, read_cycle_finish_events,
                 exec_resume);
    else
        snprintf(reply, sizeof(reply),
                 "raw_slot_abort_read_cycle_status slot=%u generation=%llu symbol=%s installed=%u armed=%u read_events=%u failures=%u last_far=%llx last_esr=%x last_ec=%u last_fsc_type=%x last_wnr=%u permission_fault=%u translation_fault=%u begin_result=%d target_mm_scoped=1 page_record_routed=1 source=raw_va_prot_none action=begin_read_cycle observe_only=0 pte_switch=%u data_fault=sync_el0_dabt read_cycle=uxn_original_exec_resume read_cycle_active=%lu read_cycle_begin_events=%lu read_cycle_finish_events=%lu skip_origin=1 exec_resume=%s\n",
                 (unsigned int)slot_id, (unsigned long long)generation,
                 g_do_mem_abort ? "do_mem_abort" : "absent",
                 installed ? 1 : 0, armed ? 1 : 0, events, failures,
                 (uint64_t)last_far, last_esr, last_ec, last_fsc_type,
                 last_wnr, permission_fault, translation_fault, begin_result,
                 events && !begin_result ? 1 : 0, read_cycle_active,
                 read_cycle_begin_events, read_cycle_finish_events,
                 exec_resume);
    return r0lab_copy_reply(out_msg, outlen, reply);
}

static long r0lab_raw_abort_read_cycle_status(uint64_t token,
                                              char __user *out_msg,
                                              int outlen)
{
    return r0lab_raw_abort_read_cycle_status_common(
        token, R0LAB_RAW_PRIMARY_SLOT, out_msg, outlen, true);
}

static long r0lab_raw_abort_read_cycle_clear_common(uint64_t token,
                                                    uint16_t slot_id,
                                                    char __user *out_msg,
                                                    int outlen,
                                                    bool legacy_reply)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    struct r0lab_raw_shadow_page *page;
    unsigned long flags;
    bool armed;
    uint64_t generation;
    uint32_t events;
    uint32_t failures;
    uint32_t last_esr;
    unsigned long last_far;
    int begin_result;
    int result = r0lab_validate_owner(token);

    if (result)
        return result;
    if (slot_id >= R0LAB_RAW_PAGE_SLOT_CAPACITY)
        return R0LAB_EINVAL;
    flags = r0lab_lock();
    page = r0lab_raw_page_slot_locked(slot_id);
    if (!page) {
        r0lab_unlock(flags);
        return R0LAB_EINVAL;
    }
    armed = page->abort_read_cycle_armed;
    page->abort_read_cycle_armed = false;
    generation = page->generation;
    events = page->abort_read_cycle_events;
    failures = page->abort_read_cycle_failures;
    last_esr = page->abort_read_cycle_last_esr;
    last_far = page->abort_read_cycle_last_far;
    begin_result = page->abort_read_cycle_last_result;
    r0lab_unlock(flags);

    if (armed) {
        result = r0lab_raw_wait_for_callbacks();
        if (result)
            goto record;
    }
    if (legacy_reply)
        snprintf(reply, sizeof(reply),
                 "raw_abort_read_cycle_cleared armed=0 read_events=%u failures=%u last_far=%llx last_esr=%x begin_result=%d\n",
                 events, failures, (uint64_t)last_far, last_esr,
                 begin_result);
    else
        snprintf(reply, sizeof(reply),
                 "raw_slot_abort_read_cycle_cleared slot=%u generation=%llu armed=0 read_events=%u failures=%u last_far=%llx last_esr=%x begin_result=%d page_record_routed=1\n",
                 (unsigned int)slot_id, (unsigned long long)generation,
                 events, failures, (uint64_t)last_far, last_esr,
                 begin_result);
    return r0lab_copy_reply(out_msg, outlen, reply);

record:
    r0lab_record(R0LAB_EVENT_REJECT, result);
    return result;
}

static long r0lab_raw_abort_read_cycle_clear(uint64_t token,
                                             char __user *out_msg, int outlen)
{
    return r0lab_raw_abort_read_cycle_clear_common(
        token, R0LAB_RAW_PRIMARY_SLOT, out_msg, outlen, true);
}

static long r0lab_raw_abort_write_release_arm_common(uint64_t token,
                                                     uint16_t slot_id,
                                                     char __user *out_msg,
                                                     int outlen,
                                                     bool legacy_reply)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    struct r0lab_raw_shadow_page *page;
    unsigned long flags;
    uint64_t generation;
    int result = r0lab_validate_owner(token);

    if (result)
        goto record;
    if (slot_id >= R0LAB_RAW_PAGE_SLOT_CAPACITY) {
        result = R0LAB_EINVAL;
        goto record;
    }

    flags = r0lab_lock();
    page = r0lab_raw_page_slot_locked(slot_id);
    if (!page || !page->armed || !page->hook_installed ||
        page->clearing || page->transitioning ||
        page->raw.state != R0LAB_RAW_SHADOW_RX ||
        g_session.owner_tgid != r0lab_current_tgid() ||
        page->abort_probe_armed ||
        page->abort_write_release_armed ||
        page->abort_read_cycle_armed) {
        r0lab_unlock(flags);
        result = R0LAB_EAGAIN;
        goto record;
    }
    generation = page->generation;
    page->abort_write_release_armed = true;
    page->abort_write_release_events = 0;
    page->abort_write_release_failures = 0;
    page->abort_write_release_last_esr = 0;
    page->abort_write_release_last_far = 0;
    page->abort_write_release_last_result = R0LAB_EINVAL;
    r0lab_unlock(flags);

    if (legacy_reply)
        snprintf(reply, sizeof(reply),
                 "raw_abort_write_release_ready symbol=do_mem_abort installed=1 armed=1 target_mm_scoped=1 source=raw_va_rx_write action=restore_original logical_release=1 observe_only=0 pte_switch=1 data_fault=sync_el0_dabt read_cycle=absent skip_origin=0\n");
    else
        snprintf(reply, sizeof(reply),
                 "raw_slot_abort_write_release_ready slot=%u generation=%llu symbol=do_mem_abort installed=1 armed=1 target_mm_scoped=1 page_record_routed=1 source=raw_va_rx_write action=restore_original logical_release=1 observe_only=0 pte_switch=1 data_fault=sync_el0_dabt read_cycle=absent skip_origin=0\n",
                 (unsigned int)slot_id, (unsigned long long)generation);
    return r0lab_copy_reply(out_msg, outlen, reply);

record:
    r0lab_record(R0LAB_EVENT_REJECT, result);
    return result;
}

static long r0lab_raw_abort_write_release_arm(uint64_t token,
                                              char __user *out_msg,
                                              int outlen)
{
    return r0lab_raw_abort_write_release_arm_common(
        token, R0LAB_RAW_PRIMARY_SLOT, out_msg, outlen, true);
}

static long r0lab_raw_abort_write_release_status_common(uint64_t token,
                                                        uint16_t slot_id,
                                                        char __user *out_msg,
                                                        int outlen,
                                                        bool legacy_reply)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    struct r0lab_raw_shadow_page *page;
    unsigned long flags;
    bool installed;
    bool armed;
    uint64_t generation;
    uint32_t release_events;
    uint32_t failures;
    uint32_t last_esr;
    unsigned long last_far;
    uint32_t last_ec;
    uint32_t last_fsc_type;
    uint32_t last_wnr;
    uint32_t permission_fault;
    uint32_t translation_fault;
    int restore_result;
    unsigned int pte_switch;
    int result = r0lab_validate_owner(token);

    if (result)
        return result;
    if (slot_id >= R0LAB_RAW_PAGE_SLOT_CAPACITY)
        return R0LAB_EINVAL;
    flags = r0lab_lock();
    page = r0lab_raw_page_slot_locked(slot_id);
    if (!page) {
        r0lab_unlock(flags);
        return R0LAB_EINVAL;
    }
    installed = page->hook_installed;
    armed = page->abort_write_release_armed;
    generation = page->generation;
    release_events = page->abort_write_release_events;
    failures = page->abort_write_release_failures;
    last_esr = page->abort_write_release_last_esr;
    last_far = page->abort_write_release_last_far;
    restore_result = page->abort_write_release_last_result;
    r0lab_unlock(flags);

    last_ec = last_esr >> R0LAB_M3_ESR_EC_SHIFT;
    last_fsc_type = last_esr & R0LAB_M3_ESR_FSC_TYPE;
    last_wnr = (last_esr & R0LAB_M3_ESR_WNR) ? 1 : 0;
    permission_fault = last_fsc_type == R0LAB_M3_ESR_FSC_PERM ? 1 : 0;
    translation_fault =
        last_fsc_type == R0LAB_M3_ESR_FSC_TRANSLATION ? 1 : 0;
    pte_switch = release_events && !restore_result ? 1 : 0;
    if (legacy_reply)
        snprintf(reply, sizeof(reply),
                 "raw_abort_write_release_status symbol=%s installed=%u armed=%u release_events=%u failures=%u last_far=%llx last_esr=%x last_ec=%u last_fsc_type=%x last_wnr=%u permission_fault=%u translation_fault=%u restore_result=%d target_mm_scoped=1 source=raw_va_rx_write action=restore_original logical_release=1 observe_only=0 pte_switch=%u data_fault=sync_el0_dabt read_cycle=absent skip_origin=0\n",
                 g_do_mem_abort ? "do_mem_abort" : "absent",
                 installed ? 1 : 0, armed ? 1 : 0, release_events,
                 failures, (uint64_t)last_far, last_esr, last_ec,
                 last_fsc_type, last_wnr, permission_fault,
                 translation_fault, restore_result, pte_switch);
    else
        snprintf(reply, sizeof(reply),
                 "raw_slot_abort_write_release_status slot=%u generation=%llu symbol=%s installed=%u armed=%u release_events=%u failures=%u last_far=%llx last_esr=%x last_ec=%u last_fsc_type=%x last_wnr=%u permission_fault=%u translation_fault=%u restore_result=%d target_mm_scoped=1 page_record_routed=1 source=raw_va_rx_write action=restore_original logical_release=1 observe_only=0 pte_switch=%u data_fault=sync_el0_dabt read_cycle=absent skip_origin=0\n",
                 (unsigned int)slot_id, (unsigned long long)generation,
                 g_do_mem_abort ? "do_mem_abort" : "absent",
                 installed ? 1 : 0, armed ? 1 : 0, release_events,
                 failures, (uint64_t)last_far, last_esr, last_ec,
                 last_fsc_type, last_wnr, permission_fault,
                 translation_fault, restore_result, pte_switch);
    return r0lab_copy_reply(out_msg, outlen, reply);
}

static long r0lab_raw_abort_write_release_status(uint64_t token,
                                                 char __user *out_msg,
                                                 int outlen)
{
    return r0lab_raw_abort_write_release_status_common(
        token, R0LAB_RAW_PRIMARY_SLOT, out_msg, outlen, true);
}

static long r0lab_raw_abort_write_release_clear_common(uint64_t token,
                                                       uint16_t slot_id,
                                                       char __user *out_msg,
                                                       int outlen,
                                                       bool legacy_reply)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    struct r0lab_raw_shadow_page *page;
    unsigned long flags;
    uint64_t generation;
    uint32_t release_events;
    uint32_t failures;
    uint32_t last_esr;
    unsigned long last_far;
    int restore_result;
    int result = r0lab_validate_owner(token);

    if (result)
        return result;
    if (slot_id >= R0LAB_RAW_PAGE_SLOT_CAPACITY)
        return R0LAB_EINVAL;
    flags = r0lab_lock();
    page = r0lab_raw_page_slot_locked(slot_id);
    if (!page) {
        r0lab_unlock(flags);
        return R0LAB_EINVAL;
    }
    page->abort_write_release_armed = false;
    generation = page->generation;
    release_events = page->abort_write_release_events;
    failures = page->abort_write_release_failures;
    last_esr = page->abort_write_release_last_esr;
    last_far = page->abort_write_release_last_far;
    restore_result = page->abort_write_release_last_result;
    r0lab_unlock(flags);

    if (legacy_reply)
        snprintf(reply, sizeof(reply),
                 "raw_abort_write_release_cleared armed=0 release_events=%u failures=%u last_far=%llx last_esr=%x restore_result=%d source=raw_va_rx_write action=restore_original logical_release=1\n",
                 release_events, failures, (uint64_t)last_far, last_esr,
                 restore_result);
    else
        snprintf(reply, sizeof(reply),
                 "raw_slot_abort_write_release_cleared slot=%u generation=%llu armed=0 release_events=%u failures=%u last_far=%llx last_esr=%x restore_result=%d source=raw_va_rx_write action=restore_original logical_release=1 page_record_routed=1\n",
                 (unsigned int)slot_id, (unsigned long long)generation,
                 release_events, failures, (uint64_t)last_far, last_esr,
                 restore_result);
    return r0lab_copy_reply(out_msg, outlen, reply);
}

static long r0lab_raw_abort_write_release_clear(uint64_t token,
                                                char __user *out_msg,
                                                int outlen)
{
    return r0lab_raw_abort_write_release_clear_common(
        token, R0LAB_RAW_PRIMARY_SLOT, out_msg, outlen, true);
}

static long r0lab_raw_fault_probe_arm(uint64_t token,
                                      unsigned long probe_address,
                                      char __user *out_msg, int outlen)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    unsigned long flags;
    int result = r0lab_validate_owner(token);

    if (result)
        goto record;
    if (!probe_address ||
        (probe_address & (R0LAB_RAW_PAGE_SIZE - 1UL))) {
        result = R0LAB_EINVAL;
        goto record;
    }

    flags = r0lab_lock();
    if (!g_raw_page.armed || g_raw_page.clearing ||
        g_raw_page.transitioning || !g_raw_page.fault_hook_installed ||
        g_raw_page.raw.state != R0LAB_RAW_SHADOW_RX ||
        g_session.owner_tgid != r0lab_current_tgid() ||
        probe_address == g_raw_page.raw.address ||
        g_raw_page.fault_probe_armed) {
        r0lab_unlock(flags);
        result = R0LAB_EAGAIN;
        goto record;
    }
    g_raw_page.fault_probe_address = probe_address;
    g_raw_page.fault_probe_reader_tgid = 0;
    g_raw_page.fault_probe_armed = true;
    g_raw_page.fault_probe_read_events = 0;
    g_raw_page.fault_probe_write_events = 0;
    g_raw_page.fault_probe_exec_events = 0;
    g_raw_page.fault_probe_failures = 0;
    r0lab_unlock(flags);

    snprintf(reply, sizeof(reply),
             "raw_fault_probe_ready symbol=handle_mm_fault installed=1 armed=1 page=%llx reader_tgid=0 target_mm_scoped=1 observe_only=1 pte_switch=0 source=normal_anon_remote_gup\n",
             (uint64_t)probe_address);
    return r0lab_copy_reply(out_msg, outlen, reply);

record:
    r0lab_record(R0LAB_EVENT_REJECT, result);
    return result;
}

static long r0lab_raw_fault_probe_reader(uint64_t token, uint64_t reader_value,
                                         char __user *out_msg, int outlen)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    unsigned long flags;
    unsigned long probe_address = 0;
    pid_t reader_tgid;
    int result = r0lab_validate_owner(token);

    if (result)
        goto record;
    if (!reader_value || reader_value > 0x7fffffffULL) {
        result = R0LAB_EINVAL;
        goto record;
    }
    reader_tgid = (pid_t)reader_value;

    flags = r0lab_lock();
    if (!g_raw_page.armed || g_raw_page.clearing ||
        g_raw_page.transitioning || !g_raw_page.fault_hook_installed ||
        !g_raw_page.fault_probe_armed || !g_raw_page.fault_probe_address ||
        g_raw_page.raw.state != R0LAB_RAW_SHADOW_RX ||
        g_session.owner_tgid != r0lab_current_tgid()) {
        r0lab_unlock(flags);
        result = R0LAB_EAGAIN;
        goto record;
    }
    g_raw_page.fault_probe_reader_tgid = reader_tgid;
    probe_address = g_raw_page.fault_probe_address;
    r0lab_unlock(flags);

    snprintf(reply, sizeof(reply),
             "raw_fault_probe_reader_ready reader_tgid=%d armed=1 page=%llx target_mm_scoped=1 remote_only=1 observe_only=1 pte_switch=0 source=normal_anon_remote_gup\n",
             reader_tgid, (uint64_t)probe_address);
    return r0lab_copy_reply(out_msg, outlen, reply);

record:
    r0lab_record(R0LAB_EVENT_REJECT, result);
    return result;
}

static long r0lab_raw_fault_probe_status(uint64_t token, char __user *out_msg,
                                          int outlen)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    unsigned long flags;
    unsigned long probe_address;
    bool hook_installed;
    bool armed;
    pid_t reader_tgid;
    uint32_t read_events;
    uint32_t write_events;
    uint32_t exec_events;
    uint32_t failures;
    int result = r0lab_validate_owner(token);

    if (result)
        return result;
    flags = r0lab_lock();
    hook_installed = g_raw_page.fault_hook_installed;
    armed = g_raw_page.fault_probe_armed;
    probe_address = g_raw_page.fault_probe_address;
    reader_tgid = g_raw_page.fault_probe_reader_tgid;
    read_events = g_raw_page.fault_probe_read_events;
    write_events = g_raw_page.fault_probe_write_events;
    exec_events = g_raw_page.fault_probe_exec_events;
    failures = g_raw_page.fault_probe_failures;
    r0lab_unlock(flags);

    snprintf(reply, sizeof(reply),
             "raw_fault_probe_status symbol=%s installed=%u armed=%u page=%llx reader_tgid=%d read_events=%u write_events=%u exec_events=%u hit_events=%u failures=%u target_mm_scoped=1 remote_only=1 observe_only=1 pte_switch=0 source=normal_anon_remote_gup\n",
             g_handle_mm_fault ? "handle_mm_fault" : "absent",
             hook_installed ? 1 : 0, armed ? 1 : 0,
             (uint64_t)probe_address, reader_tgid, read_events, write_events,
             exec_events, read_events + write_events + exec_events, failures);
    return r0lab_copy_reply(out_msg, outlen, reply);
}

static long r0lab_raw_fault_probe_clear(uint64_t token, char __user *out_msg,
                                         int outlen)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    unsigned long flags;
    unsigned long probe_address;
    bool armed;
    pid_t reader_tgid;
    uint32_t read_events;
    uint32_t write_events;
    uint32_t exec_events;
    uint32_t failures;
    int result = r0lab_validate_owner(token);

    if (result)
        return result;
    flags = r0lab_lock();
    armed = g_raw_page.fault_probe_armed;
    probe_address = g_raw_page.fault_probe_address;
    reader_tgid = g_raw_page.fault_probe_reader_tgid;
    g_raw_page.fault_probe_armed = false;
    g_raw_page.fault_probe_address = 0;
    g_raw_page.fault_probe_reader_tgid = 0;
    read_events = g_raw_page.fault_probe_read_events;
    write_events = g_raw_page.fault_probe_write_events;
    exec_events = g_raw_page.fault_probe_exec_events;
    failures = g_raw_page.fault_probe_failures;
    r0lab_unlock(flags);

    if (armed) {
        result = r0lab_raw_wait_for_callbacks();
        if (result)
            goto record;
    }
    snprintf(reply, sizeof(reply),
             "raw_fault_probe_cleared page=%llx reader_tgid=%d armed=0 read_events=%u write_events=%u exec_events=%u hit_events=%u failures=%u\n",
             (uint64_t)probe_address, reader_tgid, read_events, write_events,
             exec_events, read_events + write_events + exec_events, failures);
    return r0lab_copy_reply(out_msg, outlen, reply);

record:
    r0lab_record(R0LAB_EVENT_REJECT, result);
    return result;
}

static long r0lab_raw_fault_hook_clear_common(uint64_t token, uint16_t slot_id,
                                              char __user *out_msg, int outlen,
                                              bool legacy_reply)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    struct r0lab_raw_shadow_page *page;
    unsigned long flags;
    uint64_t generation;
    uint32_t read_events;
    uint32_t write_events;
    uint32_t exec_events;
    uint32_t failures;
    int result = r0lab_validate_owner(token);

    if (result)
        return result;
    if (slot_id >= R0LAB_RAW_PAGE_SLOT_CAPACITY)
        return R0LAB_EINVAL;
    flags = r0lab_lock();
    page = r0lab_raw_page_slot_locked(slot_id);
    if (!page) {
        r0lab_unlock(flags);
        return R0LAB_EINVAL;
    }
    generation = page->generation;
    read_events = page->fault_hook_read_events;
    write_events = page->fault_hook_write_events;
    exec_events = page->fault_hook_exec_events;
    failures = page->fault_hook_failures;
    r0lab_unlock(flags);
    r0lab_raw_fault_hook_release(page);
    if (legacy_reply)
        snprintf(reply, sizeof(reply),
                 "raw_fault_hook_cleared symbol=%s installed=0 read_events=%u write_events=%u exec_events=%u hit_events=%u failures=%u\n",
                 g_handle_mm_fault ? "handle_mm_fault" : "absent",
                 read_events, write_events, exec_events,
                 read_events + write_events + exec_events, failures);
    else
        snprintf(reply, sizeof(reply),
                 "raw_slot_fault_hook_cleared slot=%u generation=%llu symbol=%s installed=0 read_events=%u write_events=%u exec_events=%u hit_events=%u failures=%u page_record_routed=1\n",
                 (unsigned int)slot_id, (unsigned long long)generation,
                 g_handle_mm_fault ? "handle_mm_fault" : "absent",
                 read_events, write_events, exec_events,
                 read_events + write_events + exec_events, failures);
    return r0lab_copy_reply(out_msg, outlen, reply);
}

static long r0lab_raw_fault_hook_clear(uint64_t token, char __user *out_msg,
                                       int outlen)
{
    return r0lab_raw_fault_hook_clear_common(
        token, R0LAB_RAW_PRIMARY_SLOT, out_msg, outlen, true);
}

static long r0lab_raw_syscall_hook_arm_common(uint64_t token,
                                              uint16_t slot_id,
                                              uint64_t generation,
                                              char __user *out_msg,
                                              int outlen,
                                              bool read_cycle_mode,
                                              bool legacy_reply)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    struct r0lab_raw_shadow_page *page;
    unsigned long flags;
    bool installed;
    bool should_install;
    bool existing_read_cycle_mode;
    int result = r0lab_validate_owner(token);

    if (result)
        goto record;
    if (!g_sys_getpid) {
        result = R0LAB_ENOSYS;
        goto record;
    }
    if (slot_id >= R0LAB_RAW_PAGE_SLOT_CAPACITY) {
        result = R0LAB_EINVAL;
        goto record;
    }

    flags = r0lab_lock();
    page = r0lab_raw_page_slot_locked(slot_id);
    if (!page || !page->armed || page->clearing ||
        page->transitioning || page->raw.state != R0LAB_RAW_SHADOW_RX ||
        page->raw.gup_hide_active || page->raw.fork_hide_active ||
        page->raw.read_cycle_active ||
        (!legacy_reply && page->generation != generation) ||
        g_session.owner_tgid != r0lab_current_tgid()) {
        r0lab_unlock(flags);
        result = R0LAB_EAGAIN;
        goto record;
    }
    installed = page->syscall_hook_installed;
    should_install = !installed && r0lab_raw_syscall_hook_users_locked() == 0;
    existing_read_cycle_mode = page->syscall_hook_read_cycle_mode;
    if (r0lab_raw_prctl_hook_users_locked()) {
        r0lab_unlock(flags);
        result = R0LAB_EBUSY;
        goto record;
    }
    if (installed && existing_read_cycle_mode != read_cycle_mode) {
        r0lab_unlock(flags);
        result = R0LAB_EBUSY;
        goto record;
    }
    generation = page->generation;
    if (!installed) {
        page->syscall_hook_events = 0;
        page->syscall_hook_read_cycle_events = 0;
        page->syscall_hook_failures = 0;
    }
    r0lab_unlock(flags);

    if (should_install) {
        result = hook_wrap1(g_sys_getpid, r0lab_raw_syscall_before,
                            NULL, NULL);
        if (result)
            goto record;
    }
    if (!installed) {
        flags = r0lab_lock();
        page = r0lab_raw_page_slot_locked(slot_id);
        if (page && page->armed && !page->clearing &&
            page->generation == generation &&
            page->raw.state == R0LAB_RAW_SHADOW_RX) {
            page->syscall_hook_installed = true;
            page->syscall_hook_read_cycle_mode = read_cycle_mode;
            g_raw_page_table.selected_slot = slot_id;
        } else {
            result = R0LAB_EAGAIN;
        }
        r0lab_unlock(flags);
        if (result && should_install) {
            r0lab_hook_detach(g_sys_getpid, r0lab_raw_syscall_before, NULL);
            goto record;
        }
    } else {
        flags = r0lab_lock();
        if (page && page->generation == generation)
            g_raw_page_table.selected_slot = slot_id;
        r0lab_unlock(flags);
    }

    if (read_cycle_mode && legacy_reply)
        snprintf(reply, sizeof(reply),
                 "raw_syscall_read_cycle_hook_ready symbol=getpid installed=1 target_mm_scoped=1 trigger=syscall_getpid read_cycle=uxn_original_exec_resume observe_only=0 pte_switch=1 data_fault=absent exec_resume=pending\n");
    else if (read_cycle_mode)
        snprintf(reply, sizeof(reply),
                 "raw_slot_syscall_read_cycle_hook_ready slot=%u generation=%llu symbol=getpid installed=1 selected=1 target_mm_scoped=1 page_record_routed=1 trigger=syscall_getpid read_cycle=uxn_original_exec_resume observe_only=0 pte_switch=1 data_fault=absent exec_resume=pending route=selected_slot\n",
                 (unsigned int)slot_id, (unsigned long long)generation);
    else if (legacy_reply)
        snprintf(reply, sizeof(reply),
                 "raw_syscall_hook_ready symbol=getpid installed=1 target_mm_scoped=1 trigger=syscall_getpid observe_only=1 pte_switch=0\n");
    else
        snprintf(reply, sizeof(reply),
                 "raw_slot_syscall_hook_ready slot=%u generation=%llu symbol=getpid installed=1 selected=1 target_mm_scoped=1 page_record_routed=1 trigger=syscall_getpid observe_only=1 pte_switch=0 route=selected_slot\n",
                 (unsigned int)slot_id, (unsigned long long)generation);
    return r0lab_copy_reply(out_msg, outlen, reply);

record:
    r0lab_record(R0LAB_EVENT_REJECT, result);
    return result;
}

static long r0lab_raw_syscall_hook_arm(uint64_t token, char __user *out_msg,
                                       int outlen)
{
    return r0lab_raw_syscall_hook_arm_common(
        token, R0LAB_RAW_PRIMARY_SLOT, 0, out_msg, outlen, false, true);
}

static long r0lab_raw_syscall_read_cycle_hook_arm(uint64_t token,
                                                  char __user *out_msg,
                                                  int outlen)
{
    return r0lab_raw_syscall_hook_arm_common(
        token, R0LAB_RAW_PRIMARY_SLOT, 0, out_msg, outlen, true, true);
}

static long r0lab_raw_syscall_hook_select(uint64_t token, uint16_t slot_id,
                                          uint64_t generation,
                                          char __user *out_msg, int outlen)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    struct r0lab_raw_shadow_page *page;
    unsigned long flags;
    bool read_cycle_mode;
    int result = r0lab_validate_owner(token);

    if (result)
        goto record;
    if (slot_id >= R0LAB_RAW_PAGE_SLOT_CAPACITY) {
        result = R0LAB_EINVAL;
        goto record;
    }
    flags = r0lab_lock();
    page = r0lab_raw_page_slot_locked(slot_id);
    if (!page || !page->syscall_hook_installed ||
        page->generation != generation || !page->armed ||
        page->clearing || page->raw.state != R0LAB_RAW_SHADOW_RX) {
        r0lab_unlock(flags);
        result = R0LAB_EAGAIN;
        goto record;
    }
    read_cycle_mode = page->syscall_hook_read_cycle_mode;
    g_raw_page_table.selected_slot = slot_id;
    r0lab_unlock(flags);

    if (read_cycle_mode)
        snprintf(reply, sizeof(reply),
                 "raw_slot_syscall_read_cycle_hook_selected slot=%u generation=%llu symbol=getpid selected=1 target_mm_scoped=1 page_record_routed=1 trigger=syscall_getpid read_cycle=uxn_original_exec_resume route=selected_slot\n",
                 (unsigned int)slot_id, (unsigned long long)generation);
    else
        snprintf(reply, sizeof(reply),
                 "raw_slot_syscall_hook_selected slot=%u generation=%llu symbol=getpid selected=1 target_mm_scoped=1 page_record_routed=1 trigger=syscall_getpid route=selected_slot\n",
                 (unsigned int)slot_id, (unsigned long long)generation);
    return r0lab_copy_reply(out_msg, outlen, reply);

record:
    r0lab_record(R0LAB_EVENT_REJECT, result);
    return result;
}

static long r0lab_raw_syscall_hook_status_common(uint64_t token,
                                                 uint16_t slot_id,
                                                 char __user *out_msg,
                                                 int outlen,
                                                 bool legacy_reply)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    struct r0lab_raw_shadow_page *page;
    unsigned long flags;
    bool installed;
    bool read_cycle_mode;
    bool selected;
    uint64_t generation;
    uint32_t hit_events;
    uint32_t read_cycle_events;
    unsigned long read_cycle_finish_events;
    uint32_t failures;
    uint32_t route_hits;
    uint32_t route_rejects;
    int result = r0lab_validate_owner(token);

    if (result)
        return result;
    if (slot_id >= R0LAB_RAW_PAGE_SLOT_CAPACITY)
        return R0LAB_EINVAL;
    flags = r0lab_lock();
    page = r0lab_raw_page_slot_locked(slot_id);
    if (!page) {
        r0lab_unlock(flags);
        return R0LAB_EINVAL;
    }
    installed = page->syscall_hook_installed;
    read_cycle_mode = page->syscall_hook_read_cycle_mode;
    selected = g_raw_page_table.selected_slot == slot_id;
    generation = page->generation;
    hit_events = page->syscall_hook_events;
    read_cycle_events = page->syscall_hook_read_cycle_events;
    read_cycle_finish_events = page->raw.read_cycle_finish_events;
    failures = page->syscall_hook_failures;
    route_hits = page->hook_route_stats.route_hits;
    route_rejects = page->hook_route_stats.route_rejects;
    r0lab_unlock(flags);

    if (read_cycle_mode && legacy_reply)
        snprintf(reply, sizeof(reply),
                 "raw_syscall_hook_status symbol=%s installed=%u hit_events=%u read_cycle_events=%u failures=%u target_mm_scoped=1 trigger=syscall_getpid read_cycle=uxn_original_exec_resume observe_only=0 pte_switch=1 data_fault=absent exec_resume=%s\n",
                 g_sys_getpid ? "getpid" : "absent", installed ? 1 : 0,
                 hit_events, read_cycle_events, failures,
                 read_cycle_finish_events ? "proven" :
                 (read_cycle_events ? "pending" : "absent"));
    else if (read_cycle_mode)
        snprintf(reply, sizeof(reply),
                 "raw_slot_syscall_hook_status slot=%u generation=%llu symbol=%s installed=%u selected=%u hit_events=%u read_cycle_events=%u failures=%u target_mm_scoped=1 page_record_routed=1 trigger=syscall_getpid read_cycle=uxn_original_exec_resume observe_only=0 pte_switch=1 data_fault=absent exec_resume=%s route_hits=%u route_rejects=%u route=selected_slot\n",
                 (unsigned int)slot_id, (unsigned long long)generation,
                 g_sys_getpid ? "getpid" : "absent", installed ? 1 : 0,
                 selected ? 1 : 0, hit_events, read_cycle_events, failures,
                 read_cycle_finish_events ? "proven" :
                 (read_cycle_events ? "pending" : "absent"), route_hits,
                 route_rejects);
    else if (legacy_reply)
        snprintf(reply, sizeof(reply),
                 "raw_syscall_hook_status symbol=%s installed=%u hit_events=%u failures=%u target_mm_scoped=1 trigger=syscall_getpid observe_only=1 pte_switch=0\n",
                 g_sys_getpid ? "getpid" : "absent", installed ? 1 : 0,
                 hit_events, failures);
    else
        snprintf(reply, sizeof(reply),
                 "raw_slot_syscall_hook_status slot=%u generation=%llu symbol=%s installed=%u selected=%u hit_events=%u failures=%u target_mm_scoped=1 page_record_routed=1 trigger=syscall_getpid observe_only=1 pte_switch=0 route_hits=%u route_rejects=%u route=selected_slot\n",
                 (unsigned int)slot_id, (unsigned long long)generation,
                 g_sys_getpid ? "getpid" : "absent", installed ? 1 : 0,
                 selected ? 1 : 0, hit_events, failures, route_hits,
                 route_rejects);
    return r0lab_copy_reply(out_msg, outlen, reply);
}

static long r0lab_raw_syscall_hook_status(uint64_t token,
                                          char __user *out_msg, int outlen)
{
    return r0lab_raw_syscall_hook_status_common(
        token, R0LAB_RAW_PRIMARY_SLOT, out_msg, outlen, true);
}

static long r0lab_raw_syscall_hook_clear_common(uint64_t token,
                                                uint16_t slot_id,
                                                uint64_t generation,
                                                char __user *out_msg,
                                                int outlen,
                                                bool legacy_reply)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    struct r0lab_raw_shadow_page *page;
    unsigned long flags;
    uint32_t hit_events;
    uint32_t read_cycle_events;
    unsigned long read_cycle_finish_events;
    uint32_t failures;
    int result = r0lab_validate_owner(token);

    if (result)
        return result;
    if (slot_id >= R0LAB_RAW_PAGE_SLOT_CAPACITY)
        return R0LAB_EINVAL;
    flags = r0lab_lock();
    page = r0lab_raw_page_slot_locked(slot_id);
    if (!page || (!legacy_reply && page->generation != generation)) {
        r0lab_unlock(flags);
        return R0LAB_EAGAIN;
    }
    r0lab_unlock(flags);

    if (legacy_reply)
        r0lab_raw_syscall_unhook();
    else
        r0lab_raw_syscall_hook_release(page);

    flags = r0lab_lock();
    page = r0lab_raw_page_slot_locked(slot_id);
    if (!page) {
        r0lab_unlock(flags);
        return R0LAB_EINVAL;
    }
    generation = page->generation;
    hit_events = page->syscall_hook_events;
    read_cycle_events = page->syscall_hook_read_cycle_events;
    read_cycle_finish_events = page->raw.read_cycle_finish_events;
    failures = page->syscall_hook_failures;
    r0lab_unlock(flags);
    if (legacy_reply)
        snprintf(reply, sizeof(reply),
                 "raw_syscall_hook_cleared symbol=%s installed=0 hit_events=%u read_cycle_events=%u failures=%u\n",
                 g_sys_getpid ? "getpid" : "absent", hit_events,
                 read_cycle_events, failures);
    else
        snprintf(reply, sizeof(reply),
                 "raw_slot_syscall_hook_cleared slot=%u generation=%llu symbol=%s installed=0 hit_events=%u read_cycle_events=%u read_cycle_finish_events=%lu failures=%u page_record_routed=1 trigger=syscall_getpid route=selected_slot\n",
                 (unsigned int)slot_id, (unsigned long long)generation,
                 g_sys_getpid ? "getpid" : "absent", hit_events,
                 read_cycle_events, read_cycle_finish_events, failures);
    return r0lab_copy_reply(out_msg, outlen, reply);
}

static long r0lab_raw_syscall_hook_clear(uint64_t token, char __user *out_msg,
                                         int outlen)
{
    return r0lab_raw_syscall_hook_clear_common(
        token, R0LAB_RAW_PRIMARY_SLOT, 0, out_msg, outlen, true);
}

static long r0lab_raw_prctl_hook_arm_common(uint64_t token,
                                            uint16_t slot_id,
                                            uint64_t generation,
                                            char __user *out_msg,
                                            int outlen,
                                            bool legacy_reply)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    struct r0lab_raw_shadow_page *page;
    unsigned long flags;
    bool installed;
    bool should_install;
    int result = r0lab_validate_owner(token);

    if (result)
        goto record;
    if (!g_sys_prctl || !g_sync_icache_aliases) {
        result = R0LAB_ENOSYS;
        goto record;
    }
    if (slot_id >= R0LAB_RAW_PAGE_SLOT_CAPACITY) {
        result = R0LAB_EINVAL;
        goto record;
    }

    flags = r0lab_lock();
    page = r0lab_raw_page_slot_locked(slot_id);
    if (!page || !page->armed || page->clearing ||
        page->transitioning || page->raw.state != R0LAB_RAW_SHADOW_RX ||
        page->raw.gup_hide_active || page->raw.fork_hide_active ||
        page->raw.read_cycle_active ||
        (!legacy_reply && page->generation != generation) ||
        g_session.owner_tgid != r0lab_current_tgid()) {
        r0lab_unlock(flags);
        result = R0LAB_EAGAIN;
        goto record;
    }
    if (r0lab_raw_syscall_hook_users_locked()) {
        r0lab_unlock(flags);
        result = R0LAB_EBUSY;
        goto record;
    }
    installed = page->prctl_hook_installed;
    should_install = !installed && r0lab_raw_prctl_hook_users_locked() == 0;
    generation = page->generation;
    if (!installed) {
        page->prctl_hook_events = 0;
        page->prctl_hook_read_cycle_events = 0;
        page->prctl_hook_patch_events = 0;
        page->prctl_hook_release_events = 0;
        page->prctl_hook_reject_events = 0;
        page->prctl_hook_failures = 0;
    }
    r0lab_unlock(flags);

    if (should_install) {
        result = hook_wrap1(g_sys_prctl, r0lab_raw_prctl_before, NULL, NULL);
        if (result)
            goto record;
    }
    if (!installed) {
        flags = r0lab_lock();
        page = r0lab_raw_page_slot_locked(slot_id);
        if (page && page->armed && !page->clearing &&
            page->generation == generation &&
            page->raw.state == R0LAB_RAW_SHADOW_RX &&
            !r0lab_raw_syscall_hook_users_locked()) {
            page->prctl_hook_installed = true;
            g_raw_page_table.selected_slot = slot_id;
        } else {
            result = R0LAB_EAGAIN;
        }
        r0lab_unlock(flags);
        if (result && should_install) {
            r0lab_hook_detach(g_sys_prctl, r0lab_raw_prctl_before, NULL);
            goto record;
        }
    } else {
        flags = r0lab_lock();
        if (page && page->generation == generation)
            g_raw_page_table.selected_slot = slot_id;
        r0lab_unlock(flags);
    }

    if (legacy_reply)
        snprintf(reply, sizeof(reply),
                 "raw_prctl_hook_ready symbol=prctl installed=1 abi=prctl_magic option=0x%x operations=%u,%u,%u,%u,%u read_cycle_op=%u patch_word_op=%u release_patch_op=%u patch_range_op=%u release_range_op=%u token_arg=2 operation_arg=3 request_arg=4 request_size_arg=5 lab_uid_scoped=1 target_mm_scoped=1 token_scoped=1 patch_scope=single_page_ranges patch_capacity=%u overlap=version_last_write_wins rebuild=original_seed_active_records release=exact_start dirty_tracking=byte_bitmap cache_sync=sync_icache_aliases user_copy=%s read_cycle=uxn_original_exec_resume observe_only=0 pte_switch=1 data_fault=absent exec_resume=pending passthrough=nonmagic\n",
                 R0LAB_PRCTL_MAGIC, R0LAB_PRCTL_OP_READ_CYCLE,
                 R0LAB_PRCTL_OP_PATCH_WORD,
                 R0LAB_PRCTL_OP_RELEASE_PATCH,
                 R0LAB_PRCTL_OP_PATCH_RANGE,
                 R0LAB_PRCTL_OP_RELEASE_RANGE,
                 R0LAB_PRCTL_OP_READ_CYCLE,
                 R0LAB_PRCTL_OP_PATCH_WORD,
                 R0LAB_PRCTL_OP_RELEASE_PATCH,
                 R0LAB_PRCTL_OP_PATCH_RANGE,
                 R0LAB_PRCTL_OP_RELEASE_RANGE,
                 R0LAB_PATCH_RECORD_CAPACITY,
                 g_copy_from_user_nofault ? "copy_from_user_nofault" :
                 "absent");
    else
        snprintf(reply, sizeof(reply),
                 "raw_slot_prctl_hook_ready slot=%u generation=%llu symbol=prctl installed=1 selected=1 page_record_routed=1 abi=prctl_magic option=0x%x operations=%u,%u,%u,%u,%u read_cycle_op=%u patch_word_op=%u release_patch_op=%u patch_range_op=%u release_range_op=%u token_arg=2 operation_arg=3 request_arg=4 request_size_arg=5 lab_uid_scoped=1 target_mm_scoped=1 token_scoped=1 patch_scope=single_page_ranges patch_capacity=%u overlap=version_last_write_wins rebuild=original_seed_active_records release=exact_start dirty_tracking=byte_bitmap cache_sync=sync_icache_aliases user_copy=%s read_cycle=uxn_original_exec_resume observe_only=0 pte_switch=1 data_fault=absent exec_resume=pending route=selected_slot,address passthrough=nonmagic\n",
                 (unsigned int)slot_id, (unsigned long long)generation,
                 R0LAB_PRCTL_MAGIC, R0LAB_PRCTL_OP_READ_CYCLE,
                 R0LAB_PRCTL_OP_PATCH_WORD,
                 R0LAB_PRCTL_OP_RELEASE_PATCH,
                 R0LAB_PRCTL_OP_PATCH_RANGE,
                 R0LAB_PRCTL_OP_RELEASE_RANGE,
                 R0LAB_PRCTL_OP_READ_CYCLE,
                 R0LAB_PRCTL_OP_PATCH_WORD,
                 R0LAB_PRCTL_OP_RELEASE_PATCH,
                 R0LAB_PRCTL_OP_PATCH_RANGE,
                 R0LAB_PRCTL_OP_RELEASE_RANGE,
                 R0LAB_PATCH_RECORD_CAPACITY,
                 g_copy_from_user_nofault ? "copy_from_user_nofault" :
                 "absent");
    return r0lab_copy_reply(out_msg, outlen, reply);

record:
    r0lab_record(R0LAB_EVENT_REJECT, result);
    return result;
}

static long r0lab_raw_prctl_hook_arm(uint64_t token, char __user *out_msg,
                                     int outlen)
{
    return r0lab_raw_prctl_hook_arm_common(
        token, R0LAB_RAW_PRIMARY_SLOT, 0, out_msg, outlen, true);
}

static long r0lab_raw_prctl_hook_select(uint64_t token, uint16_t slot_id,
                                        uint64_t generation,
                                        char __user *out_msg, int outlen)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    struct r0lab_raw_shadow_page *page;
    unsigned long flags;
    int result = r0lab_validate_owner(token);

    if (result)
        goto record;
    if (slot_id >= R0LAB_RAW_PAGE_SLOT_CAPACITY) {
        result = R0LAB_EINVAL;
        goto record;
    }
    flags = r0lab_lock();
    page = r0lab_raw_page_slot_locked(slot_id);
    if (!page || !page->prctl_hook_installed ||
        page->generation != generation || !page->armed ||
        page->clearing || page->raw.state != R0LAB_RAW_SHADOW_RX) {
        r0lab_unlock(flags);
        result = R0LAB_EAGAIN;
        goto record;
    }
    g_raw_page_table.selected_slot = slot_id;
    r0lab_unlock(flags);

    snprintf(reply, sizeof(reply),
             "raw_slot_prctl_hook_selected slot=%u generation=%llu symbol=prctl selected=1 page_record_routed=1 route=selected_slot\n",
             (unsigned int)slot_id, (unsigned long long)generation);
    return r0lab_copy_reply(out_msg, outlen, reply);

record:
    r0lab_record(R0LAB_EVENT_REJECT, result);
    return result;
}

static long r0lab_raw_prctl_hook_status_common(uint64_t token,
                                               uint16_t slot_id,
                                               char __user *out_msg,
                                               int outlen,
                                               bool legacy_reply)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    struct r0lab_raw_shadow_page *page;
    unsigned long flags;
    bool installed;
    bool selected;
    uint64_t generation;
    uint32_t hit_events;
    uint32_t read_cycle_events;
    uint32_t patch_events;
    uint32_t release_events;
    uint32_t reject_events;
    uint32_t failures;
    bool patch_active;
    unsigned long patch_address;
    uint32_t patch_word;
    uint16_t patch_record_slots;
    uint16_t patch_active_count;
    uint16_t patch_dirty_bytes;
    uint64_t patch_version;
    unsigned long read_cycle_finish_events;
    uint32_t route_hits;
    uint32_t route_rejects;
    unsigned int index;
    int result = r0lab_validate_owner(token);

    if (result)
        return result;
    if (slot_id >= R0LAB_RAW_PAGE_SLOT_CAPACITY)
        return R0LAB_EINVAL;
    flags = r0lab_lock();
    page = r0lab_raw_page_slot_locked(slot_id);
    if (!page) {
        r0lab_unlock(flags);
        return R0LAB_EINVAL;
    }
    installed = page->prctl_hook_installed;
    selected = g_raw_page_table.selected_slot == slot_id;
    generation = page->generation;
    hit_events = page->prctl_hook_events;
    read_cycle_events = page->prctl_hook_read_cycle_events;
    patch_events = page->prctl_hook_patch_events;
    release_events = page->prctl_hook_release_events;
    reject_events = page->prctl_hook_reject_events;
    failures = page->prctl_hook_failures;
    patch_record_slots = page->patch_record_slots;
    patch_active_count = page->patch_active_count;
    patch_dirty_bytes = page->patch_dirty_bytes;
    patch_version = page->patch_version;
    patch_active = patch_active_count != 0;
    patch_address = 0;
    patch_word = 0;
    for (index = 0; index < patch_record_slots; ++index) {
        const struct r0lab_patch_record *record = &page->patch_records[index];

        if (!record->active)
            continue;
        patch_address = page->raw.address + record->offset;
        if (record->data && record->length >= sizeof(patch_word))
            memcpy(&patch_word, record->data, sizeof(patch_word));
        break;
    }
    read_cycle_finish_events = page->raw.read_cycle_finish_events;
    route_hits = page->hook_route_stats.route_hits;
    route_rejects = page->hook_route_stats.route_rejects;
    r0lab_unlock(flags);

    if (legacy_reply)
        snprintf(reply, sizeof(reply),
                 "raw_prctl_hook_status symbol=%s installed=%u abi=prctl_magic option=0x%x operations=%u,%u,%u,%u,%u hit_events=%u read_cycle_events=%u patch_events=%u release_events=%u reject_events=%u failures=%u patch_active=%u patch_address=%lx patch_word=%08x patch_record_slots=%u patch_active_count=%u patch_dirty_bytes=%u patch_version=%llu patch_capacity=%u overlap=version_last_write_wins rebuild=original_seed_active_records release=exact_start dirty_tracking=byte_bitmap lab_uid_scoped=1 target_mm_scoped=1 token_scoped=1 patch_scope=single_page_ranges cache_sync=sync_icache_aliases user_copy=%s read_cycle=uxn_original_exec_resume observe_only=0 pte_switch=1 data_fault=absent exec_resume=%s passthrough=nonmagic\n",
                 g_sys_prctl ? "prctl" : "absent", installed ? 1 : 0,
                 R0LAB_PRCTL_MAGIC, R0LAB_PRCTL_OP_READ_CYCLE,
                 R0LAB_PRCTL_OP_PATCH_WORD,
                 R0LAB_PRCTL_OP_RELEASE_PATCH,
                 R0LAB_PRCTL_OP_PATCH_RANGE,
                 R0LAB_PRCTL_OP_RELEASE_RANGE,
                 hit_events, read_cycle_events, patch_events, release_events,
                 reject_events, failures, patch_active ? 1 : 0,
                 patch_address, patch_word, patch_record_slots,
                 patch_active_count, patch_dirty_bytes, patch_version,
                 R0LAB_PATCH_RECORD_CAPACITY,
                 g_copy_from_user_nofault ? "copy_from_user_nofault" :
                 "absent",
                 read_cycle_finish_events ? "proven" :
                 (read_cycle_events ? "pending" : "absent"));
    else
        snprintf(reply, sizeof(reply),
                 "raw_slot_prctl_hook_status slot=%u generation=%llu symbol=%s installed=%u selected=%u hit_events=%u read_cycle_events=%u patch_events=%u release_events=%u reject_events=%u failures=%u page_record_routed=1 abi=prctl_magic option=0x%x operations=%u,%u,%u,%u,%u patch_active=%u patch_address=%lx patch_word=%08x patch_record_slots=%u patch_active_count=%u patch_dirty_bytes=%u patch_version=%llu patch_capacity=%u overlap=version_last_write_wins rebuild=original_seed_active_records release=exact_start dirty_tracking=byte_bitmap lab_uid_scoped=1 target_mm_scoped=1 token_scoped=1 patch_scope=single_page_ranges cache_sync=sync_icache_aliases user_copy=%s read_cycle=uxn_original_exec_resume observe_only=0 pte_switch=1 data_fault=absent exec_resume=%s route_hits=%u route_rejects=%u route=selected_slot,address passthrough=nonmagic\n",
                 (unsigned int)slot_id, (unsigned long long)generation,
                 g_sys_prctl ? "prctl" : "absent", installed ? 1 : 0,
                 selected ? 1 : 0, hit_events, read_cycle_events,
                 patch_events, release_events, reject_events, failures,
                 R0LAB_PRCTL_MAGIC, R0LAB_PRCTL_OP_READ_CYCLE,
                 R0LAB_PRCTL_OP_PATCH_WORD,
                 R0LAB_PRCTL_OP_RELEASE_PATCH,
                 R0LAB_PRCTL_OP_PATCH_RANGE,
                 R0LAB_PRCTL_OP_RELEASE_RANGE,
                 patch_active ? 1 : 0, patch_address, patch_word,
                 patch_record_slots, patch_active_count, patch_dirty_bytes,
                 patch_version, R0LAB_PATCH_RECORD_CAPACITY,
                 g_copy_from_user_nofault ? "copy_from_user_nofault" :
                 "absent",
                 read_cycle_finish_events ? "proven" :
                 (read_cycle_events ? "pending" : "absent"), route_hits,
                 route_rejects);
    return r0lab_copy_reply(out_msg, outlen, reply);
}

static long r0lab_raw_prctl_hook_status(uint64_t token, char __user *out_msg,
                                        int outlen)
{
    return r0lab_raw_prctl_hook_status_common(
        token, R0LAB_RAW_PRIMARY_SLOT, out_msg, outlen, true);
}

static long r0lab_raw_prctl_hook_clear_common(uint64_t token,
                                              uint16_t slot_id,
                                              uint64_t generation,
                                              char __user *out_msg,
                                              int outlen,
                                              bool legacy_reply)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    struct r0lab_raw_shadow_page *page;
    unsigned long flags;
    uint32_t hit_events;
    uint32_t read_cycle_events;
    uint32_t patch_events;
    uint32_t release_events;
    uint32_t reject_events;
    uint32_t failures;
    bool patch_active;
    uint16_t patch_record_slots;
    uint16_t patch_active_count;
    uint16_t patch_dirty_bytes;
    int result = r0lab_validate_owner(token);

    if (result)
        return result;
    if (slot_id >= R0LAB_RAW_PAGE_SLOT_CAPACITY)
        return R0LAB_EINVAL;
    flags = r0lab_lock();
    page = r0lab_raw_page_slot_locked(slot_id);
    if (!page || (!legacy_reply && page->generation != generation)) {
        r0lab_unlock(flags);
        return R0LAB_EAGAIN;
    }
    r0lab_unlock(flags);

    if (legacy_reply)
        r0lab_raw_prctl_unhook();
    else
        r0lab_raw_prctl_hook_release(page);

    flags = r0lab_lock();
    page = r0lab_raw_page_slot_locked(slot_id);
    if (!page) {
        r0lab_unlock(flags);
        return R0LAB_EINVAL;
    }
    generation = page->generation;
    hit_events = page->prctl_hook_events;
    read_cycle_events = page->prctl_hook_read_cycle_events;
    patch_events = page->prctl_hook_patch_events;
    release_events = page->prctl_hook_release_events;
    reject_events = page->prctl_hook_reject_events;
    failures = page->prctl_hook_failures;
    patch_record_slots = page->patch_record_slots;
    patch_active_count = page->patch_active_count;
    patch_dirty_bytes = page->patch_dirty_bytes;
    patch_active = patch_active_count != 0;
    r0lab_unlock(flags);
    if (legacy_reply)
        snprintf(reply, sizeof(reply),
                 "raw_prctl_hook_cleared symbol=%s installed=0 hit_events=%u read_cycle_events=%u patch_events=%u release_events=%u reject_events=%u failures=%u patch_active=%u patch_record_slots=%u patch_active_count=%u patch_dirty_bytes=%u\n",
                 g_sys_prctl ? "prctl" : "absent", hit_events,
                 read_cycle_events, patch_events, release_events,
                 reject_events, failures, patch_active ? 1 : 0,
                 patch_record_slots, patch_active_count, patch_dirty_bytes);
    else
        snprintf(reply, sizeof(reply),
                 "raw_slot_prctl_hook_cleared slot=%u generation=%llu symbol=%s installed=0 hit_events=%u read_cycle_events=%u patch_events=%u release_events=%u reject_events=%u failures=%u page_record_routed=1 patch_active=%u patch_record_slots=%u patch_active_count=%u patch_dirty_bytes=%u route=selected_slot,address\n",
                 (unsigned int)slot_id, (unsigned long long)generation,
                 g_sys_prctl ? "prctl" : "absent", hit_events,
                 read_cycle_events, patch_events, release_events,
                 reject_events, failures, patch_active ? 1 : 0,
                 patch_record_slots, patch_active_count, patch_dirty_bytes);
    return r0lab_copy_reply(out_msg, outlen, reply);
}

static long r0lab_raw_prctl_hook_clear(uint64_t token, char __user *out_msg,
                                       int outlen)
{
    return r0lab_raw_prctl_hook_clear_common(
        token, R0LAB_RAW_PRIMARY_SLOT, 0, out_msg, outlen, true);
}

static long r0lab_raw_exit_hook_arm_common(uint64_t token, uint16_t slot_id,
                                           uint64_t generation,
                                           char __user *out_msg,
                                           int outlen,
                                           bool legacy_reply)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    struct r0lab_raw_shadow_page *page;
    unsigned long flags;
    int result = r0lab_validate_owner(token);

    if (result)
        goto record;
    if (!g_exit_mmap) {
        result = R0LAB_ENOSYS;
        goto record;
    }
    if (slot_id >= R0LAB_RAW_PAGE_SLOT_CAPACITY) {
        result = R0LAB_EINVAL;
        goto record;
    }

    flags = r0lab_lock();
    page = r0lab_raw_page_slot_locked(slot_id);
    if (!page || !page->armed || page->clearing ||
        page->transitioning || page->raw.state != R0LAB_RAW_SHADOW_RX ||
        (!legacy_reply && page->generation != generation) ||
        g_session.owner_tgid != r0lab_current_tgid()) {
        r0lab_unlock(flags);
        result = R0LAB_EAGAIN;
        goto record;
    }
    generation = page->generation;
    r0lab_unlock(flags);

    result = r0lab_raw_exit_hook_acquire(page);
    if (result)
        goto record;

    if (legacy_reply)
        snprintf(reply, sizeof(reply),
                 "raw_exit_hook_ready symbol=exit_mmap installed=1 target_mm_scoped=1 observe_only=1 pte_switch=0 cleanup=monitor\n");
    else
        snprintf(reply, sizeof(reply),
                 "raw_slot_exit_hook_ready slot=%u generation=%llu symbol=exit_mmap installed=1 target_mm_scoped=1 page_record_routed=1 observe_only=0 cleanup=monitor\n",
                 (unsigned int)slot_id, (unsigned long long)generation);
    return r0lab_copy_reply(out_msg, outlen, reply);

record:
    r0lab_record(R0LAB_EVENT_REJECT, result);
    return result;
}

static long r0lab_raw_exit_hook_arm(uint64_t token, char __user *out_msg,
                                    int outlen)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    unsigned long flags;
    bool installed;
    int result = r0lab_validate_owner(token);

    if (result)
        goto record;
    if (!g_exit_mmap) {
        result = R0LAB_ENOSYS;
        goto record;
    }

    flags = r0lab_lock();
    if (!g_raw_page.armed || g_raw_page.clearing ||
        g_raw_page.transitioning ||
        g_raw_page.raw.state != R0LAB_RAW_SHADOW_RX ||
        g_session.owner_tgid != r0lab_current_tgid()) {
        r0lab_unlock(flags);
        result = R0LAB_EAGAIN;
        goto record;
    }
    installed = g_raw_page.exit_hook_installed;
    if (!installed) {
        g_raw_page.exit_hook_events = 0;
        g_raw_page.exit_hook_failures = 0;
    }
    r0lab_unlock(flags);

    if (!installed) {
        result = hook_wrap1(g_exit_mmap, r0lab_raw_exit_mmap_before,
                            NULL, NULL);
        if (result)
            goto record;
        flags = r0lab_lock();
        if (g_raw_page.armed && !g_raw_page.clearing &&
            g_raw_page.raw.state == R0LAB_RAW_SHADOW_RX) {
            g_raw_page.exit_hook_installed = true;
        } else {
            result = R0LAB_EAGAIN;
        }
        r0lab_unlock(flags);
        if (result) {
            r0lab_hook_detach(g_exit_mmap, r0lab_raw_exit_mmap_before, NULL);
            goto record;
        }
    }

    snprintf(reply, sizeof(reply),
             "raw_exit_hook_ready symbol=exit_mmap installed=1 target_mm_scoped=1 observe_only=1 pte_switch=0 cleanup=monitor\n");
    return r0lab_copy_reply(out_msg, outlen, reply);

record:
    r0lab_record(R0LAB_EVENT_REJECT, result);
    return result;
}

static long r0lab_raw_exit_hook_status_common(uint64_t token,
                                              uint16_t slot_id,
                                              char __user *out_msg,
                                              int outlen,
                                              bool legacy_reply)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    struct r0lab_raw_shadow_page *page;
    unsigned long flags;
    bool installed;
    uint64_t generation;
    uint32_t hit_events;
    uint32_t failures;
    int result = r0lab_validate_owner(token);

    if (result)
        return result;
    if (slot_id >= R0LAB_RAW_PAGE_SLOT_CAPACITY)
        return R0LAB_EINVAL;
    flags = r0lab_lock();
    page = r0lab_raw_page_slot_locked(slot_id);
    if (!page) {
        r0lab_unlock(flags);
        return R0LAB_EINVAL;
    }
    installed = page->exit_hook_installed;
    generation = page->generation;
    hit_events = page->exit_hook_events;
    failures = page->exit_hook_failures;
    r0lab_unlock(flags);

    if (legacy_reply)
        snprintf(reply, sizeof(reply),
                 "raw_exit_hook_status symbol=%s installed=%u hit_events=%u failures=%u target_mm_scoped=1 observe_only=1 pte_switch=0 cleanup=monitor\n",
                 g_exit_mmap ? "exit_mmap" : "absent", installed ? 1 : 0,
                 hit_events, failures);
    else
        snprintf(reply, sizeof(reply),
                 "raw_slot_exit_hook_status slot=%u generation=%llu symbol=%s installed=%u hit_events=%u failures=%u target_mm_scoped=1 page_record_routed=1 observe_only=0 cleanup=monitor\n",
                 (unsigned int)slot_id, (unsigned long long)generation,
                 g_exit_mmap ? "exit_mmap" : "absent",
                 installed ? 1 : 0, hit_events, failures);
    return r0lab_copy_reply(out_msg, outlen, reply);
}

static long r0lab_raw_exit_hook_status(uint64_t token, char __user *out_msg,
                                       int outlen)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    unsigned long flags;
    bool installed;
    uint32_t hit_events;
    uint32_t failures;
    int result = r0lab_validate_owner(token);

    if (result)
        return result;
    flags = r0lab_lock();
    installed = g_raw_page.exit_hook_installed;
    hit_events = g_raw_page.exit_hook_events;
    failures = g_raw_page.exit_hook_failures;
    r0lab_unlock(flags);

    snprintf(reply, sizeof(reply),
             "raw_exit_hook_status symbol=%s installed=%u hit_events=%u failures=%u target_mm_scoped=1 observe_only=1 pte_switch=0 cleanup=monitor\n",
             g_exit_mmap ? "exit_mmap" : "absent", installed ? 1 : 0,
             hit_events, failures);
    return r0lab_copy_reply(out_msg, outlen, reply);
}

static long r0lab_raw_exit_hook_clear_common(uint64_t token,
                                             uint16_t slot_id,
                                             uint64_t generation,
                                             char __user *out_msg,
                                             int outlen,
                                             bool legacy_reply)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    struct r0lab_raw_shadow_page *page;
    unsigned long flags;
    uint32_t hit_events;
    uint32_t failures;
    int result = r0lab_validate_owner(token);

    if (result)
        return result;
    if (slot_id >= R0LAB_RAW_PAGE_SLOT_CAPACITY)
        return R0LAB_EINVAL;
    flags = r0lab_lock();
    page = r0lab_raw_page_slot_locked(slot_id);
    if (!page || (!legacy_reply && page->generation != generation)) {
        r0lab_unlock(flags);
        return R0LAB_EAGAIN;
    }
    r0lab_unlock(flags);

    if (legacy_reply)
        r0lab_raw_exit_unhook();
    else
        r0lab_raw_exit_hook_release(page);

    flags = r0lab_lock();
    page = r0lab_raw_page_slot_locked(slot_id);
    if (!page) {
        r0lab_unlock(flags);
        return R0LAB_EINVAL;
    }
    generation = page->generation;
    hit_events = page->exit_hook_events;
    failures = page->exit_hook_failures;
    r0lab_unlock(flags);
    if (legacy_reply)
        snprintf(reply, sizeof(reply),
                 "raw_exit_hook_cleared symbol=%s installed=0 hit_events=%u failures=%u\n",
                 g_exit_mmap ? "exit_mmap" : "absent", hit_events,
                 failures);
    else
        snprintf(reply, sizeof(reply),
                 "raw_slot_exit_hook_cleared slot=%u generation=%llu symbol=%s installed=0 hit_events=%u failures=%u page_record_routed=1 cleanup=monitor\n",
                 (unsigned int)slot_id, (unsigned long long)generation,
                 g_exit_mmap ? "exit_mmap" : "absent", hit_events,
                 failures);
    return r0lab_copy_reply(out_msg, outlen, reply);
}

static long r0lab_raw_exit_hook_clear(uint64_t token, char __user *out_msg,
                                      int outlen)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    unsigned long flags;
    uint32_t hit_events;
    uint32_t failures;
    int result = r0lab_validate_owner(token);

    if (result)
        return result;
    r0lab_raw_exit_unhook_primary_legacy();
    flags = r0lab_lock();
    hit_events = g_raw_page.exit_hook_events;
    failures = g_raw_page.exit_hook_failures;
    r0lab_unlock(flags);
    snprintf(reply, sizeof(reply),
             "raw_exit_hook_cleared symbol=%s installed=0 hit_events=%u failures=%u\n",
             g_exit_mmap ? "exit_mmap" : "absent", hit_events, failures);
    return r0lab_copy_reply(out_msg, outlen, reply);
}

static long r0lab_raw_gup_hook_status_common(uint64_t token, uint16_t slot_id,
                                             char __user *out_msg, int outlen,
                                             bool legacy_reply)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    struct r0lab_raw_shadow_page *page;
    unsigned long flags;
    bool installed;
    bool uses_pte;
    uint64_t generation;
    uint32_t hook_begin_events;
    uint32_t hook_finish_events;
    uint32_t hook_failures;
    unsigned long primitive_begin_events;
    unsigned long primitive_finish_events;
    int result = r0lab_validate_owner(token);

    if (result)
        return result;
    if (slot_id >= R0LAB_RAW_PAGE_SLOT_CAPACITY)
        return R0LAB_EINVAL;
    flags = r0lab_lock();
    page = r0lab_raw_page_slot_locked(slot_id);
    if (!page) {
        r0lab_unlock(flags);
        return R0LAB_EINVAL;
    }
    installed = page->gup_hook_installed;
    uses_pte = page->gup_hook_uses_pte;
    generation = page->generation;
    hook_begin_events = page->gup_hook_begin_events;
    hook_finish_events = page->gup_hook_finish_events;
    hook_failures = page->gup_hook_failures;
    primitive_begin_events = page->raw.gup_begin_events;
    primitive_finish_events = page->raw.gup_finish_events;
    r0lab_unlock(flags);

    if (legacy_reply)
        snprintf(reply, sizeof(reply),
                 "raw_gup_hook_status symbol=%s follow_page_pte=%s follow_page_mask=%s installed=%u hook_begin_events=%u hook_finish_events=%u hook_failures=%u primitive_begin_events=%lu primitive_finish_events=%lu target_mm_scoped=1 external_reader=1\n",
                 installed ? (uses_pte ? "follow_page_pte" :
                              "follow_page_mask") :
                             (g_follow_page_pte ? "follow_page_pte" :
                              (g_follow_page_mask ? "follow_page_mask" :
                               "absent")),
                 g_follow_page_pte ? "present" : "absent",
                 g_follow_page_mask ? "present" : "absent",
                 installed ? 1 : 0, hook_begin_events,
                 hook_finish_events, hook_failures, primitive_begin_events,
                 primitive_finish_events);
    else
        snprintf(reply, sizeof(reply),
                 "raw_slot_gup_hook_status slot=%u generation=%llu symbol=%s follow_page_pte=%s follow_page_mask=%s installed=%u hook_begin_events=%u hook_finish_events=%u hook_failures=%u primitive_begin_events=%lu primitive_finish_events=%lu target_mm_scoped=1 page_record_routed=1 external_reader=1\n",
                 (unsigned int)slot_id, (unsigned long long)generation,
                 installed ? (uses_pte ? "follow_page_pte" :
                              "follow_page_mask") :
                             (g_follow_page_pte ? "follow_page_pte" :
                              (g_follow_page_mask ? "follow_page_mask" :
                               "absent")),
                 g_follow_page_pte ? "present" : "absent",
                 g_follow_page_mask ? "present" : "absent",
                 installed ? 1 : 0, hook_begin_events,
                 hook_finish_events, hook_failures, primitive_begin_events,
                 primitive_finish_events);
    return r0lab_copy_reply(out_msg, outlen, reply);
}

static long r0lab_raw_gup_hook_status(uint64_t token, char __user *out_msg,
                                      int outlen)
{
    return r0lab_raw_gup_hook_status_common(
        token, R0LAB_RAW_PRIMARY_SLOT, out_msg, outlen, true);
}

static long r0lab_raw_gup_hook_clear_common(uint64_t token, uint16_t slot_id,
                                            char __user *out_msg, int outlen,
                                            bool legacy_reply)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    struct r0lab_raw_shadow_page *page;
    unsigned long flags;
    bool installed;
    bool uses_pte;
    bool should_detach;
    uint64_t generation;
    uint32_t hook_begin_events;
    uint32_t hook_finish_events;
    uint32_t hook_failures;
    unsigned long primitive_begin_events;
    unsigned long primitive_finish_events;
    int result = r0lab_validate_owner(token);

    if (result)
        return result;
    if (slot_id >= R0LAB_RAW_PAGE_SLOT_CAPACITY)
        return R0LAB_EINVAL;

    flags = r0lab_lock();
    page = r0lab_raw_page_slot_locked(slot_id);
    if (!page) {
        r0lab_unlock(flags);
        return R0LAB_EINVAL;
    }
    installed = page->gup_hook_installed;
    uses_pte = page->gup_hook_uses_pte;
    page->gup_hook_installed = false;
    page->gup_hook_uses_pte = false;
    should_detach = installed && r0lab_raw_gup_hook_users_locked() == 0;
    r0lab_unlock(flags);

    if (installed)
        (void)r0lab_raw_wait_for_callbacks();
    if (should_detach && uses_pte)
        r0lab_hook_detach(g_follow_page_pte, r0lab_raw_gup_pte_before,
                          r0lab_raw_gup_pte_after);
    else if (should_detach)
        r0lab_hook_detach(g_follow_page_mask, r0lab_raw_gup_mask_before,
                          r0lab_raw_gup_mask_after);
    if (installed)
        (void)r0lab_raw_wait_for_callbacks();

    flags = r0lab_lock();
    page = r0lab_raw_page_slot_locked(slot_id);
    if (!page) {
        r0lab_unlock(flags);
        return R0LAB_EINVAL;
    }
    generation = page->generation;
    hook_begin_events = page->gup_hook_begin_events;
    hook_finish_events = page->gup_hook_finish_events;
    hook_failures = page->gup_hook_failures;
    primitive_begin_events = page->raw.gup_begin_events;
    primitive_finish_events = page->raw.gup_finish_events;
    r0lab_unlock(flags);

    if (legacy_reply)
        snprintf(reply, sizeof(reply),
                 "raw_gup_hook_cleared follow_page_pte=%s follow_page_mask=%s installed=0 hook_begin_events=%u hook_finish_events=%u hook_failures=%u primitive_begin_events=%lu primitive_finish_events=%lu\n",
                 g_follow_page_pte ? "present" : "absent",
                 g_follow_page_mask ? "present" : "absent",
                 hook_begin_events, hook_finish_events, hook_failures,
                 primitive_begin_events, primitive_finish_events);
    else
        snprintf(reply, sizeof(reply),
                 "raw_slot_gup_hook_cleared slot=%u generation=%llu follow_page_pte=%s follow_page_mask=%s installed=0 hook_begin_events=%u hook_finish_events=%u hook_failures=%u primitive_begin_events=%lu primitive_finish_events=%lu page_record_routed=1\n",
                 (unsigned int)slot_id, (unsigned long long)generation,
                 g_follow_page_pte ? "present" : "absent",
                 g_follow_page_mask ? "present" : "absent",
                 hook_begin_events, hook_finish_events, hook_failures,
                 primitive_begin_events, primitive_finish_events);
    return r0lab_copy_reply(out_msg, outlen, reply);
}

static long r0lab_raw_gup_hook_clear(uint64_t token, char __user *out_msg,
                                     int outlen)
{
    return r0lab_raw_gup_hook_clear_common(
        token, R0LAB_RAW_PRIMARY_SLOT, out_msg, outlen, true);
}

static long r0lab_raw_fork_hook_arm_common(uint64_t token, uint16_t slot_id,
                                           char __user *out_msg, int outlen,
                                           bool legacy_reply)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    struct r0lab_raw_shadow_page *page;
    unsigned long flags;
    bool installed;
    bool should_install;
    uint64_t generation;
    int result = r0lab_validate_owner(token);

    if (result)
        goto record;
    if (!g_dup_mmap) {
        result = R0LAB_ENOSYS;
        goto record;
    }
    if (slot_id >= R0LAB_RAW_PAGE_SLOT_CAPACITY) {
        result = R0LAB_EINVAL;
        goto record;
    }

    flags = r0lab_lock();
    page = r0lab_raw_page_slot_locked(slot_id);
    if (!page || !page->armed || page->clearing ||
        page->transitioning || page->raw.state != R0LAB_RAW_SHADOW_RX ||
        page->raw.gup_hide_active || page->raw.fork_hide_active ||
        page->raw.read_cycle_active ||
        g_session.owner_tgid != r0lab_current_tgid()) {
        r0lab_unlock(flags);
        result = R0LAB_EAGAIN;
        goto record;
    }
    installed = page->fork_hook_installed;
    should_install = !installed && r0lab_raw_fork_hook_users_locked() == 0;
    generation = page->generation;
    if (!installed) {
        page->fork_hook_begin_events = 0;
        page->fork_hook_finish_events = 0;
        page->fork_hook_failures = 0;
    }
    r0lab_unlock(flags);

    if (should_install) {
        result = hook_wrap2(g_dup_mmap, r0lab_raw_fork_before,
                            r0lab_raw_fork_after, NULL);
        if (result)
            goto record;
    }
    if (!installed) {
        flags = r0lab_lock();
        page = r0lab_raw_page_slot_locked(slot_id);
        if (page && page->armed && !page->clearing &&
            page->generation == generation &&
            page->raw.state == R0LAB_RAW_SHADOW_RX) {
            page->fork_hook_installed = true;
        } else {
            result = R0LAB_EAGAIN;
        }
        r0lab_unlock(flags);
        if (result && should_install) {
            r0lab_hook_detach(g_dup_mmap, r0lab_raw_fork_before,
                              r0lab_raw_fork_after);
            goto record;
        }
    }

    if (legacy_reply)
        snprintf(reply, sizeof(reply),
                 "raw_fork_hook_ready symbol=dup_mmap installed=1 parent_pause=1 child_original_inherit=1 target_mm_scoped=1\n");
    else
        snprintf(reply, sizeof(reply),
                 "raw_slot_fork_hook_ready slot=%u generation=%llu symbol=dup_mmap installed=1 parent_pause=1 child_original_inherit=1 target_mm_scoped=1 page_record_routed=1\n",
                 (unsigned int)slot_id, (unsigned long long)generation);
    return r0lab_copy_reply(out_msg, outlen, reply);

record:
    r0lab_record(R0LAB_EVENT_REJECT, result);
    return result;
}

static long r0lab_raw_fork_hook_arm(uint64_t token, char __user *out_msg,
                                    int outlen)
{
    return r0lab_raw_fork_hook_arm_common(
        token, R0LAB_RAW_PRIMARY_SLOT, out_msg, outlen, true);
}

static long r0lab_raw_fork_hook_status_common(uint64_t token,
                                              uint16_t slot_id,
                                              char __user *out_msg,
                                              int outlen,
                                              bool legacy_reply)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    struct r0lab_raw_shadow_page *page;
    unsigned long flags;
    bool installed;
    uint64_t generation;
    uint32_t hook_begin_events;
    uint32_t hook_finish_events;
    uint32_t hook_failures;
    unsigned long primitive_begin_events;
    unsigned long primitive_finish_events;
    int result = r0lab_validate_owner(token);

    if (result)
        return result;
    if (slot_id >= R0LAB_RAW_PAGE_SLOT_CAPACITY)
        return R0LAB_EINVAL;
    flags = r0lab_lock();
    page = r0lab_raw_page_slot_locked(slot_id);
    if (!page) {
        r0lab_unlock(flags);
        return R0LAB_EINVAL;
    }
    installed = page->fork_hook_installed;
    generation = page->generation;
    hook_begin_events = page->fork_hook_begin_events;
    hook_finish_events = page->fork_hook_finish_events;
    hook_failures = page->fork_hook_failures;
    primitive_begin_events = page->raw.fork_begin_events;
    primitive_finish_events = page->raw.fork_finish_events;
    r0lab_unlock(flags);

    if (legacy_reply)
        snprintf(reply, sizeof(reply),
                 "raw_fork_hook_status symbol=%s installed=%u hook_begin_events=%u hook_finish_events=%u hook_failures=%u primitive_begin_events=%lu primitive_finish_events=%lu parent_pause=1 child_original_inherit=1 target_mm_scoped=1\n",
                 g_dup_mmap ? "dup_mmap" : "absent", installed ? 1 : 0,
                 hook_begin_events, hook_finish_events, hook_failures,
                 primitive_begin_events, primitive_finish_events);
    else
        snprintf(reply, sizeof(reply),
                 "raw_slot_fork_hook_status slot=%u generation=%llu symbol=%s installed=%u hook_begin_events=%u hook_finish_events=%u hook_failures=%u primitive_begin_events=%lu primitive_finish_events=%lu parent_pause=1 child_original_inherit=1 target_mm_scoped=1 page_record_routed=1\n",
                 (unsigned int)slot_id, (unsigned long long)generation,
                 g_dup_mmap ? "dup_mmap" : "absent", installed ? 1 : 0,
                 hook_begin_events, hook_finish_events, hook_failures,
                 primitive_begin_events, primitive_finish_events);
    return r0lab_copy_reply(out_msg, outlen, reply);
}

static long r0lab_raw_fork_hook_status(uint64_t token, char __user *out_msg,
                                       int outlen)
{
    return r0lab_raw_fork_hook_status_common(
        token, R0LAB_RAW_PRIMARY_SLOT, out_msg, outlen, true);
}

static long r0lab_raw_fork_hook_clear_common(uint64_t token, uint16_t slot_id,
                                             char __user *out_msg, int outlen,
                                             bool legacy_reply)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    struct r0lab_raw_shadow_page *page;
    unsigned long flags;
    uint64_t generation;
    uint32_t hook_begin_events;
    uint32_t hook_finish_events;
    uint32_t hook_failures;
    unsigned long primitive_begin_events;
    unsigned long primitive_finish_events;
    int result = r0lab_validate_owner(token);

    if (result)
        return result;
    if (slot_id >= R0LAB_RAW_PAGE_SLOT_CAPACITY)
        return R0LAB_EINVAL;
    flags = r0lab_lock();
    page = r0lab_raw_page_slot_locked(slot_id);
    if (!page) {
        r0lab_unlock(flags);
        return R0LAB_EINVAL;
    }
    r0lab_unlock(flags);

    r0lab_raw_fork_hook_release(page);

    flags = r0lab_lock();
    page = r0lab_raw_page_slot_locked(slot_id);
    if (!page) {
        r0lab_unlock(flags);
        return R0LAB_EINVAL;
    }
    generation = page->generation;
    hook_begin_events = page->fork_hook_begin_events;
    hook_finish_events = page->fork_hook_finish_events;
    hook_failures = page->fork_hook_failures;
    primitive_begin_events = page->raw.fork_begin_events;
    primitive_finish_events = page->raw.fork_finish_events;
    r0lab_unlock(flags);

    if (legacy_reply)
        snprintf(reply, sizeof(reply),
                 "raw_fork_hook_cleared symbol=%s installed=0 hook_begin_events=%u hook_finish_events=%u hook_failures=%u primitive_begin_events=%lu primitive_finish_events=%lu\n",
                 g_dup_mmap ? "dup_mmap" : "absent", hook_begin_events,
                 hook_finish_events, hook_failures, primitive_begin_events,
                 primitive_finish_events);
    else
        snprintf(reply, sizeof(reply),
                 "raw_slot_fork_hook_cleared slot=%u generation=%llu symbol=%s installed=0 hook_begin_events=%u hook_finish_events=%u hook_failures=%u primitive_begin_events=%lu primitive_finish_events=%lu page_record_routed=1\n",
                 (unsigned int)slot_id, (unsigned long long)generation,
                 g_dup_mmap ? "dup_mmap" : "absent", hook_begin_events,
                 hook_finish_events, hook_failures, primitive_begin_events,
                 primitive_finish_events);
    return r0lab_copy_reply(out_msg, outlen, reply);
}

static long r0lab_raw_fork_hook_clear(uint64_t token, char __user *out_msg,
                                      int outlen)
{
    return r0lab_raw_fork_hook_clear_common(
        token, R0LAB_RAW_PRIMARY_SLOT, out_msg, outlen, true);
}

static long r0lab_raw_clear(uint64_t token, char __user *out_msg, int outlen)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    unsigned long flags;
    int result = r0lab_validate_owner(token);

    if (result)
        goto record;
    flags = r0lab_lock();
    if ((!g_raw_page.reserving && !g_raw_page.armed) || g_raw_page.clearing) {
        r0lab_unlock(flags);
        result = R0LAB_EAGAIN;
        goto record;
    }
    g_raw_page.record.state = R0LAB_PAGE_RECORD_RESTORING;
    g_raw_page.clearing = true;
    if (g_raw_page.reserving) {
        r0lab_unlock(flags);
        return r0lab_copy_reply(out_msg, outlen, "raw_cancel_pending\n");
    }
    r0lab_unlock(flags);
    result = r0lab_raw_start_worker(r0lab_raw_clear_worker);
    if (result) {
        flags = r0lab_lock();
        if (g_raw_page.armed)
            g_raw_page.clearing = false;
        r0lab_unlock(flags);
        goto record;
    }
    snprintf(reply, sizeof(reply), "raw_clear_pending\n");
    return r0lab_copy_reply(out_msg, outlen, reply);

record:
    r0lab_record(R0LAB_EVENT_REJECT, result);
    return result;
}

static long r0lab_raw_cleared(uint64_t token, char __user *out_msg, int outlen)
{
    unsigned long flags;
    int result = r0lab_validate_owner(token);

    if (result)
        return result;
    flags = r0lab_lock();
    if (r0lab_raw_slot_count_locked() || r0lab_s4_slot_count_locked()) {
        r0lab_unlock(flags);
        return R0LAB_EAGAIN;
    }
    r0lab_unlock(flags);
    return r0lab_copy_reply(out_msg, outlen, "raw_cleared\n");
}

static long r0lab_raw_slot_clear(uint64_t token, uint16_t slot_id,
                                 char __user *out_msg, int outlen)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    struct r0lab_raw_shadow_page *page;
    unsigned long flags;
    int result = r0lab_validate_owner(token);

    if (result)
        goto record;
    if (slot_id >= R0LAB_RAW_PAGE_SLOT_CAPACITY) {
        result = R0LAB_EINVAL;
        goto record;
    }
    flags = r0lab_lock();
    page = r0lab_raw_page_slot_locked(slot_id);
    if (!page || ((!page->reserving && !page->armed) || page->clearing)) {
        r0lab_unlock(flags);
        result = R0LAB_EAGAIN;
        goto record;
    }
    page->record.state = R0LAB_PAGE_RECORD_RESTORING;
    page->clearing = true;
    if (page->reserving) {
        r0lab_unlock(flags);
        snprintf(reply, sizeof(reply), "raw_slot_cancel_pending slot=%u\n",
                 (unsigned int)slot_id);
        return r0lab_copy_reply(out_msg, outlen, reply);
    }
    r0lab_unlock(flags);
    result = r0lab_raw_start_worker(r0lab_raw_clear_worker);
    if (result) {
        flags = r0lab_lock();
        if (page->armed)
            page->clearing = false;
        r0lab_unlock(flags);
        goto record;
    }
    snprintf(reply, sizeof(reply), "raw_slot_clear_pending slot=%u\n",
             (unsigned int)slot_id);
    return r0lab_copy_reply(out_msg, outlen, reply);

record:
    r0lab_record(R0LAB_EVENT_REJECT, result);
    return result;
}

static long r0lab_raw_slot_cleared(uint64_t token, uint16_t slot_id,
                                   char __user *out_msg, int outlen)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    struct r0lab_raw_shadow_page *page;
    unsigned long flags;
    int result = r0lab_validate_owner(token);

    if (result)
        return result;
    if (slot_id >= R0LAB_RAW_PAGE_SLOT_CAPACITY)
        return R0LAB_EINVAL;
    flags = r0lab_lock();
    page = r0lab_raw_page_slot_locked(slot_id);
    if (!page || r0lab_raw_page_slot_owned_locked(page) ||
        r0lab_s4_slot_count_locked()) {
        r0lab_unlock(flags);
        return R0LAB_EAGAIN;
    }
    r0lab_unlock(flags);
    snprintf(reply, sizeof(reply), "raw_slot_cleared slot=%u\n",
             (unsigned int)slot_id);
    return r0lab_copy_reply(out_msg, outlen, reply);
}

static long r0lab_workers_shutdown(char __user *out_msg, int outlen)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    unsigned long flags;
    unsigned int workers_live;

    flags = r0lab_lock();
    if (g_session.active || r0lab_hwbp_slot_count_locked() ||
        r0lab_m3_slot_count_locked() || r0lab_m4_slot_count_locked() ||
        r0lab_raw_slot_count_locked() || r0lab_s4_slot_count_locked()) {
        r0lab_unlock(flags);
        r0lab_record(R0LAB_EVENT_REJECT, R0LAB_EBUSY);
        return R0LAB_EBUSY;
    }
    g_workers_shutdown_requested = true;
    workers_live = g_workers_live;
    r0lab_unlock(flags);

    snprintf(reply, sizeof(reply), "workers_shutdown_requested live=%u\n",
             workers_live);
    return r0lab_copy_reply(out_msg, outlen, reply);
}

/* Only init failure can synchronously stop threads; .kpm.exit must not sleep. */
static void r0lab_stop_workers_after_init_failure(void)
{
    unsigned int index;

    if (!g_workers_started || !g_kthread_stop)
        return;
    for (index = 0; index < R0LAB_HWBP_SLOT_CAPACITY; ++index) {
        if (g_hwbp_workers[index]) {
            g_kthread_stop(g_hwbp_workers[index]);
            g_hwbp_workers[index] = NULL;
        }
    }
    if (g_m3_worker) {
        g_kthread_stop(g_m3_worker);
        g_m3_worker = NULL;
    }
    if (g_m3_monitor_worker_task) {
        g_kthread_stop(g_m3_monitor_worker_task);
        g_m3_monitor_worker_task = NULL;
    }
    if (g_m4_worker) {
        g_kthread_stop(g_m4_worker);
        g_m4_worker = NULL;
    }
    if (g_m4_monitor_worker_task) {
        g_kthread_stop(g_m4_monitor_worker_task);
        g_m4_monitor_worker_task = NULL;
    }
    if (g_raw_worker) {
        g_kthread_stop(g_raw_worker);
        g_raw_worker = NULL;
    }
    if (g_raw_monitor_worker_task) {
        g_kthread_stop(g_raw_monitor_worker_task);
        g_raw_monitor_worker_task = NULL;
    }
    if (g_s4_monitor_worker_task) {
        g_kthread_stop(g_s4_monitor_worker_task);
        g_s4_monitor_worker_task = NULL;
    }
    if (g_session_monitor_worker_task) {
        g_kthread_stop(g_session_monitor_worker_task);
        g_session_monitor_worker_task = NULL;
    }
    g_workers_started = false;
}

static int r0lab_start_workers(void)
{
    unsigned int index;

    g_workers_shutdown_requested = false;
    g_workers_live = 0;
    for (index = 0; index < R0LAB_HWBP_SLOT_CAPACITY; ++index) {
        g_hwbp_workers[index] = g_kthread_create(r0lab_hwbp_worker,
                                                 &g_hwbp_slots[index], -1,
                                                 "r0lab-hwbp");
        if (r0lab_is_error_pointer(g_hwbp_workers[index])) {
            int result = (long)(unsigned long)g_hwbp_workers[index];

            g_hwbp_workers[index] = NULL;
            g_workers_started = true;
            r0lab_stop_workers_after_init_failure();
            return result;
        }
        ++g_workers_live;
        g_wake_up_process(g_hwbp_workers[index]);
    }

    g_m3_worker = g_kthread_create(r0lab_m3_worker_loop, NULL, -1,
                                   "r0lab-m3");
    if (r0lab_is_error_pointer(g_m3_worker)) {
        int result = (long)(unsigned long)g_m3_worker;

        g_m3_worker = NULL;
        g_workers_started = true;
        r0lab_stop_workers_after_init_failure();
        return result;
    }
    ++g_workers_live;
    g_wake_up_process(g_m3_worker);

    g_m3_monitor_worker_task = g_kthread_create(r0lab_m3_monitor_loop, NULL,
                                                -1, "r0lab-m3mon");
    if (r0lab_is_error_pointer(g_m3_monitor_worker_task)) {
        int result = (long)(unsigned long)g_m3_monitor_worker_task;

        g_m3_monitor_worker_task = NULL;
        g_workers_started = true;
        r0lab_stop_workers_after_init_failure();
        return result;
    }
    ++g_workers_live;
    g_wake_up_process(g_m3_monitor_worker_task);

    g_m4_worker = g_kthread_create(r0lab_m4_worker_loop, NULL, -1,
                                   "r0lab-m4");
    if (r0lab_is_error_pointer(g_m4_worker)) {
        int result = (long)(unsigned long)g_m4_worker;

        g_m4_worker = NULL;
        g_workers_started = true;
        r0lab_stop_workers_after_init_failure();
        return result;
    }
    ++g_workers_live;
    g_wake_up_process(g_m4_worker);

    g_m4_monitor_worker_task = g_kthread_create(r0lab_m4_monitor_loop, NULL,
                                                -1, "r0lab-m4mon");
    if (r0lab_is_error_pointer(g_m4_monitor_worker_task)) {
        int result = (long)(unsigned long)g_m4_monitor_worker_task;

        g_m4_monitor_worker_task = NULL;
        g_workers_started = true;
        r0lab_stop_workers_after_init_failure();
        return result;
    }
    ++g_workers_live;
    g_wake_up_process(g_m4_monitor_worker_task);

    g_raw_worker = g_kthread_create(r0lab_raw_worker_loop, NULL, -1,
                                    "r0lab-raw");
    if (r0lab_is_error_pointer(g_raw_worker)) {
        int result = (long)(unsigned long)g_raw_worker;

        g_raw_worker = NULL;
        g_workers_started = true;
        r0lab_stop_workers_after_init_failure();
        return result;
    }
    ++g_workers_live;
    g_wake_up_process(g_raw_worker);

    g_raw_monitor_worker_task = g_kthread_create(r0lab_raw_monitor_loop, NULL,
                                                 -1, "r0lab-rawmon");
    if (r0lab_is_error_pointer(g_raw_monitor_worker_task)) {
        int result = (long)(unsigned long)g_raw_monitor_worker_task;

        g_raw_monitor_worker_task = NULL;
        g_workers_started = true;
        r0lab_stop_workers_after_init_failure();
        return result;
    }
    ++g_workers_live;
    g_wake_up_process(g_raw_monitor_worker_task);

    g_s4_monitor_worker_task = g_kthread_create(r0lab_s4_monitor_loop, NULL,
                                                -1, "r0lab-s4mon");
    if (r0lab_is_error_pointer(g_s4_monitor_worker_task)) {
        int result = (long)(unsigned long)g_s4_monitor_worker_task;

        g_s4_monitor_worker_task = NULL;
        g_workers_started = true;
        r0lab_stop_workers_after_init_failure();
        return result;
    }
    ++g_workers_live;
    g_wake_up_process(g_s4_monitor_worker_task);

    g_session_monitor_worker_task =
        g_kthread_create(r0lab_session_monitor_loop, NULL, -1,
                         "r0lab-sessionmon");
    if (r0lab_is_error_pointer(g_session_monitor_worker_task)) {
        int result = (long)(unsigned long)g_session_monitor_worker_task;

        g_session_monitor_worker_task = NULL;
        g_workers_started = true;
        r0lab_stop_workers_after_init_failure();
        return result;
    }
    ++g_workers_live;
    g_wake_up_process(g_session_monitor_worker_task);

    g_workers_started = true;
    return 0;
}

static void *r0lab_lookup_first(const char *primary, const char *fallback)
{
    void *symbol = NULL;

    if (primary)
        symbol = (void *)kallsyms_lookup_name(primary);
    if (!symbol && fallback)
        symbol = (void *)kallsyms_lookup_name(fallback);
    return symbol;
}

static int r0lab_init_raw_bridge(void)
{
    int64_t *kernel_memstart;
    unsigned long *kernel_hwcaps;

    g_vmalloc = (r0lab_vmalloc_fn_t)r0lab_lookup_first("vmalloc.cfi_jt", "vmalloc");
    g_vfree = (r0lab_vfree_fn_t)r0lab_lookup_first("vfree.cfi_jt", "vfree");
    g_vmalloc_to_page = (r0lab_vmalloc_to_page_fn_t)
        r0lab_lookup_first("vmalloc_to_page.cfi_jt", "vmalloc_to_page");
    g_raw_down_read = (r0lab_down_read_fn_t)
        r0lab_lookup_first("down_read.cfi_jt", "down_read");
    g_raw_up_read = (r0lab_up_read_fn_t)
        r0lab_lookup_first("up_read", "up_read.cfi_jt");
    g_raw_spin_lock = (r0lab_raw_spin_fn_t)
        r0lab_lookup_first("_raw_spin_lock.cfi_jt", "_raw_spin_lock");
    g_raw_spin_unlock = (r0lab_raw_spin_fn_t)
        r0lab_lookup_first("_raw_spin_unlock.cfi_jt", "_raw_spin_unlock");
    g_raw_find_vma = (r0lab_find_vma_fn_t)
        r0lab_lookup_first("find_vma.cfi_jt", "find_vma");
    g_sync_icache_dcache = (r0lab_sync_icache_dcache_fn_t)
        r0lab_lookup_first("__sync_icache_dcache", "__sync_icache_dcache.cfi_jt");
    g_sync_icache_aliases = (r0lab_sync_icache_aliases_fn_t)
        r0lab_lookup_first("sync_icache_aliases.cfi_jt",
                           "sync_icache_aliases");
    g_mte_sync_tags = (r0lab_mte_sync_tags_fn_t)
        r0lab_lookup_first("mte_sync_tags", "mte_sync_tags.cfi_jt");

    kernel_memstart = (int64_t *)kallsyms_lookup_name("memstart_addr");
    kernel_hwcaps = (unsigned long *)kallsyms_lookup_name("cpu_hwcaps");
    if (!g_vmalloc || !g_vfree || !g_vmalloc_to_page || !g_get_free_pages ||
        !g_free_pages || !g_mmdrop || !g_raw_down_read ||
        !g_raw_up_read || !g_raw_spin_lock || !g_raw_spin_unlock ||
        !g_raw_find_vma || !g_sync_icache_dcache ||
        !g_sync_icache_aliases || !g_mte_sync_tags ||
        !kernel_memstart || !kernel_hwcaps)
        return R0LAB_ENOSYS;

    memstart_addr = *kernel_memstart;
    memcpy(cpu_hwcaps, kernel_hwcaps, sizeof(cpu_hwcaps));
    memset(arm64_const_caps_ready, 0, sizeof(arm64_const_caps_ready));
    memset(cpu_hwcap_keys, 0, sizeof(cpu_hwcap_keys));

    if (r0lab_raw_abi_page_size() != R0LAB_RAW_PAGE_SIZE ||
        !r0lab_raw_abi_pte_uxn_bit() ||
        !r0lab_raw_abi_pte_user_bit() ||
        !r0lab_raw_abi_pte_valid_bit())
        return R0LAB_ENOSYS;
    return 0;
}

static long r0lab_control0(const char *args, char __user *out_msg, int outlen)
{
    const char *value;
    uint64_t token;
    uint64_t entry_pc;
    uint64_t return_pc;
    uint64_t page_address;
    uint64_t page_address2;
    uint64_t slot_value;
    uint64_t generation;
    uint64_t range_offset;
    uint64_t range_length;
    uint64_t patch_value;
    uint16_t slot_id;
    int result;

    result = r0lab_validate_lab_caller();
    if (result) {
        r0lab_record(R0LAB_EVENT_REJECT, result);
        return result;
    }
    if (!args)
        return R0LAB_EINVAL;
    if (!strcmp(args, "unload allow")) {
        unsigned long flags = r0lab_lock();

        g_exit_probe_armed = false;
        r0lab_unlock(flags);
        return r0lab_copy_reply(out_msg, outlen, "unload_allowed\n");
    }
    if (!strcmp(args, "workers shutdown"))
        return r0lab_workers_shutdown(out_msg, outlen);
    if (!strcmp(args, "status"))
        return r0lab_status(out_msg, outlen);
    if (!strcmp(args, "s4 abi"))
        return r0lab_s4_abi_probe(out_msg, outlen);
    if (!strncmp(args, "s4 descriptor routing arm ", 26)) {
        value = args + 26;
        if (r0lab_parse_u64_triplet(value, &token, &page_address,
                                    &page_address2))
            return R0LAB_EINVAL;
        return r0lab_s4_descriptor_routing_arm(token, page_address,
                                               page_address2, out_msg,
                                               outlen);
    }
    if (!strncmp(args, "s4 descriptor routing observed ", 31)) {
        value = args + 31;
        if (strnlen(value, R0LAB_TOKEN_MAX + 1) > R0LAB_TOKEN_MAX ||
            r0lab_parse_u64(value, &token))
            return R0LAB_EINVAL;
        return r0lab_s4_descriptor_routing_observed(token, out_msg, outlen);
    }
    if (!strncmp(args, "s4 descriptor routing clear ", 28)) {
        value = args + 28;
        if (strnlen(value, R0LAB_TOKEN_MAX + 1) > R0LAB_TOKEN_MAX ||
            r0lab_parse_u64(value, &token))
            return R0LAB_EINVAL;
        return r0lab_s4_descriptor_routing_clear(token, out_msg, outlen);
    }
    if (!strncmp(args, "s4 descriptor negative probe ", 29)) {
        value = args + 29;
        if (strnlen(value, R0LAB_TOKEN_MAX + 1) > R0LAB_TOKEN_MAX ||
            r0lab_parse_u64(value, &token))
            return R0LAB_EINVAL;
        return r0lab_s4_descriptor_negative_probe(token, out_msg, outlen);
    }
    if (!strncmp(args, "s4 brk arm ", 11)) {
        value = args + 11;
        if (r0lab_parse_u64_pair(value, &token, &page_address))
            return R0LAB_EINVAL;
        return r0lab_s4_brk_arm(token, page_address, false, false, false,
                                out_msg, outlen);
    }
    if (!strncmp(args, "s4 step arm ", 12)) {
        value = args + 12;
        if (r0lab_parse_u64_pair(value, &token, &page_address))
            return R0LAB_EINVAL;
        return r0lab_s4_brk_arm(token, page_address, true, false, false,
                                out_msg, outlen);
    }
    if (!strncmp(args, "s4 raw-step arm ", 16)) {
        value = args + 16;
        if (r0lab_parse_u64_pair(value, &token, &page_address))
            return R0LAB_EINVAL;
        return r0lab_s4_brk_arm(token, page_address, true, true, false,
                                out_msg, outlen);
    }
    if (!strncmp(args, "s4 raw-reg arm ", 15)) {
        value = args + 15;
        if (r0lab_parse_u64_pair(value, &token, &page_address))
            return R0LAB_EINVAL;
        return r0lab_s4_brk_arm(token, page_address, true, true, true,
                                out_msg, outlen);
    }
    if (!strncmp(args, "s4 brk observed ", 16)) {
        value = args + 16;
        if (strnlen(value, R0LAB_TOKEN_MAX + 1) > R0LAB_TOKEN_MAX ||
            r0lab_parse_u64(value, &token))
            return R0LAB_EINVAL;
        return r0lab_s4_brk_observed(token, out_msg, outlen);
    }
    if (!strncmp(args, "s4 step observed ", 17)) {
        value = args + 17;
        if (strnlen(value, R0LAB_TOKEN_MAX + 1) > R0LAB_TOKEN_MAX ||
            r0lab_parse_u64(value, &token))
            return R0LAB_EINVAL;
        return r0lab_s4_step_observed(token, out_msg, outlen);
    }
    if (!strncmp(args, "s4 brk clear ", 13)) {
        value = args + 13;
        if (strnlen(value, R0LAB_TOKEN_MAX + 1) > R0LAB_TOKEN_MAX ||
            r0lab_parse_u64(value, &token))
            return R0LAB_EINVAL;
        return r0lab_s4_brk_clear(token, out_msg, outlen);
    }
    if (!strncmp(args, "s4 brk cleared ", 15)) {
        value = args + 15;
        if (strnlen(value, R0LAB_TOKEN_MAX + 1) > R0LAB_TOKEN_MAX ||
            r0lab_parse_u64(value, &token))
            return R0LAB_EINVAL;
        return r0lab_s4_brk_cleared(token, out_msg, outlen);
    }
    if (!strncmp(args, "arm ", 4)) {
        value = args + 4;
        if (strnlen(value, R0LAB_TOKEN_MAX + 1) > R0LAB_TOKEN_MAX || r0lab_parse_u64(value, &token))
            return R0LAB_EINVAL;
        return r0lab_arm(token, out_msg, outlen);
    }
    if (!strncmp(args, "close ", 6)) {
        value = args + 6;
        if (strnlen(value, R0LAB_TOKEN_MAX + 1) > R0LAB_TOKEN_MAX || r0lab_parse_u64(value, &token))
            return R0LAB_EINVAL;
        return r0lab_close(token, out_msg, outlen);
    }
    if (!strncmp(args, "events ", 7)) {
        value = args + 7;
        if (strnlen(value, R0LAB_TOKEN_MAX + 1) > R0LAB_TOKEN_MAX || r0lab_parse_u64(value, &token))
            return R0LAB_EINVAL;
        return r0lab_events(token, out_msg, outlen);
    }
    if (!strncmp(args, "hwbp arm ", 9)) {
        value = args + 9;
        if (r0lab_parse_u64_triplet(value, &token, &entry_pc, &return_pc))
            return R0LAB_EINVAL;
        return r0lab_hwbp_arm(token, entry_pc, return_pc,
                              out_msg, outlen);
    }
    if (!strncmp(args, "hwbp clear ", 11)) {
        value = args + 11;
        if (strnlen(value, R0LAB_TOKEN_MAX + 1) > R0LAB_TOKEN_MAX ||
            r0lab_parse_u64(value, &token))
            return R0LAB_EINVAL;
        return r0lab_hwbp_clear(token, out_msg, outlen);
    }
    if (!strncmp(args, "hwbp ready ", 11)) {
        value = args + 11;
        if (strnlen(value, R0LAB_TOKEN_MAX + 1) > R0LAB_TOKEN_MAX ||
            r0lab_parse_u64(value, &token))
            return R0LAB_EINVAL;
        return r0lab_hwbp_ready(token, out_msg, outlen);
    }
    if (!strncmp(args, "hwbp cleared ", 13)) {
        value = args + 13;
        if (strnlen(value, R0LAB_TOKEN_MAX + 1) > R0LAB_TOKEN_MAX ||
            r0lab_parse_u64(value, &token))
            return R0LAB_EINVAL;
        return r0lab_hwbp_cleared(token, out_msg, outlen);
    }
    if (!strncmp(args, "m3 arm ", 7)) {
        value = args + 7;
        if (r0lab_parse_u64_pair(value, &token, &page_address))
            return R0LAB_EINVAL;
        return r0lab_m3_arm(token, page_address, out_msg, outlen);
    }
    if (!strncmp(args, "m3 ready ", 9)) {
        value = args + 9;
        if (strnlen(value, R0LAB_TOKEN_MAX + 1) > R0LAB_TOKEN_MAX ||
            r0lab_parse_u64(value, &token))
            return R0LAB_EINVAL;
        return r0lab_m3_ready(token, out_msg, outlen);
    }
    if (!strncmp(args, "m3 observed ", 12)) {
        value = args + 12;
        if (strnlen(value, R0LAB_TOKEN_MAX + 1) > R0LAB_TOKEN_MAX ||
            r0lab_parse_u64(value, &token))
            return R0LAB_EINVAL;
        return r0lab_m3_observed(token, out_msg, outlen);
    }
    if (!strncmp(args, "m3 clear ", 9)) {
        value = args + 9;
        if (strnlen(value, R0LAB_TOKEN_MAX + 1) > R0LAB_TOKEN_MAX ||
            r0lab_parse_u64(value, &token))
            return R0LAB_EINVAL;
        return r0lab_m3_clear(token, out_msg, outlen);
    }
    if (!strncmp(args, "m3 cleared ", 11)) {
        value = args + 11;
        if (strnlen(value, R0LAB_TOKEN_MAX + 1) > R0LAB_TOKEN_MAX ||
            r0lab_parse_u64(value, &token))
            return R0LAB_EINVAL;
        return r0lab_m3_cleared(token, out_msg, outlen);
    }
    if (!strncmp(args, "fault m3 arm-delay ", 19)) {
        value = args + 19;
        if (strnlen(value, R0LAB_TOKEN_MAX + 1) > R0LAB_TOKEN_MAX ||
            r0lab_parse_u64(value, &token))
            return R0LAB_EINVAL;
        return r0lab_m3_set_test_fault(token, true, out_msg, outlen);
    }
    if (!strncmp(args, "fault m3 arm-enomem ", 20)) {
        value = args + 20;
        if (strnlen(value, R0LAB_TOKEN_MAX + 1) > R0LAB_TOKEN_MAX ||
            r0lab_parse_u64(value, &token))
            return R0LAB_EINVAL;
        return r0lab_m3_set_test_fault(token, false, out_msg, outlen);
    }
    if (!strncmp(args, "m4 arm ", 7)) {
        value = args + 7;
        if (r0lab_parse_u64_triplet(value, &token, &page_address, &entry_pc))
            return R0LAB_EINVAL;
        return r0lab_m4_arm(token, page_address, entry_pc, out_msg, outlen);
    }
    if (!strncmp(args, "m4 ready ", 9)) {
        value = args + 9;
        if (strnlen(value, R0LAB_TOKEN_MAX + 1) > R0LAB_TOKEN_MAX ||
            r0lab_parse_u64(value, &token))
            return R0LAB_EINVAL;
        return r0lab_m4_ready(token, out_msg, outlen);
    }
    if (!strncmp(args, "m4 observed ", 12)) {
        value = args + 12;
        if (strnlen(value, R0LAB_TOKEN_MAX + 1) > R0LAB_TOKEN_MAX ||
            r0lab_parse_u64(value, &token))
            return R0LAB_EINVAL;
        return r0lab_m4_observed(token, out_msg, outlen);
    }
    if (!strncmp(args, "m4 clear ", 9)) {
        value = args + 9;
        if (strnlen(value, R0LAB_TOKEN_MAX + 1) > R0LAB_TOKEN_MAX ||
            r0lab_parse_u64(value, &token))
            return R0LAB_EINVAL;
        return r0lab_m4_clear(token, out_msg, outlen);
    }
    if (!strncmp(args, "m4 cleared ", 11)) {
        value = args + 11;
        if (strnlen(value, R0LAB_TOKEN_MAX + 1) > R0LAB_TOKEN_MAX ||
            r0lab_parse_u64(value, &token))
            return R0LAB_EINVAL;
        return r0lab_m4_cleared(token, out_msg, outlen);
    }
    if (!strncmp(args, "raw slot arm no-abort ", 22)) {
        value = args + 22;
        if (r0lab_parse_u64_triplet(value, &token, &slot_value,
                                    &page_address) ||
            r0lab_raw_parse_slot_id(slot_value, &slot_id))
            return R0LAB_EINVAL;
        return r0lab_raw_slot_arm(token, slot_id, page_address, true, false,
                                  false, false, false, false, false, false,
                                  out_msg, outlen);
    }
    if (!strncmp(args, "raw slot arm abort-passthrough ", 31)) {
        value = args + 31;
        if (r0lab_parse_u64_triplet(value, &token, &slot_value,
                                    &page_address) ||
            r0lab_raw_parse_slot_id(slot_value, &slot_id))
            return R0LAB_EINVAL;
        return r0lab_raw_slot_arm(token, slot_id, page_address, false, true,
                                  false, false, false, false, false, false,
                                  out_msg, outlen);
    }
    if (!strncmp(args, "raw slot arm abort-mmget ", 25)) {
        value = args + 25;
        if (r0lab_parse_u64_triplet(value, &token, &slot_value,
                                    &page_address) ||
            r0lab_raw_parse_slot_id(slot_value, &slot_id))
            return R0LAB_EINVAL;
        return r0lab_raw_slot_arm(token, slot_id, page_address, false, false,
                                  true, false, false, false, false, false,
                                  out_msg, outlen);
    }
    if (!strncmp(args, "raw slot arm abort-lock ", 24)) {
        value = args + 24;
        if (r0lab_parse_u64_triplet(value, &token, &slot_value,
                                    &page_address) ||
            r0lab_raw_parse_slot_id(slot_value, &slot_id))
            return R0LAB_EINVAL;
        return r0lab_raw_slot_arm(token, slot_id, page_address, false, false,
                                  false, true, false, false, false, false,
                                  out_msg, outlen);
    }
    if (!strncmp(args, "raw slot arm abort-inflight ", 28)) {
        value = args + 28;
        if (r0lab_parse_u64_triplet(value, &token, &slot_value,
                                    &page_address) ||
            r0lab_raw_parse_slot_id(slot_value, &slot_id))
            return R0LAB_EINVAL;
        return r0lab_raw_slot_arm(token, slot_id, page_address, false, false,
                                  false, false, true, false, false, false,
                                  out_msg, outlen);
    }
    if (!strncmp(args, "raw slot arm abort-iabt-route ", 30)) {
        value = args + 30;
        if (r0lab_parse_u64_triplet(value, &token, &slot_value,
                                    &page_address) ||
            r0lab_raw_parse_slot_id(slot_value, &slot_id))
            return R0LAB_EINVAL;
        return r0lab_raw_slot_arm(token, slot_id, page_address, false, false,
                                  false, false, false, true, false, false,
                                  out_msg, outlen);
    }
    if (!strncmp(args, "raw slot arm abort-iabt-transition ", 35)) {
        value = args + 35;
        if (r0lab_parse_u64_triplet(value, &token, &slot_value,
                                    &page_address) ||
            r0lab_raw_parse_slot_id(slot_value, &slot_id))
            return R0LAB_EINVAL;
        return r0lab_raw_slot_arm(token, slot_id, page_address, false, false,
                                  false, false, false, false, true, false,
                                  out_msg, outlen);
    }
    if (!strncmp(args, "raw slot arm ", 13)) {
        value = args + 13;
        if (r0lab_parse_u64_triplet(value, &token, &slot_value,
                                    &page_address) ||
            r0lab_raw_parse_slot_id(slot_value, &slot_id))
            return R0LAB_EINVAL;
        return r0lab_raw_slot_arm(token, slot_id, page_address, false, false,
                                  false, false, false, false, false, false,
                                  out_msg, outlen);
    }
    if (!strncmp(args, "raw slot ready ", 15)) {
        value = args + 15;
        if (r0lab_parse_u64_pair(value, &token, &slot_value) ||
            r0lab_raw_parse_slot_id(slot_value, &slot_id))
            return R0LAB_EINVAL;
        return r0lab_raw_slot_ready(token, slot_id, out_msg, outlen);
    }
    if (!strncmp(args, "raw slot observed ", 18)) {
        value = args + 18;
        if (r0lab_parse_u64_pair(value, &token, &slot_value) ||
            r0lab_raw_parse_slot_id(slot_value, &slot_id))
            return R0LAB_EINVAL;
        return r0lab_raw_slot_observed(token, slot_id, out_msg, outlen);
    }
    if (!strncmp(args, "raw slot inspect ", 17)) {
        value = args + 17;
        if (r0lab_parse_u64_pair(value, &token, &slot_value) ||
            r0lab_raw_parse_slot_id(slot_value, &slot_id))
            return R0LAB_EINVAL;
        return r0lab_raw_slot_inspect(token, slot_id, out_msg, outlen);
    }
    if (!strncmp(args, "raw slot live pte ", 18)) {
        value = args + 18;
        if (r0lab_parse_u64_pair(value, &token, &slot_value) ||
            r0lab_raw_parse_slot_id(slot_value, &slot_id))
            return R0LAB_EINVAL;
        return r0lab_raw_slot_live_pte(token, slot_id, out_msg, outlen);
    }
    if (!strncmp(args, "raw slot patch byte ", 20)) {
        value = args + 20;
        if (r0lab_parse_u64_quintet(value, &token, &slot_value,
                                    &generation, &range_offset,
                                    &patch_value) ||
            r0lab_raw_parse_slot_id(slot_value, &slot_id))
            return R0LAB_EINVAL;
        return r0lab_raw_slot_patch_apply(token, slot_id, generation,
                                          range_offset, patch_value, 1U,
                                          out_msg, outlen);
    }
    if (!strncmp(args, "raw slot patch word ", 20)) {
        value = args + 20;
        if (r0lab_parse_u64_quintet(value, &token, &slot_value,
                                    &generation, &range_offset,
                                    &patch_value) ||
            r0lab_raw_parse_slot_id(slot_value, &slot_id))
            return R0LAB_EINVAL;
        return r0lab_raw_slot_patch_apply(token, slot_id, generation,
                                          range_offset, patch_value,
                                          sizeof(uint32_t), out_msg, outlen);
    }
    if (!strncmp(args, "raw slot patch release ", 23)) {
        value = args + 23;
        if (r0lab_parse_u64_quad(value, &token, &slot_value, &generation,
                                 &range_offset) ||
            r0lab_raw_parse_slot_id(slot_value, &slot_id))
            return R0LAB_EINVAL;
        return r0lab_raw_slot_patch_release(token, slot_id, generation,
                                            range_offset, out_msg, outlen);
    }
    if (!strncmp(args, "raw slot patch status ", 22)) {
        value = args + 22;
        if (r0lab_parse_u64_pair(value, &token, &slot_value) ||
            r0lab_raw_parse_slot_id(slot_value, &slot_id))
            return R0LAB_EINVAL;
        return r0lab_raw_slot_patch_status(token, slot_id, out_msg, outlen);
    }
    if (!strncmp(args, "raw hook route status ", 22)) {
        value = args + 22;
        if (r0lab_parse_u64_pair(value, &token, &slot_value) ||
            r0lab_raw_parse_slot_id(slot_value, &slot_id))
            return R0LAB_EINVAL;
        return r0lab_raw_hook_route_status(token, slot_id, out_msg, outlen);
    }
    if (!strncmp(args, "raw slot patch check ", 21)) {
        value = args + 21;
        if (r0lab_parse_u64_quintet(value, &token, &slot_value,
                                    &generation, &range_offset,
                                    &range_length) ||
            r0lab_raw_parse_slot_id(slot_value, &slot_id))
            return R0LAB_EINVAL;
        return r0lab_raw_slot_patch_check(token, slot_id, generation,
                                          range_offset, range_length,
                                          out_msg, outlen);
    }
    if (!strncmp(args, "raw arm ", 8)) {
        value = args + 8;
        if (r0lab_parse_u64_pair(value, &token, &page_address))
            return R0LAB_EINVAL;
        return r0lab_raw_arm(token, page_address, out_msg, outlen);
    }
    if (!strncmp(args, "raw xom read fault arm ", 23)) {
        value = args + 23;
        if (strnlen(value, R0LAB_TOKEN_MAX + 1) > R0LAB_TOKEN_MAX ||
            r0lab_parse_u64(value, &token))
            return R0LAB_EINVAL;
        return r0lab_raw_xom_read_fault_arm(token, out_msg, outlen);
    }
    if (!strncmp(args, "raw xom read fault status ", 26)) {
        value = args + 26;
        if (strnlen(value, R0LAB_TOKEN_MAX + 1) > R0LAB_TOKEN_MAX ||
            r0lab_parse_u64(value, &token))
            return R0LAB_EINVAL;
        return r0lab_raw_abort_probe_status(token, out_msg, outlen);
    }
    if (!strncmp(args, "raw xom read fault clear ", 25)) {
        value = args + 25;
        if (strnlen(value, R0LAB_TOKEN_MAX + 1) > R0LAB_TOKEN_MAX ||
            r0lab_parse_u64(value, &token))
            return R0LAB_EINVAL;
        return r0lab_raw_abort_probe_clear(token, out_msg, outlen);
    }
    if (!strncmp(args, "raw xom preflight arm ", 22)) {
        value = args + 22;
        if (strnlen(value, R0LAB_TOKEN_MAX + 1) > R0LAB_TOKEN_MAX ||
            r0lab_parse_u64(value, &token))
            return R0LAB_EINVAL;
        return r0lab_raw_xom_preflight_arm(token, out_msg, outlen);
    }
    if (!strncmp(args, "raw xom arm ", 12)) {
        return R0LAB_ENOSYS;
    }
    if (!strncmp(args, "raw ready ", 10)) {
        value = args + 10;
        if (strnlen(value, R0LAB_TOKEN_MAX + 1) > R0LAB_TOKEN_MAX ||
            r0lab_parse_u64(value, &token))
            return R0LAB_EINVAL;
        return r0lab_raw_ready(token, out_msg, outlen);
    }
    if (!strncmp(args, "raw observed ", 13)) {
        value = args + 13;
        if (strnlen(value, R0LAB_TOKEN_MAX + 1) > R0LAB_TOKEN_MAX ||
            r0lab_parse_u64(value, &token))
            return R0LAB_EINVAL;
        return r0lab_raw_observed(token, out_msg, outlen);
    }
    if (!strncmp(args, "raw inspect ", 12)) {
        value = args + 12;
        if (strnlen(value, R0LAB_TOKEN_MAX + 1) > R0LAB_TOKEN_MAX ||
            r0lab_parse_u64(value, &token))
            return R0LAB_EINVAL;
        return r0lab_raw_inspect(token, out_msg, outlen);
    }
    if (!strncmp(args, "raw read cycle begin ", 21)) {
        value = args + 21;
        if (strnlen(value, R0LAB_TOKEN_MAX + 1) > R0LAB_TOKEN_MAX ||
            r0lab_parse_u64(value, &token))
            return R0LAB_EINVAL;
        return r0lab_raw_read_cycle_begin_cmd(token, out_msg, outlen);
    }
    if (!strncmp(args, "raw read cycle status ", 22)) {
        value = args + 22;
        if (strnlen(value, R0LAB_TOKEN_MAX + 1) > R0LAB_TOKEN_MAX ||
            r0lab_parse_u64(value, &token))
            return R0LAB_EINVAL;
        return r0lab_raw_read_cycle_status(token, out_msg, outlen);
    }
    if (!strncmp(args, "raw slot fault hook arm ", 24)) {
        value = args + 24;
        if (r0lab_parse_u64_pair(value, &token, &slot_value) ||
            r0lab_raw_parse_slot_id(slot_value, &slot_id))
            return R0LAB_EINVAL;
        return r0lab_raw_fault_hook_arm_common(token, slot_id, out_msg,
                                               outlen, false);
    }
    if (!strncmp(args, "raw slot fault hook status ", 27)) {
        value = args + 27;
        if (r0lab_parse_u64_pair(value, &token, &slot_value) ||
            r0lab_raw_parse_slot_id(slot_value, &slot_id))
            return R0LAB_EINVAL;
        return r0lab_raw_fault_hook_status_common(token, slot_id, out_msg,
                                                  outlen, false);
    }
    if (!strncmp(args, "raw slot fault af clear ", 24)) {
        value = args + 24;
        if (r0lab_parse_u64_triplet(value, &token, &slot_value,
                                    &generation) ||
            r0lab_raw_parse_slot_id(slot_value, &slot_id))
            return R0LAB_EINVAL;
        return r0lab_raw_fault_af_clear(token, slot_id, generation,
                                        out_msg, outlen);
    }
    if (!strncmp(args, "raw slot fault hook clear ", 26)) {
        value = args + 26;
        if (r0lab_parse_u64_pair(value, &token, &slot_value) ||
            r0lab_raw_parse_slot_id(slot_value, &slot_id))
            return R0LAB_EINVAL;
        return r0lab_raw_fault_hook_clear_common(token, slot_id, out_msg,
                                                 outlen, false);
    }
    if (!strncmp(args, "raw fault probe arm ", 20)) {
        value = args + 20;
        if (r0lab_parse_u64_pair(value, &token, &page_address))
            return R0LAB_EINVAL;
        return r0lab_raw_fault_probe_arm(token, page_address, out_msg, outlen);
    }
    if (!strncmp(args, "raw fault probe reader ", 23)) {
        uint64_t reader_tgid;

        value = args + 23;
        if (r0lab_parse_u64_pair(value, &token, &reader_tgid))
            return R0LAB_EINVAL;
        return r0lab_raw_fault_probe_reader(token, reader_tgid, out_msg,
                                            outlen);
    }
    if (!strncmp(args, "raw fault probe status ", 23)) {
        value = args + 23;
        if (strnlen(value, R0LAB_TOKEN_MAX + 1) > R0LAB_TOKEN_MAX ||
            r0lab_parse_u64(value, &token))
            return R0LAB_EINVAL;
        return r0lab_raw_fault_probe_status(token, out_msg, outlen);
    }
    if (!strncmp(args, "raw fault probe clear ", 22)) {
        value = args + 22;
        if (strnlen(value, R0LAB_TOKEN_MAX + 1) > R0LAB_TOKEN_MAX ||
            r0lab_parse_u64(value, &token))
            return R0LAB_EINVAL;
        return r0lab_raw_fault_probe_clear(token, out_msg, outlen);
    }
    if (!strncmp(args, "raw fault hook arm ", 19)) {
        value = args + 19;
        if (strnlen(value, R0LAB_TOKEN_MAX + 1) > R0LAB_TOKEN_MAX ||
            r0lab_parse_u64(value, &token))
            return R0LAB_EINVAL;
        return r0lab_raw_fault_hook_arm(token, out_msg, outlen);
    }
    if (!strncmp(args, "raw fault hook status ", 22)) {
        value = args + 22;
        if (strnlen(value, R0LAB_TOKEN_MAX + 1) > R0LAB_TOKEN_MAX ||
            r0lab_parse_u64(value, &token))
            return R0LAB_EINVAL;
        return r0lab_raw_fault_hook_status(token, out_msg, outlen);
    }
    if (!strncmp(args, "raw fault hook clear ", 21)) {
        value = args + 21;
        if (strnlen(value, R0LAB_TOKEN_MAX + 1) > R0LAB_TOKEN_MAX ||
            r0lab_parse_u64(value, &token))
            return R0LAB_EINVAL;
        return r0lab_raw_fault_hook_clear(token, out_msg, outlen);
    }
    if (!strncmp(args, "raw slot abort write probe arm ", 31)) {
        value = args + 31;
        if (r0lab_parse_u64_pair(value, &token, &slot_value) ||
            r0lab_raw_parse_slot_id(slot_value, &slot_id))
            return R0LAB_EINVAL;
        return r0lab_raw_abort_probe_arm_common(
            token, slot_id, out_msg, outlen,
            R0LAB_ABORT_PROBE_SOURCE_WRITE_PERMISSION, false);
    }
    if (!strncmp(args, "raw slot abort write probe status ", 34)) {
        value = args + 34;
        if (r0lab_parse_u64_pair(value, &token, &slot_value) ||
            r0lab_raw_parse_slot_id(slot_value, &slot_id))
            return R0LAB_EINVAL;
        return r0lab_raw_abort_probe_status_common(
            token, slot_id, out_msg, outlen, false);
    }
    if (!strncmp(args, "raw slot abort write probe clear ", 33)) {
        value = args + 33;
        if (r0lab_parse_u64_pair(value, &token, &slot_value) ||
            r0lab_raw_parse_slot_id(slot_value, &slot_id))
            return R0LAB_EINVAL;
        return r0lab_raw_abort_probe_clear_common(
            token, slot_id, out_msg, outlen, false);
    }
    if (!strncmp(args, "raw slot abort write release arm ", 33)) {
        value = args + 33;
        if (r0lab_parse_u64_pair(value, &token, &slot_value) ||
            r0lab_raw_parse_slot_id(slot_value, &slot_id))
            return R0LAB_EINVAL;
        return r0lab_raw_abort_write_release_arm_common(
            token, slot_id, out_msg, outlen, false);
    }
    if (!strncmp(args, "raw slot abort write release status ", 36)) {
        value = args + 36;
        if (r0lab_parse_u64_pair(value, &token, &slot_value) ||
            r0lab_raw_parse_slot_id(slot_value, &slot_id))
            return R0LAB_EINVAL;
        return r0lab_raw_abort_write_release_status_common(
            token, slot_id, out_msg, outlen, false);
    }
    if (!strncmp(args, "raw slot abort write release clear ", 35)) {
        value = args + 35;
        if (r0lab_parse_u64_pair(value, &token, &slot_value) ||
            r0lab_raw_parse_slot_id(slot_value, &slot_id))
            return R0LAB_EINVAL;
        return r0lab_raw_abort_write_release_clear_common(
            token, slot_id, out_msg, outlen, false);
    }
    if (!strncmp(args, "raw slot abort read cycle arm ", 30)) {
        value = args + 30;
        if (r0lab_parse_u64_pair(value, &token, &slot_value) ||
            r0lab_raw_parse_slot_id(slot_value, &slot_id))
            return R0LAB_EINVAL;
        return r0lab_raw_abort_read_cycle_arm_common(
            token, slot_id, out_msg, outlen, false);
    }
    if (!strncmp(args, "raw slot abort read cycle status ", 33)) {
        value = args + 33;
        if (r0lab_parse_u64_pair(value, &token, &slot_value) ||
            r0lab_raw_parse_slot_id(slot_value, &slot_id))
            return R0LAB_EINVAL;
        return r0lab_raw_abort_read_cycle_status_common(
            token, slot_id, out_msg, outlen, false);
    }
    if (!strncmp(args, "raw slot abort read cycle clear ", 32)) {
        value = args + 32;
        if (r0lab_parse_u64_pair(value, &token, &slot_value) ||
            r0lab_raw_parse_slot_id(slot_value, &slot_id))
            return R0LAB_EINVAL;
        return r0lab_raw_abort_read_cycle_clear_common(
            token, slot_id, out_msg, outlen, false);
    }
    if (!strncmp(args, "raw slot abort probe arm ", 25)) {
        value = args + 25;
        if (r0lab_parse_u64_pair(value, &token, &slot_value) ||
            r0lab_raw_parse_slot_id(slot_value, &slot_id))
            return R0LAB_EINVAL;
        return r0lab_raw_abort_probe_arm_common(
            token, slot_id, out_msg, outlen,
            R0LAB_ABORT_PROBE_SOURCE_READ_TRANSLATION, false);
    }
    if (!strncmp(args, "raw slot abort probe status ", 28)) {
        value = args + 28;
        if (r0lab_parse_u64_pair(value, &token, &slot_value) ||
            r0lab_raw_parse_slot_id(slot_value, &slot_id))
            return R0LAB_EINVAL;
        return r0lab_raw_abort_probe_status_common(
            token, slot_id, out_msg, outlen, false);
    }
    if (!strncmp(args, "raw slot abort probe clear ", 27)) {
        value = args + 27;
        if (r0lab_parse_u64_pair(value, &token, &slot_value) ||
            r0lab_raw_parse_slot_id(slot_value, &slot_id))
            return R0LAB_EINVAL;
        return r0lab_raw_abort_probe_clear_common(
            token, slot_id, out_msg, outlen, false);
    }
    if (!strncmp(args, "raw abort write probe arm ", 26)) {
        value = args + 26;
        if (strnlen(value, R0LAB_TOKEN_MAX + 1) > R0LAB_TOKEN_MAX ||
            r0lab_parse_u64(value, &token))
            return R0LAB_EINVAL;
        return r0lab_raw_abort_write_probe_arm(token, out_msg, outlen);
    }
    if (!strncmp(args, "raw abort write probe status ", 29)) {
        value = args + 29;
        if (strnlen(value, R0LAB_TOKEN_MAX + 1) > R0LAB_TOKEN_MAX ||
            r0lab_parse_u64(value, &token))
            return R0LAB_EINVAL;
        return r0lab_raw_abort_probe_status(token, out_msg, outlen);
    }
    if (!strncmp(args, "raw abort write probe clear ", 28)) {
        value = args + 28;
        if (strnlen(value, R0LAB_TOKEN_MAX + 1) > R0LAB_TOKEN_MAX ||
            r0lab_parse_u64(value, &token))
            return R0LAB_EINVAL;
        return r0lab_raw_abort_probe_clear(token, out_msg, outlen);
    }
    if (!strncmp(args, "raw abort write release arm ", 28)) {
        value = args + 28;
        if (strnlen(value, R0LAB_TOKEN_MAX + 1) > R0LAB_TOKEN_MAX ||
            r0lab_parse_u64(value, &token))
            return R0LAB_EINVAL;
        return r0lab_raw_abort_write_release_arm(token, out_msg, outlen);
    }
    if (!strncmp(args, "raw abort write release status ", 31)) {
        value = args + 31;
        if (strnlen(value, R0LAB_TOKEN_MAX + 1) > R0LAB_TOKEN_MAX ||
            r0lab_parse_u64(value, &token))
            return R0LAB_EINVAL;
        return r0lab_raw_abort_write_release_status(token, out_msg, outlen);
    }
    if (!strncmp(args, "raw abort write release clear ", 30)) {
        value = args + 30;
        if (strnlen(value, R0LAB_TOKEN_MAX + 1) > R0LAB_TOKEN_MAX ||
            r0lab_parse_u64(value, &token))
            return R0LAB_EINVAL;
        return r0lab_raw_abort_write_release_clear(token, out_msg, outlen);
    }
    if (!strncmp(args, "raw abort read cycle arm ", 25)) {
        value = args + 25;
        if (strnlen(value, R0LAB_TOKEN_MAX + 1) > R0LAB_TOKEN_MAX ||
            r0lab_parse_u64(value, &token))
            return R0LAB_EINVAL;
        return r0lab_raw_abort_read_cycle_arm(token, out_msg, outlen);
    }
    if (!strncmp(args, "raw abort read cycle status ", 28)) {
        value = args + 28;
        if (strnlen(value, R0LAB_TOKEN_MAX + 1) > R0LAB_TOKEN_MAX ||
            r0lab_parse_u64(value, &token))
            return R0LAB_EINVAL;
        return r0lab_raw_abort_read_cycle_status(token, out_msg, outlen);
    }
    if (!strncmp(args, "raw abort read cycle clear ", 27)) {
        value = args + 27;
        if (strnlen(value, R0LAB_TOKEN_MAX + 1) > R0LAB_TOKEN_MAX ||
            r0lab_parse_u64(value, &token))
            return R0LAB_EINVAL;
        return r0lab_raw_abort_read_cycle_clear(token, out_msg, outlen);
    }
    if (!strncmp(args, "raw abort probe arm ", 20)) {
        value = args + 20;
        if (strnlen(value, R0LAB_TOKEN_MAX + 1) > R0LAB_TOKEN_MAX ||
            r0lab_parse_u64(value, &token))
            return R0LAB_EINVAL;
        return r0lab_raw_abort_probe_arm(token, out_msg, outlen);
    }
    if (!strncmp(args, "raw abort probe status ", 23)) {
        value = args + 23;
        if (strnlen(value, R0LAB_TOKEN_MAX + 1) > R0LAB_TOKEN_MAX ||
            r0lab_parse_u64(value, &token))
            return R0LAB_EINVAL;
        return r0lab_raw_abort_probe_status(token, out_msg, outlen);
    }
    if (!strncmp(args, "raw abort probe clear ", 22)) {
        value = args + 22;
        if (strnlen(value, R0LAB_TOKEN_MAX + 1) > R0LAB_TOKEN_MAX ||
            r0lab_parse_u64(value, &token))
            return R0LAB_EINVAL;
        return r0lab_raw_abort_probe_clear(token, out_msg, outlen);
    }
    if (!strncmp(args, "raw slot syscall hook arm ", 26)) {
        value = args + 26;
        if (r0lab_parse_u64_triplet(value, &token, &slot_value,
                                    &generation) ||
            r0lab_raw_parse_slot_id(slot_value, &slot_id))
            return R0LAB_EINVAL;
        return r0lab_raw_syscall_hook_arm_common(
            token, slot_id, generation, out_msg, outlen, false, false);
    }
    if (!strncmp(args, "raw slot syscall hook status ", 29)) {
        value = args + 29;
        if (r0lab_parse_u64_pair(value, &token, &slot_value) ||
            r0lab_raw_parse_slot_id(slot_value, &slot_id))
            return R0LAB_EINVAL;
        return r0lab_raw_syscall_hook_status_common(
            token, slot_id, out_msg, outlen, false);
    }
    if (!strncmp(args, "raw slot syscall hook select ", 29)) {
        value = args + 29;
        if (r0lab_parse_u64_triplet(value, &token, &slot_value,
                                    &generation) ||
            r0lab_raw_parse_slot_id(slot_value, &slot_id))
            return R0LAB_EINVAL;
        return r0lab_raw_syscall_hook_select(token, slot_id, generation,
                                             out_msg, outlen);
    }
    if (!strncmp(args, "raw slot syscall hook clear ", 28)) {
        value = args + 28;
        if (r0lab_parse_u64_triplet(value, &token, &slot_value,
                                    &generation) ||
            r0lab_raw_parse_slot_id(slot_value, &slot_id))
            return R0LAB_EINVAL;
        return r0lab_raw_syscall_hook_clear_common(
            token, slot_id, generation, out_msg, outlen, false);
    }
    if (!strncmp(args, "raw slot syscall read cycle hook arm ", 37)) {
        value = args + 37;
        if (r0lab_parse_u64_triplet(value, &token, &slot_value,
                                    &generation) ||
            r0lab_raw_parse_slot_id(slot_value, &slot_id))
            return R0LAB_EINVAL;
        return r0lab_raw_syscall_hook_arm_common(
            token, slot_id, generation, out_msg, outlen, true, false);
    }
    if (!strncmp(args, "raw slot syscall read cycle hook status ", 40)) {
        value = args + 40;
        if (r0lab_parse_u64_pair(value, &token, &slot_value) ||
            r0lab_raw_parse_slot_id(slot_value, &slot_id))
            return R0LAB_EINVAL;
        return r0lab_raw_syscall_hook_status_common(
            token, slot_id, out_msg, outlen, false);
    }
    if (!strncmp(args, "raw slot syscall read cycle hook select ", 40)) {
        value = args + 40;
        if (r0lab_parse_u64_triplet(value, &token, &slot_value,
                                    &generation) ||
            r0lab_raw_parse_slot_id(slot_value, &slot_id))
            return R0LAB_EINVAL;
        return r0lab_raw_syscall_hook_select(token, slot_id, generation,
                                             out_msg, outlen);
    }
    if (!strncmp(args, "raw slot syscall read cycle hook clear ", 39)) {
        value = args + 39;
        if (r0lab_parse_u64_triplet(value, &token, &slot_value,
                                    &generation) ||
            r0lab_raw_parse_slot_id(slot_value, &slot_id))
            return R0LAB_EINVAL;
        return r0lab_raw_syscall_hook_clear_common(
            token, slot_id, generation, out_msg, outlen, false);
    }
    if (!strncmp(args, "raw syscall hook arm ", 21)) {
        value = args + 21;
        if (strnlen(value, R0LAB_TOKEN_MAX + 1) > R0LAB_TOKEN_MAX ||
            r0lab_parse_u64(value, &token))
            return R0LAB_EINVAL;
        return r0lab_raw_syscall_hook_arm(token, out_msg, outlen);
    }
    if (!strncmp(args, "raw syscall hook status ", 24)) {
        value = args + 24;
        if (strnlen(value, R0LAB_TOKEN_MAX + 1) > R0LAB_TOKEN_MAX ||
            r0lab_parse_u64(value, &token))
            return R0LAB_EINVAL;
        return r0lab_raw_syscall_hook_status(token, out_msg, outlen);
    }
    if (!strncmp(args, "raw syscall hook clear ", 23)) {
        value = args + 23;
        if (strnlen(value, R0LAB_TOKEN_MAX + 1) > R0LAB_TOKEN_MAX ||
            r0lab_parse_u64(value, &token))
            return R0LAB_EINVAL;
        return r0lab_raw_syscall_hook_clear(token, out_msg, outlen);
    }
    if (!strncmp(args, "raw syscall read cycle hook arm ", 32)) {
        value = args + 32;
        if (strnlen(value, R0LAB_TOKEN_MAX + 1) > R0LAB_TOKEN_MAX ||
            r0lab_parse_u64(value, &token))
            return R0LAB_EINVAL;
        return r0lab_raw_syscall_read_cycle_hook_arm(token, out_msg,
                                                     outlen);
    }
    if (!strncmp(args, "raw syscall read cycle hook status ", 35)) {
        value = args + 35;
        if (strnlen(value, R0LAB_TOKEN_MAX + 1) > R0LAB_TOKEN_MAX ||
            r0lab_parse_u64(value, &token))
            return R0LAB_EINVAL;
        return r0lab_raw_syscall_hook_status(token, out_msg, outlen);
    }
    if (!strncmp(args, "raw syscall read cycle hook clear ", 34)) {
        value = args + 34;
        if (strnlen(value, R0LAB_TOKEN_MAX + 1) > R0LAB_TOKEN_MAX ||
            r0lab_parse_u64(value, &token))
            return R0LAB_EINVAL;
        return r0lab_raw_syscall_hook_clear(token, out_msg, outlen);
    }
    if (!strncmp(args, "raw slot prctl hook arm ", 24)) {
        value = args + 24;
        if (r0lab_parse_u64_triplet(value, &token, &slot_value,
                                    &generation) ||
            r0lab_raw_parse_slot_id(slot_value, &slot_id))
            return R0LAB_EINVAL;
        return r0lab_raw_prctl_hook_arm_common(
            token, slot_id, generation, out_msg, outlen, false);
    }
    if (!strncmp(args, "raw slot prctl hook status ", 27)) {
        value = args + 27;
        if (r0lab_parse_u64_pair(value, &token, &slot_value) ||
            r0lab_raw_parse_slot_id(slot_value, &slot_id))
            return R0LAB_EINVAL;
        return r0lab_raw_prctl_hook_status_common(
            token, slot_id, out_msg, outlen, false);
    }
    if (!strncmp(args, "raw slot prctl hook select ", 27)) {
        value = args + 27;
        if (r0lab_parse_u64_triplet(value, &token, &slot_value,
                                    &generation) ||
            r0lab_raw_parse_slot_id(slot_value, &slot_id))
            return R0LAB_EINVAL;
        return r0lab_raw_prctl_hook_select(token, slot_id, generation,
                                           out_msg, outlen);
    }
    if (!strncmp(args, "raw slot prctl hook clear ", 26)) {
        value = args + 26;
        if (r0lab_parse_u64_triplet(value, &token, &slot_value,
                                    &generation) ||
            r0lab_raw_parse_slot_id(slot_value, &slot_id))
            return R0LAB_EINVAL;
        return r0lab_raw_prctl_hook_clear_common(
            token, slot_id, generation, out_msg, outlen, false);
    }
    if (!strncmp(args, "raw prctl hook arm ", 19)) {
        value = args + 19;
        if (strnlen(value, R0LAB_TOKEN_MAX + 1) > R0LAB_TOKEN_MAX ||
            r0lab_parse_u64(value, &token))
            return R0LAB_EINVAL;
        return r0lab_raw_prctl_hook_arm(token, out_msg, outlen);
    }
    if (!strncmp(args, "raw prctl hook status ", 22)) {
        value = args + 22;
        if (strnlen(value, R0LAB_TOKEN_MAX + 1) > R0LAB_TOKEN_MAX ||
            r0lab_parse_u64(value, &token))
            return R0LAB_EINVAL;
        return r0lab_raw_prctl_hook_status(token, out_msg, outlen);
    }
    if (!strncmp(args, "raw prctl hook clear ", 21)) {
        value = args + 21;
        if (strnlen(value, R0LAB_TOKEN_MAX + 1) > R0LAB_TOKEN_MAX ||
            r0lab_parse_u64(value, &token))
            return R0LAB_EINVAL;
        return r0lab_raw_prctl_hook_clear(token, out_msg, outlen);
    }
    if (!strncmp(args, "raw slot exit hook arm ", 23)) {
        value = args + 23;
        if (r0lab_parse_u64_triplet(value, &token, &slot_value,
                                    &generation) ||
            r0lab_raw_parse_slot_id(slot_value, &slot_id))
            return R0LAB_EINVAL;
        return r0lab_raw_exit_hook_arm_common(
            token, slot_id, generation, out_msg, outlen, false);
    }
    if (!strncmp(args, "raw slot exit hook status ", 26)) {
        value = args + 26;
        if (r0lab_parse_u64_pair(value, &token, &slot_value) ||
            r0lab_raw_parse_slot_id(slot_value, &slot_id))
            return R0LAB_EINVAL;
        return r0lab_raw_exit_hook_status_common(
            token, slot_id, out_msg, outlen, false);
    }
    if (!strncmp(args, "raw slot exit hook clear ", 25)) {
        value = args + 25;
        if (r0lab_parse_u64_triplet(value, &token, &slot_value,
                                    &generation) ||
            r0lab_raw_parse_slot_id(slot_value, &slot_id))
            return R0LAB_EINVAL;
        return r0lab_raw_exit_hook_clear_common(
            token, slot_id, generation, out_msg, outlen, false);
    }
    if (!strncmp(args, "raw exit hook arm ", 18)) {
        value = args + 18;
        if (strnlen(value, R0LAB_TOKEN_MAX + 1) > R0LAB_TOKEN_MAX ||
            r0lab_parse_u64(value, &token))
            return R0LAB_EINVAL;
        return r0lab_raw_exit_hook_arm(token, out_msg, outlen);
    }
    if (!strncmp(args, "raw exit hook status ", 21)) {
        value = args + 21;
        if (strnlen(value, R0LAB_TOKEN_MAX + 1) > R0LAB_TOKEN_MAX ||
            r0lab_parse_u64(value, &token))
            return R0LAB_EINVAL;
        return r0lab_raw_exit_hook_status(token, out_msg, outlen);
    }
    if (!strncmp(args, "raw exit hook clear ", 20)) {
        value = args + 20;
        if (strnlen(value, R0LAB_TOKEN_MAX + 1) > R0LAB_TOKEN_MAX ||
            r0lab_parse_u64(value, &token))
            return R0LAB_EINVAL;
        return r0lab_raw_exit_hook_clear(token, out_msg, outlen);
    }
    if (!strncmp(args, "raw gup hook arm ", 17)) {
        value = args + 17;
        if (strnlen(value, R0LAB_TOKEN_MAX + 1) > R0LAB_TOKEN_MAX ||
            r0lab_parse_u64(value, &token))
            return R0LAB_EINVAL;
        return r0lab_raw_gup_hook_arm(token, out_msg, outlen);
    }
    if (!strncmp(args, "raw gup hook status ", 20)) {
        value = args + 20;
        if (strnlen(value, R0LAB_TOKEN_MAX + 1) > R0LAB_TOKEN_MAX ||
            r0lab_parse_u64(value, &token))
            return R0LAB_EINVAL;
        return r0lab_raw_gup_hook_status(token, out_msg, outlen);
    }
    if (!strncmp(args, "raw gup hook clear ", 19)) {
        value = args + 19;
        if (strnlen(value, R0LAB_TOKEN_MAX + 1) > R0LAB_TOKEN_MAX ||
            r0lab_parse_u64(value, &token))
            return R0LAB_EINVAL;
        return r0lab_raw_gup_hook_clear(token, out_msg, outlen);
    }
    if (!strncmp(args, "raw slot gup hook arm ", 22)) {
        value = args + 22;
        if (r0lab_parse_u64_pair(value, &token, &slot_value) ||
            r0lab_raw_parse_slot_id(slot_value, &slot_id))
            return R0LAB_EINVAL;
        return r0lab_raw_gup_hook_arm_common(token, slot_id, out_msg,
                                             outlen, false);
    }
    if (!strncmp(args, "raw slot gup hook status ", 25)) {
        value = args + 25;
        if (r0lab_parse_u64_pair(value, &token, &slot_value) ||
            r0lab_raw_parse_slot_id(slot_value, &slot_id))
            return R0LAB_EINVAL;
        return r0lab_raw_gup_hook_status_common(token, slot_id, out_msg,
                                                outlen, false);
    }
    if (!strncmp(args, "raw slot gup hook clear ", 24)) {
        value = args + 24;
        if (r0lab_parse_u64_pair(value, &token, &slot_value) ||
            r0lab_raw_parse_slot_id(slot_value, &slot_id))
            return R0LAB_EINVAL;
        return r0lab_raw_gup_hook_clear_common(token, slot_id, out_msg,
                                               outlen, false);
    }
    if (!strncmp(args, "raw slot fork hook arm ", 23)) {
        value = args + 23;
        if (r0lab_parse_u64_pair(value, &token, &slot_value) ||
            r0lab_raw_parse_slot_id(slot_value, &slot_id))
            return R0LAB_EINVAL;
        return r0lab_raw_fork_hook_arm_common(token, slot_id, out_msg,
                                              outlen, false);
    }
    if (!strncmp(args, "raw slot fork hook status ", 26)) {
        value = args + 26;
        if (r0lab_parse_u64_pair(value, &token, &slot_value) ||
            r0lab_raw_parse_slot_id(slot_value, &slot_id))
            return R0LAB_EINVAL;
        return r0lab_raw_fork_hook_status_common(token, slot_id, out_msg,
                                                 outlen, false);
    }
    if (!strncmp(args, "raw slot fork hook clear ", 25)) {
        value = args + 25;
        if (r0lab_parse_u64_pair(value, &token, &slot_value) ||
            r0lab_raw_parse_slot_id(slot_value, &slot_id))
            return R0LAB_EINVAL;
        return r0lab_raw_fork_hook_clear_common(token, slot_id, out_msg,
                                                outlen, false);
    }
    if (!strncmp(args, "raw fork hook arm ", 18)) {
        value = args + 18;
        if (strnlen(value, R0LAB_TOKEN_MAX + 1) > R0LAB_TOKEN_MAX ||
            r0lab_parse_u64(value, &token))
            return R0LAB_EINVAL;
        return r0lab_raw_fork_hook_arm(token, out_msg, outlen);
    }
    if (!strncmp(args, "raw fork hook status ", 21)) {
        value = args + 21;
        if (strnlen(value, R0LAB_TOKEN_MAX + 1) > R0LAB_TOKEN_MAX ||
            r0lab_parse_u64(value, &token))
            return R0LAB_EINVAL;
        return r0lab_raw_fork_hook_status(token, out_msg, outlen);
    }
    if (!strncmp(args, "raw fork hook clear ", 20)) {
        value = args + 20;
        if (strnlen(value, R0LAB_TOKEN_MAX + 1) > R0LAB_TOKEN_MAX ||
            r0lab_parse_u64(value, &token))
            return R0LAB_EINVAL;
        return r0lab_raw_fork_hook_clear(token, out_msg, outlen);
    }
    if (!strncmp(args, "raw gup begin ", 14)) {
        value = args + 14;
        if (strnlen(value, R0LAB_TOKEN_MAX + 1) > R0LAB_TOKEN_MAX ||
            r0lab_parse_u64(value, &token))
            return R0LAB_EINVAL;
        return r0lab_raw_gup_hide_transition(token, true, out_msg, outlen);
    }
    if (!strncmp(args, "raw gup finish ", 15)) {
        value = args + 15;
        if (strnlen(value, R0LAB_TOKEN_MAX + 1) > R0LAB_TOKEN_MAX ||
            r0lab_parse_u64(value, &token))
            return R0LAB_EINVAL;
        return r0lab_raw_gup_hide_transition(token, false, out_msg, outlen);
    }
    if (!strncmp(args, "raw slot clear ", 15)) {
        value = args + 15;
        if (r0lab_parse_u64_pair(value, &token, &slot_value) ||
            r0lab_raw_parse_slot_id(slot_value, &slot_id))
            return R0LAB_EINVAL;
        return r0lab_raw_slot_clear(token, slot_id, out_msg, outlen);
    }
    if (!strncmp(args, "raw slot cleared ", 17)) {
        value = args + 17;
        if (r0lab_parse_u64_pair(value, &token, &slot_value) ||
            r0lab_raw_parse_slot_id(slot_value, &slot_id))
            return R0LAB_EINVAL;
        return r0lab_raw_slot_cleared(token, slot_id, out_msg, outlen);
    }
    if (!strncmp(args, "raw clear ", 10)) {
        value = args + 10;
        if (strnlen(value, R0LAB_TOKEN_MAX + 1) > R0LAB_TOKEN_MAX ||
            r0lab_parse_u64(value, &token))
            return R0LAB_EINVAL;
        return r0lab_raw_clear(token, out_msg, outlen);
    }
    if (!strncmp(args, "raw cleared ", 12)) {
        value = args + 12;
        if (strnlen(value, R0LAB_TOKEN_MAX + 1) > R0LAB_TOKEN_MAX ||
            r0lab_parse_u64(value, &token))
            return R0LAB_EINVAL;
        return r0lab_raw_cleared(token, out_msg, outlen);
    }
    return R0LAB_ENOENT;
}

static long r0lab_init(const char *args, const char *event, void *reserved)
{
    int result;

    (void)reserved;
    spin_lock_init(&g_r0lab_lock);
    memset(g_events, 0, sizeof(g_events));
    memset(&g_session, 0, sizeof(g_session));
    memset(g_hwbp_slots, 0, sizeof(g_hwbp_slots));
    memset(&g_m3_page, 0, sizeof(g_m3_page));
    memset(&g_m4_page, 0, sizeof(g_m4_page));
    r0lab_raw_page_table_reset_locked();
    memset(&g_s4_brk, 0, sizeof(g_s4_brk));
    g_next_seq = 0;
    g_m3_generation = 0;
    g_m4_generation = 0;
    g_raw_generation = 0;
    g_s4_generation = 0;
    g_hwbp_entry_events = 0;
    g_hwbp_return_events = 0;
    g_hwbp_callback_log_count = 0;
    g_exit_probe_armed = false;
    g_m3_inflight = 0;
    g_m4_inflight = 0;
    g_raw_inflight = 0;
    g_s4_inflight = 0;
    g_m3_arm_delay_once = false;
    g_m3_arm_enomem_once = false;
    memset(g_hwbp_workers, 0, sizeof(g_hwbp_workers));
    g_m3_worker = NULL;
    g_m3_monitor_worker_task = NULL;
    g_m4_worker = NULL;
    g_m4_monitor_worker_task = NULL;
    g_raw_worker = NULL;
    g_raw_monitor_worker_task = NULL;
    g_s4_monitor_worker_task = NULL;
    g_session_monitor_worker_task = NULL;
    g_workers_started = false;
    g_workers_shutdown_requested = false;
    g_workers_live = 0;
    g_raw_abort_hook_transitioning = false;
    g_raw_abort_resident_callback = NULL;
    g_raw_exit_hook_resident = false;
    g_raw_exit_hook_transitioning = false;
    g_clock = (r0lab_clock_fn_t)kallsyms_lookup_name("sched_clock");
    g_current_cpu = (r0lab_current_cpu_fn_t)
        kallsyms_lookup_name("bpf_get_smp_processor_id");
    g_task_pid = (r0lab_task_pid_fn_t)kallsyms_lookup_name("__task_pid_nr_ns");
    g_lock_irqsave = (r0lab_lock_irqsave_fn_t)kallsyms_lookup_name("_raw_spin_lock_irqsave");
    g_unlock_irqrestore = (r0lab_unlock_irqrestore_fn_t)kallsyms_lookup_name("_raw_spin_unlock_irqrestore");
    g_register_hwbp = (r0lab_register_hwbp_fn_t)kallsyms_lookup_name("register_user_hw_breakpoint");
    g_disable_hwbp_inatomic = (r0lab_disable_hwbp_inatomic_fn_t)
        kallsyms_lookup_name("perf_event_disable_inatomic");
    g_disable_hwbp_local = (r0lab_disable_hwbp_local_fn_t)
        kallsyms_lookup_name("perf_event_disable_local");
    g_unregister_hwbp = (r0lab_unregister_hwbp_fn_t)kallsyms_lookup_name("unregister_hw_breakpoint");
    g_synchronize_rcu = (r0lab_synchronize_rcu_fn_t)kallsyms_lookup_name("synchronize_rcu");
    g_find_get_task = (r0lab_find_get_task_fn_t)kallsyms_lookup_name("find_get_task_by_vpid");
    g_put_task = (r0lab_put_task_fn_t)kallsyms_lookup_name("put_task_struct");
    g_kthread_create = (r0lab_kthread_create_fn_t)kallsyms_lookup_name("kthread_create_on_node");
    g_wake_up_process = (r0lab_wake_up_process_fn_t)kallsyms_lookup_name("wake_up_process");
    g_kthread_stop = (r0lab_kthread_stop_fn_t)kallsyms_lookup_name("kthread_stop");
    g_kthread_should_stop = (r0lab_kthread_should_stop_fn_t)
        kallsyms_lookup_name("kthread_should_stop");
    g_msleep = (r0lab_msleep_fn_t)kallsyms_lookup_name("msleep");
    g_get_task_mm = (r0lab_get_task_mm_fn_t)kallsyms_lookup_name("get_task_mm");
    g_mmput = (r0lab_mmput_fn_t)kallsyms_lookup_name("mmput");
    g_mmdrop = (r0lab_mmdrop_fn_t)
        r0lab_lookup_first("__mmdrop.cfi_jt", "__mmdrop");
    g_get_free_pages = (r0lab_get_free_pages_fn_t)
        r0lab_lookup_first("__get_free_pages.cfi_jt", "__get_free_pages");
    g_free_pages = (r0lab_free_pages_fn_t)
        r0lab_lookup_first("free_pages.cfi_jt", "free_pages");
    g_do_mem_abort = (void *)kallsyms_lookup_name("do_mem_abort");
    g_handle_mm_fault = r0lab_lookup_first("handle_mm_fault.cfi_jt",
                                           "handle_mm_fault");
    g_dup_mmap = r0lab_lookup_first("dup_mmap.cfi_jt", "dup_mmap");
    g_exit_mmap = r0lab_lookup_first("exit_mmap.cfi_jt", "exit_mmap");
    g_sys_getpid = r0lab_lookup_first("__arm64_sys_getpid.cfi_jt",
                                      "__arm64_sys_getpid");
    g_sys_prctl = r0lab_lookup_first("__arm64_sys_prctl.cfi_jt",
                                     "__arm64_sys_prctl");
    g_copy_from_user_nofault = (r0lab_copy_from_user_nofault_fn_t)
        r0lab_lookup_first("copy_from_user_nofault.cfi_jt",
                           "copy_from_user_nofault");
    g_follow_page_pte = r0lab_lookup_first("follow_page_pte.cfi_jt",
                                           "follow_page_pte");
    g_follow_page_mask = r0lab_lookup_first("follow_page_mask.cfi_jt",
                                            "follow_page_mask");
    g_s4_brk_handler = r0lab_lookup_first("brk_handler.cfi_jt", "brk_handler");
    g_s4_single_step_handler =
        r0lab_lookup_first("single_step_handler.cfi_jt", "single_step_handler");
    g_s4_user_enable_single_step = (r0lab_user_step_fn_t)
        r0lab_lookup_first("user_enable_single_step.cfi_jt",
                           "user_enable_single_step");
    g_s4_user_disable_single_step = (r0lab_user_step_fn_t)
        r0lab_lookup_first("user_disable_single_step.cfi_jt",
                           "user_disable_single_step");
    if (!g_clock || !g_current_cpu || !g_task_pid || !g_lock_irqsave || !g_unlock_irqrestore ||
        !g_register_hwbp || (!g_disable_hwbp_inatomic && !g_disable_hwbp_local) ||
        !g_unregister_hwbp ||
        !g_synchronize_rcu || !g_find_get_task || !g_put_task ||
        !g_kthread_create || !g_wake_up_process || !g_kthread_stop ||
        !g_kthread_should_stop || !g_msleep ||
        !g_get_task_mm || !g_mmput || !g_mmdrop || !g_get_free_pages ||
        !g_free_pages || !g_do_mem_abort)
        return R0LAB_ENOSYS;
    result = r0lab_init_raw_bridge();
    if (result)
        return result;
    result = r0lab_parse_lab_uid(args, &g_session.lab_uid);
    if (result)
        return result;
    g_exit_probe_armed = r0lab_has_arg_flag(args, "exit_probe=1");
    /*
     * FolkPatch currently frees KPM memory even when .kpm.exit returns
     * -EBUSY. Keep the compatibility probe resource-free so that forced
     * unload cannot strand a worker in released module memory.
     */
    if (!g_exit_probe_armed) {
        result = r0lab_start_workers();
        if (result)
            return result;
    }
    g_initialized = true;
    r0lab_record(R0LAB_EVENT_LOAD, 0);
    pr_info("r0lab-m1: M0-M5 loaded event=%s lab_uid=%u\n",
            event ? event : "(null)", g_session.lab_uid);
    return 0;
}

static long r0lab_exit(void *reserved)
{
    hook_chain3_callback abort_callback;
    unsigned long flags;
    bool active;
    bool exit_probe_armed;
    bool workers_started;
    bool workers_shutdown;
    unsigned int hwbp_slots;
    unsigned int m3_slots;
    unsigned int m4_slots;
    unsigned int raw_slots;
    unsigned int s4_slots;
    unsigned int workers_live;
    bool exit_hook_resident;

    (void)reserved;
    flags = r0lab_lock();
    active = g_session.active;
    hwbp_slots = r0lab_hwbp_slot_count_locked();
    m3_slots = r0lab_m3_slot_count_locked();
    m4_slots = r0lab_m4_slot_count_locked();
    raw_slots = r0lab_raw_slot_count_locked();
    s4_slots = r0lab_s4_slot_count_locked();
    exit_probe_armed = g_exit_probe_armed;
    workers_started = g_workers_started;
    workers_shutdown = g_workers_shutdown_requested;
    workers_live = g_workers_live;
    r0lab_unlock(flags);
    if (active || hwbp_slots || m3_slots || m4_slots || raw_slots || s4_slots ||
        exit_probe_armed ||
        (workers_started && (!workers_shutdown || workers_live))) {
        r0lab_record(R0LAB_EVENT_UNLOAD, R0LAB_EBUSY);
        pr_warn("r0lab-m1: refusing unload while session=%u hwbp_slots=%u m3_slots=%u m4_slots=%u raw_slots=%u s4_slots=%u exit_probe=%u workers_started=%u workers_shutdown=%u workers_live=%u\n",
                active, hwbp_slots, m3_slots, m4_slots, raw_slots,
                s4_slots, exit_probe_armed, workers_started, workers_shutdown,
                workers_live);
        return R0LAB_EBUSY;
    }
    flags = r0lab_lock();
    g_initialized = false;
    abort_callback = g_raw_abort_resident_callback;
    exit_hook_resident = g_raw_exit_hook_resident;
    g_raw_abort_resident_callback = NULL;
    g_raw_exit_hook_resident = false;
    r0lab_unlock(flags);
    if (abort_callback)
        r0lab_hook_detach(g_do_mem_abort, abort_callback, NULL);
    if (exit_hook_resident)
        r0lab_hook_detach(g_exit_mmap, r0lab_raw_exit_mmap_before, NULL);
    if (abort_callback || exit_hook_resident)
        (void)r0lab_raw_wait_for_callbacks();
    r0lab_record(R0LAB_EVENT_UNLOAD, 0);
    pr_info("r0lab-m1: unloaded\n");
    return 0;
}

KPM_INIT(r0lab_init);
KPM_CTL0(r0lab_control0);
KPM_EXIT(r0lab_exit);
