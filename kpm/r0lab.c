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
#define R0LAB_M3_ESR_FSC_PERM 0x0cU
#define R0LAB_M3_ESR_WNR 0x40U
#define R0LAB_M3_DRAIN_LIMIT 2000U
#define R0LAB_M5_MONITOR_INTERVAL_MS 20U
#define R0LAB_M5_TEST_ARM_DELAY_MS 200U
#define R0LAB_M4_SEQUENCE_BYTES 8UL
#define R0LAB_RAW_CODE_MOV_W0_99 0x52800c60U
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

#define R0LAB_EINVAL (-22)
#define R0LAB_EPERM (-1)
#define R0LAB_ENOSYS (-38)
#define R0LAB_EBUSY (-16)
#define R0LAB_ENOENT (-2)
#define R0LAB_EAGAIN (-11)
#define R0LAB_ESRCH (-3)
#define R0LAB_ENOMEM (-12)

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
    bool reserving;
    bool armed;
    bool clearing;
    bool hook_installed;
    bool target_exiting;
    bool monitor_running;
    bool transitioning;
    bool s4_shadow_brk_layout;
    bool gup_hook_installed;
    bool gup_hook_uses_pte;
    bool fork_hook_installed;
    bool fault_hook_installed;
    bool exit_hook_installed;
    bool syscall_hook_installed;
    bool syscall_hook_read_cycle_mode;
    bool fault_probe_armed;
    bool abort_probe_armed;
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
    pid_t step_tid;
    uint8_t state;
    bool reserving;
    bool armed;
    bool clearing;
    bool hook_installed;
    bool step_hook_installed;
    bool step_mode;
    bool raw_step_mode;
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
typedef void *(*r0lab_vmalloc_fn_t)(unsigned long size);
typedef void (*r0lab_vfree_fn_t)(const void *addr);
typedef void *(*r0lab_vmalloc_to_page_fn_t)(const void *addr);
typedef void (*r0lab_down_read_fn_t)(void *sem);
typedef void (*r0lab_up_read_fn_t)(void *sem);
typedef void (*r0lab_raw_spin_fn_t)(void *lock);
typedef void *(*r0lab_find_vma_fn_t)(void *mm, unsigned long addr);
typedef void (*r0lab_sync_icache_dcache_fn_t)(unsigned long pte);
typedef void (*r0lab_mte_sync_tags_fn_t)(unsigned long old_pte,
                                         unsigned long new_pte);
typedef void (*r0lab_user_step_fn_t)(struct task_struct *task);

KPM_NAME("r0lab-m1");
KPM_VERSION("0.7.0");
KPM_LICENSE("GPL v2");
KPM_AUTHOR("r0hook research");
KPM_DESCRIPTION("Controlled M0-M5 session, HWBP, UXN, clone, and lifecycle lab");

DEFINE_SPINLOCK(g_r0lab_lock);

static struct r0lab_event g_events[R0LAB_EVENT_CAPACITY];
static struct r0lab_session g_session;
static struct r0lab_hwbp_slot g_hwbp_slots[R0LAB_HWBP_SLOT_CAPACITY];
static struct r0lab_m3_page g_m3_page;
static struct r0lab_m4_page g_m4_page;
static struct r0lab_raw_shadow_page g_raw_page;
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
static r0lab_vmalloc_fn_t g_vmalloc;
static r0lab_vfree_fn_t g_vfree;
static r0lab_vmalloc_to_page_fn_t g_vmalloc_to_page;
static r0lab_down_read_fn_t g_raw_down_read;
static r0lab_up_read_fn_t g_raw_up_read;
static r0lab_raw_spin_fn_t g_raw_spin_lock;
static r0lab_raw_spin_fn_t g_raw_spin_unlock;
static r0lab_find_vma_fn_t g_raw_find_vma;
static r0lab_sync_icache_dcache_fn_t g_sync_icache_dcache;
static r0lab_mte_sync_tags_fn_t g_mte_sync_tags;
static void *g_do_mem_abort;
static void *g_handle_mm_fault;
static void *g_dup_mmap;
static void *g_exit_mmap;
static void *g_sys_getpid;
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

static void r0lab_close_exited_session(pid_t owner_tgid);
static bool r0lab_target_mm_live(pid_t owner_tgid,
                                 struct mm_struct *expected_mm);
static bool r0lab_target_task_live(pid_t owner_tgid);
static uint64_t r0lab_record_values(enum r0lab_event_op op, int result,
                                    uint64_t pc, uint64_t x0, uint64_t x30);
static int r0lab_raw_wait_for_callbacks(void);
static void r0lab_raw_unhook_except_exit(void);
static void r0lab_raw_unhook(void);
static void r0lab_raw_reset(struct mm_struct *mm);
static void r0lab_raw_reset_final(struct mm_struct *mm, uint64_t generation);
static int r0lab_raw_prepare_shadow(struct r0lab_raw_shadow_page *page);

static uint64_t r0lab_now(void)
{
    return g_clock ? g_clock() : 0;
}

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

static unsigned int r0lab_raw_slot_count_locked(void)
{
    return g_raw_page.reserving || g_raw_page.armed || g_raw_page.clearing ? 1U : 0U;
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

static unsigned int r0lab_page_record_count_locked(void)
{
    unsigned int count = 0;

    if (g_m4_page.record.backend != R0LAB_PAGE_RECORD_NONE)
        ++count;
    if (g_raw_page.record.backend != R0LAB_PAGE_RECORD_NONE)
        ++count;
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
    uint8_t page_backend;
    uint8_t page_state;
    uint32_t m3_fault_events;
    uint32_t m4_redirect_events;
    uint32_t raw_activation_events;
    uint32_t s4_brk_events;
    uint32_t s4_step_events;
    uint8_t s4_state;
    uint32_t hwbp_entry_events;
    uint32_t hwbp_return_events;
    unsigned int workers_live;
    bool workers_shutdown;
    int current_cpu;

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
    if (g_raw_page.record.backend != R0LAB_PAGE_RECORD_NONE) {
        page_backend = g_raw_page.record.backend;
        page_state = g_raw_page.record.state;
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
    hwbp_entry_events = g_hwbp_entry_events;
    hwbp_return_events = g_hwbp_return_events;
    workers_live = g_workers_live;
    workers_shutdown = g_workers_shutdown_requested;
    r0lab_unlock(flags);
    current_cpu = (int)g_current_cpu();

    snprintf(reply, sizeof(reply),
             "version=5 lab_uid=%u active=%u owner_tgid=%d next_seq=%llu last_summary_seq=%llu hwbp_slots=%u hwbp_entry_events=%u hwbp_return_events=%u m3_slots=%u m3_fault_events=%u m4_slots=%u m4_redirect_events=%u raw_slots=%u raw_activation_events=%u s4_slots=%u s4_brk_events=%u s4_step_events=%u s4_state=%s page_records=%u page_backend=%s page_state=%s workers_live=%u workers_shutdown=%u cpu_id=%d clock=sched_clock\n",
             g_session.lab_uid, active, owner_tgid, next_seq, last_summary_seq,
             hwbp_slots, hwbp_entry_events, hwbp_return_events, m3_slots,
             m3_fault_events, m4_slots, m4_redirect_events, raw_slots,
             raw_activation_events, s4_slots, s4_brk_events, s4_step_events,
             r0lab_s4_state_name(s4_state), page_records,
             r0lab_page_record_backend_name(page_backend),
             r0lab_page_record_state_name(page_state), workers_live,
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
    uint64_t raw_generation = 0;
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
        g_s4_brk.target_address == regs->pc &&
        (esr & 0xffffU) == R0LAB_S4_BRK_COMMENT) {
        ++g_s4_brk.brk_events;
        step_ready = g_s4_brk.step_mode && g_s4_user_enable_single_step &&
                     g_s4_brk.step_hook_installed;
        event_pc = regs->pc;
        event_x0 = regs->regs[0];
        event_x30 = regs->regs[30];
        if (step_ready && g_s4_brk.raw_step_mode) {
            if (g_raw_page.armed && !g_raw_page.clearing &&
                !g_raw_page.transitioning &&
                g_raw_page.raw.state == R0LAB_RAW_SHADOW_RX &&
                g_raw_page.raw.address ==
                    (g_s4_brk.target_address &
                     ~(R0LAB_RAW_PAGE_SIZE - 1UL))) {
                g_raw_page.transitioning = true;
                raw_generation = g_raw_page.generation;
                raw_begin = true;
            }
        } else if (step_ready) {
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
    r0lab_unlock(flags);

    if (raw_begin)
        raw_result = r0lab_raw_begin_stepping(&g_raw_page.raw);

    if (raw_begin) {
        flags = r0lab_lock();
        if (g_raw_page.generation == raw_generation) {
            g_raw_page.transitioning = false;
            if (!raw_result && g_s4_brk.armed && !g_s4_brk.clearing &&
                g_s4_brk.raw_step_mode &&
                g_s4_brk.state == R0LAB_S4_HOOKED) {
                g_s4_brk.state = R0LAB_S4_STEP_ARMED;
                g_s4_brk.step_tid = r0lab_current_tid();
                ++g_s4_brk.step_enable_events;
                ++g_s4_brk.pte_begin_events;
                g_raw_page.record.state = R0LAB_PAGE_RECORD_ORIGINAL_STEP;
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
        g_s4_brk.state == R0LAB_S4_STEP_ARMED && g_session.active &&
        g_session.owner_tgid == r0lab_current_tgid() &&
        g_s4_brk.step_tid == r0lab_current_tid()) {
        ++g_s4_brk.step_disable_events;
        disable_step = true;
        raw_step_mode = g_s4_brk.raw_step_mode;
        if (raw_step_mode && g_raw_page.armed && !g_raw_page.clearing &&
            !g_raw_page.transitioning &&
            g_raw_page.raw.state == R0LAB_RAW_ORIGINAL_STEP) {
            g_raw_page.transitioning = true;
            raw_generation = g_raw_page.generation;
            raw_finish = true;
        }
        if (!raw_step_mode && regs->pc == g_s4_brk.target_address + 8) {
            ++g_s4_brk.step_events;
            g_s4_brk.state = R0LAB_S4_STEP_OBSERVED;
            matched = true;
        }
        args->skip_origin = 1;
        args->ret = 0;
    }
    r0lab_unlock(flags);

    if (raw_finish)
        raw_result = r0lab_raw_finish_stepping(&g_raw_page.raw);

    if (raw_finish) {
        flags = r0lab_lock();
        if (g_raw_page.generation == raw_generation) {
            g_raw_page.transitioning = false;
            if (!raw_result && g_s4_brk.armed && !g_s4_brk.clearing &&
                g_s4_brk.raw_step_mode &&
                regs->pc == g_s4_brk.target_address + 4) {
                ++g_s4_brk.step_events;
                ++g_s4_brk.pte_finish_events;
                g_s4_brk.state = R0LAB_S4_STEP_OBSERVED;
                g_raw_page.record.state = R0LAB_PAGE_RECORD_SHADOW_ACTIVE;
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
        hook_unwrap(g_s4_single_step_handler, r0lab_s4_step_before, NULL);
    if (brk_installed)
        hook_unwrap(g_s4_brk_handler, r0lab_s4_brk_before, NULL);
    if (brk_installed || step_installed)
        (void)r0lab_s4_wait_for_callbacks();
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
    uint64_t raw_generation;
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
    raw_generation = g_raw_page.generation;
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
        result = r0lab_raw_restore_original(&g_raw_page.raw);
    if (!result && raw_step_mode)
        result = r0lab_raw_wait_for_callbacks();
    if (raw_step_mode)
        r0lab_raw_reset_final((struct mm_struct *)g_raw_page.raw.mm,
                              raw_generation);
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
        memset(&g_raw_page, 0, sizeof(g_raw_page));
        g_raw_page.raw.mm = raw_mm;
        g_raw_page.raw.address = (unsigned long)target_address;
        g_raw_page.generation = ++g_raw_generation;
        g_raw_page.record.source_address = g_raw_page.raw.address;
        g_raw_page.record.generation = g_raw_page.generation;
        g_raw_page.record.backend = R0LAB_PAGE_RECORD_RAW_TWO_PFN;
        g_raw_page.record.state = R0LAB_PAGE_RECORD_PREPARING;
        g_raw_page.s4_shadow_brk_layout = true;
        g_raw_page.reserving = true;
    }
    g_s4_brk.mm = mm;
    g_s4_brk.target_address = (unsigned long)target_address;
    g_s4_brk.generation = ++g_s4_generation;
    g_s4_brk.state = R0LAB_S4_PREPARING;
    g_s4_brk.step_mode = step_mode;
    g_s4_brk.raw_step_mode = raw_step_mode;
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
            hook_unwrap(g_s4_brk_handler, r0lab_s4_brk_before, NULL);
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
            int restore_result = 0;

            if (g_raw_page.raw.state == R0LAB_RAW_SOURCE_UXN ||
                g_raw_page.raw.state == R0LAB_RAW_SHADOW_RX ||
                g_raw_page.raw.state == R0LAB_RAW_ORIGINAL_STEP)
                restore_result = r0lab_raw_restore_original(&g_raw_page.raw);
            if (!restore_result)
                r0lab_raw_reset(raw_mm);
            else
                result = restore_result;
        }
        r0lab_s4_reset(mm);
        goto record;
    }

    result = r0lab_s4_start_monitor();
    if (result) {
        r0lab_s4_unhook();
        if (raw_step_mode) {
            int restore_result = 0;

            if (g_raw_page.raw.state == R0LAB_RAW_SOURCE_UXN ||
                g_raw_page.raw.state == R0LAB_RAW_SHADOW_RX ||
                g_raw_page.raw.state == R0LAB_RAW_ORIGINAL_STEP)
                restore_result = r0lab_raw_restore_original(&g_raw_page.raw);
            if (!restore_result)
                r0lab_raw_reset(raw_mm);
            else
                result = restore_result;
        }
        r0lab_s4_reset(mm);
        goto record;
    }
    r0lab_record(R0LAB_EVENT_S4_ARM, 0);
    if (raw_step_mode)
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
    uint8_t state;
    bool raw_step_mode;
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
    state = g_s4_brk.state;
    raw_step_mode = g_s4_brk.raw_step_mode;
    r0lab_unlock(flags);

    if (raw_step_mode)
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
        struct mm_struct *raw_mm = (struct mm_struct *)g_raw_page.raw.mm;

        result = r0lab_raw_restore_original(&g_raw_page.raw);
        if (!result)
            result = r0lab_raw_wait_for_callbacks();
        if (result)
            goto record;
        r0lab_raw_reset(raw_mm);
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

static long r0lab_s4_brk_cleared(uint64_t token, char __user *out_msg,
                                 int outlen)
{
    unsigned long flags;
    int result = r0lab_validate_owner(token);

    if (result)
        return result;
    flags = r0lab_lock();
    if (r0lab_s4_slot_count_locked()) {
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
    if (g_session.active && g_session.owner_tgid == owner_tgid) {
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
        hook_unwrap(g_do_mem_abort, r0lab_m3_before_abort, NULL);
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
        hook_unwrap(g_do_mem_abort, r0lab_m4_before_abort, NULL);
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

static void r0lab_raw_before_abort(hook_fargs3_t *args, void *udata)
{
    const unsigned long far = (unsigned long)args->arg0;
    const unsigned int esr = (unsigned int)args->arg1;
    struct pt_regs *regs = (struct pt_regs *)(unsigned long)args->arg2;
    unsigned long page_address;
    uint64_t generation;
    unsigned long flags;
    bool should_handle = false;
    bool handled = false;
    bool read_cycle_resume = false;
    bool abort_probe_hit = false;
    uint32_t abort_probe_kind = 0;
    uint32_t abort_probe_events = 0;
    int result = R0LAB_EINVAL;

    (void)udata;
    flags = r0lab_lock();
    ++g_raw_inflight;
    if (regs && (esr >> R0LAB_M3_ESR_EC_SHIFT) == R0LAB_M3_ESR_EC_IABT_LOW &&
        (esr & R0LAB_M3_ESR_FSC_TYPE) == R0LAB_M3_ESR_FSC_PERM &&
        g_session.active && g_raw_page.armed && !g_raw_page.clearing &&
        !g_raw_page.transitioning &&
        (g_raw_page.raw.state == R0LAB_RAW_SOURCE_UXN ||
         (g_raw_page.raw.state == R0LAB_RAW_ORIGINAL_READ &&
          g_raw_page.raw.read_cycle_active)) &&
        g_session.owner_tgid == r0lab_current_tgid()) {
        page_address = g_raw_page.raw.address;
        if ((far & ~(R0LAB_RAW_PAGE_SIZE - 1UL)) == page_address &&
            regs->pc >= page_address &&
            regs->pc < page_address + R0LAB_RAW_PAGE_SIZE) {
            read_cycle_resume =
                g_raw_page.raw.state == R0LAB_RAW_ORIGINAL_READ;
            g_raw_page.transitioning = true;
            generation = g_raw_page.generation;
            should_handle = true;
        }
    } else if (regs &&
               (esr >> R0LAB_M3_ESR_EC_SHIFT) ==
                   R0LAB_M3_ESR_EC_DABT_LOW &&
               g_session.active && g_raw_page.armed &&
               g_raw_page.abort_probe_armed && !g_raw_page.clearing &&
               !g_raw_page.transitioning &&
               g_raw_page.raw.state == R0LAB_RAW_SHADOW_RX &&
               g_session.owner_tgid == r0lab_current_tgid()) {
        page_address = g_raw_page.raw.address;
        if ((far & ~(R0LAB_RAW_PAGE_SIZE - 1UL)) == page_address) {
            abort_probe_kind = (esr & R0LAB_M3_ESR_WNR) ?
                               R0LAB_FAULT_KIND_WRITE :
                               R0LAB_FAULT_KIND_READ;
            if (abort_probe_kind == R0LAB_FAULT_KIND_WRITE)
                ++g_raw_page.abort_probe_write_events;
            else
                ++g_raw_page.abort_probe_read_events;
            abort_probe_events = g_raw_page.abort_probe_read_events +
                                 g_raw_page.abort_probe_write_events +
                                 g_raw_page.abort_probe_exec_events;
            g_raw_page.abort_probe_last_esr = esr;
            g_raw_page.abort_probe_last_far = far;
            abort_probe_hit = true;
        }
    }
    r0lab_unlock(flags);

    if (should_handle) {
        if (read_cycle_resume)
            result = r0lab_raw_finish_read_cycle(&g_raw_page.raw);
        else
            result = r0lab_raw_activate_shadow(&g_raw_page.raw);
    }

    flags = r0lab_lock();
    if (should_handle && g_raw_page.generation == generation) {
        g_raw_page.transitioning = false;
        if (!result && g_raw_page.raw.state == R0LAB_RAW_SHADOW_RX) {
            if (!read_cycle_resume) {
                ++g_raw_page.activation_events;
                g_raw_page.record.events = g_raw_page.activation_events;
                g_raw_page.record.source_pfn = g_raw_page.raw.source_pfn;
                g_raw_page.record.shadow_pfn = g_raw_page.raw.shadow_pfn;
            }
            g_raw_page.record.state = R0LAB_PAGE_RECORD_SHADOW_ACTIVE;
            args->skip_origin = 1;
            handled = true;
        }
    }
    --g_raw_inflight;
    r0lab_unlock(flags);

    if (handled)
        r0lab_record_regs(read_cycle_resume ?
                          R0LAB_EVENT_RAW_READ_CYCLE_FINISH :
                          R0LAB_EVENT_RAW_ACTIVATE, 0, regs);
    else if (should_handle)
        r0lab_record_regs(R0LAB_EVENT_REJECT, result, regs);
    if (abort_probe_hit)
        r0lab_record_values(R0LAB_EVENT_RAW_ABORT_PROBE_HIT, 0, far, esr,
                            abort_probe_kind | ((uint64_t)abort_probe_events
                                                << 32));
}

static void r0lab_raw_gup_before_common(void *vma, unsigned long address,
                                        hook_local_t *local)
{
    uint64_t generation = 0;
    unsigned long flags;
    bool should_hide = false;
    int result = R0LAB_EINVAL;

    local->data0 = 0;
    local->data1 = 0;
    local->data7 = 1;

    flags = r0lab_lock();
    ++g_raw_inflight;
    if (g_session.active && g_raw_page.armed && g_raw_page.gup_hook_installed &&
        !g_raw_page.clearing && !g_raw_page.transitioning &&
        !g_raw_page.raw.gup_hide_active &&
        g_raw_page.raw.state == R0LAB_RAW_SHADOW_RX &&
        r0lab_raw_vma_matches(&g_raw_page.raw, vma, address)) {
        g_raw_page.transitioning = true;
        generation = g_raw_page.generation;
        should_hide = true;
    }
    r0lab_unlock(flags);

    if (should_hide)
        result = r0lab_raw_begin_gup_hide(&g_raw_page.raw);

    flags = r0lab_lock();
    if (should_hide && g_raw_page.generation == generation) {
        g_raw_page.transitioning = false;
        if (!result) {
            ++g_raw_page.gup_hook_begin_events;
            local->data0 = 1;
            local->data1 = generation;
        } else {
            ++g_raw_page.gup_hook_failures;
        }
    }
    r0lab_unlock(flags);

    if (should_hide)
        r0lab_record(R0LAB_EVENT_RAW_GUP_HOOK_BEGIN, result);
}

static void r0lab_raw_gup_after_common(hook_local_t *local)
{
    uint64_t generation = local->data1;
    unsigned long flags;
    bool should_finish = local->data0 == 1;
    int result = R0LAB_EINVAL;

    if (should_finish) {
        flags = r0lab_lock();
        if (g_raw_page.generation == generation &&
            g_raw_page.raw.gup_hide_active && !g_raw_page.transitioning) {
            g_raw_page.transitioning = true;
        } else {
            should_finish = false;
        }
        r0lab_unlock(flags);
    }

    if (should_finish)
        result = r0lab_raw_finish_gup_hide(&g_raw_page.raw);

    flags = r0lab_lock();
    if (should_finish && g_raw_page.generation == generation) {
        g_raw_page.transitioning = false;
        if (!result)
            ++g_raw_page.gup_hook_finish_events;
        else
            ++g_raw_page.gup_hook_failures;
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
    unsigned long address = (unsigned long)args->arg1;
    unsigned int fault_flags = (unsigned int)args->arg2;
    unsigned long flags;
    struct r0lab_raw_page fault_probe_page;
    uint32_t kind = R0LAB_FAULT_KIND_READ;
    bool hit = false;

    (void)udata;
    if (fault_flags & R0LAB_FAULT_FLAG_INSTRUCTION)
        kind = R0LAB_FAULT_KIND_EXEC;
    else if (fault_flags & R0LAB_FAULT_FLAG_WRITE)
        kind = R0LAB_FAULT_KIND_WRITE;

    flags = r0lab_lock();
    if (!g_raw_page.fault_hook_installed) {
        r0lab_unlock(flags);
        return;
    }
    ++g_raw_inflight;
    if (g_session.active && g_raw_page.armed && !g_raw_page.clearing &&
        !g_raw_page.transitioning &&
        g_raw_page.raw.state == R0LAB_RAW_SHADOW_RX &&
        g_session.owner_tgid == r0lab_current_tgid() &&
        (fault_flags & R0LAB_FAULT_FLAG_USER) &&
        r0lab_raw_vma_matches(&g_raw_page.raw, vma, address)) {
        if (kind == R0LAB_FAULT_KIND_EXEC)
            ++g_raw_page.fault_hook_exec_events;
        else if (kind == R0LAB_FAULT_KIND_WRITE)
            ++g_raw_page.fault_hook_write_events;
        else
            ++g_raw_page.fault_hook_read_events;
        hit = true;
    } else if (g_session.active && g_raw_page.armed &&
               !g_raw_page.clearing && !g_raw_page.transitioning &&
               g_raw_page.raw.state == R0LAB_RAW_SHADOW_RX &&
               g_raw_page.fault_probe_armed &&
               g_raw_page.fault_probe_address &&
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
    unsigned long flags;
    unsigned long address = 0;
    unsigned long state = 0;
    uint64_t generation = 0;
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
    if (!g_raw_page.syscall_hook_installed) {
        r0lab_unlock(flags);
        if (current_mm)
            g_mmput(current_mm);
        return;
    }
    ++g_raw_inflight;
    if (g_session.active && g_raw_page.armed && !g_raw_page.clearing &&
        !g_raw_page.transitioning &&
        g_raw_page.raw.state == R0LAB_RAW_SHADOW_RX &&
        current_mm && g_raw_page.raw.mm == current_mm &&
        g_session.owner_tgid == r0lab_current_tgid()) {
        ++g_raw_page.syscall_hook_events;
        events = g_raw_page.syscall_hook_events;
        address = g_raw_page.raw.address;
        state = g_raw_page.raw.state;
        read_cycle_mode = g_raw_page.syscall_hook_read_cycle_mode;
        if (read_cycle_mode) {
            if (!g_raw_page.raw.gup_hide_active &&
                !g_raw_page.raw.fork_hide_active &&
                !g_raw_page.raw.read_cycle_active) {
                g_raw_page.transitioning = true;
                generation = g_raw_page.generation;
                should_begin_read_cycle = true;
            } else {
                ++g_raw_page.syscall_hook_failures;
                read_cycle_result = R0LAB_EAGAIN;
            }
        }
        hit = true;
    }
    r0lab_unlock(flags);

    if (should_begin_read_cycle)
        read_cycle_result = r0lab_raw_begin_read_cycle(&g_raw_page.raw);

    if (should_begin_read_cycle) {
        flags = r0lab_lock();
        if (g_raw_page.generation == generation) {
            g_raw_page.transitioning = false;
            if (!read_cycle_result &&
                g_raw_page.raw.state == R0LAB_RAW_ORIGINAL_READ) {
                g_raw_page.record.state = R0LAB_PAGE_RECORD_ORIGINAL_READ;
                ++g_raw_page.syscall_hook_read_cycle_events;
                read_cycle_events =
                    g_raw_page.syscall_hook_read_cycle_events;
                state = g_raw_page.raw.state;
            } else {
                ++g_raw_page.syscall_hook_failures;
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

static void r0lab_raw_exit_mmap_before(hook_fargs1_t *args, void *udata)
{
    void *mm = (void *)(unsigned long)args->arg0;
    unsigned long flags;
    unsigned long address = 0;
    unsigned long state = 0;
    uint32_t events = 0;
    bool hit = false;

    (void)udata;
    flags = r0lab_lock();
    if (!g_raw_page.exit_hook_installed) {
        r0lab_unlock(flags);
        return;
    }
    ++g_raw_inflight;
    if (mm && g_raw_page.raw.mm == mm && g_raw_page.raw.address &&
        g_raw_page.generation) {
        ++g_raw_page.exit_hook_events;
        events = g_raw_page.exit_hook_events;
        address = g_raw_page.raw.address;
        state = g_raw_page.raw.state;
        hit = true;
    }
    r0lab_unlock(flags);

    if (hit)
        r0lab_record_values(R0LAB_EVENT_RAW_EXIT_MMAP_HIT, 0, address,
                            state, events);

    flags = r0lab_lock();
    --g_raw_inflight;
    r0lab_unlock(flags);
}

static void r0lab_raw_fork_before(hook_fargs2_t *args, void *udata)
{
    void *oldmm = (void *)(unsigned long)args->arg1;
    uint64_t generation = 0;
    unsigned long flags;
    bool should_hide = false;
    int result = R0LAB_EINVAL;

    (void)udata;
    args->local.data0 = 0;
    args->local.data1 = 0;
    args->local.data7 = 1;

    flags = r0lab_lock();
    ++g_raw_inflight;
    if (g_session.active && g_raw_page.armed && g_raw_page.fork_hook_installed &&
        !g_raw_page.clearing && !g_raw_page.transitioning &&
        !g_raw_page.raw.gup_hide_active &&
        !g_raw_page.raw.fork_hide_active &&
        g_raw_page.raw.state == R0LAB_RAW_SHADOW_RX &&
        g_raw_page.raw.mm == oldmm &&
        g_session.owner_tgid == r0lab_current_tgid()) {
        g_raw_page.transitioning = true;
        generation = g_raw_page.generation;
        should_hide = true;
    }
    r0lab_unlock(flags);

    if (should_hide)
        result = r0lab_raw_begin_fork_hide(&g_raw_page.raw, oldmm);

    flags = r0lab_lock();
    if (should_hide && g_raw_page.generation == generation) {
        if (!result) {
            ++g_raw_page.fork_hook_begin_events;
            args->local.data0 = 1;
            args->local.data1 = generation;
            args->local.data2 = (uint64_t)(unsigned long)oldmm;
        } else {
            g_raw_page.transitioning = false;
            ++g_raw_page.fork_hook_failures;
        }
    }
    if (!should_hide)
        --g_raw_inflight;
    r0lab_unlock(flags);

    if (should_hide)
        r0lab_record(R0LAB_EVENT_RAW_FORK_HOOK_BEGIN, result);
}

static void r0lab_raw_fork_after(hook_fargs2_t *args, void *udata)
{
    uint64_t generation = args->local.data1;
    void *oldmm = (void *)(unsigned long)args->local.data2;
    unsigned long flags;
    bool should_finish = args->local.data0 == 1;
    int result = R0LAB_EINVAL;

    (void)udata;
    if (should_finish) {
        flags = r0lab_lock();
        if (g_raw_page.generation == generation &&
            g_raw_page.raw.fork_hide_active &&
            g_raw_page.transitioning &&
            g_raw_page.raw.mm == oldmm) {
            /* Keep transitioning set until the parent PTE is back to shadow. */
        } else {
            if (g_raw_page.generation == generation) {
                g_raw_page.transitioning = false;
                ++g_raw_page.fork_hook_failures;
            }
            should_finish = false;
        }
        r0lab_unlock(flags);
    }

    if (should_finish)
        result = r0lab_raw_finish_fork_hide(&g_raw_page.raw, oldmm);

    flags = r0lab_lock();
    if (should_finish && g_raw_page.generation == generation) {
        g_raw_page.transitioning = false;
        if (!result)
            ++g_raw_page.fork_hook_finish_events;
        else
            ++g_raw_page.fork_hook_failures;
    }
    if (args->local.data7 == 1)
        --g_raw_inflight;
    r0lab_unlock(flags);

    if (should_finish)
        r0lab_record(R0LAB_EVENT_RAW_FORK_HOOK_FINISH, result);
}

static void r0lab_raw_fork_unhook(void)
{
    bool installed;
    unsigned long flags = r0lab_lock();

    installed = g_raw_page.fork_hook_installed;
    g_raw_page.fork_hook_installed = false;
    r0lab_unlock(flags);
    if (installed)
        (void)r0lab_raw_wait_for_callbacks();
    if (installed)
        hook_unwrap(g_dup_mmap, r0lab_raw_fork_before,
                    r0lab_raw_fork_after);
    if (installed)
        (void)r0lab_raw_wait_for_callbacks();
}

static void r0lab_raw_fault_unhook(void)
{
    bool installed;
    unsigned long flags = r0lab_lock();

    installed = g_raw_page.fault_hook_installed;
    g_raw_page.fault_hook_installed = false;
    g_raw_page.fault_probe_armed = false;
    g_raw_page.fault_probe_address = 0;
    g_raw_page.fault_probe_reader_tgid = 0;
    r0lab_unlock(flags);
    if (installed)
        (void)r0lab_raw_wait_for_callbacks();
    if (installed)
        hook_unwrap(g_handle_mm_fault, r0lab_raw_fault_before, NULL);
    if (installed)
        (void)r0lab_raw_wait_for_callbacks();
}

static void r0lab_raw_syscall_unhook(void)
{
    bool installed;
    unsigned long flags = r0lab_lock();

    installed = g_raw_page.syscall_hook_installed;
    g_raw_page.syscall_hook_installed = false;
    g_raw_page.syscall_hook_read_cycle_mode = false;
    r0lab_unlock(flags);
    if (installed)
        (void)r0lab_raw_wait_for_callbacks();
    if (installed)
        hook_unwrap(g_sys_getpid, r0lab_raw_syscall_before, NULL);
    if (installed)
        (void)r0lab_raw_wait_for_callbacks();
}

static void r0lab_raw_exit_unhook(void)
{
    bool installed;
    unsigned long flags = r0lab_lock();

    installed = g_raw_page.exit_hook_installed;
    g_raw_page.exit_hook_installed = false;
    r0lab_unlock(flags);
    if (installed)
        (void)r0lab_raw_wait_for_callbacks();
    if (installed)
        hook_unwrap(g_exit_mmap, r0lab_raw_exit_mmap_before, NULL);
    if (installed)
        (void)r0lab_raw_wait_for_callbacks();
}

static void r0lab_raw_gup_unhook(void)
{
    bool installed;
    bool uses_pte;
    unsigned long flags = r0lab_lock();

    installed = g_raw_page.gup_hook_installed;
    uses_pte = g_raw_page.gup_hook_uses_pte;
    g_raw_page.gup_hook_installed = false;
    g_raw_page.gup_hook_uses_pte = false;
    r0lab_unlock(flags);
    if (installed)
        (void)r0lab_raw_wait_for_callbacks();
    if (installed && uses_pte)
        hook_unwrap(g_follow_page_pte, r0lab_raw_gup_pte_before,
                    r0lab_raw_gup_pte_after);
    else if (installed)
        hook_unwrap(g_follow_page_mask, r0lab_raw_gup_mask_before,
                    r0lab_raw_gup_mask_after);
    if (installed)
        (void)r0lab_raw_wait_for_callbacks();
}

static void r0lab_raw_unhook(void)
{
    r0lab_raw_unhook_except_exit();
    r0lab_raw_exit_unhook();
}

static void r0lab_raw_unhook_except_exit(void)
{
    bool installed;
    unsigned long flags;

    r0lab_raw_fault_unhook();
    r0lab_raw_fork_unhook();
    r0lab_raw_syscall_unhook();
    r0lab_raw_gup_unhook();

    flags = r0lab_lock();
    installed = g_raw_page.hook_installed;
    g_raw_page.hook_installed = false;
    r0lab_unlock(flags);
    if (installed)
        (void)r0lab_raw_wait_for_callbacks();
    if (installed)
        hook_unwrap(g_do_mem_abort, r0lab_raw_before_abort, NULL);
    if (installed)
        (void)r0lab_raw_wait_for_callbacks();
}

static void r0lab_raw_reset(struct mm_struct *mm)
{
    void *shadow_kaddr = NULL;
    unsigned long flags = r0lab_lock();

    if (!mm)
        return;
    if (g_raw_page.raw.mm == mm) {
        bool monitor_running = g_raw_page.monitor_running;
        uint64_t generation = g_raw_page.generation;

        shadow_kaddr = g_raw_page.raw.shadow_kaddr;
        memset(&g_raw_page, 0, sizeof(g_raw_page));
        g_raw_page.generation = generation;
        g_raw_page.monitor_running = monitor_running;
    }
    r0lab_unlock(flags);
    if (shadow_kaddr && g_vfree)
        g_vfree(shadow_kaddr);
    g_mmput(mm);
}

static void r0lab_raw_reset_final(struct mm_struct *mm, uint64_t generation)
{
    void *shadow_kaddr = NULL;
    bool exit_hook_installed = false;
    unsigned long flags = r0lab_lock();

    if (!mm)
        return;
    if (g_raw_page.raw.mm == mm && g_raw_page.generation == generation) {
        exit_hook_installed = g_raw_page.exit_hook_installed;
        if (!exit_hook_installed) {
            shadow_kaddr = g_raw_page.raw.shadow_kaddr;
            memset(&g_raw_page, 0, sizeof(g_raw_page));
        }
    }
    r0lab_unlock(flags);

    if (exit_hook_installed) {
        g_mmput(mm);
        flags = r0lab_lock();
        if (g_raw_page.raw.mm == mm && g_raw_page.generation == generation) {
            shadow_kaddr = g_raw_page.raw.shadow_kaddr;
            g_raw_page.exit_hook_installed = false;
        }
        r0lab_unlock(flags);
        (void)r0lab_raw_wait_for_callbacks();
        hook_unwrap(g_exit_mmap, r0lab_raw_exit_mmap_before, NULL);
        (void)r0lab_raw_wait_for_callbacks();
        flags = r0lab_lock();
        if (g_raw_page.raw.mm == mm && g_raw_page.generation == generation)
            memset(&g_raw_page, 0, sizeof(g_raw_page));
        r0lab_unlock(flags);
        if (shadow_kaddr && g_vfree)
            g_vfree(shadow_kaddr);
        return;
    }

    if (shadow_kaddr && g_vfree)
        g_vfree(shadow_kaddr);
    g_mmput(mm);
}

static void r0lab_raw_monitor_done(uint64_t generation)
{
    unsigned long flags = r0lab_lock();

    if (g_raw_page.generation == generation)
        g_raw_page.monitor_running = false;
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
    if (!page || page != &g_raw_page || !page->monitor_running ||
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
        if (g_raw_page.generation != generation || !g_raw_page.monitor_running) {
            r0lab_unlock(flags);
            return 0;
        }
        mm = (struct mm_struct *)g_raw_page.raw.mm;
        owner_tgid = g_session.owner_tgid;
        active = g_session.active;
        page_owned = g_raw_page.reserving || g_raw_page.armed ||
                     g_raw_page.clearing;
        already_clearing = g_raw_page.clearing && !g_raw_page.target_exiting;
        if (!mm || !active || !page_owned) {
            g_raw_page.monitor_running = false;
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
    if (g_raw_page.generation != generation || g_raw_page.raw.mm != mm) {
        r0lab_unlock(flags);
        r0lab_raw_monitor_done(generation);
        return 0;
    }
    g_raw_page.target_exiting = true;
    g_raw_page.clearing = true;
    g_raw_page.record.state = R0LAB_PAGE_RECORD_RESTORING;
    r0lab_unlock(flags);

    r0lab_record(R0LAB_EVENT_TARGET_EXIT, 0);
    result = r0lab_raw_restore_original(&g_raw_page.raw);
    r0lab_raw_unhook_except_exit();
    if (!result)
        result = r0lab_raw_wait_for_callbacks();
    r0lab_raw_reset_final(mm, generation);
    r0lab_close_exited_session(owner_tgid);
    r0lab_record(R0LAB_EVENT_RAW_CLEAR, result);
    return 0;
}

static int r0lab_raw_start_monitor(void)
{
    unsigned long flags;

    flags = r0lab_lock();
    if (!g_raw_page.armed || g_raw_page.monitor_running) {
        r0lab_unlock(flags);
        return R0LAB_EBUSY;
    }
    g_raw_page.monitor_running = true;
    r0lab_unlock(flags);

    if (!g_raw_monitor_worker_task)
        return R0LAB_ESRCH;
    g_wake_up_process(g_raw_monitor_worker_task);
    return 0;
}

static int r0lab_raw_prepare_shadow(struct r0lab_raw_shadow_page *page)
{
    uint32_t *shadow_words;
    void *source_kaddr;
    unsigned long flags;
    int result;

    result = r0lab_raw_capture(&page->raw);
    if (result)
        return result;
    source_kaddr = r0lab_raw_source_kernel_address(&page->raw);
    if (!source_kaddr)
        return R0LAB_ENOENT;
    page->raw.shadow_kaddr = g_vmalloc(R0LAB_RAW_PAGE_SIZE);
    if (!page->raw.shadow_kaddr)
        return R0LAB_ENOMEM;
    memcpy(page->raw.shadow_kaddr, source_kaddr, R0LAB_RAW_PAGE_SIZE);
    shadow_words = (uint32_t *)page->raw.shadow_kaddr;
    if (page->s4_shadow_brk_layout) {
        shadow_words[0] = R0LAB_S4_BRK_COMMENT << 5 | 0xd4200000U;
        shadow_words[1] = R0LAB_RAW_CODE_MOV_W0_99;
        shadow_words[2] = 0xd65f03c0U;
    } else {
        shadow_words[0] = R0LAB_RAW_CODE_MOV_W0_99;
    }
    result = r0lab_raw_shadow_pfn_from_kaddr(&page->raw);
    if (result) {
        g_vfree(page->raw.shadow_kaddr);
        page->raw.shadow_kaddr = NULL;
        return result;
    }
    flags = r0lab_lock();
    if (page == &g_raw_page) {
        page->record.source_pfn = page->raw.source_pfn;
        page->record.shadow_pfn = page->raw.shadow_pfn;
    }
    r0lab_unlock(flags);
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
    if (!page || page != &g_raw_page || !page->reserving || !page->raw.mm ||
        !g_session.active) {
        if (page == &g_raw_page && page->raw.mm) {
            mm = (struct mm_struct *)page->raw.mm;
            memset(&g_raw_page, 0, sizeof(g_raw_page));
        }
        r0lab_unlock(flags);
        if (mm)
            g_mmput(mm);
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

    result = hook_wrap3(g_do_mem_abort, r0lab_raw_before_abort, NULL, NULL);
    if (result)
        goto fail_free_shadow;
    flags = r0lab_lock();
    if (page->raw.mm == mm)
        page->hook_installed = true;
    r0lab_unlock(flags);

    if (!r0lab_target_mm_live(owner_tgid, mm))
        goto target_exit;

    result = r0lab_raw_arm_source_uxn(&page->raw);
    if (result)
        goto fail_unhook;

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
        result = r0lab_raw_start_monitor();
        if (result)
            goto fail_restore;
        r0lab_record(R0LAB_EVENT_RAW_ARM, 0);
        return 0;
    }

fail_restore:
    cleanup_result = result;
    flags = r0lab_lock();
    if (page == &g_raw_page && page->raw.mm == mm)
        page->record.state = R0LAB_PAGE_RECORD_RESTORING;
    r0lab_unlock(flags);
    result = r0lab_raw_restore_original(&page->raw);
    r0lab_raw_unhook();
    if (!result)
        result = r0lab_raw_wait_for_callbacks();
    if (!result)
        result = cleanup_result;
    r0lab_raw_reset(mm);
    r0lab_record(R0LAB_EVENT_RAW_CLEAR, result);
    return 0;

fail_unhook:
    r0lab_raw_unhook();
    (void)r0lab_raw_wait_for_callbacks();
fail_free_shadow:
    if (page->raw.shadow_kaddr) {
        g_vfree(page->raw.shadow_kaddr);
        page->raw.shadow_kaddr = NULL;
    }
fail:
    r0lab_raw_reset(mm);
    r0lab_record(R0LAB_EVENT_REJECT, result);
    return 0;

target_exit:
    r0lab_record(R0LAB_EVENT_TARGET_EXIT, 0);
    flags = r0lab_lock();
    if (page == &g_raw_page && page->raw.mm == mm)
        page->record.state = R0LAB_PAGE_RECORD_RESTORING;
    r0lab_unlock(flags);
    result = r0lab_raw_restore_original(&page->raw);
    r0lab_raw_unhook_except_exit();
    if (!result)
        result = r0lab_raw_wait_for_callbacks();
    r0lab_raw_reset_final(mm, generation);
    r0lab_close_exited_session(owner_tgid);
    r0lab_record(R0LAB_EVENT_RAW_CLEAR, result);
    return 0;
}

static int r0lab_raw_clear_worker(void *opaque)
{
    struct r0lab_raw_shadow_page *page = opaque;
    struct mm_struct *mm;
    unsigned long flags;
    int result;

    flags = r0lab_lock();
    if (!page || page != &g_raw_page || !page->armed || !page->clearing ||
        !page->raw.mm) {
        r0lab_unlock(flags);
        r0lab_record(R0LAB_EVENT_REJECT, R0LAB_ESRCH);
        return 0;
    }
    mm = (struct mm_struct *)page->raw.mm;
    page->record.state = R0LAB_PAGE_RECORD_RESTORING;
    r0lab_unlock(flags);

    result = r0lab_raw_restore_original(&page->raw);
    r0lab_raw_unhook();
    if (!result)
        result = r0lab_raw_wait_for_callbacks();
    r0lab_raw_reset(mm);
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
        bool arm_pending;
        bool clear_pending;
        unsigned long flags = r0lab_lock();

        arm_pending = g_raw_page.reserving;
        clear_pending = !arm_pending && g_raw_page.armed &&
                        g_raw_page.clearing && !g_raw_page.target_exiting;
        r0lab_unlock(flags);
        if (arm_pending) {
            r0lab_raw_arm_worker(&g_raw_page);
            continue;
        }
        if (clear_pending) {
            r0lab_raw_clear_worker(&g_raw_page);
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
        bool monitor_pending;
        unsigned long flags = r0lab_lock();

        monitor_pending = g_raw_page.monitor_running;
        r0lab_unlock(flags);
        if (monitor_pending) {
            r0lab_raw_monitor_worker(&g_raw_page);
            continue;
        }
        g_msleep(R0LAB_M5_MONITOR_INTERVAL_MS);
    }
    r0lab_worker_finished();
    return 0;
}

static long r0lab_raw_arm(uint64_t token, uint64_t page_address,
                          char __user *out_msg, int outlen)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    struct mm_struct *mm;
    unsigned long flags;
    int result = r0lab_validate_owner(token);

    if (result)
        goto record;
    if (!page_address || (page_address & (R0LAB_RAW_PAGE_SIZE - 1UL))) {
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
        g_session.token != token || r0lab_hwbp_slot_count_locked() ||
        r0lab_m3_slot_count_locked() || r0lab_m4_slot_count_locked() ||
        r0lab_raw_slot_count_locked() || r0lab_s4_slot_count_locked()) {
        r0lab_unlock(flags);
        g_mmput(mm);
        result = R0LAB_EBUSY;
        goto record;
    }
    memset(&g_raw_page, 0, sizeof(g_raw_page));
    g_raw_page.raw.mm = mm;
    g_raw_page.raw.address = (unsigned long)page_address;
    g_raw_page.generation = ++g_raw_generation;
    g_raw_page.record.source_address = g_raw_page.raw.address;
    g_raw_page.record.generation = g_raw_page.generation;
    g_raw_page.record.backend = R0LAB_PAGE_RECORD_RAW_TWO_PFN;
    g_raw_page.record.state = R0LAB_PAGE_RECORD_PREPARING;
    g_raw_page.reserving = true;
    r0lab_unlock(flags);

    result = r0lab_raw_start_worker(r0lab_raw_arm_worker);
    if (result) {
        r0lab_raw_reset(mm);
        goto record;
    }
    snprintf(reply, sizeof(reply), "raw_pending page=%llx\n", page_address);
    return r0lab_copy_reply(out_msg, outlen, reply);

record:
    r0lab_record(R0LAB_EVENT_REJECT, result);
    return result;
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
    r0lab_unlock(flags);
    snprintf(reply, sizeof(reply),
             "raw_ready page=%llx source_pfn_low=%lx shadow_pfn_low=%lx state=source_uxn record_backend=%s record_state=%s\n",
             (uint64_t)address, source_pfn & 0xffffUL, shadow_pfn & 0xffffUL,
             r0lab_page_record_backend_name(record_backend),
             r0lab_page_record_state_name(record_state));
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
    uint32_t activation_events;
    uint8_t record_backend;
    uint8_t record_state;
    const char *active_kind = "none";
    const char *read_cycle_mode = "absent";
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
    activation_events = g_raw_page.activation_events;
    record_backend = g_raw_page.record.backend;
    record_state = g_raw_page.record.state;
    r0lab_unlock(flags);

    if (state == R0LAB_RAW_ORIGINAL_READ)
        active_kind = "original_read";
    else if (active_pte == original_pte)
        active_kind = "original";
    else if (active_pte == source_uxn_pte)
        active_kind = "source_uxn";
    else if (active_pte == shadow_rx_pte)
        active_kind = "shadow_rx";
    else if (state == R0LAB_RAW_ORIGINAL_STEP)
        active_kind = "original_step";

    if (read_cycle_active || read_cycle_begin_events ||
        read_cycle_finish_events)
        read_cycle_mode = "uxn_original_exec_resume";
    if (read_cycle_begin_events)
        read_cycle_pte_switch = 1;

    snprintf(reply, sizeof(reply),
             "raw_inspect page=%llx state=%lu activations=%u active_kind=%s source_pfn_low=%lx shadow_pfn_low=%lx gup_hide_active=%lu gup_begin_events=%lu gup_finish_events=%lu gup_hook_installed=%u gup_hook_begin_events=%u gup_hook_finish_events=%u gup_hook_failures=%u fork_hide_active=%lu fork_begin_events=%lu fork_finish_events=%lu read_cycle_active=%lu read_cycle_begin_events=%lu read_cycle_finish_events=%lu fork_hook_installed=%u fork_hook_begin_events=%u fork_hook_finish_events=%u fork_hook_failures=%u fault_hook_installed=%u fault_hook_read_events=%u fault_hook_write_events=%u fault_hook_exec_events=%u fault_hook_failures=%u exit_hook_installed=%u exit_hook_events=%u exit_hook_failures=%u fault_probe_armed=%u fault_probe_page=%llx fault_probe_reader_tgid=%d fault_probe_read_events=%u fault_probe_write_events=%u fault_probe_exec_events=%u fault_probe_failures=%u record_backend=%s record_state=%s read_cycle=%s read_cycle_pte_switch=%u read_cycle_data_fault=absent read_cycle_exec_resume=%s fault_hook=observe_only exit_hook=exit_mmap_observe fault_probe=normal_anon_remote_gup original_view_after_shadow=%s gup_hide_primitive=%s fork_hide_primitive=%s\n",
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
             r0lab_page_record_backend_name(record_backend),
             r0lab_page_record_state_name(record_state), read_cycle_mode,
             read_cycle_pte_switch,
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

static long r0lab_raw_gup_hook_arm(uint64_t token, char __user *out_msg,
                                   int outlen)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    const char *hook_name;
    void *hook_target;
    unsigned long flags;
    bool installed;
    bool uses_pte;
    int result = r0lab_validate_owner(token);

    if (result)
        goto record;
    hook_target = g_follow_page_pte ? g_follow_page_pte : g_follow_page_mask;
    hook_name = g_follow_page_pte ? "follow_page_pte" : "follow_page_mask";
    uses_pte = !!g_follow_page_pte;
    if (!hook_target) {
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
    installed = g_raw_page.gup_hook_installed;
    if (!installed) {
        g_raw_page.gup_hook_begin_events = 0;
        g_raw_page.gup_hook_finish_events = 0;
        g_raw_page.gup_hook_failures = 0;
    }
    r0lab_unlock(flags);

    if (!installed) {
        if (uses_pte)
            result = hook_wrap5(hook_target, r0lab_raw_gup_pte_before,
                                r0lab_raw_gup_pte_after, NULL);
        else
            result = hook_wrap4(hook_target, r0lab_raw_gup_mask_before,
                                r0lab_raw_gup_mask_after, NULL);
        if (result)
            goto record;
        flags = r0lab_lock();
        if (g_raw_page.armed && !g_raw_page.clearing &&
            g_raw_page.raw.state == R0LAB_RAW_SHADOW_RX) {
            g_raw_page.gup_hook_installed = true;
            g_raw_page.gup_hook_uses_pte = uses_pte;
        } else {
            result = R0LAB_EAGAIN;
        }
        r0lab_unlock(flags);
        if (result) {
            if (uses_pte)
                hook_unwrap(hook_target, r0lab_raw_gup_pte_before,
                            r0lab_raw_gup_pte_after);
            else
                hook_unwrap(hook_target, r0lab_raw_gup_mask_before,
                            r0lab_raw_gup_mask_after);
            goto record;
        }
    }

    snprintf(reply, sizeof(reply),
             "raw_gup_hook_ready symbol=%s follow_page_pte=%s follow_page_mask=%s installed=1 mode=%s target_mm_scoped=1 external_reader=1\n",
             hook_name, g_follow_page_pte ? "present" : "absent",
             g_follow_page_mask ? "present" : "absent", hook_name);
    return r0lab_copy_reply(out_msg, outlen, reply);

record:
    r0lab_record(R0LAB_EVENT_REJECT, result);
    return result;
}

static long r0lab_raw_fault_hook_arm(uint64_t token, char __user *out_msg,
                                     int outlen)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    unsigned long flags;
    bool installed;
    int result = r0lab_validate_owner(token);

    if (result)
        goto record;
    if (!g_handle_mm_fault) {
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
    installed = g_raw_page.fault_hook_installed;
    if (!installed) {
        g_raw_page.fault_hook_read_events = 0;
        g_raw_page.fault_hook_write_events = 0;
        g_raw_page.fault_hook_exec_events = 0;
        g_raw_page.fault_hook_failures = 0;
    }
    r0lab_unlock(flags);

    if (!installed) {
        result = hook_wrap4(g_handle_mm_fault, r0lab_raw_fault_before,
                            NULL, NULL);
        if (result)
            goto record;
        flags = r0lab_lock();
        if (g_raw_page.armed && !g_raw_page.clearing &&
            g_raw_page.raw.state == R0LAB_RAW_SHADOW_RX) {
            g_raw_page.fault_hook_installed = true;
        } else {
            result = R0LAB_EAGAIN;
        }
        r0lab_unlock(flags);
        if (result) {
            hook_unwrap(g_handle_mm_fault, r0lab_raw_fault_before, NULL);
            goto record;
        }
    }

    snprintf(reply, sizeof(reply),
             "raw_fault_hook_ready symbol=handle_mm_fault installed=1 target_mm_scoped=1 observe_only=1 pte_switch=0\n");
    return r0lab_copy_reply(out_msg, outlen, reply);

record:
    r0lab_record(R0LAB_EVENT_REJECT, result);
    return result;
}

static long r0lab_raw_fault_hook_status(uint64_t token, char __user *out_msg,
                                        int outlen)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    unsigned long flags;
    bool installed;
    uint32_t read_events;
    uint32_t write_events;
    uint32_t exec_events;
    uint32_t failures;
    int result = r0lab_validate_owner(token);

    if (result)
        return result;
    flags = r0lab_lock();
    installed = g_raw_page.fault_hook_installed;
    read_events = g_raw_page.fault_hook_read_events;
    write_events = g_raw_page.fault_hook_write_events;
    exec_events = g_raw_page.fault_hook_exec_events;
    failures = g_raw_page.fault_hook_failures;
    r0lab_unlock(flags);

    snprintf(reply, sizeof(reply),
             "raw_fault_hook_status symbol=%s installed=%u read_events=%u write_events=%u exec_events=%u hit_events=%u failures=%u target_mm_scoped=1 observe_only=1 pte_switch=0\n",
             g_handle_mm_fault ? "handle_mm_fault" : "absent",
             installed ? 1 : 0, read_events, write_events, exec_events,
             read_events + write_events + exec_events, failures);
    return r0lab_copy_reply(out_msg, outlen, reply);
}

static long r0lab_raw_abort_probe_arm(uint64_t token, char __user *out_msg,
                                      int outlen)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    unsigned long flags;
    int result = r0lab_validate_owner(token);

    if (result)
        goto record;

    flags = r0lab_lock();
    if (!g_raw_page.armed || !g_raw_page.hook_installed ||
        g_raw_page.clearing || g_raw_page.transitioning ||
        g_raw_page.raw.state != R0LAB_RAW_SHADOW_RX ||
        g_session.owner_tgid != r0lab_current_tgid() ||
        g_raw_page.abort_probe_armed) {
        r0lab_unlock(flags);
        result = R0LAB_EAGAIN;
        goto record;
    }
    g_raw_page.abort_probe_armed = true;
    g_raw_page.abort_probe_read_events = 0;
    g_raw_page.abort_probe_write_events = 0;
    g_raw_page.abort_probe_exec_events = 0;
    g_raw_page.abort_probe_failures = 0;
    g_raw_page.abort_probe_last_esr = 0;
    g_raw_page.abort_probe_last_far = 0;
    r0lab_unlock(flags);

    snprintf(reply, sizeof(reply),
             "raw_abort_probe_ready symbol=do_mem_abort installed=1 armed=1 target_mm_scoped=1 source=raw_va_prot_none observe_only=1 pte_switch=0 data_fault=sync_el0_dabt read_cycle=absent\n");
    return r0lab_copy_reply(out_msg, outlen, reply);

record:
    r0lab_record(R0LAB_EVENT_REJECT, result);
    return result;
}

static long r0lab_raw_abort_probe_status(uint64_t token, char __user *out_msg,
                                         int outlen)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    unsigned long flags;
    bool installed;
    bool armed;
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
    int result = r0lab_validate_owner(token);

    if (result)
        return result;
    flags = r0lab_lock();
    installed = g_raw_page.hook_installed;
    armed = g_raw_page.abort_probe_armed;
    read_events = g_raw_page.abort_probe_read_events;
    write_events = g_raw_page.abort_probe_write_events;
    exec_events = g_raw_page.abort_probe_exec_events;
    failures = g_raw_page.abort_probe_failures;
    last_esr = g_raw_page.abort_probe_last_esr;
    last_far = g_raw_page.abort_probe_last_far;
    r0lab_unlock(flags);

    last_ec = last_esr >> R0LAB_M3_ESR_EC_SHIFT;
    last_fsc_type = last_esr & R0LAB_M3_ESR_FSC_TYPE;
    last_wnr = (last_esr & R0LAB_M3_ESR_WNR) ? 1 : 0;
    permission_fault = last_fsc_type == R0LAB_M3_ESR_FSC_PERM ? 1 : 0;
    translation_fault = last_fsc_type == 0x04U ? 1 : 0;
    snprintf(reply, sizeof(reply),
             "raw_abort_probe_status symbol=%s installed=%u armed=%u read_events=%u write_events=%u exec_events=%u hit_events=%u failures=%u last_far=%llx last_esr=%x last_ec=%u last_fsc_type=%x last_wnr=%u permission_fault=%u translation_fault=%u target_mm_scoped=1 source=raw_va_prot_none observe_only=1 pte_switch=0 data_fault=sync_el0_dabt read_cycle=absent\n",
             g_do_mem_abort ? "do_mem_abort" : "absent",
             installed ? 1 : 0, armed ? 1 : 0, read_events, write_events,
             exec_events, read_events + write_events + exec_events, failures,
             (uint64_t)last_far, last_esr, last_ec, last_fsc_type,
             last_wnr, permission_fault, translation_fault);
    return r0lab_copy_reply(out_msg, outlen, reply);
}

static long r0lab_raw_abort_probe_clear(uint64_t token, char __user *out_msg,
                                        int outlen)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    unsigned long flags;
    bool armed;
    uint32_t read_events;
    uint32_t write_events;
    uint32_t exec_events;
    uint32_t failures;
    uint32_t last_esr;
    unsigned long last_far;
    int result = r0lab_validate_owner(token);

    if (result)
        return result;
    flags = r0lab_lock();
    armed = g_raw_page.abort_probe_armed;
    g_raw_page.abort_probe_armed = false;
    read_events = g_raw_page.abort_probe_read_events;
    write_events = g_raw_page.abort_probe_write_events;
    exec_events = g_raw_page.abort_probe_exec_events;
    failures = g_raw_page.abort_probe_failures;
    last_esr = g_raw_page.abort_probe_last_esr;
    last_far = g_raw_page.abort_probe_last_far;
    r0lab_unlock(flags);

    if (armed) {
        result = r0lab_raw_wait_for_callbacks();
        if (result)
            goto record;
    }
    snprintf(reply, sizeof(reply),
             "raw_abort_probe_cleared armed=0 read_events=%u write_events=%u exec_events=%u hit_events=%u failures=%u last_far=%llx last_esr=%x\n",
             read_events, write_events, exec_events,
             read_events + write_events + exec_events, failures,
             (uint64_t)last_far, last_esr);
    return r0lab_copy_reply(out_msg, outlen, reply);

record:
    r0lab_record(R0LAB_EVENT_REJECT, result);
    return result;
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

static long r0lab_raw_fault_hook_clear(uint64_t token, char __user *out_msg,
                                       int outlen)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    unsigned long flags;
    uint32_t read_events;
    uint32_t write_events;
    uint32_t exec_events;
    uint32_t failures;
    int result = r0lab_validate_owner(token);

    if (result)
        return result;
    r0lab_raw_fault_unhook();
    flags = r0lab_lock();
    read_events = g_raw_page.fault_hook_read_events;
    write_events = g_raw_page.fault_hook_write_events;
    exec_events = g_raw_page.fault_hook_exec_events;
    failures = g_raw_page.fault_hook_failures;
    r0lab_unlock(flags);
    snprintf(reply, sizeof(reply),
             "raw_fault_hook_cleared symbol=%s installed=0 read_events=%u write_events=%u exec_events=%u hit_events=%u failures=%u\n",
             g_handle_mm_fault ? "handle_mm_fault" : "absent", read_events,
             write_events, exec_events, read_events + write_events + exec_events,
             failures);
    return r0lab_copy_reply(out_msg, outlen, reply);
}

static long r0lab_raw_syscall_hook_arm_common(uint64_t token,
                                              char __user *out_msg,
                                              int outlen,
                                              bool read_cycle_mode)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    unsigned long flags;
    bool installed;
    bool existing_read_cycle_mode;
    int result = r0lab_validate_owner(token);

    if (result)
        goto record;
    if (!g_sys_getpid) {
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
    installed = g_raw_page.syscall_hook_installed;
    existing_read_cycle_mode = g_raw_page.syscall_hook_read_cycle_mode;
    if (installed && existing_read_cycle_mode != read_cycle_mode) {
        r0lab_unlock(flags);
        result = R0LAB_EBUSY;
        goto record;
    }
    if (!installed) {
        g_raw_page.syscall_hook_events = 0;
        g_raw_page.syscall_hook_read_cycle_events = 0;
        g_raw_page.syscall_hook_failures = 0;
    }
    r0lab_unlock(flags);

    if (!installed) {
        result = hook_wrap1(g_sys_getpid, r0lab_raw_syscall_before,
                            NULL, NULL);
        if (result)
            goto record;
        flags = r0lab_lock();
        if (g_raw_page.armed && !g_raw_page.clearing &&
            g_raw_page.raw.state == R0LAB_RAW_SHADOW_RX) {
            g_raw_page.syscall_hook_installed = true;
            g_raw_page.syscall_hook_read_cycle_mode = read_cycle_mode;
        } else {
            result = R0LAB_EAGAIN;
        }
        r0lab_unlock(flags);
        if (result) {
            hook_unwrap(g_sys_getpid, r0lab_raw_syscall_before, NULL);
            goto record;
        }
    }

    if (read_cycle_mode)
        snprintf(reply, sizeof(reply),
                 "raw_syscall_read_cycle_hook_ready symbol=getpid installed=1 target_mm_scoped=1 trigger=syscall_getpid read_cycle=uxn_original_exec_resume observe_only=0 pte_switch=1 data_fault=absent exec_resume=pending\n");
    else
        snprintf(reply, sizeof(reply),
                 "raw_syscall_hook_ready symbol=getpid installed=1 target_mm_scoped=1 trigger=syscall_getpid observe_only=1 pte_switch=0\n");
    return r0lab_copy_reply(out_msg, outlen, reply);

record:
    r0lab_record(R0LAB_EVENT_REJECT, result);
    return result;
}

static long r0lab_raw_syscall_hook_arm(uint64_t token, char __user *out_msg,
                                       int outlen)
{
    return r0lab_raw_syscall_hook_arm_common(token, out_msg, outlen, false);
}

static long r0lab_raw_syscall_read_cycle_hook_arm(uint64_t token,
                                                  char __user *out_msg,
                                                  int outlen)
{
    return r0lab_raw_syscall_hook_arm_common(token, out_msg, outlen, true);
}

static long r0lab_raw_syscall_hook_status(uint64_t token,
                                          char __user *out_msg, int outlen)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    unsigned long flags;
    bool installed;
    bool read_cycle_mode;
    uint32_t hit_events;
    uint32_t read_cycle_events;
    unsigned long read_cycle_finish_events;
    uint32_t failures;
    int result = r0lab_validate_owner(token);

    if (result)
        return result;
    flags = r0lab_lock();
    installed = g_raw_page.syscall_hook_installed;
    read_cycle_mode = g_raw_page.syscall_hook_read_cycle_mode;
    hit_events = g_raw_page.syscall_hook_events;
    read_cycle_events = g_raw_page.syscall_hook_read_cycle_events;
    read_cycle_finish_events = g_raw_page.raw.read_cycle_finish_events;
    failures = g_raw_page.syscall_hook_failures;
    r0lab_unlock(flags);

    if (read_cycle_mode)
        snprintf(reply, sizeof(reply),
                 "raw_syscall_hook_status symbol=%s installed=%u hit_events=%u read_cycle_events=%u failures=%u target_mm_scoped=1 trigger=syscall_getpid read_cycle=uxn_original_exec_resume observe_only=0 pte_switch=1 data_fault=absent exec_resume=%s\n",
                 g_sys_getpid ? "getpid" : "absent", installed ? 1 : 0,
                 hit_events, read_cycle_events, failures,
                 read_cycle_finish_events ? "proven" :
                 (read_cycle_events ? "pending" : "absent"));
    else
        snprintf(reply, sizeof(reply),
                 "raw_syscall_hook_status symbol=%s installed=%u hit_events=%u failures=%u target_mm_scoped=1 trigger=syscall_getpid observe_only=1 pte_switch=0\n",
                 g_sys_getpid ? "getpid" : "absent", installed ? 1 : 0,
                 hit_events, failures);
    return r0lab_copy_reply(out_msg, outlen, reply);
}

static long r0lab_raw_syscall_hook_clear(uint64_t token, char __user *out_msg,
                                         int outlen)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    unsigned long flags;
    uint32_t hit_events;
    uint32_t read_cycle_events;
    uint32_t failures;
    int result = r0lab_validate_owner(token);

    if (result)
        return result;
    r0lab_raw_syscall_unhook();
    flags = r0lab_lock();
    hit_events = g_raw_page.syscall_hook_events;
    read_cycle_events = g_raw_page.syscall_hook_read_cycle_events;
    failures = g_raw_page.syscall_hook_failures;
    r0lab_unlock(flags);
    snprintf(reply, sizeof(reply),
             "raw_syscall_hook_cleared symbol=%s installed=0 hit_events=%u read_cycle_events=%u failures=%u\n",
             g_sys_getpid ? "getpid" : "absent", hit_events,
             read_cycle_events, failures);
    return r0lab_copy_reply(out_msg, outlen, reply);
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
            hook_unwrap(g_exit_mmap, r0lab_raw_exit_mmap_before, NULL);
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
    r0lab_raw_exit_unhook();
    flags = r0lab_lock();
    hit_events = g_raw_page.exit_hook_events;
    failures = g_raw_page.exit_hook_failures;
    r0lab_unlock(flags);
    snprintf(reply, sizeof(reply),
             "raw_exit_hook_cleared symbol=%s installed=0 hit_events=%u failures=%u\n",
             g_exit_mmap ? "exit_mmap" : "absent", hit_events, failures);
    return r0lab_copy_reply(out_msg, outlen, reply);
}

static long r0lab_raw_gup_hook_status(uint64_t token, char __user *out_msg,
                                      int outlen)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    unsigned long flags;
    bool installed;
    bool uses_pte;
    uint32_t hook_begin_events;
    uint32_t hook_finish_events;
    uint32_t hook_failures;
    unsigned long primitive_begin_events;
    unsigned long primitive_finish_events;
    int result = r0lab_validate_owner(token);

    if (result)
        return result;
    flags = r0lab_lock();
    installed = g_raw_page.gup_hook_installed;
    uses_pte = g_raw_page.gup_hook_uses_pte;
    hook_begin_events = g_raw_page.gup_hook_begin_events;
    hook_finish_events = g_raw_page.gup_hook_finish_events;
    hook_failures = g_raw_page.gup_hook_failures;
    primitive_begin_events = g_raw_page.raw.gup_begin_events;
    primitive_finish_events = g_raw_page.raw.gup_finish_events;
    r0lab_unlock(flags);

    snprintf(reply, sizeof(reply),
             "raw_gup_hook_status symbol=%s follow_page_pte=%s follow_page_mask=%s installed=%u hook_begin_events=%u hook_finish_events=%u hook_failures=%u primitive_begin_events=%lu primitive_finish_events=%lu target_mm_scoped=1 external_reader=1\n",
             installed ? (uses_pte ? "follow_page_pte" : "follow_page_mask") :
                         (g_follow_page_pte ? "follow_page_pte" :
                          (g_follow_page_mask ? "follow_page_mask" : "absent")),
             g_follow_page_pte ? "present" : "absent",
             g_follow_page_mask ? "present" : "absent", installed ? 1 : 0,
             hook_begin_events, hook_finish_events, hook_failures,
             primitive_begin_events, primitive_finish_events);
    return r0lab_copy_reply(out_msg, outlen, reply);
}

static long r0lab_raw_gup_hook_clear(uint64_t token, char __user *out_msg,
                                     int outlen)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    unsigned long flags;
    uint32_t hook_begin_events;
    uint32_t hook_finish_events;
    uint32_t hook_failures;
    int result = r0lab_validate_owner(token);

    if (result)
        return result;
    r0lab_raw_gup_unhook();
    flags = r0lab_lock();
    hook_begin_events = g_raw_page.gup_hook_begin_events;
    hook_finish_events = g_raw_page.gup_hook_finish_events;
    hook_failures = g_raw_page.gup_hook_failures;
    r0lab_unlock(flags);
    snprintf(reply, sizeof(reply),
             "raw_gup_hook_cleared follow_page_pte=%s follow_page_mask=%s installed=0 hook_begin_events=%u hook_finish_events=%u hook_failures=%u\n",
             g_follow_page_pte ? "present" : "absent",
             g_follow_page_mask ? "present" : "absent", hook_begin_events,
             hook_finish_events, hook_failures);
    return r0lab_copy_reply(out_msg, outlen, reply);
}

static long r0lab_raw_fork_hook_arm(uint64_t token, char __user *out_msg,
                                    int outlen)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    unsigned long flags;
    bool installed;
    int result = r0lab_validate_owner(token);

    if (result)
        goto record;
    if (!g_dup_mmap) {
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
    installed = g_raw_page.fork_hook_installed;
    if (!installed) {
        g_raw_page.fork_hook_begin_events = 0;
        g_raw_page.fork_hook_finish_events = 0;
        g_raw_page.fork_hook_failures = 0;
    }
    r0lab_unlock(flags);

    if (!installed) {
        result = hook_wrap2(g_dup_mmap, r0lab_raw_fork_before,
                            r0lab_raw_fork_after, NULL);
        if (result)
            goto record;
        flags = r0lab_lock();
        if (g_raw_page.armed && !g_raw_page.clearing &&
            g_raw_page.raw.state == R0LAB_RAW_SHADOW_RX) {
            g_raw_page.fork_hook_installed = true;
        } else {
            result = R0LAB_EAGAIN;
        }
        r0lab_unlock(flags);
        if (result) {
            hook_unwrap(g_dup_mmap, r0lab_raw_fork_before,
                        r0lab_raw_fork_after);
            goto record;
        }
    }

    snprintf(reply, sizeof(reply),
             "raw_fork_hook_ready symbol=dup_mmap installed=1 parent_pause=1 child_original_inherit=1 target_mm_scoped=1\n");
    return r0lab_copy_reply(out_msg, outlen, reply);

record:
    r0lab_record(R0LAB_EVENT_REJECT, result);
    return result;
}

static long r0lab_raw_fork_hook_status(uint64_t token, char __user *out_msg,
                                       int outlen)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    unsigned long flags;
    bool installed;
    uint32_t hook_begin_events;
    uint32_t hook_finish_events;
    uint32_t hook_failures;
    unsigned long primitive_begin_events;
    unsigned long primitive_finish_events;
    int result = r0lab_validate_owner(token);

    if (result)
        return result;
    flags = r0lab_lock();
    installed = g_raw_page.fork_hook_installed;
    hook_begin_events = g_raw_page.fork_hook_begin_events;
    hook_finish_events = g_raw_page.fork_hook_finish_events;
    hook_failures = g_raw_page.fork_hook_failures;
    primitive_begin_events = g_raw_page.raw.fork_begin_events;
    primitive_finish_events = g_raw_page.raw.fork_finish_events;
    r0lab_unlock(flags);

    snprintf(reply, sizeof(reply),
             "raw_fork_hook_status symbol=%s installed=%u hook_begin_events=%u hook_finish_events=%u hook_failures=%u primitive_begin_events=%lu primitive_finish_events=%lu parent_pause=1 child_original_inherit=1 target_mm_scoped=1\n",
             g_dup_mmap ? "dup_mmap" : "absent", installed ? 1 : 0,
             hook_begin_events, hook_finish_events, hook_failures,
             primitive_begin_events, primitive_finish_events);
    return r0lab_copy_reply(out_msg, outlen, reply);
}

static long r0lab_raw_fork_hook_clear(uint64_t token, char __user *out_msg,
                                      int outlen)
{
    char reply[R0LAB_OUTPUT_CAPACITY];
    unsigned long flags;
    uint32_t hook_begin_events;
    uint32_t hook_finish_events;
    uint32_t hook_failures;
    int result = r0lab_validate_owner(token);

    if (result)
        return result;
    r0lab_raw_fork_unhook();
    flags = r0lab_lock();
    hook_begin_events = g_raw_page.fork_hook_begin_events;
    hook_finish_events = g_raw_page.fork_hook_finish_events;
    hook_failures = g_raw_page.fork_hook_failures;
    r0lab_unlock(flags);
    snprintf(reply, sizeof(reply),
             "raw_fork_hook_cleared symbol=%s installed=0 hook_begin_events=%u hook_finish_events=%u hook_failures=%u\n",
             g_dup_mmap ? "dup_mmap" : "absent", hook_begin_events,
             hook_finish_events, hook_failures);
    return r0lab_copy_reply(out_msg, outlen, reply);
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
    g_mte_sync_tags = (r0lab_mte_sync_tags_fn_t)
        r0lab_lookup_first("mte_sync_tags", "mte_sync_tags.cfi_jt");

    kernel_memstart = (int64_t *)kallsyms_lookup_name("memstart_addr");
    kernel_hwcaps = (unsigned long *)kallsyms_lookup_name("cpu_hwcaps");
    if (!g_vmalloc || !g_vfree || !g_vmalloc_to_page || !g_raw_down_read ||
        !g_raw_up_read || !g_raw_spin_lock || !g_raw_spin_unlock ||
        !g_raw_find_vma || !g_sync_icache_dcache || !g_mte_sync_tags ||
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
    if (!strncmp(args, "s4 brk arm ", 11)) {
        value = args + 11;
        if (r0lab_parse_u64_pair(value, &token, &page_address))
            return R0LAB_EINVAL;
        return r0lab_s4_brk_arm(token, page_address, false, false, out_msg, outlen);
    }
    if (!strncmp(args, "s4 step arm ", 12)) {
        value = args + 12;
        if (r0lab_parse_u64_pair(value, &token, &page_address))
            return R0LAB_EINVAL;
        return r0lab_s4_brk_arm(token, page_address, true, false, out_msg, outlen);
    }
    if (!strncmp(args, "s4 raw-step arm ", 16)) {
        value = args + 16;
        if (r0lab_parse_u64_pair(value, &token, &page_address))
            return R0LAB_EINVAL;
        return r0lab_s4_brk_arm(token, page_address, true, true, out_msg, outlen);
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
    if (!strncmp(args, "raw arm ", 8)) {
        value = args + 8;
        if (r0lab_parse_u64_pair(value, &token, &page_address))
            return R0LAB_EINVAL;
        return r0lab_raw_arm(token, page_address, out_msg, outlen);
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
    memset(&g_raw_page, 0, sizeof(g_raw_page));
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
    g_do_mem_abort = (void *)kallsyms_lookup_name("do_mem_abort");
    g_handle_mm_fault = r0lab_lookup_first("handle_mm_fault.cfi_jt",
                                           "handle_mm_fault");
    g_dup_mmap = r0lab_lookup_first("dup_mmap.cfi_jt", "dup_mmap");
    g_exit_mmap = r0lab_lookup_first("exit_mmap.cfi_jt", "exit_mmap");
    g_sys_getpid = r0lab_lookup_first("__arm64_sys_getpid.cfi_jt",
                                      "__arm64_sys_getpid");
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
        !g_get_task_mm || !g_mmput || !g_do_mem_abort)
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
    r0lab_record(R0LAB_EVENT_UNLOAD, 0);
    g_initialized = false;
    pr_info("r0lab-m1: unloaded\n");
    return 0;
}

KPM_INIT(r0lab_init);
KPM_CTL0(r0lab_control0);
KPM_EXIT(r0lab_exit);
