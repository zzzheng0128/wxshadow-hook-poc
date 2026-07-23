#include <jni.h>

#include <dlfcn.h>
#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <unistd.h>

#define R0LAB_SUPERCALL_NR 45
#define R0LAB_KERNELPATCH_VERSION 0x0d01UL
#define R0LAB_SUPERCALL_MAGIC 0x1158UL
#define R0LAB_SUPERCALL_KPM_CONTROL 0x1022UL
#define R0LAB_MODULE_NAME "r0lab-m1"
#define R0LAB_M3_PAGE_SIZE 4096U
#define R0LAB_M3_CODE_MOV_W0_42 0x52800540U
#define R0LAB_M4_CODE_MOV_W0_99 0x52800c60U
#define R0LAB_CODE_MOV_W0_77 0x528009a0U
#define R0LAB_M3_CODE_RET 0xd65f03c0U
#define R0LAB_S4_CODE_BRK_7 0xd42000e0U
#define R0LAB_S4_RAW_REG_VALUE 73
#define R0LAB_FAULT_PROBE_WORD 0x13579bdfU
#define R0LAB_PRCTL_MAGIC 0x52304c42U
#define R0LAB_PRCTL_OP_READ_CYCLE 1U
#define R0LAB_PRCTL_OP_PATCH_WORD 2U
#define R0LAB_PRCTL_OP_RELEASE_PATCH 3U
#define R0LAB_PRCTL_OP_PATCH_RANGE 4U
#define R0LAB_PRCTL_OP_RELEASE_RANGE 5U
#define R0LAB_PATCH_RECORD_CAPACITY 1024U
#define R0LAB_PRCTL_GET_DUMPABLE 3U

static pthread_mutex_t g_r0lab_control_lock = PTHREAD_MUTEX_INITIALIZER;
static sigjmp_buf g_r0lab_xom_jump;
static volatile sig_atomic_t g_r0lab_xom_stage;
static volatile sig_atomic_t g_r0lab_xom_exec_faults;
static volatile sig_atomic_t g_r0lab_xom_read_faults;
static void *g_r0lab_xom_page;
static size_t g_r0lab_xom_page_size;
static void *g_r0lab_s4_brk_page;
static sigjmp_buf g_r0lab_s4_jump;
static volatile sig_atomic_t g_r0lab_s4_stage;
static volatile sig_atomic_t g_r0lab_s4_brk_traps;
static volatile sig_atomic_t g_r0lab_s4_step_traps;
static volatile sig_atomic_t g_r0lab_s4_brk_unexpected;
static volatile sig_atomic_t g_r0lab_s4_last_signal;
static volatile sig_atomic_t g_r0lab_s4_last_si_code;
static volatile uintptr_t g_r0lab_s4_last_pc;

struct r0lab_m5_hold_state {
    void *source_page;
    void *clone_page;
    size_t page_size;
    uint64_t token;
    int mode;
    int armed;
};

static struct r0lab_m5_hold_state g_r0lab_m5_hold;

struct r0lab_gup_reader_result {
    int32_t rc;
    int32_t err;
    uint32_t word;
};

struct r0lab_fork_child_result {
    int32_t value;
    int32_t err;
    uint32_t word;
};

struct r0lab_prctl_passthrough_stress {
    atomic_bool stop;
    atomic_ulong iterations;
    atomic_ulong failures;
    long expected;
};

struct r0lab_prctl_patch_request {
    uint64_t address;
    uint32_t length;
    uint32_t flags;
    uint8_t data[16];
};

extern char **environ;

static void *r0lab_prctl_passthrough_stress_thread(void *opaque)
{
    struct r0lab_prctl_passthrough_stress *stress = opaque;

    while (!atomic_load_explicit(&stress->stop, memory_order_relaxed)) {
        long value = syscall(__NR_prctl, R0LAB_PRCTL_GET_DUMPABLE,
                             0, 0, 0, 0);

        if (value != stress->expected)
            atomic_fetch_add_explicit(&stress->failures, 1,
                                      memory_order_relaxed);
        atomic_fetch_add_explicit(&stress->iterations, 1,
                                  memory_order_relaxed);
    }
    return NULL;
}

static long r0lab_supercall_command(unsigned long command)
{
    return ((long)R0LAB_KERNELPATCH_VERSION << 32) |
           ((long)R0LAB_SUPERCALL_MAGIC << 16) |
           (long)command;
}

__attribute__((noinline, visibility("hidden"))) int r0lab_marker(int value)
{
    return value * 5 + 7;
}

extern int r0lab_marker_callsite(int value);
extern const char r0lab_marker_return_site[];

/* A fixed, visible return instruction for the paired return-address test. */
__asm__(
    ".text\n"
    ".align 2\n"
    ".global r0lab_marker_callsite\n"
    ".hidden r0lab_marker_callsite\n"
    ".type r0lab_marker_callsite,%function\n"
    "r0lab_marker_callsite:\n"
    "stp x29, x30, [sp, #-16]!\n"
    "bl r0lab_marker\n"
    "ldp x29, x30, [sp], #16\n"
    ".global r0lab_marker_return_site\n"
    "r0lab_marker_return_site:\n"
    "ret\n"
    ".size r0lab_marker_callsite, .-r0lab_marker_callsite\n");

struct r0lab_m2_result {
    uint64_t token;
    long arm_rc;
    long ready_rc;
    long clear_rc;
    long cleared_rc;
    int marker_value;
    int invoke_marker;
};

static long r0lab_control_raw(const char *args, char *reply, size_t reply_size)
{
    long result;

    pthread_mutex_lock(&g_r0lab_control_lock);
    result = syscall(R0LAB_SUPERCALL_NR, "su",
                     r0lab_supercall_command(R0LAB_SUPERCALL_KPM_CONTROL),
                     R0LAB_MODULE_NAME, args, reply, reply_size);
    pthread_mutex_unlock(&g_r0lab_control_lock);
    return result;
}

static int r0lab_parse_token(const char *text, uint64_t *token)
{
    uint64_t value = 0;
    int base = 10;

    if (!text || !text[0] || !token)
        return -1;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
        text += 2;
        if (!text[0])
            return -1;
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
            return -1;
        if (digit >= base || value > (UINT64_MAX - (uint64_t)digit) / (uint64_t)base)
            return -1;
        value = value * (uint64_t)base + (uint64_t)digit;
        ++text;
    }
    *token = value;
    return 0;
}

static void r0lab_xom_signal_handler(int signal_number, siginfo_t *info,
                                     void *context)
{
    uintptr_t page_start = (uintptr_t)g_r0lab_xom_page;
    uintptr_t page_end = page_start + g_r0lab_xom_page_size;
    uintptr_t fault_address = info ? (uintptr_t)info->si_addr : 0;

    (void)context;
    if (signal_number != SIGSEGV || !page_start ||
        fault_address < page_start || fault_address >= page_end) {
        syscall(__NR_exit_group, 128 + SIGSEGV);
        return;
    }
    if (g_r0lab_xom_stage == 1) {
        ++g_r0lab_xom_exec_faults;
        siglongjmp(g_r0lab_xom_jump, 1);
    }
    if (g_r0lab_xom_stage == 2) {
        ++g_r0lab_xom_read_faults;
        siglongjmp(g_r0lab_xom_jump, 1);
    }
    syscall(__NR_exit_group, 128 + SIGSEGV);
}

static void r0lab_s4_brk_signal_handler(int signal_number, siginfo_t *info,
                                         void *context)
{
#if defined(__aarch64__)
    ucontext_t *ucontext = (ucontext_t *)context;
    uintptr_t brk_pc = (uintptr_t)g_r0lab_s4_brk_page;
    uintptr_t pc = ucontext ? (uintptr_t)ucontext->uc_mcontext.pc : 0;

    g_r0lab_s4_last_signal = signal_number;
    g_r0lab_s4_last_si_code = info ? info->si_code : 0;
    g_r0lab_s4_last_pc = pc;
    if (signal_number == SIGTRAP && brk_pc && pc == brk_pc) {
        ++g_r0lab_s4_brk_traps;
        ucontext->uc_mcontext.pc = brk_pc + 4;
        return;
    }
    if (signal_number == SIGTRAP && brk_pc && pc == brk_pc + 4) {
        ++g_r0lab_s4_step_traps;
        return;
    }
    if (signal_number == SIGTRAP && brk_pc && pc == brk_pc + 8) {
        ++g_r0lab_s4_step_traps;
        return;
    }
#else
    (void)info;
    (void)context;
#endif
    ++g_r0lab_s4_brk_unexpected;
    if (g_r0lab_s4_stage)
        siglongjmp(g_r0lab_s4_jump, 1);
    syscall(__NR_exit_group, 128 + SIGTRAP);
}

static int r0lab_xom_probe(char *output, size_t output_size)
{
    struct sigaction action = {0};
    struct sigaction previous_action = {0};
    volatile uint32_t *readable_code;
    uint32_t *code;
    void *page;
    size_t page_size;
    int exec_value = -1;
    uint32_t read_value = 0;
    int mprotect_rc;
    int handler_installed = 0;
    int result;

    page_size = (size_t)sysconf(_SC_PAGESIZE);
    if (page_size != R0LAB_M3_PAGE_SIZE) {
        snprintf(output, output_size,
                 "xom_probe result=unsupported page_size=%zu", page_size);
        return -1;
    }
    page = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) {
        snprintf(output, output_size,
                 "xom_probe result=fail mmap_errno=%d", errno);
        return -1;
    }

    code = page;
    code[0] = R0LAB_M3_CODE_MOV_W0_42;
    code[1] = R0LAB_M3_CODE_RET;
    __builtin___clear_cache((char *)page, (char *)page + page_size);
    errno = 0;
    mprotect_rc = mprotect(page, page_size, PROT_EXEC);

    g_r0lab_xom_page = page;
    g_r0lab_xom_page_size = page_size;
    g_r0lab_xom_stage = 0;
    g_r0lab_xom_exec_faults = 0;
    g_r0lab_xom_read_faults = 0;
    action.sa_sigaction = r0lab_xom_signal_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_SIGINFO;
    if (!sigaction(SIGSEGV, &action, &previous_action))
        handler_installed = 1;

    if (!mprotect_rc && handler_installed) {
        if (!sigsetjmp(g_r0lab_xom_jump, 1)) {
            g_r0lab_xom_stage = 1;
            exec_value = ((int (*)(void))page)();
        }
        if (!sigsetjmp(g_r0lab_xom_jump, 1)) {
            g_r0lab_xom_stage = 2;
            readable_code = (volatile uint32_t *)page;
            read_value = readable_code[0];
        }
    }
    g_r0lab_xom_stage = 0;

    if (handler_installed)
        sigaction(SIGSEGV, &previous_action, NULL);
    g_r0lab_xom_page = NULL;
    g_r0lab_xom_page_size = 0;
    result = (mprotect_rc == 0 && exec_value == 42 &&
              g_r0lab_xom_exec_faults == 0 &&
              g_r0lab_xom_read_faults == 1);
    snprintf(output, output_size,
             "xom_probe result=%s mprotect_exec_rc=%d exec_value=%d exec_faults=%d read_faults=%d read_value=%08x user_xom_read_fault=%s",
             result ? "pass" : "blocked", mprotect_rc, exec_value,
             (int)g_r0lab_xom_exec_faults, (int)g_r0lab_xom_read_faults,
             read_value, result ? "proven" : "absent");
    munmap(page, page_size);
    return result ? 0 : -1;
}

static long r0lab_m2_wait_for(const char *verb, uint64_t token)
{
    char command[64];
    char reply[128];
    int attempt;

    snprintf(command, sizeof(command), "%s 0x%llx", verb,
             (unsigned long long)token);
    for (attempt = 0; attempt < 1000; ++attempt) {
        long rc;

        memset(reply, 0, sizeof(reply));
        errno = 0;
        rc = r0lab_control_raw(command, reply, sizeof(reply));
        if (rc >= 0)
            return rc;
        if (errno != EAGAIN)
            return rc;
        usleep(1000);
    }
    errno = EAGAIN;
    return -1;
}

static void r0lab_m2_clear_one(struct r0lab_m2_result *result)
{
    char command[64];
    char reply[128];
    int attempt;

    if (result->arm_rc < 0)
        return;
    snprintf(command, sizeof(command), "hwbp clear 0x%llx",
             (unsigned long long)result->token);
    for (attempt = 0; attempt < 1000; ++attempt) {
        long rc;

        memset(reply, 0, sizeof(reply));
        errno = 0;
        rc = r0lab_control_raw(command, reply, sizeof(reply));
        if (rc >= 0) {
            result->clear_rc = rc;
            result->cleared_rc = r0lab_m2_wait_for("hwbp cleared", result->token);
            return;
        }
        if (errno != EAGAIN) {
            result->clear_rc = rc;
            return;
        }
        usleep(1000);
    }
    errno = EAGAIN;
    result->clear_rc = -1;
}

static void r0lab_m2_run_one(struct r0lab_m2_result *result)
{
    char command[160];
    char reply[1024] = {0};

    snprintf(command, sizeof(command), "hwbp arm 0x%llx 0x%llx 0x%llx",
             (unsigned long long)result->token,
             (unsigned long long)(uintptr_t)&r0lab_marker,
             (unsigned long long)(uintptr_t)r0lab_marker_return_site);
    errno = 0;
    result->arm_rc = r0lab_control_raw(command, reply, sizeof(reply));
    if (result->arm_rc < 0)
        return;

    result->ready_rc = r0lab_m2_wait_for("hwbp ready", result->token);
    if (result->ready_rc < 0) {
        r0lab_m2_clear_one(result);
        return;
    }
    if (result->invoke_marker)
        result->marker_value = r0lab_marker_callsite(11);
    r0lab_m2_clear_one(result);
}

static void *r0lab_m2_worker(void *opaque)
{
    r0lab_m2_run_one((struct r0lab_m2_result *)opaque);
    return NULL;
}

static int r0lab_m2_run_phase(const char *token_text, int thread_count,
                              int invoke_marker, const char *mode,
                              char *output, size_t output_size)
{
    enum { R0LAB_M2_MAX_THREAD_COUNT = 3 };
    struct r0lab_m2_result results[R0LAB_M2_MAX_THREAD_COUNT] = {0};
    pthread_t workers[R0LAB_M2_MAX_THREAD_COUNT - 1];
    uint64_t token;
    int failures = 0;
    int created = 0;
    int index;

    if (thread_count < 1 || thread_count > R0LAB_M2_MAX_THREAD_COUNT ||
        r0lab_parse_token(token_text, &token)) {
        snprintf(output, output_size, "rc=-22 error=invalid M2 token");
        return -1;
    }
    for (index = 0; index < thread_count; ++index) {
        results[index].token = token;
        results[index].arm_rc = -1;
        results[index].ready_rc = -1;
        results[index].clear_rc = -1;
        results[index].cleared_rc = -1;
        results[index].marker_value = -1;
        results[index].invoke_marker = invoke_marker;
    }
    for (index = 1; index < thread_count; ++index) {
        if (pthread_create(&workers[index - 1], NULL, r0lab_m2_worker,
                           &results[index])) {
            int worker_index;

            for (worker_index = 0; worker_index < created; ++worker_index)
                pthread_join(workers[worker_index], NULL);
            snprintf(output, output_size, "rc=-12 error=worker create index=%d", index);
            return -1;
        }
        ++created;
    }
    r0lab_m2_run_one(&results[0]);
    for (index = 0; index < created; ++index)
        pthread_join(workers[index], NULL);
    for (index = 0; index < thread_count; ++index) {
        if (results[index].arm_rc < 0 || results[index].ready_rc < 0 ||
            results[index].clear_rc < 0 || results[index].cleared_rc < 0 ||
            (invoke_marker && results[index].marker_value != 62))
            ++failures;
    }
    snprintf(output, output_size,
             "m2 threads=%d expected_entry=%d expected_return=%d failures=%d mode=%s marker_values=%d,%d,%d arm_rc=%ld,%ld,%ld ready_rc=%ld,%ld,%ld clear_rc=%ld,%ld,%ld cleared_rc=%ld,%ld,%ld",
             thread_count, invoke_marker ? thread_count : 0,
             invoke_marker ? thread_count : 0, failures, mode,
             results[0].marker_value, results[1].marker_value, results[2].marker_value,
             results[0].arm_rc, results[1].arm_rc, results[2].arm_rc,
             results[0].ready_rc, results[1].ready_rc, results[2].ready_rc,
             results[0].clear_rc, results[1].clear_rc, results[2].clear_rc,
             results[0].cleared_rc, results[1].cleared_rc, results[2].cleared_rc);
    return failures ? -1 : 0;
}

static int r0lab_m2_run(const char *token_text, char *output, size_t output_size)
{
    return r0lab_m2_run_phase(token_text, 3, 1, "multi", output, output_size);
}

static int r0lab_m2_run_single(const char *token_text, char *output,
                                size_t output_size)
{
    return r0lab_m2_run_phase(token_text, 1, 1, "single", output, output_size);
}

static int r0lab_m2_run_preflight(const char *token_text, char *output,
                                   size_t output_size)
{
    return r0lab_m2_run_phase(token_text, 1, 0, "preflight", output, output_size);
}

struct r0lab_m3_thread {
    void *page;
    volatile int *ready;
    volatile int *start;
    int value;
};

static volatile sig_atomic_t g_r0lab_m3_expected_signals;
static volatile sig_atomic_t g_r0lab_m3_arrived_signals;
static volatile sig_atomic_t g_r0lab_m3_handler_failures;
static void *g_r0lab_m3_signal_page;
static size_t g_r0lab_m3_signal_page_size;
static volatile sig_atomic_t g_r0lab_raw_handler_faults;
static volatile sig_atomic_t g_r0lab_raw_signal_restore_prot;
static volatile sig_atomic_t g_r0lab_raw_signal_jump_on_fault;
static sigjmp_buf g_r0lab_raw_signal_jump;
static void *g_r0lab_raw_signal_page;
static size_t g_r0lab_raw_signal_page_size;

static void r0lab_m3_signal_handler(int signal_number, siginfo_t *info,
                                    void *context)
{
    uintptr_t page_start = (uintptr_t)g_r0lab_m3_signal_page;
    uintptr_t page_end = page_start + g_r0lab_m3_signal_page_size;
    uintptr_t fault_address = info ? (uintptr_t)info->si_addr : 0;

    (void)context;
    if (signal_number != SIGSEGV || !page_start ||
        fault_address < page_start || fault_address >= page_end) {
        syscall(__NR_exit_group, 128 + SIGSEGV);
        return;
    }
    __atomic_add_fetch(&g_r0lab_m3_arrived_signals, 1, __ATOMIC_RELEASE);
    __atomic_add_fetch(&g_r0lab_m3_handler_failures, 1, __ATOMIC_RELEASE);
    syscall(__NR_exit_group, 128 + SIGSEGV);
}

static void *r0lab_m3_worker(void *opaque)
{
    struct r0lab_m3_thread *thread = opaque;
    int (*entry)(void) = (int (*)(void))thread->page;

    __atomic_add_fetch(thread->ready, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(thread->start, __ATOMIC_ACQUIRE)) {
    }
    thread->value = entry();
    return NULL;
}

static long r0lab_wait_for_reply(const char *verb, uint64_t token,
                                  char *reply, size_t reply_size)
{
    char command[64];
    int attempt;

    if (!reply || !reply_size) {
        errno = EINVAL;
        return -1;
    }
    snprintf(command, sizeof(command), "%s 0x%llx", verb,
             (unsigned long long)token);
    for (attempt = 0; attempt < 1000; ++attempt) {
        long rc;

        memset(reply, 0, reply_size);
        errno = 0;
        rc = r0lab_control_raw(command, reply, reply_size);
        if (rc >= 0)
            return rc;
        if (errno != EAGAIN)
            return rc;
        usleep(1000);
    }
    errno = EAGAIN;
    return -1;
}

static long r0lab_m3_wait_for(const char *verb, uint64_t token)
{
    char reply[128];

    return r0lab_wait_for_reply(verb, token, reply, sizeof(reply));
}

static void r0lab_inline_reply(char *reply)
{
    if (!reply)
        return;
    while (*reply) {
        if (*reply == '\r' || *reply == '\n')
            *reply = ' ';
        ++reply;
    }
}

static void r0lab_m3_clear(uint64_t token, long *clear_rc, long *cleared_rc)
{
    char command[64];
    char reply[128];

    snprintf(command, sizeof(command), "m3 clear 0x%llx",
             (unsigned long long)token);
    memset(reply, 0, sizeof(reply));
    errno = 0;
    *clear_rc = r0lab_control_raw(command, reply, sizeof(reply));
    if (*clear_rc >= 0)
        *cleared_rc = r0lab_m3_wait_for("m3 cleared", token);
}

static int r0lab_m3_run_phase(const char *token_text, int thread_count,
                               const char *mode, char *output,
                               size_t output_size)
{
    enum { R0LAB_M3_MAX_THREADS = 3 };
    struct r0lab_m3_thread threads[R0LAB_M3_MAX_THREADS] = {0};
    pthread_t workers[R0LAB_M3_MAX_THREADS - 1];
    struct sigaction action = {0};
    struct sigaction previous_action = {0};
    uint32_t *code;
    volatile uint32_t *readable_code;
    volatile int ready = 0;
    volatile int start = 0;
    uint64_t token;
    void *page = MAP_FAILED;
    size_t page_size;
    char command[96];
    char reply[128] = {0};
    unsigned int observed_faults = 0;
    unsigned int expected_faults = 1;
    uint32_t read_before = 0;
    uint32_t read_during = 0;
    uint32_t read_after = 0;
    long arm_rc = -1;
    long ready_rc = -1;
    long observed_rc = -1;
    long clear_rc = -1;
    long cleared_rc = -1;
    int normal_value = -1;
    int failures = 0;
    int created = 0;
    int handler_installed = 0;
    int index;

    if (thread_count < 1 || thread_count > R0LAB_M3_MAX_THREADS ||
        r0lab_parse_token(token_text, &token)) {
        snprintf(output, output_size, "rc=-22 error=invalid M3 token");
        return -1;
    }
    page_size = (size_t)sysconf(_SC_PAGESIZE);
    if (page_size != R0LAB_M3_PAGE_SIZE) {
        snprintf(output, output_size, "rc=-38 error=unsupported page size=%zu", page_size);
        return -1;
    }
    page = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) {
        snprintf(output, output_size, "rc=-12 error=page allocation errno=%d", errno);
        return -1;
    }
    code = page;
    code[0] = R0LAB_M3_CODE_MOV_W0_42;
    code[1] = R0LAB_M3_CODE_RET;
    __builtin___clear_cache((char *)page, (char *)page + page_size);
    if (mprotect(page, page_size, PROT_READ | PROT_EXEC)) {
        snprintf(output, output_size, "rc=-1 error=initial mprotect errno=%d", errno);
        munmap(page, page_size);
        return -1;
    }
    readable_code = (volatile uint32_t *)page;
    read_before = readable_code[0];
    normal_value = ((int (*)(void))page)();

    snprintf(command, sizeof(command), "m3 arm 0x%llx 0x%llx",
             (unsigned long long)token, (unsigned long long)(uintptr_t)page);
    errno = 0;
    arm_rc = r0lab_control_raw(command, reply, sizeof(reply));
    if (arm_rc < 0)
        goto finish;
    ready_rc = r0lab_m3_wait_for("m3 ready", token);
    if (ready_rc < 0)
        goto clear;
    read_during = readable_code[0];

    g_r0lab_m3_expected_signals = thread_count;
    g_r0lab_m3_arrived_signals = 0;
    g_r0lab_m3_handler_failures = 0;
    g_r0lab_m3_signal_page = page;
    g_r0lab_m3_signal_page_size = page_size;
    action.sa_sigaction = r0lab_m3_signal_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_SIGINFO;
    if (sigaction(SIGSEGV, &action, &previous_action))
        goto clear;
    handler_installed = 1;

    for (index = 0; index < thread_count; ++index) {
        threads[index].page = page;
        threads[index].ready = &ready;
        threads[index].start = &start;
        threads[index].value = -1;
    }
    for (index = 1; index < thread_count; ++index) {
        if (pthread_create(&workers[index - 1], NULL, r0lab_m3_worker,
                           &threads[index])) {
            failures++;
            break;
        }
        ++created;
    }
    while (__atomic_load_n(&ready, __ATOMIC_ACQUIRE) < created) {
    }
    __atomic_store_n(&start, 1, __ATOMIC_RELEASE);
    threads[0].value = ((int (*)(void))page)();
    for (index = 0; index < created; ++index)
        pthread_join(workers[index], NULL);

    memset(reply, 0, sizeof(reply));
    errno = 0;
    snprintf(command, sizeof(command), "m3 observed 0x%llx",
             (unsigned long long)token);
    observed_rc = r0lab_control_raw(command, reply, sizeof(reply));
    if (observed_rc < 0 || sscanf(reply, "m3_observed faults=%u", &observed_faults) != 1)
        ++failures;

clear:
    r0lab_m3_clear(token, &clear_rc, &cleared_rc);
    if (handler_installed)
        sigaction(SIGSEGV, &previous_action, NULL);
    g_r0lab_m3_signal_page = NULL;
    g_r0lab_m3_signal_page_size = 0;
    read_after = readable_code[0];

finish:
    if (normal_value != 42 || read_before != R0LAB_M3_CODE_MOV_W0_42 ||
        read_during != R0LAB_M3_CODE_MOV_W0_42 ||
        read_after != R0LAB_M3_CODE_MOV_W0_42 || arm_rc < 0 || ready_rc < 0 ||
        observed_rc < 0 || clear_rc < 0 || cleared_rc < 0 ||
        observed_faults != expected_faults ||
        g_r0lab_m3_handler_failures || created != thread_count - 1)
        ++failures;
    for (index = 0; index < thread_count; ++index) {
        if (threads[index].value != 42)
            ++failures;
    }
    snprintf(output, output_size,
             "m3 threads=%d expected_faults=%d observed_faults=%u failures=%d mode=%s normal_value=%d values=%d,%d,%d read_before=%08x read_during=%08x read_after=%08x arm_rc=%ld ready_rc=%ld observed_rc=%ld clear_rc=%ld cleared_rc=%ld handler_failures=%d",
             thread_count, expected_faults, observed_faults, failures, mode,
             normal_value, threads[0].value, threads[1].value, threads[2].value,
             read_before, read_during, read_after, arm_rc, ready_rc, observed_rc,
             clear_rc, cleared_rc, (int)g_r0lab_m3_handler_failures);
    munmap(page, page_size);
    return failures ? -1 : 0;
}

static int r0lab_m3_run_single(const char *token_text, char *output,
                                size_t output_size)
{
    return r0lab_m3_run_phase(token_text, 1, "single", output, output_size);
}

static int r0lab_m3_run_multi(const char *token_text, char *output,
                               size_t output_size)
{
    return r0lab_m3_run_phase(token_text, 3, "multi", output, output_size);
}

static int r0lab_m4_map_permissions(const void *address, char permissions[5])
{
    FILE *maps;
    char line[256];
    unsigned long long start;
    unsigned long long end;
    char parsed_permissions[5];
    uintptr_t target = (uintptr_t)address;

    maps = fopen("/proc/self/maps", "r");
    if (!maps)
        return -1;
    while (fgets(line, sizeof(line), maps)) {
        if (sscanf(line, "%llx-%llx %4s", &start, &end, parsed_permissions) != 3)
            continue;
        if (target >= start && target < end) {
            memcpy(permissions, parsed_permissions, sizeof(parsed_permissions));
            permissions[4] = 0;
            fclose(maps);
            return 0;
        }
    }
    fclose(maps);
    return -1;
}

static int r0lab_m4_wait_for_permissions(const void *address,
                                         const char *expected,
                                         char permissions[5])
{
    int attempt;
    int rc = -1;

    for (attempt = 0; attempt < 200; ++attempt) {
        rc = r0lab_m4_map_permissions(address, permissions);
        if (!rc && !strcmp(permissions, expected))
            return 0;
        usleep(1000);
    }
    return rc ? rc : -1;
}

static void r0lab_m4_clear(uint64_t token, long *clear_rc, long *cleared_rc)
{
    char command[64];
    char reply[128];

    snprintf(command, sizeof(command), "m4 clear 0x%llx",
             (unsigned long long)token);
    memset(reply, 0, sizeof(reply));
    errno = 0;
    *clear_rc = r0lab_control_raw(command, reply, sizeof(reply));
    if (*clear_rc >= 0)
        *cleared_rc = r0lab_m3_wait_for("m4 cleared", token);
}

static int r0lab_m4_run_phase(const char *token_text, int execute_source,
                               const char *mode, char *output,
                               size_t output_size)
{
    uint32_t *source_code;
    uint32_t *clone_code;
    volatile uint32_t *source_readable;
    volatile uint32_t *clone_readable;
    uint64_t token;
    void *source_page = MAP_FAILED;
    void *clone_page = MAP_FAILED;
    size_t page_size;
    char command[128];
    char reply[128] = {0};
    char ready_reply[160] = {0};
    char observed_reply[160] = {0};
    char source_before_permissions[5] = "????";
    char clone_before_permissions[5] = "????";
    char source_during_permissions[5] = "????";
    char clone_during_permissions[5] = "????";
    char source_after_permissions[5] = "????";
    char clone_after_permissions[5] = "????";
    unsigned int redirects = 0;
    uint32_t source_word_before = 0;
    uint32_t clone_word_before = 0;
    uint32_t source_word_during = 0;
    uint32_t clone_word_during = 0;
    uint32_t source_word_after = 0;
    uint32_t clone_word_after = 0;
    long unsupported_rc = -1;
    long arm_rc = -1;
    long ready_rc = -1;
    long observed_rc = -1;
    long clear_rc = -1;
    long cleared_rc = -1;
    int source_after_permissions_rc = -1;
    int clone_after_permissions_rc = -1;
    int source_normal_value = -1;
    int clone_normal_value = -1;
    int redirected_value = -1;
    int source_after_value = -1;
    int clone_after_value = -1;
    int failures = 0;

    if (r0lab_parse_token(token_text, &token)) {
        snprintf(output, output_size, "rc=-22 error=invalid M4 token");
        return -1;
    }
    page_size = (size_t)sysconf(_SC_PAGESIZE);
    if (page_size != R0LAB_M3_PAGE_SIZE) {
        snprintf(output, output_size, "rc=-38 error=unsupported page size=%zu", page_size);
        return -1;
    }
    source_page = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (source_page == MAP_FAILED) {
        snprintf(output, output_size, "rc=-12 error=source allocation errno=%d", errno);
        return -1;
    }
    clone_page = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (clone_page == MAP_FAILED) {
        snprintf(output, output_size, "rc=-12 error=clone allocation errno=%d", errno);
        munmap(source_page, page_size);
        return -1;
    }
    source_code = source_page;
    clone_code = clone_page;
    source_code[0] = R0LAB_M3_CODE_MOV_W0_42;
    source_code[1] = R0LAB_M3_CODE_RET;
    clone_code[0] = R0LAB_M4_CODE_MOV_W0_99;
    clone_code[1] = R0LAB_M3_CODE_RET;
    __builtin___clear_cache((char *)source_page, (char *)source_page + page_size);
    __builtin___clear_cache((char *)clone_page, (char *)clone_page + page_size);
    if (mprotect(source_page, page_size, PROT_READ | PROT_EXEC) ||
        mprotect(clone_page, page_size, PROT_READ | PROT_EXEC)) {
        snprintf(output, output_size, "rc=-1 error=initial mprotect errno=%d", errno);
        munmap(clone_page, page_size);
        munmap(source_page, page_size);
        return -1;
    }
    source_readable = (volatile uint32_t *)source_page;
    clone_readable = (volatile uint32_t *)clone_page;
    source_word_before = source_readable[0];
    clone_word_before = clone_readable[0];
    source_normal_value = ((int (*)(void))source_page)();
    clone_normal_value = ((int (*)(void))clone_page)();
    if (r0lab_m4_map_permissions(source_page, source_before_permissions) ||
        r0lab_m4_map_permissions(clone_page, clone_before_permissions))
        ++failures;

    snprintf(command, sizeof(command), "m4 arm 0x%llx 0x%llx 0x%llx",
             (unsigned long long)token, (unsigned long long)(uintptr_t)source_page,
             (unsigned long long)(uintptr_t)source_page);
    errno = 0;
    unsupported_rc = r0lab_control_raw(command, reply, sizeof(reply));
    if (unsupported_rc >= 0)
        ++failures;

    memset(reply, 0, sizeof(reply));
    snprintf(command, sizeof(command), "m4 arm 0x%llx 0x%llx 0x%llx",
             (unsigned long long)token, (unsigned long long)(uintptr_t)source_page,
             (unsigned long long)(uintptr_t)clone_page);
    errno = 0;
    arm_rc = r0lab_control_raw(command, reply, sizeof(reply));
    if (arm_rc < 0)
        goto finish;
    ready_rc = r0lab_wait_for_reply("m4 ready", token, ready_reply,
                                    sizeof(ready_reply));
    if (ready_rc < 0)
        goto clear;
    r0lab_inline_reply(ready_reply);
    source_word_during = source_readable[0];
    clone_word_during = clone_readable[0];
    if (r0lab_m4_map_permissions(source_page, source_during_permissions) ||
        r0lab_m4_map_permissions(clone_page, clone_during_permissions))
        ++failures;

    if (execute_source)
        redirected_value = ((int (*)(void))source_page)();

    memset(reply, 0, sizeof(reply));
    errno = 0;
    snprintf(command, sizeof(command), "m4 observed 0x%llx",
             (unsigned long long)token);
    observed_rc = r0lab_control_raw(command, reply, sizeof(reply));
    if (observed_rc < 0 || sscanf(reply, "m4_observed redirects=%u", &redirects) != 1)
        ++failures;
    snprintf(observed_reply, sizeof(observed_reply), "%s", reply);
    r0lab_inline_reply(observed_reply);

clear:
    r0lab_m4_clear(token, &clear_rc, &cleared_rc);
    if (cleared_rc >= 0) {
        source_after_permissions_rc = r0lab_m4_wait_for_permissions(
            source_page, "r-xp", source_after_permissions);
        clone_after_permissions_rc = r0lab_m4_wait_for_permissions(
            clone_page, "r-xp", clone_after_permissions);
        source_word_after = source_readable[0];
        clone_word_after = clone_readable[0];
        if (!source_after_permissions_rc)
            source_after_value = ((int (*)(void))source_page)();
        if (!clone_after_permissions_rc)
            clone_after_value = ((int (*)(void))clone_page)();
        if (source_after_permissions_rc || clone_after_permissions_rc)
            ++failures;
    }

finish:
    if (source_normal_value != 42 || clone_normal_value != 99 ||
        source_word_before != R0LAB_M3_CODE_MOV_W0_42 ||
        clone_word_before != R0LAB_M4_CODE_MOV_W0_99 ||
        source_word_during != R0LAB_M3_CODE_MOV_W0_42 ||
        clone_word_during != R0LAB_M4_CODE_MOV_W0_99 ||
        source_word_after != R0LAB_M3_CODE_MOV_W0_42 ||
        clone_word_after != R0LAB_M4_CODE_MOV_W0_99 ||
        strcmp(source_before_permissions, "r-xp") ||
        strcmp(clone_before_permissions, "r-xp") ||
        strcmp(source_during_permissions, "r-xp") ||
        strcmp(clone_during_permissions, "r-xp") ||
        strcmp(source_after_permissions, "r-xp") ||
        strcmp(clone_after_permissions, "r-xp") ||
        unsupported_rc >= 0 || arm_rc < 0 || ready_rc < 0 || observed_rc < 0 ||
        clear_rc < 0 || cleared_rc < 0 ||
        source_after_permissions_rc || clone_after_permissions_rc ||
        redirects != (unsigned int)(execute_source ? 1 : 0) ||
        (execute_source && redirected_value != 99) ||
        (!execute_source && redirected_value != -1) ||
        source_after_value != 42 || clone_after_value != 99)
        ++failures;
    snprintf(output, output_size,
             "m4 mode=%s expected_redirects=%d observed_redirects=%u failures=%d source_normal=%d clone_normal=%d redirected_value=%d source_after=%d clone_after=%d source_perms=%s/%s/%s clone_perms=%s/%s/%s after_perm_rc=%d/%d source_words=%08x/%08x/%08x clone_words=%08x/%08x/%08x unsupported_rc=%ld arm_rc=%ld ready_rc=%ld observed_rc=%ld clear_rc=%ld cleared_rc=%ld ready=\"%s\" observed=\"%s\"",
             mode, execute_source ? 1 : 0, redirects, failures,
             source_normal_value, clone_normal_value, redirected_value,
             source_after_value, clone_after_value, source_before_permissions,
             source_during_permissions, source_after_permissions,
             clone_before_permissions, clone_during_permissions,
             clone_after_permissions, source_after_permissions_rc,
             clone_after_permissions_rc, source_word_before, source_word_during,
             source_word_after, clone_word_before, clone_word_during,
             clone_word_after, unsupported_rc, arm_rc, ready_rc, observed_rc,
             clear_rc, cleared_rc, ready_reply, observed_reply);
    munmap(clone_page, page_size);
    munmap(source_page, page_size);
    return failures ? -1 : 0;
}

static int r0lab_m4_run_preflight(const char *token_text, char *output,
                                   size_t output_size)
{
    return r0lab_m4_run_phase(token_text, 0, "preflight", output, output_size);
}

static int r0lab_m4_run_redirect(const char *token_text, char *output,
                                  size_t output_size)
{
    return r0lab_m4_run_phase(token_text, 1, "redirect", output, output_size);
}

static void r0lab_raw_signal_handler(int signal_number, siginfo_t *info,
                                     void *context)
{
    uintptr_t page_start = (uintptr_t)g_r0lab_raw_signal_page;
    uintptr_t page_end = page_start + g_r0lab_raw_signal_page_size;
    uintptr_t fault_address = info ? (uintptr_t)info->si_addr : 0;
    int restore_prot = g_r0lab_raw_signal_restore_prot;

    (void)context;
    if (signal_number != SIGSEGV || !page_start ||
        fault_address < page_start || fault_address >= page_end) {
        syscall(__NR_exit_group, 128 + SIGSEGV);
        return;
    }
    if (!restore_prot)
        restore_prot = PROT_READ | PROT_EXEC;
    __atomic_add_fetch(&g_r0lab_raw_handler_faults, 1, __ATOMIC_RELEASE);
    if (g_r0lab_raw_signal_jump_on_fault)
        siglongjmp(g_r0lab_raw_signal_jump, 1);
    (void)syscall(SYS_mprotect, g_r0lab_raw_signal_page,
                  g_r0lab_raw_signal_page_size, restore_prot);
}

static void r0lab_raw_clear(uint64_t token, long *clear_rc, long *cleared_rc)
{
    char command[64];
    char reply[128];

    snprintf(command, sizeof(command), "raw clear 0x%llx",
             (unsigned long long)token);
    memset(reply, 0, sizeof(reply));
    errno = 0;
    *clear_rc = r0lab_control_raw(command, reply, sizeof(reply));
    if (*clear_rc >= 0)
        *cleared_rc = r0lab_m3_wait_for("raw cleared", token);
}

static int r0lab_raw_run(const char *token_text, char *output,
                         size_t output_size)
{
    struct sigaction action = {0};
    struct sigaction previous_action = {0};
    uint32_t *code;
    volatile uint32_t *readable_code;
    uint64_t token;
    void *page = MAP_FAILED;
    size_t page_size;
    char command[96];
    char reply[256] = {0};
    char inspect_reply[2048] = {0};
    unsigned int activations = 0;
    unsigned long state = 0;
    uint32_t word_before = 0;
    uint32_t word_during = 0;
    uint32_t word_after_exec = 0;
    uint32_t word_after_clear = 0;
    long arm_rc = -1;
    long ready_rc = -1;
    long observed_rc = -1;
    long inspect_rc = -1;
    long clear_rc = -1;
    long cleared_rc = -1;
    int normal_value = -1;
    int shadow_value = -1;
    int restored_value = -1;
    int handler_installed = 0;
    int failures = 0;

    if (r0lab_parse_token(token_text, &token)) {
        snprintf(output, output_size, "rc=-22 error=invalid raw token");
        return -1;
    }
    page_size = (size_t)sysconf(_SC_PAGESIZE);
    if (page_size != R0LAB_M3_PAGE_SIZE) {
        snprintf(output, output_size, "rc=-38 error=unsupported page size=%zu", page_size);
        return -1;
    }
    page = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) {
        snprintf(output, output_size, "rc=-12 error=raw page allocation errno=%d", errno);
        return -1;
    }
    code = page;
    code[0] = R0LAB_M3_CODE_MOV_W0_42;
    code[1] = R0LAB_M3_CODE_RET;
    __builtin___clear_cache((char *)page, (char *)page + page_size);
    if (mprotect(page, page_size, PROT_READ | PROT_EXEC)) {
        snprintf(output, output_size, "rc=-1 error=initial raw mprotect errno=%d", errno);
        munmap(page, page_size);
        return -1;
    }

    readable_code = (volatile uint32_t *)page;
    word_before = readable_code[0];
    normal_value = ((int (*)(void))page)();

    snprintf(command, sizeof(command), "raw arm 0x%llx 0x%llx",
             (unsigned long long)token, (unsigned long long)(uintptr_t)page);
    errno = 0;
    arm_rc = r0lab_control_raw(command, reply, sizeof(reply));
    if (arm_rc < 0)
        goto finish;
    ready_rc = r0lab_m3_wait_for("raw ready", token);
    if (ready_rc < 0)
        goto clear;
    word_during = readable_code[0];

    g_r0lab_raw_handler_faults = 0;
    g_r0lab_raw_signal_page = page;
    g_r0lab_raw_signal_page_size = page_size;
    action.sa_sigaction = r0lab_raw_signal_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_SIGINFO;
    if (sigaction(SIGSEGV, &action, &previous_action))
        goto clear;
    handler_installed = 1;

    shadow_value = ((int (*)(void))page)();
    word_after_exec = readable_code[0];

    memset(reply, 0, sizeof(reply));
    errno = 0;
    snprintf(command, sizeof(command), "raw observed 0x%llx",
             (unsigned long long)token);
    observed_rc = r0lab_control_raw(command, reply, sizeof(reply));
    if (observed_rc < 0 ||
        sscanf(reply, "raw_observed activations=%u state=%lu",
               &activations, &state) != 2)
        ++failures;

    snprintf(command, sizeof(command), "raw inspect 0x%llx",
             (unsigned long long)token);
    inspect_rc = r0lab_control_raw(command, inspect_reply,
                                   sizeof(inspect_reply));
    if (inspect_rc < 0 ||
        !strstr(inspect_reply, "active_kind=shadow_rx") ||
        !strstr(inspect_reply, "read_cycle=absent") ||
        !strstr(inspect_reply, "original_view_after_shadow=absent"))
        ++failures;

clear:
    r0lab_raw_clear(token, &clear_rc, &cleared_rc);
    if (handler_installed)
        sigaction(SIGSEGV, &previous_action, NULL);
    g_r0lab_raw_signal_page = NULL;
    g_r0lab_raw_signal_page_size = 0;
    if (cleared_rc >= 0) {
        word_after_clear = readable_code[0];
        restored_value = ((int (*)(void))page)();
    }

finish:
    if (normal_value != 42 || shadow_value != 99 || restored_value != 42 ||
        word_before != R0LAB_M3_CODE_MOV_W0_42 ||
        word_during != R0LAB_M3_CODE_MOV_W0_42 ||
        word_after_exec != R0LAB_M4_CODE_MOV_W0_99 ||
        word_after_clear != R0LAB_M3_CODE_MOV_W0_42 ||
        arm_rc < 0 || ready_rc < 0 || observed_rc < 0 || inspect_rc < 0 ||
        clear_rc < 0 || cleared_rc < 0 || activations != 1 ||
        state != 3 || g_r0lab_raw_handler_faults)
        ++failures;
    snprintf(output, output_size,
             "raw mode=two-pfn failures=%d normal_value=%d shadow_value=%d restored_value=%d read_view=shadow activations=%u state=%lu words=%08x/%08x/%08x/%08x arm_rc=%ld ready_rc=%ld observed_rc=%ld inspect_rc=%ld clear_rc=%ld cleared_rc=%ld handler_faults=%d inspect=\"%s\"",
             failures, normal_value, shadow_value, restored_value,
             activations, state, word_before, word_during, word_after_exec,
             word_after_clear, arm_rc, ready_rc, observed_rc, inspect_rc,
             clear_rc, cleared_rc, (int)g_r0lab_raw_handler_faults,
             inspect_reply);
    munmap(page, page_size);
    return failures ? -1 : 0;
}

static int r0lab_raw_gup_run(const char *token_text, char *output,
                             size_t output_size)
{
    struct sigaction action = {0};
    struct sigaction previous_action = {0};
    uint32_t *code;
    volatile uint32_t *readable_code;
    uint64_t token;
    void *page = MAP_FAILED;
    size_t page_size;
    char command[96];
    char reply[256] = {0};
    char begin_reply[256] = {0};
    char finish_reply[256] = {0};
    char inspect_reply[2048] = {0};
    unsigned int activations = 0;
    unsigned long state = 0;
    uint32_t word_before = 0;
    uint32_t word_shadow = 0;
    uint32_t word_during_hide = 0;
    uint32_t word_after_finish = 0;
    uint32_t word_after_clear = 0;
    long arm_rc = -1;
    long ready_rc = -1;
    long observed_rc = -1;
    long begin_rc = -1;
    long finish_rc = -1;
    long inspect_rc = -1;
    long clear_rc = -1;
    long cleared_rc = -1;
    int normal_value = -1;
    int shadow_value = -1;
    int hide_value = -1;
    int shadow_after_finish_value = -1;
    int restored_value = -1;
    int handler_installed = 0;
    int failures = 0;

    if (r0lab_parse_token(token_text, &token)) {
        snprintf(output, output_size, "rc=-22 error=invalid raw gup token");
        return -1;
    }
    page_size = (size_t)sysconf(_SC_PAGESIZE);
    if (page_size != R0LAB_M3_PAGE_SIZE) {
        snprintf(output, output_size, "rc=-38 error=unsupported page size=%zu", page_size);
        return -1;
    }
    page = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) {
        snprintf(output, output_size, "rc=-12 error=raw gup page allocation errno=%d", errno);
        return -1;
    }
    code = page;
    code[0] = R0LAB_M3_CODE_MOV_W0_42;
    code[1] = R0LAB_M3_CODE_RET;
    __builtin___clear_cache((char *)page, (char *)page + page_size);
    if (mprotect(page, page_size, PROT_READ | PROT_EXEC)) {
        snprintf(output, output_size, "rc=-1 error=initial raw gup mprotect errno=%d", errno);
        munmap(page, page_size);
        return -1;
    }

    readable_code = (volatile uint32_t *)page;
    word_before = readable_code[0];
    normal_value = ((int (*)(void))page)();

    snprintf(command, sizeof(command), "raw arm 0x%llx 0x%llx",
             (unsigned long long)token, (unsigned long long)(uintptr_t)page);
    errno = 0;
    arm_rc = r0lab_control_raw(command, reply, sizeof(reply));
    if (arm_rc < 0)
        goto finish;
    ready_rc = r0lab_m3_wait_for("raw ready", token);
    if (ready_rc < 0)
        goto clear;

    g_r0lab_raw_handler_faults = 0;
    g_r0lab_raw_signal_page = page;
    g_r0lab_raw_signal_page_size = page_size;
    action.sa_sigaction = r0lab_raw_signal_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_SIGINFO;
    if (sigaction(SIGSEGV, &action, &previous_action))
        goto clear;
    handler_installed = 1;

    shadow_value = ((int (*)(void))page)();
    word_shadow = readable_code[0];

    snprintf(command, sizeof(command), "raw observed 0x%llx",
             (unsigned long long)token);
    observed_rc = r0lab_control_raw(command, reply, sizeof(reply));
    if (observed_rc < 0 ||
        sscanf(reply, "raw_observed activations=%u state=%lu",
               &activations, &state) != 2)
        ++failures;

    snprintf(command, sizeof(command), "raw gup begin 0x%llx",
             (unsigned long long)token);
    begin_rc = r0lab_control_raw(command, begin_reply, sizeof(begin_reply));
    if (begin_rc >= 0) {
        word_during_hide = readable_code[0];
        hide_value = ((int (*)(void))page)();
    }

    snprintf(command, sizeof(command), "raw gup finish 0x%llx",
             (unsigned long long)token);
    finish_rc = r0lab_control_raw(command, finish_reply, sizeof(finish_reply));
    if (finish_rc >= 0) {
        word_after_finish = readable_code[0];
        shadow_after_finish_value = ((int (*)(void))page)();
    }

    snprintf(command, sizeof(command), "raw inspect 0x%llx",
             (unsigned long long)token);
    inspect_rc = r0lab_control_raw(command, inspect_reply,
                                   sizeof(inspect_reply));
    if (inspect_rc < 0 ||
        !strstr(begin_reply, "active_kind=original") ||
        !strstr(begin_reply, "gup_hide_active=1") ||
        !strstr(finish_reply, "active_kind=shadow_rx") ||
        !strstr(finish_reply, "gup_hide_active=0") ||
        !strstr(inspect_reply, "gup_begin_events=1") ||
        !strstr(inspect_reply, "gup_finish_events=1") ||
        !strstr(inspect_reply, "original_view_after_shadow=proven") ||
        !strstr(inspect_reply, "gup_hide_primitive=proven"))
        ++failures;

clear:
    r0lab_raw_clear(token, &clear_rc, &cleared_rc);
    if (handler_installed)
        sigaction(SIGSEGV, &previous_action, NULL);
    g_r0lab_raw_signal_page = NULL;
    g_r0lab_raw_signal_page_size = 0;
    if (cleared_rc >= 0) {
        word_after_clear = readable_code[0];
        restored_value = ((int (*)(void))page)();
    }

finish:
    if (normal_value != 42 || shadow_value != 99 || hide_value != 42 ||
        shadow_after_finish_value != 99 || restored_value != 42 ||
        word_before != R0LAB_M3_CODE_MOV_W0_42 ||
        word_shadow != R0LAB_M4_CODE_MOV_W0_99 ||
        word_during_hide != R0LAB_M3_CODE_MOV_W0_42 ||
        word_after_finish != R0LAB_M4_CODE_MOV_W0_99 ||
        word_after_clear != R0LAB_M3_CODE_MOV_W0_42 ||
        arm_rc < 0 || ready_rc < 0 || observed_rc < 0 || begin_rc < 0 ||
        finish_rc < 0 || inspect_rc < 0 || clear_rc < 0 || cleared_rc < 0 ||
        activations != 1 || state != 3 || g_r0lab_raw_handler_faults)
        ++failures;
    snprintf(output, output_size,
             "raw mode=gup-hide failures=%d normal_value=%d shadow_value=%d hide_value=%d shadow_after_finish=%d restored_value=%d words=%08x/%08x/%08x/%08x/%08x activations=%u state=%lu arm_rc=%ld ready_rc=%ld observed_rc=%ld begin_rc=%ld finish_rc=%ld inspect_rc=%ld clear_rc=%ld cleared_rc=%ld handler_faults=%d begin=\"%s\" finish=\"%s\" inspect=\"%s\"",
             failures, normal_value, shadow_value, hide_value,
             shadow_after_finish_value, restored_value, word_before,
             word_shadow, word_during_hide, word_after_finish,
             word_after_clear, activations, state, arm_rc, ready_rc,
             observed_rc, begin_rc, finish_rc, inspect_rc, clear_rc,
             cleared_rc, (int)g_r0lab_raw_handler_faults, begin_reply,
             finish_reply, inspect_reply);
    munmap(page, page_size);
    return failures ? -1 : 0;
}

static int r0lab_raw_read_cycle_run(const char *token_text, char *output,
                                    size_t output_size)
{
    struct sigaction action = {0};
    struct sigaction previous_action = {0};
    uint32_t *code;
    volatile uint32_t *readable_code;
    uint64_t token;
    void *page = MAP_FAILED;
    size_t page_size;
    char command[96];
    char reply[256] = {0};
    char begin_reply[512] = {0};
    char status_before_exec[512] = {0};
    char status_after_exec[512] = {0};
    char inspect_reply[2048] = {0};
    unsigned int activations = 0;
    unsigned long state = 0;
    uint32_t word_before = 0;
    uint32_t word_shadow = 0;
    uint32_t original_read_word = 0;
    uint32_t word_after_resume = 0;
    uint32_t word_after_clear = 0;
    long arm_rc = -1;
    long ready_rc = -1;
    long observed_rc = -1;
    long begin_rc = -1;
    long status_before_rc = -1;
    long status_after_rc = -1;
    long inspect_rc = -1;
    long clear_rc = -1;
    long cleared_rc = -1;
    int normal_value = -1;
    int shadow_value = -1;
    int resume_value = -1;
    int restored_value = -1;
    int handler_installed = 0;
    int failures = 0;

    if (r0lab_parse_token(token_text, &token)) {
        snprintf(output, output_size,
                 "rc=-22 error=invalid raw read cycle token");
        return -1;
    }
    page_size = (size_t)sysconf(_SC_PAGESIZE);
    if (page_size != R0LAB_M3_PAGE_SIZE) {
        snprintf(output, output_size,
                 "rc=-38 error=unsupported page size=%zu", page_size);
        return -1;
    }
    page = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) {
        snprintf(output, output_size,
                 "rc=-12 error=raw read cycle page allocation errno=%d",
                 errno);
        return -1;
    }
    code = page;
    code[0] = R0LAB_M3_CODE_MOV_W0_42;
    code[1] = R0LAB_M3_CODE_RET;
    __builtin___clear_cache((char *)page, (char *)page + page_size);
    if (mprotect(page, page_size, PROT_READ | PROT_EXEC)) {
        snprintf(output, output_size,
                 "rc=-1 error=initial raw read cycle mprotect errno=%d",
                 errno);
        munmap(page, page_size);
        return -1;
    }

    readable_code = (volatile uint32_t *)page;
    word_before = readable_code[0];
    normal_value = ((int (*)(void))page)();

    snprintf(command, sizeof(command), "raw arm 0x%llx 0x%llx",
             (unsigned long long)token, (unsigned long long)(uintptr_t)page);
    arm_rc = r0lab_control_raw(command, reply, sizeof(reply));
    if (arm_rc < 0)
        goto finish;
    ready_rc = r0lab_m3_wait_for("raw ready", token);
    if (ready_rc < 0)
        goto clear;

    g_r0lab_raw_handler_faults = 0;
    g_r0lab_raw_signal_page = page;
    g_r0lab_raw_signal_page_size = page_size;
    action.sa_sigaction = r0lab_raw_signal_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_SIGINFO;
    if (sigaction(SIGSEGV, &action, &previous_action))
        goto clear;
    handler_installed = 1;

    shadow_value = ((int (*)(void))page)();
    word_shadow = readable_code[0];

    snprintf(command, sizeof(command), "raw observed 0x%llx",
             (unsigned long long)token);
    observed_rc = r0lab_control_raw(command, reply, sizeof(reply));
    if (observed_rc < 0 ||
        sscanf(reply, "raw_observed activations=%u state=%lu",
               &activations, &state) != 2)
        ++failures;

    snprintf(command, sizeof(command), "raw read cycle begin 0x%llx",
             (unsigned long long)token);
    begin_rc = r0lab_control_raw(command, begin_reply, sizeof(begin_reply));
    if (begin_rc >= 0) {
        original_read_word = readable_code[0];
        snprintf(command, sizeof(command), "raw read cycle status 0x%llx",
                 (unsigned long long)token);
        status_before_rc = r0lab_control_raw(command, status_before_exec,
                                             sizeof(status_before_exec));
        resume_value = ((int (*)(void))page)();
        word_after_resume = readable_code[0];
    }

    snprintf(command, sizeof(command), "raw read cycle status 0x%llx",
             (unsigned long long)token);
    status_after_rc = r0lab_control_raw(command, status_after_exec,
                                        sizeof(status_after_exec));

    snprintf(command, sizeof(command), "raw inspect 0x%llx",
             (unsigned long long)token);
    inspect_rc = r0lab_control_raw(command, inspect_reply,
                                   sizeof(inspect_reply));
    if (begin_rc < 0 || status_before_rc < 0 || status_after_rc < 0 ||
        inspect_rc < 0 ||
        !strstr(begin_reply, "raw_read_cycle_begin") ||
        !strstr(begin_reply, "active_kind=original_read") ||
        !strstr(begin_reply, "read_cycle_active=1") ||
        !strstr(begin_reply, "read_cycle_begin_events=1") ||
        !strstr(begin_reply, "read_cycle_finish_events=0") ||
        !strstr(begin_reply, "trigger=supercall") ||
        !strstr(begin_reply, "data_fault=absent") ||
        !strstr(status_before_exec, "read_cycle_active=1") ||
        !strstr(status_before_exec, "exec_resume=pending") ||
        !strstr(status_after_exec, "active_kind=shadow_rx") ||
        !strstr(status_after_exec, "read_cycle_active=0") ||
        !strstr(status_after_exec, "read_cycle_begin_events=1") ||
        !strstr(status_after_exec, "read_cycle_finish_events=1") ||
        !strstr(status_after_exec, "exec_resume=proven") ||
        !strstr(inspect_reply, "read_cycle=uxn_original_exec_resume") ||
        !strstr(inspect_reply, "read_cycle_pte_switch=1") ||
        !strstr(inspect_reply, "read_cycle_data_fault=absent") ||
        !strstr(inspect_reply, "read_cycle_exec_resume=proven") ||
        !strstr(inspect_reply, "record_state=shadow_active"))
        ++failures;

clear:
    r0lab_raw_clear(token, &clear_rc, &cleared_rc);
    if (handler_installed)
        sigaction(SIGSEGV, &previous_action, NULL);
    g_r0lab_raw_signal_page = NULL;
    g_r0lab_raw_signal_page_size = 0;
    if (cleared_rc >= 0) {
        word_after_clear = readable_code[0];
        restored_value = ((int (*)(void))page)();
    }

finish:
    if (normal_value != 42 || shadow_value != 99 ||
        resume_value != 99 || restored_value != 42 ||
        word_before != R0LAB_M3_CODE_MOV_W0_42 ||
        word_shadow != R0LAB_M4_CODE_MOV_W0_99 ||
        original_read_word != R0LAB_M3_CODE_MOV_W0_42 ||
        word_after_resume != R0LAB_M4_CODE_MOV_W0_99 ||
        word_after_clear != R0LAB_M3_CODE_MOV_W0_42 ||
        arm_rc < 0 || ready_rc < 0 || observed_rc < 0 ||
        begin_rc < 0 || status_before_rc < 0 || status_after_rc < 0 ||
        inspect_rc < 0 || clear_rc < 0 || cleared_rc < 0 ||
        activations != 1 || state != 3 || g_r0lab_raw_handler_faults)
        ++failures;
    snprintf(output, output_size,
             "raw mode=read-cycle failures=%d trigger=supercall data_fault=absent pte_switch=1 exec_resume=1 normal_value=%d shadow_value=%d original_read_word=%08x resume_value=%d shadow_after_resume=%08x restored_value=%d words=%08x/%08x/%08x/%08x/%08x activations=%u state=%lu arm_rc=%ld ready_rc=%ld observed_rc=%ld begin_rc=%ld status_before_rc=%ld status_after_rc=%ld inspect_rc=%ld clear_rc=%ld cleared_rc=%ld handler_faults=%d begin=\"%s\" status_before=\"%s\" status_after=\"%s\" inspect=\"%s\"",
             failures, normal_value, shadow_value, original_read_word,
             resume_value, word_after_resume, restored_value, word_before,
             word_shadow, original_read_word, word_after_resume,
             word_after_clear, activations, state, arm_rc, ready_rc,
             observed_rc, begin_rc, status_before_rc, status_after_rc,
             inspect_rc, clear_rc, cleared_rc,
             (int)g_r0lab_raw_handler_faults, begin_reply,
             status_before_exec, status_after_exec, inspect_reply);
    munmap(page, page_size);
    return failures ? -1 : 0;
}

static int r0lab_raw_gup_hook_run(const char *token_text, char *output,
                                  size_t output_size)
{
    struct sigaction action = {0};
    struct sigaction previous_action = {0};
    uint32_t *code;
    volatile uint32_t *readable_code;
    uint64_t token;
    void *page = MAP_FAILED;
    size_t page_size;
    char command[96];
    char reply[256] = {0};
    char hook_reply[512] = {0};
    char hook_status[512] = {0};
    char hook_clear_reply[512] = {0};
    char inspect_reply[2048] = {0};
    unsigned int activations = 0;
    unsigned long state = 0;
    uint32_t word_before = 0;
    uint32_t word_shadow = 0;
    uint32_t gup_read_word = 0;
    uint32_t word_after_gup = 0;
    uint32_t word_after_clear = 0;
    ssize_t gup_read_rc = -1;
    ssize_t result_read = -1;
    long arm_rc = -1;
    long ready_rc = -1;
    long observed_rc = -1;
    long hook_arm_rc = -1;
    long hook_status_rc = -1;
    long hook_clear_rc = -1;
    long inspect_rc = -1;
    long clear_rc = -1;
    long cleared_rc = -1;
    int normal_value = -1;
    int shadow_value = -1;
    int shadow_after_gup_value = -1;
    int restored_value = -1;
    int handler_installed = 0;
    int pipe_fds[2] = {-1, -1};
    pid_t child = -1;
    pid_t waited = -1;
    int child_status = -1;
    int child_exit = -1;
    struct r0lab_gup_reader_result reader_result = {
        .rc = -1,
        .err = 0,
        .word = 0,
    };
    int failures = 0;

    if (r0lab_parse_token(token_text, &token)) {
        snprintf(output, output_size,
                 "rc=-22 error=invalid raw gup hook token");
        return -1;
    }
    page_size = (size_t)sysconf(_SC_PAGESIZE);
    if (page_size != R0LAB_M3_PAGE_SIZE) {
        snprintf(output, output_size,
                 "rc=-38 error=unsupported page size=%zu", page_size);
        return -1;
    }
    page = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) {
        snprintf(output, output_size,
                 "rc=-12 error=raw gup hook page allocation errno=%d", errno);
        return -1;
    }
    code = page;
    code[0] = R0LAB_M3_CODE_MOV_W0_42;
    code[1] = R0LAB_M3_CODE_RET;
    __builtin___clear_cache((char *)page, (char *)page + page_size);
    if (mprotect(page, page_size, PROT_READ | PROT_EXEC)) {
        snprintf(output, output_size,
                 "rc=-1 error=initial raw gup hook mprotect errno=%d", errno);
        munmap(page, page_size);
        return -1;
    }

    readable_code = (volatile uint32_t *)page;
    word_before = readable_code[0];
    normal_value = ((int (*)(void))page)();

    snprintf(command, sizeof(command), "raw arm 0x%llx 0x%llx",
             (unsigned long long)token, (unsigned long long)(uintptr_t)page);
    arm_rc = r0lab_control_raw(command, reply, sizeof(reply));
    if (arm_rc < 0)
        goto finish;
    ready_rc = r0lab_m3_wait_for("raw ready", token);
    if (ready_rc < 0)
        goto clear;

    g_r0lab_raw_handler_faults = 0;
    g_r0lab_raw_signal_page = page;
    g_r0lab_raw_signal_page_size = page_size;
    action.sa_sigaction = r0lab_raw_signal_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_SIGINFO;
    if (sigaction(SIGSEGV, &action, &previous_action))
        goto clear;
    handler_installed = 1;

    shadow_value = ((int (*)(void))page)();
    word_shadow = readable_code[0];

    snprintf(command, sizeof(command), "raw observed 0x%llx",
             (unsigned long long)token);
    observed_rc = r0lab_control_raw(command, reply, sizeof(reply));
    if (observed_rc < 0 ||
        sscanf(reply, "raw_observed activations=%u state=%lu",
               &activations, &state) != 2)
        ++failures;

    snprintf(command, sizeof(command), "raw gup hook arm 0x%llx",
             (unsigned long long)token);
    hook_arm_rc = r0lab_control_raw(command, hook_reply, sizeof(hook_reply));
    if (hook_arm_rc >= 0) {
        errno = 0;
        if (pipe(pipe_fds)) {
            reader_result.err = errno;
            gup_read_rc = -1;
        } else {
            pid_t parent_pid = getpid();

            child = fork();
            if (child == 0) {
                struct r0lab_gup_reader_result child_result = {
                    .rc = -1,
                    .err = 0,
                    .word = 0,
                };

                close(pipe_fds[0]);
#ifdef __NR_process_vm_readv
                struct iovec local_iov = {
                    &child_result.word,
                    sizeof(child_result.word),
                };
                struct iovec remote_iov = {
                    page,
                    sizeof(child_result.word),
                };

                errno = 0;
                child_result.rc = (int32_t)syscall(__NR_process_vm_readv,
                                                   parent_pid, &local_iov, 1,
                                                   &remote_iov, 1, 0);
                child_result.err = errno;
#else
                child_result.err = ENOSYS;
#endif
                (void)write(pipe_fds[1], &child_result,
                            sizeof(child_result));
                close(pipe_fds[1]);
                _exit(child_result.rc == (int32_t)sizeof(child_result.word)
                          ? 0
                          : 64);
            }

            close(pipe_fds[1]);
            pipe_fds[1] = -1;
            if (child > 0) {
                result_read = read(pipe_fds[0], &reader_result,
                                   sizeof(reader_result));
                close(pipe_fds[0]);
                pipe_fds[0] = -1;
                waited = waitpid(child, &child_status, 0);
                if (waited == child && WIFEXITED(child_status))
                    child_exit = WEXITSTATUS(child_status);
                gup_read_rc = reader_result.rc;
                gup_read_word = reader_result.word;
            } else {
                reader_result.err = errno;
                close(pipe_fds[0]);
                pipe_fds[0] = -1;
                gup_read_rc = -1;
            }
        }
        word_after_gup = readable_code[0];
        shadow_after_gup_value = ((int (*)(void))page)();
    }

    snprintf(command, sizeof(command), "raw gup hook status 0x%llx",
             (unsigned long long)token);
    hook_status_rc = r0lab_control_raw(command, hook_status,
                                       sizeof(hook_status));

    snprintf(command, sizeof(command), "raw inspect 0x%llx",
             (unsigned long long)token);
    inspect_rc = r0lab_control_raw(command, inspect_reply,
                                   sizeof(inspect_reply));
    if (hook_arm_rc < 0 ||
        gup_read_rc != (ssize_t)sizeof(gup_read_word) ||
        result_read != (ssize_t)sizeof(reader_result) ||
        waited != child || child_exit != 0 ||
        hook_status_rc < 0 || inspect_rc < 0 ||
        !strstr(hook_reply, "raw_gup_hook_ready") ||
        !strstr(hook_reply, "installed=1") ||
        !strstr(hook_reply, "target_mm_scoped=1") ||
        !strstr(hook_reply, "external_reader=1") ||
        !strstr(hook_status, "hook_begin_events=1") ||
        !strstr(hook_status, "hook_finish_events=1") ||
        !strstr(hook_status, "hook_failures=0") ||
        !strstr(hook_status, "target_mm_scoped=1") ||
        !strstr(hook_status, "external_reader=1") ||
        !strstr(hook_status, "primitive_begin_events=1") ||
        !strstr(hook_status, "primitive_finish_events=1") ||
        !strstr(inspect_reply, "gup_hook_begin_events=1") ||
        !strstr(inspect_reply, "gup_hook_finish_events=1") ||
        !strstr(inspect_reply, "gup_hook_failures=0"))
        ++failures;

    snprintf(command, sizeof(command), "raw gup hook clear 0x%llx",
             (unsigned long long)token);
    hook_clear_rc = r0lab_control_raw(command, hook_clear_reply,
                                      sizeof(hook_clear_reply));

clear:
    if (pipe_fds[0] >= 0)
        close(pipe_fds[0]);
    if (pipe_fds[1] >= 0)
        close(pipe_fds[1]);
    r0lab_raw_clear(token, &clear_rc, &cleared_rc);
    if (handler_installed)
        sigaction(SIGSEGV, &previous_action, NULL);
    g_r0lab_raw_signal_page = NULL;
    g_r0lab_raw_signal_page_size = 0;
    if (cleared_rc >= 0) {
        word_after_clear = readable_code[0];
        restored_value = ((int (*)(void))page)();
    }

finish:
    if (normal_value != 42 || shadow_value != 99 ||
        shadow_after_gup_value != 99 || restored_value != 42 ||
        word_before != R0LAB_M3_CODE_MOV_W0_42 ||
        word_shadow != R0LAB_M4_CODE_MOV_W0_99 ||
        gup_read_word != R0LAB_M3_CODE_MOV_W0_42 ||
        word_after_gup != R0LAB_M4_CODE_MOV_W0_99 ||
        word_after_clear != R0LAB_M3_CODE_MOV_W0_42 ||
        arm_rc < 0 || ready_rc < 0 || observed_rc < 0 ||
        hook_arm_rc < 0 || hook_status_rc < 0 || hook_clear_rc < 0 ||
        inspect_rc < 0 || clear_rc < 0 || cleared_rc < 0 ||
        activations != 1 || state != 3 || g_r0lab_raw_handler_faults)
        ++failures;
    snprintf(output, output_size,
             "raw mode=gup-hook failures=%d reader=external normal_value=%d shadow_value=%d gup_read_word=%08x shadow_after_gup=%08x shadow_after_gup_value=%d restored_value=%d words=%08x/%08x/%08x gup_read_rc=%zd result_read=%zd reader_errno=%d child_pid=%d waited_pid=%d child_status=%d child_exit=%d activations=%u state=%lu arm_rc=%ld ready_rc=%ld observed_rc=%ld hook_arm_rc=%ld hook_status_rc=%ld hook_clear_rc=%ld inspect_rc=%ld clear_rc=%ld cleared_rc=%ld handler_faults=%d hook=\"%s\" status=\"%s\" hook_clear=\"%s\" inspect=\"%s\"",
             failures, normal_value, shadow_value, gup_read_word,
             word_after_gup, shadow_after_gup_value, restored_value,
             word_before, word_shadow, word_after_clear, gup_read_rc,
             result_read, reader_result.err, (int)child, (int)waited,
             child_status, child_exit, activations, state, arm_rc, ready_rc,
             observed_rc, hook_arm_rc, hook_status_rc, hook_clear_rc,
             inspect_rc, clear_rc, cleared_rc,
             (int)g_r0lab_raw_handler_faults, hook_reply, hook_status,
             hook_clear_reply, inspect_reply);
    munmap(page, page_size);
    return failures ? -1 : 0;
}

static int r0lab_raw_fork_hook_run(const char *token_text, char *output,
                                   size_t output_size)
{
    struct sigaction action = {0};
    struct sigaction previous_action = {0};
    uint32_t *code;
    volatile uint32_t *readable_code;
    uint64_t token;
    void *page = MAP_FAILED;
    size_t page_size;
    char command[96];
    char reply[256] = {0};
    char hook_reply[512] = {0};
    char hook_status[512] = {0};
    char hook_clear_reply[512] = {0};
    char inspect_reply[2048] = {0};
    unsigned int activations = 0;
    unsigned long state = 0;
    uint32_t word_before = 0;
    uint32_t word_shadow = 0;
    uint32_t child_word = 0;
    uint32_t word_after_fork = 0;
    uint32_t word_after_clear = 0;
    ssize_t child_result_read = -1;
    long arm_rc = -1;
    long ready_rc = -1;
    long observed_rc = -1;
    long hook_arm_rc = -1;
    long hook_status_rc = -1;
    long hook_clear_rc = -1;
    long inspect_rc = -1;
    long clear_rc = -1;
    long cleared_rc = -1;
    int normal_value = -1;
    int shadow_value = -1;
    int child_value = -1;
    int shadow_after_fork_value = -1;
    int restored_value = -1;
    int handler_installed = 0;
    int pipe_fds[2] = {-1, -1};
    pid_t child = -1;
    pid_t waited = -1;
    int child_status = -1;
    int child_exit = -1;
    struct r0lab_fork_child_result child_result = {
        .value = -1,
        .err = 0,
        .word = 0,
    };
    int failures = 0;

    if (r0lab_parse_token(token_text, &token)) {
        snprintf(output, output_size,
                 "rc=-22 error=invalid raw fork hook token");
        return -1;
    }
    page_size = (size_t)sysconf(_SC_PAGESIZE);
    if (page_size != R0LAB_M3_PAGE_SIZE) {
        snprintf(output, output_size,
                 "rc=-38 error=unsupported page size=%zu", page_size);
        return -1;
    }
    page = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) {
        snprintf(output, output_size,
                 "rc=-12 error=raw fork hook page allocation errno=%d", errno);
        return -1;
    }
    code = page;
    code[0] = R0LAB_M3_CODE_MOV_W0_42;
    code[1] = R0LAB_M3_CODE_RET;
    __builtin___clear_cache((char *)page, (char *)page + page_size);
    if (mprotect(page, page_size, PROT_READ | PROT_EXEC)) {
        snprintf(output, output_size,
                 "rc=-1 error=initial raw fork hook mprotect errno=%d", errno);
        munmap(page, page_size);
        return -1;
    }

    readable_code = (volatile uint32_t *)page;
    word_before = readable_code[0];
    normal_value = ((int (*)(void))page)();

    snprintf(command, sizeof(command), "raw arm 0x%llx 0x%llx",
             (unsigned long long)token, (unsigned long long)(uintptr_t)page);
    arm_rc = r0lab_control_raw(command, reply, sizeof(reply));
    if (arm_rc < 0)
        goto finish;
    ready_rc = r0lab_m3_wait_for("raw ready", token);
    if (ready_rc < 0)
        goto clear;

    g_r0lab_raw_handler_faults = 0;
    g_r0lab_raw_signal_page = page;
    g_r0lab_raw_signal_page_size = page_size;
    action.sa_sigaction = r0lab_raw_signal_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_SIGINFO;
    if (sigaction(SIGSEGV, &action, &previous_action))
        goto clear;
    handler_installed = 1;

    shadow_value = ((int (*)(void))page)();
    word_shadow = readable_code[0];

    snprintf(command, sizeof(command), "raw observed 0x%llx",
             (unsigned long long)token);
    observed_rc = r0lab_control_raw(command, reply, sizeof(reply));
    if (observed_rc < 0 ||
        sscanf(reply, "raw_observed activations=%u state=%lu",
               &activations, &state) != 2)
        ++failures;

    snprintf(command, sizeof(command), "raw fork hook arm 0x%llx",
             (unsigned long long)token);
    hook_arm_rc = r0lab_control_raw(command, hook_reply, sizeof(hook_reply));
    if (hook_arm_rc >= 0) {
        errno = 0;
        if (pipe(pipe_fds)) {
            child_result.err = errno;
        } else {
            child = fork();
            if (child == 0) {
                struct r0lab_fork_child_result local_result = {
                    .value = -1,
                    .err = 0,
                    .word = 0,
                };

                close(pipe_fds[0]);
                errno = 0;
                local_result.word = ((volatile uint32_t *)page)[0];
                local_result.value = ((int (*)(void))page)();
                local_result.err = errno;
                (void)write(pipe_fds[1], &local_result,
                            sizeof(local_result));
                close(pipe_fds[1]);
                _exit(local_result.word == R0LAB_M3_CODE_MOV_W0_42 &&
                          local_result.value == 42
                          ? 0
                          : 65);
            }

            close(pipe_fds[1]);
            pipe_fds[1] = -1;
            if (child > 0) {
                child_result_read = read(pipe_fds[0], &child_result,
                                         sizeof(child_result));
                close(pipe_fds[0]);
                pipe_fds[0] = -1;
                waited = waitpid(child, &child_status, 0);
                if (waited == child && WIFEXITED(child_status))
                    child_exit = WEXITSTATUS(child_status);
                child_word = child_result.word;
                child_value = child_result.value;
            } else {
                child_result.err = errno;
                close(pipe_fds[0]);
                pipe_fds[0] = -1;
            }
        }
        word_after_fork = readable_code[0];
        shadow_after_fork_value = ((int (*)(void))page)();
    }

    snprintf(command, sizeof(command), "raw fork hook status 0x%llx",
             (unsigned long long)token);
    hook_status_rc = r0lab_control_raw(command, hook_status,
                                       sizeof(hook_status));

    snprintf(command, sizeof(command), "raw inspect 0x%llx",
             (unsigned long long)token);
    inspect_rc = r0lab_control_raw(command, inspect_reply,
                                   sizeof(inspect_reply));
    if (hook_arm_rc < 0 ||
        child_result_read != (ssize_t)sizeof(child_result) ||
        waited != child || child_exit != 0 ||
        hook_status_rc < 0 || inspect_rc < 0 ||
        !strstr(hook_reply, "raw_fork_hook_ready") ||
        !strstr(hook_reply, "installed=1") ||
        !strstr(hook_reply, "parent_pause=1") ||
        !strstr(hook_reply, "child_original_inherit=1") ||
        !strstr(hook_reply, "target_mm_scoped=1") ||
        !strstr(hook_status, "hook_begin_events=1") ||
        !strstr(hook_status, "hook_finish_events=1") ||
        !strstr(hook_status, "hook_failures=0") ||
        !strstr(hook_status, "primitive_begin_events=1") ||
        !strstr(hook_status, "primitive_finish_events=1") ||
        !strstr(hook_status, "child_original_inherit=1") ||
        !strstr(inspect_reply, "fork_hook_begin_events=1") ||
        !strstr(inspect_reply, "fork_hook_finish_events=1") ||
        !strstr(inspect_reply, "fork_hook_failures=0") ||
        !strstr(inspect_reply, "fork_hide_primitive=proven"))
        ++failures;

    snprintf(command, sizeof(command), "raw fork hook clear 0x%llx",
             (unsigned long long)token);
    hook_clear_rc = r0lab_control_raw(command, hook_clear_reply,
                                      sizeof(hook_clear_reply));

clear:
    if (pipe_fds[0] >= 0)
        close(pipe_fds[0]);
    if (pipe_fds[1] >= 0)
        close(pipe_fds[1]);
    r0lab_raw_clear(token, &clear_rc, &cleared_rc);
    if (handler_installed)
        sigaction(SIGSEGV, &previous_action, NULL);
    g_r0lab_raw_signal_page = NULL;
    g_r0lab_raw_signal_page_size = 0;
    if (cleared_rc >= 0) {
        word_after_clear = readable_code[0];
        restored_value = ((int (*)(void))page)();
    }

finish:
    if (normal_value != 42 || shadow_value != 99 ||
        child_value != 42 || shadow_after_fork_value != 99 ||
        restored_value != 42 ||
        word_before != R0LAB_M3_CODE_MOV_W0_42 ||
        word_shadow != R0LAB_M4_CODE_MOV_W0_99 ||
        child_word != R0LAB_M3_CODE_MOV_W0_42 ||
        word_after_fork != R0LAB_M4_CODE_MOV_W0_99 ||
        word_after_clear != R0LAB_M3_CODE_MOV_W0_42 ||
        arm_rc < 0 || ready_rc < 0 || observed_rc < 0 ||
        hook_arm_rc < 0 || hook_status_rc < 0 || hook_clear_rc < 0 ||
        inspect_rc < 0 || clear_rc < 0 || cleared_rc < 0 ||
        activations != 1 || state != 3 || g_r0lab_raw_handler_faults)
        ++failures;
    snprintf(output, output_size,
             "raw mode=fork-hook failures=%d parent_pause=1 child_original_inherit=1 normal_value=%d shadow_value=%d child_word=%08x child_value=%d shadow_after_fork=%08x shadow_after_fork_value=%d restored_value=%d words=%08x/%08x/%08x child_result_read=%zd child_errno=%d child_pid=%d waited_pid=%d child_status=%d child_exit=%d activations=%u state=%lu arm_rc=%ld ready_rc=%ld observed_rc=%ld hook_arm_rc=%ld hook_status_rc=%ld hook_clear_rc=%ld inspect_rc=%ld clear_rc=%ld cleared_rc=%ld handler_faults=%d hook=\"%s\" status=\"%s\" hook_clear=\"%s\" inspect=\"%s\"",
             failures, normal_value, shadow_value, child_word, child_value,
             word_after_fork, shadow_after_fork_value, restored_value,
             word_before, word_shadow, word_after_clear, child_result_read,
             child_result.err, (int)child, (int)waited, child_status,
             child_exit, activations, state, arm_rc, ready_rc, observed_rc,
             hook_arm_rc, hook_status_rc, hook_clear_rc, inspect_rc, clear_rc,
             cleared_rc, (int)g_r0lab_raw_handler_faults, hook_reply,
             hook_status, hook_clear_reply, inspect_reply);
    munmap(page, page_size);
    return failures ? -1 : 0;
}

static int r0lab_raw_fault_hook_run(const char *token_text, char *output,
                                    size_t output_size)
{
    struct sigaction action = {0};
    struct sigaction previous_action = {0};
    uint32_t *code;
    volatile uint32_t *readable_code;
    uint64_t token;
    void *page = MAP_FAILED;
    size_t page_size;
    char command[96];
    char reply[256] = {0};
    char hook_reply[512] = {0};
    char hook_status[512] = {0};
    char hook_clear_reply[512] = {0};
    char inspect_reply[2048] = {0};
    unsigned int activations = 0;
    unsigned long state = 0;
    uint32_t word_before = 0;
    uint32_t word_shadow = 0;
    uint32_t word_after_clear = 0;
    long arm_rc = -1;
    long ready_rc = -1;
    long observed_rc = -1;
    long hook_arm_rc = -1;
    long hook_status_rc = -1;
    long hook_clear_rc = -1;
    long inspect_rc = -1;
    long clear_rc = -1;
    long cleared_rc = -1;
    int normal_value = -1;
    int shadow_value = -1;
    int shadow_after_hook_value = -1;
    int restored_value = -1;
    int handler_installed = 0;
    int failures = 0;

    if (r0lab_parse_token(token_text, &token)) {
        snprintf(output, output_size,
                 "rc=-22 error=invalid raw fault hook token");
        return -1;
    }
    page_size = (size_t)sysconf(_SC_PAGESIZE);
    if (page_size != R0LAB_M3_PAGE_SIZE) {
        snprintf(output, output_size,
                 "rc=-38 error=unsupported page size=%zu", page_size);
        return -1;
    }
    page = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) {
        snprintf(output, output_size,
                 "rc=-12 error=raw fault hook page allocation errno=%d",
                 errno);
        return -1;
    }
    code = page;
    code[0] = R0LAB_M3_CODE_MOV_W0_42;
    code[1] = R0LAB_M3_CODE_RET;
    __builtin___clear_cache((char *)page, (char *)page + page_size);
    if (mprotect(page, page_size, PROT_READ | PROT_EXEC)) {
        snprintf(output, output_size,
                 "rc=-1 error=initial raw fault hook mprotect errno=%d",
                 errno);
        munmap(page, page_size);
        return -1;
    }

    readable_code = (volatile uint32_t *)page;
    word_before = readable_code[0];
    normal_value = ((int (*)(void))page)();

    snprintf(command, sizeof(command), "raw arm 0x%llx 0x%llx",
             (unsigned long long)token, (unsigned long long)(uintptr_t)page);
    arm_rc = r0lab_control_raw(command, reply, sizeof(reply));
    if (arm_rc < 0)
        goto finish;
    ready_rc = r0lab_m3_wait_for("raw ready", token);
    if (ready_rc < 0)
        goto clear;

    g_r0lab_raw_handler_faults = 0;
    g_r0lab_raw_signal_page = page;
    g_r0lab_raw_signal_page_size = page_size;
    action.sa_sigaction = r0lab_raw_signal_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_SIGINFO;
    if (sigaction(SIGSEGV, &action, &previous_action))
        goto clear;
    handler_installed = 1;

    shadow_value = ((int (*)(void))page)();
    word_shadow = readable_code[0];

    snprintf(command, sizeof(command), "raw observed 0x%llx",
             (unsigned long long)token);
    observed_rc = r0lab_control_raw(command, reply, sizeof(reply));
    if (observed_rc < 0 ||
        sscanf(reply, "raw_observed activations=%u state=%lu",
               &activations, &state) != 2)
        ++failures;

    snprintf(command, sizeof(command), "raw fault hook arm 0x%llx",
             (unsigned long long)token);
    hook_arm_rc = r0lab_control_raw(command, hook_reply, sizeof(hook_reply));
    if (hook_arm_rc >= 0)
        shadow_after_hook_value = ((int (*)(void))page)();

    snprintf(command, sizeof(command), "raw fault hook status 0x%llx",
             (unsigned long long)token);
    hook_status_rc = r0lab_control_raw(command, hook_status,
                                       sizeof(hook_status));

    snprintf(command, sizeof(command), "raw inspect 0x%llx",
             (unsigned long long)token);
    inspect_rc = r0lab_control_raw(command, inspect_reply,
                                   sizeof(inspect_reply));
    if (hook_arm_rc < 0 || hook_status_rc < 0 || inspect_rc < 0 ||
        !strstr(hook_reply, "raw_fault_hook_ready") ||
        !strstr(hook_reply, "symbol=handle_mm_fault") ||
        !strstr(hook_reply, "target_mm_scoped=1") ||
        !strstr(hook_reply, "observe_only=1") ||
        !strstr(hook_reply, "pte_switch=0") ||
        !strstr(hook_status, "installed=1") ||
        !strstr(hook_status, "hit_events=0") ||
        !strstr(hook_status, "failures=0") ||
        !strstr(inspect_reply, "fault_hook_installed=1") ||
        !strstr(inspect_reply, "fault_hook_read_events=0") ||
        !strstr(inspect_reply, "fault_hook_write_events=0") ||
        !strstr(inspect_reply, "fault_hook_exec_events=0") ||
        !strstr(inspect_reply, "fault_hook=observe_only"))
        ++failures;

    snprintf(command, sizeof(command), "raw fault hook clear 0x%llx",
             (unsigned long long)token);
    hook_clear_rc = r0lab_control_raw(command, hook_clear_reply,
                                      sizeof(hook_clear_reply));

clear:
    r0lab_raw_clear(token, &clear_rc, &cleared_rc);
    if (handler_installed)
        sigaction(SIGSEGV, &previous_action, NULL);
    g_r0lab_raw_signal_page = NULL;
    g_r0lab_raw_signal_page_size = 0;
    if (cleared_rc >= 0) {
        word_after_clear = readable_code[0];
        restored_value = ((int (*)(void))page)();
    }

finish:
    if (normal_value != 42 || shadow_value != 99 ||
        shadow_after_hook_value != 99 || restored_value != 42 ||
        word_before != R0LAB_M3_CODE_MOV_W0_42 ||
        word_shadow != R0LAB_M4_CODE_MOV_W0_99 ||
        word_after_clear != R0LAB_M3_CODE_MOV_W0_42 ||
        arm_rc < 0 || ready_rc < 0 || observed_rc < 0 ||
        hook_arm_rc < 0 || hook_status_rc < 0 || hook_clear_rc < 0 ||
        inspect_rc < 0 || clear_rc < 0 || cleared_rc < 0 ||
        activations != 1 || state != 3 || g_r0lab_raw_handler_faults)
        ++failures;
    snprintf(output, output_size,
             "raw mode=fault-hook failures=%d observe_only=1 target_mm_scoped=1 pte_switch=0 normal_value=%d shadow_value=%d shadow_after_hook_value=%d restored_value=%d words=%08x/%08x/%08x activations=%u state=%lu arm_rc=%ld ready_rc=%ld observed_rc=%ld hook_arm_rc=%ld hook_status_rc=%ld hook_clear_rc=%ld inspect_rc=%ld clear_rc=%ld cleared_rc=%ld handler_faults=%d hook=\"%s\" status=\"%s\" hook_clear=\"%s\" inspect=\"%s\"",
             failures, normal_value, shadow_value, shadow_after_hook_value,
             restored_value, word_before, word_shadow, word_after_clear,
             activations, state, arm_rc, ready_rc, observed_rc, hook_arm_rc,
             hook_status_rc, hook_clear_rc, inspect_rc, clear_rc, cleared_rc,
             (int)g_r0lab_raw_handler_faults, hook_reply, hook_status,
             hook_clear_reply, inspect_reply);
    munmap(page, page_size);
    return failures ? -1 : 0;
}

static int r0lab_raw_abort_probe_run(const char *token_text, char *output,
                                     size_t output_size)
{
    struct sigaction action = {0};
    struct sigaction previous_action = {0};
    uint32_t *code;
    volatile uint32_t *readable_code;
    uint64_t token;
    void *page = MAP_FAILED;
    size_t page_size;
    char command[96];
    char reply[256] = {0};
    char probe_reply[512] = {0};
    char probe_status[512] = {0};
    char probe_clear_reply[512] = {0};
    unsigned int activations = 0;
    unsigned long state = 0;
    uint32_t word_before = 0;
    uint32_t word_shadow = 0;
    uint32_t probe_read_word = 0;
    uint32_t word_after_clear = 0;
    long arm_rc = -1;
    long ready_rc = -1;
    long observed_rc = -1;
    long probe_arm_rc = -1;
    long mprotect_none_rc = -1;
    long probe_status_rc = -1;
    long probe_clear_rc = -1;
    long clear_rc = -1;
    long cleared_rc = -1;
    int mprotect_none_errno = 0;
    int normal_value = -1;
    int shadow_value = -1;
    int post_probe_exec_value = -1;
    int restored_value = -1;
    int handler_installed = 0;
    int probe_read_completed = 0;
    int failures = 0;

    if (r0lab_parse_token(token_text, &token)) {
        snprintf(output, output_size,
                 "rc=-22 error=invalid raw abort probe token");
        return -1;
    }
    page_size = (size_t)sysconf(_SC_PAGESIZE);
    if (page_size != R0LAB_M3_PAGE_SIZE) {
        snprintf(output, output_size,
                 "rc=-38 error=unsupported page size=%zu", page_size);
        return -1;
    }
    page = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) {
        snprintf(output, output_size,
                 "rc=-12 error=raw abort probe page allocation errno=%d",
                 errno);
        return -1;
    }
    code = page;
    code[0] = R0LAB_M3_CODE_MOV_W0_42;
    code[1] = R0LAB_M3_CODE_RET;
    __builtin___clear_cache((char *)page, (char *)page + page_size);
    if (mprotect(page, page_size, PROT_READ | PROT_EXEC)) {
        snprintf(output, output_size,
                 "rc=-1 error=initial raw abort probe mprotect errno=%d",
                 errno);
        munmap(page, page_size);
        return -1;
    }

    readable_code = (volatile uint32_t *)page;
    word_before = readable_code[0];
    normal_value = ((int (*)(void))page)();

    snprintf(command, sizeof(command), "raw arm 0x%llx 0x%llx",
             (unsigned long long)token, (unsigned long long)(uintptr_t)page);
    arm_rc = r0lab_control_raw(command, reply, sizeof(reply));
    if (arm_rc < 0)
        goto finish;
    ready_rc = r0lab_m3_wait_for("raw ready", token);
    if (ready_rc < 0)
        goto clear;

    g_r0lab_raw_handler_faults = 0;
    g_r0lab_raw_signal_page = page;
    g_r0lab_raw_signal_page_size = page_size;
    action.sa_sigaction = r0lab_raw_signal_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_SIGINFO;
    if (sigaction(SIGSEGV, &action, &previous_action))
        goto clear;
    handler_installed = 1;

    shadow_value = ((int (*)(void))page)();
    word_shadow = readable_code[0];

    snprintf(command, sizeof(command), "raw observed 0x%llx",
             (unsigned long long)token);
    observed_rc = r0lab_control_raw(command, reply, sizeof(reply));
    if (observed_rc < 0 ||
        sscanf(reply, "raw_observed activations=%u state=%lu",
               &activations, &state) != 2)
        ++failures;

    snprintf(command, sizeof(command), "raw abort probe arm 0x%llx",
             (unsigned long long)token);
    probe_arm_rc = r0lab_control_raw(command, probe_reply,
                                     sizeof(probe_reply));
    if (probe_arm_rc >= 0) {
        errno = 0;
        mprotect_none_rc = mprotect(page, page_size, PROT_NONE);
        mprotect_none_errno = errno;
        if (!mprotect_none_rc) {
            probe_read_word = readable_code[0];
            probe_read_completed = 1;
            post_probe_exec_value = ((int (*)(void))page)();
        }
    }

    snprintf(command, sizeof(command), "raw abort probe status 0x%llx",
             (unsigned long long)token);
    probe_status_rc = r0lab_control_raw(command, probe_status,
                                        sizeof(probe_status));
    if (probe_arm_rc < 0 || mprotect_none_rc < 0 || probe_status_rc < 0 ||
        !strstr(probe_reply, "raw_abort_probe_ready") ||
        !strstr(probe_reply, "symbol=do_mem_abort") ||
        !strstr(probe_reply, "source=raw_va_prot_none") ||
        !strstr(probe_reply, "observe_only=1") ||
        !strstr(probe_reply, "pte_switch=0") ||
        !strstr(probe_reply, "read_cycle=absent") ||
        !strstr(probe_status, "installed=1") ||
        !strstr(probe_status, "armed=1") ||
        !strstr(probe_status, "read_events=1") ||
        !strstr(probe_status, "write_events=0") ||
        !strstr(probe_status, "exec_events=0") ||
        !strstr(probe_status, "hit_events=1") ||
        !strstr(probe_status, "failures=0") ||
        !strstr(probe_status, "last_ec=36") ||
        !strstr(probe_status, "last_wnr=0") ||
        !strstr(probe_status, "last_fsc_type=4") ||
        !strstr(probe_status, "permission_fault=0") ||
        !strstr(probe_status, "translation_fault=1") ||
        !strstr(probe_status, "data_fault=sync_el0_dabt") ||
        !strstr(probe_status, "read_cycle=absent"))
        ++failures;

    snprintf(command, sizeof(command), "raw abort probe clear 0x%llx",
             (unsigned long long)token);
    probe_clear_rc = r0lab_control_raw(command, probe_clear_reply,
                                       sizeof(probe_clear_reply));

clear:
    r0lab_raw_clear(token, &clear_rc, &cleared_rc);
    if (handler_installed)
        sigaction(SIGSEGV, &previous_action, NULL);
    g_r0lab_raw_signal_page = NULL;
    g_r0lab_raw_signal_page_size = 0;
    if (cleared_rc >= 0) {
        word_after_clear = readable_code[0];
        restored_value = ((int (*)(void))page)();
    }

finish:
    if (normal_value != 42 || shadow_value != 99 ||
        post_probe_exec_value != 99 || restored_value != 42 ||
        word_before != R0LAB_M3_CODE_MOV_W0_42 ||
        word_shadow != R0LAB_M4_CODE_MOV_W0_99 ||
        probe_read_word != R0LAB_M4_CODE_MOV_W0_99 ||
        word_after_clear != R0LAB_M3_CODE_MOV_W0_42 ||
        arm_rc < 0 || ready_rc < 0 || observed_rc < 0 ||
        probe_arm_rc < 0 || mprotect_none_rc < 0 ||
        probe_status_rc < 0 || probe_clear_rc < 0 ||
        clear_rc < 0 || cleared_rc < 0 || activations != 1 ||
        state != 3 || !probe_read_completed ||
        g_r0lab_raw_handler_faults != 1)
        ++failures;
    snprintf(output, output_size,
             "raw mode=abort-probe failures=%d observe_only=1 target_mm_scoped=1 pte_switch=0 data_fault_source=raw_va_prot_none data_fault=sync_el0_dabt read_cycle=absent normal_value=%d shadow_value=%d probe_read_word=%08x post_probe_exec_value=%d restored_value=%d words=%08x/%08x/%08x activations=%u state=%lu probe_read_completed=%d mprotect_none_rc=%ld mprotect_none_errno=%d arm_rc=%ld ready_rc=%ld observed_rc=%ld probe_arm_rc=%ld probe_status_rc=%ld probe_clear_rc=%ld clear_rc=%ld cleared_rc=%ld handler_faults=%d probe=\"%s\" status=\"%s\" probe_clear=\"%s\"",
             failures, normal_value, shadow_value, probe_read_word,
             post_probe_exec_value, restored_value, word_before, word_shadow,
             word_after_clear, activations, state, probe_read_completed,
             mprotect_none_rc, mprotect_none_errno, arm_rc, ready_rc,
             observed_rc, probe_arm_rc, probe_status_rc, probe_clear_rc,
             clear_rc, cleared_rc, (int)g_r0lab_raw_handler_faults,
             probe_reply, probe_status, probe_clear_reply);
    munmap(page, page_size);
    return failures ? -1 : 0;
}

static int r0lab_raw_abort_write_probe_run(const char *token_text,
                                           char *output, size_t output_size)
{
    struct sigaction action = {0};
    struct sigaction previous_action = {0};
    uint32_t *code;
    volatile uint32_t *readable_code;
    uint64_t token;
    void *page = MAP_FAILED;
    size_t page_size;
    char command[96];
    char reply[256] = {0};
    char probe_reply[512] = {0};
    char probe_status[512] = {0};
    char probe_clear_reply[512] = {0};
    unsigned int activations = 0;
    unsigned long state = 0;
    uint32_t word_before = 0;
    uint32_t word_shadow = 0;
    uint32_t probe_write_word = 0;
    uint32_t word_after_clear = 0;
    long arm_rc = -1;
    long ready_rc = -1;
    long observed_rc = -1;
    long probe_arm_rc = -1;
    long probe_status_rc = -1;
    long probe_clear_rc = -1;
    long clear_rc = -1;
    long cleared_rc = -1;
    int normal_value = -1;
    int shadow_value = -1;
    int post_probe_exec_value = -1;
    int restored_value = -1;
    int handler_installed = 0;
    int probe_write_completed = 0;
    int probe_write_fault_caught = 0;
    int failures = 0;

    if (r0lab_parse_token(token_text, &token)) {
        snprintf(output, output_size,
                 "rc=-22 error=invalid raw abort write probe token");
        return -1;
    }
    page_size = (size_t)sysconf(_SC_PAGESIZE);
    if (page_size != R0LAB_M3_PAGE_SIZE) {
        snprintf(output, output_size,
                 "rc=-38 error=unsupported page size=%zu", page_size);
        return -1;
    }
    page = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) {
        snprintf(output, output_size,
                 "rc=-12 error=raw abort write probe page allocation errno=%d",
                 errno);
        return -1;
    }
    code = page;
    code[0] = R0LAB_M3_CODE_MOV_W0_42;
    code[1] = R0LAB_M3_CODE_RET;
    __builtin___clear_cache((char *)page, (char *)page + page_size);
    if (mprotect(page, page_size, PROT_READ | PROT_EXEC)) {
        snprintf(output, output_size,
                 "rc=-1 error=initial raw abort write probe mprotect errno=%d",
                 errno);
        munmap(page, page_size);
        return -1;
    }

    readable_code = (volatile uint32_t *)page;
    word_before = readable_code[0];
    normal_value = ((int (*)(void))page)();

    snprintf(command, sizeof(command), "raw arm 0x%llx 0x%llx",
             (unsigned long long)token, (unsigned long long)(uintptr_t)page);
    arm_rc = r0lab_control_raw(command, reply, sizeof(reply));
    if (arm_rc < 0)
        goto finish;
    ready_rc = r0lab_m3_wait_for("raw ready", token);
    if (ready_rc < 0)
        goto clear;

    g_r0lab_raw_handler_faults = 0;
    g_r0lab_raw_signal_restore_prot = PROT_READ | PROT_EXEC;
    g_r0lab_raw_signal_page = page;
    g_r0lab_raw_signal_page_size = page_size;
    action.sa_sigaction = r0lab_raw_signal_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_SIGINFO;
    if (sigaction(SIGSEGV, &action, &previous_action))
        goto clear;
    handler_installed = 1;

    shadow_value = ((int (*)(void))page)();
    word_shadow = readable_code[0];

    snprintf(command, sizeof(command), "raw observed 0x%llx",
             (unsigned long long)token);
    observed_rc = r0lab_control_raw(command, reply, sizeof(reply));
    if (observed_rc < 0 ||
        sscanf(reply, "raw_observed activations=%u state=%lu",
               &activations, &state) != 2)
        ++failures;

    snprintf(command, sizeof(command), "raw abort write probe arm 0x%llx",
             (unsigned long long)token);
    probe_arm_rc = r0lab_control_raw(command, probe_reply,
                                     sizeof(probe_reply));
    if (probe_arm_rc >= 0) {
        g_r0lab_raw_signal_jump_on_fault = 1;
        if (!sigsetjmp(g_r0lab_raw_signal_jump, 1)) {
            readable_code[0] = R0LAB_M4_CODE_MOV_W0_99;
            probe_write_completed = 1;
        } else {
            probe_write_fault_caught = 1;
        }
        g_r0lab_raw_signal_jump_on_fault = 0;
        probe_write_word = readable_code[0];
        __builtin___clear_cache((char *)page, (char *)page + page_size);
        post_probe_exec_value = ((int (*)(void))page)();
    }

    snprintf(command, sizeof(command), "raw abort write probe status 0x%llx",
             (unsigned long long)token);
    probe_status_rc = r0lab_control_raw(command, probe_status,
                                        sizeof(probe_status));
    if (probe_arm_rc < 0 || probe_status_rc < 0 ||
        !strstr(probe_reply, "raw_abort_probe_ready") ||
        !strstr(probe_reply, "symbol=do_mem_abort") ||
        !strstr(probe_reply, "source=raw_va_rx_write") ||
        !strstr(probe_reply, "observe_only=1") ||
        !strstr(probe_reply, "pte_switch=0") ||
        !strstr(probe_reply, "read_cycle=absent") ||
        !strstr(probe_status, "installed=1") ||
        !strstr(probe_status, "armed=1") ||
        !strstr(probe_status, "read_events=0") ||
        !strstr(probe_status, "write_events=1") ||
        !strstr(probe_status, "exec_events=0") ||
        !strstr(probe_status, "hit_events=1") ||
        !strstr(probe_status, "failures=0") ||
        !strstr(probe_status, "last_ec=36") ||
        !strstr(probe_status, "last_wnr=1") ||
        !strstr(probe_status, "last_fsc_type=c") ||
        !strstr(probe_status, "permission_fault=1") ||
        !strstr(probe_status, "translation_fault=0") ||
        !strstr(probe_status, "data_fault=sync_el0_dabt") ||
        !strstr(probe_status, "read_cycle=absent"))
        ++failures;

    snprintf(command, sizeof(command), "raw abort write probe clear 0x%llx",
             (unsigned long long)token);
    probe_clear_rc = r0lab_control_raw(command, probe_clear_reply,
                                       sizeof(probe_clear_reply));

clear:
    r0lab_raw_clear(token, &clear_rc, &cleared_rc);
    if (handler_installed)
        sigaction(SIGSEGV, &previous_action, NULL);
    g_r0lab_raw_signal_restore_prot = 0;
    g_r0lab_raw_signal_jump_on_fault = 0;
    g_r0lab_raw_signal_page = NULL;
    g_r0lab_raw_signal_page_size = 0;
    if (cleared_rc >= 0) {
        word_after_clear = readable_code[0];
        restored_value = ((int (*)(void))page)();
    }

finish:
    if (normal_value != 42 || shadow_value != 99 ||
        post_probe_exec_value != 99 || restored_value != 42 ||
        word_before != R0LAB_M3_CODE_MOV_W0_42 ||
        word_shadow != R0LAB_M4_CODE_MOV_W0_99 ||
        probe_write_word != R0LAB_M4_CODE_MOV_W0_99 ||
        word_after_clear != R0LAB_M3_CODE_MOV_W0_42 ||
        arm_rc < 0 || ready_rc < 0 || observed_rc < 0 ||
        probe_arm_rc < 0 || probe_status_rc < 0 || probe_clear_rc < 0 ||
        clear_rc < 0 || cleared_rc < 0 || activations != 1 ||
        state != 3 || probe_write_completed || !probe_write_fault_caught ||
        g_r0lab_raw_handler_faults != 1)
        ++failures;
    snprintf(output, output_size,
             "raw mode=abort-write-probe failures=%d observe_only=1 target_mm_scoped=1 pte_switch=0 data_fault_source=raw_va_rx_write data_fault=sync_el0_dabt read_cycle=absent normal_value=%d shadow_value=%d probe_write_word=%08x post_probe_exec_value=%d restored_value=%d words=%08x/%08x/%08x activations=%u state=%lu probe_write_completed=%d probe_write_fault_caught=%d arm_rc=%ld ready_rc=%ld observed_rc=%ld probe_arm_rc=%ld probe_status_rc=%ld probe_clear_rc=%ld clear_rc=%ld cleared_rc=%ld handler_faults=%d probe=\"%s\" status=\"%s\" probe_clear=\"%s\"",
             failures, normal_value, shadow_value, probe_write_word,
             post_probe_exec_value, restored_value, word_before, word_shadow,
             word_after_clear, activations, state, probe_write_completed,
             probe_write_fault_caught, arm_rc, ready_rc, observed_rc,
             probe_arm_rc, probe_status_rc, probe_clear_rc, clear_rc,
             cleared_rc,
             (int)g_r0lab_raw_handler_faults, probe_reply, probe_status,
             probe_clear_reply);
    munmap(page, page_size);
    return failures ? -1 : 0;
}

static int r0lab_raw_abort_write_release_run(const char *token_text,
                                             char *output, size_t output_size)
{
    struct sigaction action = {0};
    struct sigaction previous_action = {0};
    uint32_t *code;
    volatile uint32_t *readable_code;
    uint64_t token;
    void *page = MAP_FAILED;
    size_t page_size;
    char command[96];
    char reply[256] = {0};
    char release_reply[512] = {0};
    char release_status[512] = {0};
    char release_clear_reply[512] = {0};
    char inspect_reply[2048] = {0};
    unsigned int activations = 0;
    unsigned long state = 0;
    uint32_t word_before = 0;
    uint32_t word_shadow = 0;
    uint32_t release_write_word = 0;
    uint32_t word_after_clear = 0;
    long arm_rc = -1;
    long ready_rc = -1;
    long observed_rc = -1;
    long release_arm_rc = -1;
    long release_status_rc = -1;
    long inspect_rc = -1;
    long release_clear_rc = -1;
    long clear_rc = -1;
    long cleared_rc = -1;
    int normal_value = -1;
    int shadow_value = -1;
    int post_release_exec_value = -1;
    int restored_value = -1;
    int handler_installed = 0;
    int release_write_completed = 0;
    int release_write_fault_caught = 0;
    int failures = 0;

    if (r0lab_parse_token(token_text, &token)) {
        snprintf(output, output_size,
                 "rc=-22 error=invalid raw abort write release token");
        return -1;
    }
    page_size = (size_t)sysconf(_SC_PAGESIZE);
    if (page_size != R0LAB_M3_PAGE_SIZE) {
        snprintf(output, output_size,
                 "rc=-38 error=unsupported page size=%zu", page_size);
        return -1;
    }
    page = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) {
        snprintf(output, output_size,
                 "rc=-12 error=raw abort write release page allocation errno=%d",
                 errno);
        return -1;
    }
    code = page;
    code[0] = R0LAB_M3_CODE_MOV_W0_42;
    code[1] = R0LAB_M3_CODE_RET;
    __builtin___clear_cache((char *)page, (char *)page + page_size);
    if (mprotect(page, page_size, PROT_READ | PROT_EXEC)) {
        snprintf(output, output_size,
                 "rc=-1 error=initial raw abort write release mprotect errno=%d",
                 errno);
        munmap(page, page_size);
        return -1;
    }

    readable_code = (volatile uint32_t *)page;
    word_before = readable_code[0];
    normal_value = ((int (*)(void))page)();

    snprintf(command, sizeof(command), "raw arm 0x%llx 0x%llx",
             (unsigned long long)token, (unsigned long long)(uintptr_t)page);
    arm_rc = r0lab_control_raw(command, reply, sizeof(reply));
    if (arm_rc < 0)
        goto finish;
    ready_rc = r0lab_m3_wait_for("raw ready", token);
    if (ready_rc < 0)
        goto clear;

    g_r0lab_raw_handler_faults = 0;
    g_r0lab_raw_signal_restore_prot = PROT_READ | PROT_EXEC;
    g_r0lab_raw_signal_page = page;
    g_r0lab_raw_signal_page_size = page_size;
    action.sa_sigaction = r0lab_raw_signal_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_SIGINFO;
    if (sigaction(SIGSEGV, &action, &previous_action))
        goto clear;
    handler_installed = 1;

    shadow_value = ((int (*)(void))page)();
    word_shadow = readable_code[0];

    snprintf(command, sizeof(command), "raw observed 0x%llx",
             (unsigned long long)token);
    observed_rc = r0lab_control_raw(command, reply, sizeof(reply));
    if (observed_rc < 0 ||
        sscanf(reply, "raw_observed activations=%u state=%lu",
               &activations, &state) != 2)
        ++failures;

    snprintf(command, sizeof(command), "raw abort write release arm 0x%llx",
             (unsigned long long)token);
    release_arm_rc = r0lab_control_raw(command, release_reply,
                                       sizeof(release_reply));
    if (release_arm_rc >= 0) {
        g_r0lab_raw_signal_jump_on_fault = 1;
        if (!sigsetjmp(g_r0lab_raw_signal_jump, 1)) {
            readable_code[0] = R0LAB_M4_CODE_MOV_W0_99;
            release_write_completed = 1;
        } else {
            release_write_fault_caught = 1;
        }
        g_r0lab_raw_signal_jump_on_fault = 0;
        release_write_word = readable_code[0];
        __builtin___clear_cache((char *)page, (char *)page + page_size);
        post_release_exec_value = ((int (*)(void))page)();
    }

    snprintf(command, sizeof(command), "raw abort write release status 0x%llx",
             (unsigned long long)token);
    release_status_rc = r0lab_control_raw(command, release_status,
                                          sizeof(release_status));

    snprintf(command, sizeof(command), "raw inspect 0x%llx",
             (unsigned long long)token);
    inspect_rc = r0lab_control_raw(command, inspect_reply,
                                   sizeof(inspect_reply));

    if (release_arm_rc < 0 || release_status_rc < 0 || inspect_rc < 0 ||
        !strstr(release_reply, "raw_abort_write_release_ready") ||
        !strstr(release_reply, "source=raw_va_rx_write") ||
        !strstr(release_reply, "action=restore_original") ||
        !strstr(release_reply, "logical_release=1") ||
        !strstr(release_reply, "observe_only=0") ||
        !strstr(release_reply, "pte_switch=1") ||
        !strstr(release_reply, "skip_origin=0") ||
        !strstr(release_status, "armed=0") ||
        !strstr(release_status, "release_events=1") ||
        !strstr(release_status, "failures=0") ||
        !strstr(release_status, "last_ec=36") ||
        !strstr(release_status, "last_fsc_type=c") ||
        !strstr(release_status, "last_wnr=1") ||
        !strstr(release_status, "permission_fault=1") ||
        !strstr(release_status, "translation_fault=0") ||
        !strstr(release_status, "restore_result=0") ||
        !strstr(release_status, "pte_switch=1") ||
        !strstr(release_status, "skip_origin=0") ||
        !strstr(inspect_reply, "active_kind=original") ||
        !strstr(inspect_reply, "record_state=restoring"))
        ++failures;

    snprintf(command, sizeof(command), "raw abort write release clear 0x%llx",
             (unsigned long long)token);
    release_clear_rc = r0lab_control_raw(command, release_clear_reply,
                                         sizeof(release_clear_reply));

clear:
    r0lab_raw_clear(token, &clear_rc, &cleared_rc);
    if (handler_installed)
        sigaction(SIGSEGV, &previous_action, NULL);
    g_r0lab_raw_signal_restore_prot = 0;
    g_r0lab_raw_signal_jump_on_fault = 0;
    g_r0lab_raw_signal_page = NULL;
    g_r0lab_raw_signal_page_size = 0;
    if (cleared_rc >= 0) {
        word_after_clear = readable_code[0];
        restored_value = ((int (*)(void))page)();
    }

finish:
    if (normal_value != 42 || shadow_value != 99 ||
        post_release_exec_value != 42 || restored_value != 42 ||
        word_before != R0LAB_M3_CODE_MOV_W0_42 ||
        word_shadow != R0LAB_M4_CODE_MOV_W0_99 ||
        release_write_word != R0LAB_M3_CODE_MOV_W0_42 ||
        word_after_clear != R0LAB_M3_CODE_MOV_W0_42 ||
        arm_rc < 0 || ready_rc < 0 || observed_rc < 0 ||
        release_arm_rc < 0 || release_status_rc < 0 || inspect_rc < 0 ||
        release_clear_rc < 0 || clear_rc < 0 || cleared_rc < 0 ||
        activations != 1 || state != 3 || release_write_completed ||
        !release_write_fault_caught || g_r0lab_raw_handler_faults != 1)
        ++failures;
    snprintf(output, output_size,
             "raw mode=abort-write-release failures=%d target_mm_scoped=1 source=raw_va_rx_write action=restore_original logical_release=1 observe_only=0 pte_switch=1 data_fault=sync_el0_dabt read_cycle=absent skip_origin=0 normal_value=%d shadow_value=%d release_write_word=%08x post_release_exec_value=%d restored_value=%d words=%08x/%08x/%08x activations=%u state=%lu release_write_completed=%d release_write_fault_caught=%d arm_rc=%ld ready_rc=%ld observed_rc=%ld release_arm_rc=%ld release_status_rc=%ld inspect_rc=%ld release_clear_rc=%ld clear_rc=%ld cleared_rc=%ld handler_faults=%d release=\"%s\" status=\"%s\" inspect=\"%s\" release_clear=\"%s\"",
             failures, normal_value, shadow_value, release_write_word,
             post_release_exec_value, restored_value, word_before, word_shadow,
             word_after_clear, activations, state, release_write_completed,
             release_write_fault_caught, arm_rc, ready_rc, observed_rc,
             release_arm_rc, release_status_rc, inspect_rc, release_clear_rc,
             clear_rc, cleared_rc, (int)g_r0lab_raw_handler_faults,
             release_reply, release_status, inspect_reply,
             release_clear_reply);
    munmap(page, page_size);
    return failures ? -1 : 0;
}

static int r0lab_raw_syscall_hook_run(const char *token_text, char *output,
                                      size_t output_size)
{
    struct sigaction action = {0};
    struct sigaction previous_action = {0};
    uint32_t *code;
    volatile uint32_t *readable_code;
    uint64_t token;
    void *page = MAP_FAILED;
    size_t page_size;
    char command[96];
    char reply[256] = {0};
    char hook_reply[512] = {0};
    char hook_status[512] = {0};
    char hook_clear_reply[512] = {0};
    char inspect_reply[2048] = {0};
    unsigned int activations = 0;
    unsigned long state = 0;
    uint32_t word_before = 0;
    uint32_t word_shadow = 0;
    uint32_t word_after_clear = 0;
    long arm_rc = -1;
    long ready_rc = -1;
    long observed_rc = -1;
    long hook_arm_rc = -1;
    long trigger_rc = -1;
    long hook_status_rc = -1;
    long hook_clear_rc = -1;
    long inspect_rc = -1;
    long clear_rc = -1;
    long cleared_rc = -1;
    int normal_value = -1;
    int shadow_value = -1;
    int restored_value = -1;
    int handler_installed = 0;
    int failures = 0;

    if (r0lab_parse_token(token_text, &token)) {
        snprintf(output, output_size,
                 "rc=-22 error=invalid raw syscall hook token");
        return -1;
    }
    page_size = (size_t)sysconf(_SC_PAGESIZE);
    if (page_size != R0LAB_M3_PAGE_SIZE) {
        snprintf(output, output_size,
                 "rc=-38 error=unsupported page size=%zu", page_size);
        return -1;
    }
    page = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) {
        snprintf(output, output_size,
                 "rc=-12 error=raw syscall hook page allocation errno=%d",
                 errno);
        return -1;
    }
    code = page;
    code[0] = R0LAB_M3_CODE_MOV_W0_42;
    code[1] = R0LAB_M3_CODE_RET;
    __builtin___clear_cache((char *)page, (char *)page + page_size);
    if (mprotect(page, page_size, PROT_READ | PROT_EXEC)) {
        snprintf(output, output_size,
                 "rc=-1 error=initial raw syscall hook mprotect errno=%d",
                 errno);
        munmap(page, page_size);
        return -1;
    }

    readable_code = (volatile uint32_t *)page;
    word_before = readable_code[0];
    normal_value = ((int (*)(void))page)();

    snprintf(command, sizeof(command), "raw arm 0x%llx 0x%llx",
             (unsigned long long)token, (unsigned long long)(uintptr_t)page);
    arm_rc = r0lab_control_raw(command, reply, sizeof(reply));
    if (arm_rc < 0)
        goto finish;
    ready_rc = r0lab_m3_wait_for("raw ready", token);
    if (ready_rc < 0)
        goto clear;

    g_r0lab_raw_handler_faults = 0;
    g_r0lab_raw_signal_page = page;
    g_r0lab_raw_signal_page_size = page_size;
    action.sa_sigaction = r0lab_raw_signal_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_SIGINFO;
    if (sigaction(SIGSEGV, &action, &previous_action))
        goto clear;
    handler_installed = 1;

    shadow_value = ((int (*)(void))page)();
    word_shadow = readable_code[0];

    snprintf(command, sizeof(command), "raw observed 0x%llx",
             (unsigned long long)token);
    observed_rc = r0lab_control_raw(command, reply, sizeof(reply));
    if (observed_rc < 0 ||
        sscanf(reply, "raw_observed activations=%u state=%lu",
               &activations, &state) != 2)
        ++failures;

    snprintf(command, sizeof(command), "raw syscall hook arm 0x%llx",
             (unsigned long long)token);
    hook_arm_rc = r0lab_control_raw(command, hook_reply, sizeof(hook_reply));
    if (hook_arm_rc >= 0)
        trigger_rc = syscall(__NR_getpid);

    snprintf(command, sizeof(command), "raw syscall hook status 0x%llx",
             (unsigned long long)token);
    hook_status_rc = r0lab_control_raw(command, hook_status,
                                       sizeof(hook_status));

    snprintf(command, sizeof(command), "raw inspect 0x%llx",
             (unsigned long long)token);
    inspect_rc = r0lab_control_raw(command, inspect_reply,
                                   sizeof(inspect_reply));
    if (hook_arm_rc < 0 || trigger_rc <= 0 || hook_status_rc < 0 ||
        inspect_rc < 0 ||
        !strstr(hook_reply, "raw_syscall_hook_ready") ||
        !strstr(hook_reply, "symbol=getpid") ||
        !strstr(hook_reply, "target_mm_scoped=1") ||
        !strstr(hook_reply, "trigger=syscall_getpid") ||
        !strstr(hook_reply, "observe_only=1") ||
        !strstr(hook_reply, "pte_switch=0") ||
        !strstr(hook_status, "installed=1") ||
        strstr(hook_status, "hit_events=0") ||
        !strstr(hook_status, "failures=0") ||
        !strstr(hook_status, "trigger=syscall_getpid") ||
        !strstr(inspect_reply, "active_kind=shadow_rx"))
        ++failures;

    snprintf(command, sizeof(command), "raw syscall hook clear 0x%llx",
             (unsigned long long)token);
    hook_clear_rc = r0lab_control_raw(command, hook_clear_reply,
                                      sizeof(hook_clear_reply));

clear:
    r0lab_raw_clear(token, &clear_rc, &cleared_rc);
    if (handler_installed)
        sigaction(SIGSEGV, &previous_action, NULL);
    g_r0lab_raw_signal_page = NULL;
    g_r0lab_raw_signal_page_size = 0;
    if (cleared_rc >= 0) {
        word_after_clear = readable_code[0];
        restored_value = ((int (*)(void))page)();
    }

finish:
    if (normal_value != 42 || shadow_value != 99 || restored_value != 42 ||
        word_before != R0LAB_M3_CODE_MOV_W0_42 ||
        word_shadow != R0LAB_M4_CODE_MOV_W0_99 ||
        word_after_clear != R0LAB_M3_CODE_MOV_W0_42 ||
        arm_rc < 0 || ready_rc < 0 || observed_rc < 0 ||
        hook_arm_rc < 0 || trigger_rc <= 0 || hook_status_rc < 0 ||
        hook_clear_rc < 0 || inspect_rc < 0 || clear_rc < 0 ||
        cleared_rc < 0 || activations != 1 || state != 3 ||
        g_r0lab_raw_handler_faults)
        ++failures;
    snprintf(output, output_size,
             "raw mode=syscall-hook failures=%d trigger=syscall_getpid observe_only=1 target_mm_scoped=1 pte_switch=0 normal_value=%d shadow_value=%d restored_value=%d words=%08x/%08x/%08x activations=%u state=%lu trigger_pid=%ld arm_rc=%ld ready_rc=%ld observed_rc=%ld hook_arm_rc=%ld hook_status_rc=%ld hook_clear_rc=%ld inspect_rc=%ld clear_rc=%ld cleared_rc=%ld handler_faults=%d hook=\"%s\" status=\"%s\" hook_clear=\"%s\" inspect=\"%s\"",
             failures, normal_value, shadow_value, restored_value,
             word_before, word_shadow, word_after_clear, activations, state,
             trigger_rc, arm_rc, ready_rc, observed_rc, hook_arm_rc,
             hook_status_rc, hook_clear_rc, inspect_rc, clear_rc, cleared_rc,
             (int)g_r0lab_raw_handler_faults, hook_reply, hook_status,
             hook_clear_reply, inspect_reply);
    munmap(page, page_size);
    return failures ? -1 : 0;
}

static int r0lab_raw_syscall_read_cycle_run(const char *token_text,
                                            char *output,
                                            size_t output_size)
{
    struct sigaction action = {0};
    struct sigaction previous_action = {0};
    uint32_t *code;
    volatile uint32_t *readable_code;
    uint64_t token;
    void *page = MAP_FAILED;
    size_t page_size;
    char command[128];
    char reply[256] = {0};
    char hook_reply[512] = {0};
    char hook_status_before[512] = {0};
    char hook_status_after[512] = {0};
    char hook_clear_reply[512] = {0};
    char inspect_reply[2048] = {0};
    unsigned int activations = 0;
    unsigned long state = 0;
    uint32_t word_before = 0;
    uint32_t word_shadow = 0;
    uint32_t original_read_word = 0;
    uint32_t word_after_resume = 0;
    uint32_t word_after_clear = 0;
    long arm_rc = -1;
    long ready_rc = -1;
    long observed_rc = -1;
    long hook_arm_rc = -1;
    long trigger_rc = -1;
    long hook_status_before_rc = -1;
    long hook_status_after_rc = -1;
    long hook_clear_rc = -1;
    long inspect_rc = -1;
    long clear_rc = -1;
    long cleared_rc = -1;
    int normal_value = -1;
    int shadow_value = -1;
    int resume_value = -1;
    int restored_value = -1;
    int handler_installed = 0;
    int failures = 0;

    if (r0lab_parse_token(token_text, &token)) {
        snprintf(output, output_size,
                 "rc=-22 error=invalid raw syscall read cycle token");
        return -1;
    }
    page_size = (size_t)sysconf(_SC_PAGESIZE);
    if (page_size != R0LAB_M3_PAGE_SIZE) {
        snprintf(output, output_size,
                 "rc=-38 error=unsupported page size=%zu", page_size);
        return -1;
    }
    page = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) {
        snprintf(output, output_size,
                 "rc=-12 error=raw syscall read cycle page allocation errno=%d",
                 errno);
        return -1;
    }
    code = page;
    code[0] = R0LAB_M3_CODE_MOV_W0_42;
    code[1] = R0LAB_M3_CODE_RET;
    __builtin___clear_cache((char *)page, (char *)page + page_size);
    if (mprotect(page, page_size, PROT_READ | PROT_EXEC)) {
        snprintf(output, output_size,
                 "rc=-1 error=initial raw syscall read cycle mprotect errno=%d",
                 errno);
        munmap(page, page_size);
        return -1;
    }

    readable_code = (volatile uint32_t *)page;
    word_before = readable_code[0];
    normal_value = ((int (*)(void))page)();

    snprintf(command, sizeof(command), "raw arm 0x%llx 0x%llx",
             (unsigned long long)token, (unsigned long long)(uintptr_t)page);
    arm_rc = r0lab_control_raw(command, reply, sizeof(reply));
    if (arm_rc < 0)
        goto finish;
    ready_rc = r0lab_m3_wait_for("raw ready", token);
    if (ready_rc < 0)
        goto clear;

    g_r0lab_raw_handler_faults = 0;
    g_r0lab_raw_signal_page = page;
    g_r0lab_raw_signal_page_size = page_size;
    action.sa_sigaction = r0lab_raw_signal_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_SIGINFO;
    if (sigaction(SIGSEGV, &action, &previous_action))
        goto clear;
    handler_installed = 1;

    shadow_value = ((int (*)(void))page)();
    word_shadow = readable_code[0];

    snprintf(command, sizeof(command), "raw observed 0x%llx",
             (unsigned long long)token);
    observed_rc = r0lab_control_raw(command, reply, sizeof(reply));
    if (observed_rc < 0 ||
        sscanf(reply, "raw_observed activations=%u state=%lu",
               &activations, &state) != 2)
        ++failures;

    snprintf(command, sizeof(command),
             "raw syscall read cycle hook arm 0x%llx",
             (unsigned long long)token);
    hook_arm_rc = r0lab_control_raw(command, hook_reply, sizeof(hook_reply));
    if (hook_arm_rc >= 0) {
        trigger_rc = syscall(__NR_getpid);
        original_read_word = readable_code[0];
    }

    snprintf(command, sizeof(command),
             "raw syscall read cycle hook status 0x%llx",
             (unsigned long long)token);
    hook_status_before_rc = r0lab_control_raw(command, hook_status_before,
                                              sizeof(hook_status_before));

    if (hook_arm_rc >= 0) {
        resume_value = ((int (*)(void))page)();
        word_after_resume = readable_code[0];
    }

    snprintf(command, sizeof(command),
             "raw syscall read cycle hook status 0x%llx",
             (unsigned long long)token);
    hook_status_after_rc = r0lab_control_raw(command, hook_status_after,
                                             sizeof(hook_status_after));

    snprintf(command, sizeof(command), "raw inspect 0x%llx",
             (unsigned long long)token);
    inspect_rc = r0lab_control_raw(command, inspect_reply,
                                   sizeof(inspect_reply));
    if (hook_arm_rc < 0 || trigger_rc <= 0 ||
        hook_status_before_rc < 0 || hook_status_after_rc < 0 ||
        inspect_rc < 0 ||
        !strstr(hook_reply, "raw_syscall_read_cycle_hook_ready") ||
        !strstr(hook_reply, "symbol=getpid") ||
        !strstr(hook_reply, "target_mm_scoped=1") ||
        !strstr(hook_reply, "trigger=syscall_getpid") ||
        !strstr(hook_reply, "read_cycle=uxn_original_exec_resume") ||
        !strstr(hook_reply, "observe_only=0") ||
        !strstr(hook_reply, "pte_switch=1") ||
        !strstr(hook_reply, "data_fault=absent") ||
        !strstr(hook_status_before, "installed=1") ||
        !strstr(hook_status_before, "hit_events=1") ||
        !strstr(hook_status_before, "read_cycle_events=1") ||
        !strstr(hook_status_before, "exec_resume=pending") ||
        !strstr(hook_status_before, "failures=0") ||
        !strstr(hook_status_after, "installed=1") ||
        !strstr(hook_status_after, "hit_events=1") ||
        !strstr(hook_status_after, "read_cycle_events=1") ||
        !strstr(hook_status_after, "exec_resume=proven") ||
        !strstr(hook_status_after, "failures=0") ||
        !strstr(inspect_reply, "active_kind=shadow_rx") ||
        !strstr(inspect_reply, "read_cycle=uxn_original_exec_resume") ||
        !strstr(inspect_reply, "read_cycle_begin_events=1") ||
        !strstr(inspect_reply, "read_cycle_finish_events=1") ||
        !strstr(inspect_reply, "read_cycle_exec_resume=proven"))
        ++failures;

    snprintf(command, sizeof(command),
             "raw syscall read cycle hook clear 0x%llx",
             (unsigned long long)token);
    hook_clear_rc = r0lab_control_raw(command, hook_clear_reply,
                                      sizeof(hook_clear_reply));

clear:
    r0lab_raw_clear(token, &clear_rc, &cleared_rc);
    if (handler_installed)
        sigaction(SIGSEGV, &previous_action, NULL);
    g_r0lab_raw_signal_page = NULL;
    g_r0lab_raw_signal_page_size = 0;
    if (cleared_rc >= 0) {
        word_after_clear = readable_code[0];
        restored_value = ((int (*)(void))page)();
    }

finish:
    if (normal_value != 42 || shadow_value != 99 ||
        resume_value != 99 || restored_value != 42 ||
        word_before != R0LAB_M3_CODE_MOV_W0_42 ||
        word_shadow != R0LAB_M4_CODE_MOV_W0_99 ||
        original_read_word != R0LAB_M3_CODE_MOV_W0_42 ||
        word_after_resume != R0LAB_M4_CODE_MOV_W0_99 ||
        word_after_clear != R0LAB_M3_CODE_MOV_W0_42 ||
        arm_rc < 0 || ready_rc < 0 || observed_rc < 0 ||
        hook_arm_rc < 0 || trigger_rc <= 0 ||
        hook_status_before_rc < 0 || hook_status_after_rc < 0 ||
        hook_clear_rc < 0 || inspect_rc < 0 || clear_rc < 0 ||
        cleared_rc < 0 || activations != 1 || state != 3 ||
        g_r0lab_raw_handler_faults)
        ++failures;
    snprintf(output, output_size,
             "raw mode=syscall-read-cycle failures=%d trigger=syscall_getpid read_cycle=uxn_original_exec_resume data_fault=absent pte_switch=1 exec_resume=1 normal_value=%d shadow_value=%d original_read_word=%08x resume_value=%d shadow_after_resume=%08x restored_value=%d words=%08x/%08x/%08x/%08x/%08x activations=%u state=%lu trigger_pid=%ld arm_rc=%ld ready_rc=%ld observed_rc=%ld hook_arm_rc=%ld hook_status_before_rc=%ld hook_status_after_rc=%ld hook_clear_rc=%ld inspect_rc=%ld clear_rc=%ld cleared_rc=%ld handler_faults=%d hook=\"%s\" status_before=\"%s\" status_after=\"%s\" hook_clear=\"%s\" inspect=\"%s\"",
             failures, normal_value, shadow_value, original_read_word,
             resume_value, word_after_resume, restored_value, word_before,
             word_shadow, original_read_word, word_after_resume,
             word_after_clear, activations, state, trigger_rc, arm_rc,
             ready_rc, observed_rc, hook_arm_rc, hook_status_before_rc,
             hook_status_after_rc, hook_clear_rc, inspect_rc, clear_rc,
             cleared_rc, (int)g_r0lab_raw_handler_faults, hook_reply,
             hook_status_before, hook_status_after, hook_clear_reply,
             inspect_reply);
    munmap(page, page_size);
    return failures ? -1 : 0;
}

static int r0lab_raw_prctl_read_cycle_run(const char *token_text,
                                          char *output,
                                          size_t output_size)
{
    struct sigaction action = {0};
    struct sigaction previous_action = {0};
    struct r0lab_prctl_passthrough_stress passthrough_stress = {0};
    pthread_t passthrough_thread;
    uint32_t *code;
    volatile uint32_t *readable_code;
    uint64_t token;
    void *page = MAP_FAILED;
    size_t page_size;
    char command[128];
    char reply[256] = {0};
    char hook_reply[1024] = {0};
    char hook_status_before[1024] = {0};
    char hook_status_after[1024] = {0};
    char hook_clear_reply[512] = {0};
    char inspect_reply[2048] = {0};
    unsigned int activations = 0;
    unsigned long state = 0;
    uint32_t word_before = 0;
    uint32_t word_shadow = 0;
    uint32_t shadow_after_reject = 0;
    uint32_t word_after_patch = 0;
    uint32_t word_after_release = 0;
    uint32_t original_read_word = 0;
    uint32_t word_after_resume = 0;
    uint32_t word_after_clear = 0;
    long arm_rc = -1;
    long ready_rc = -1;
    long observed_rc = -1;
    long hook_arm_rc = -1;
    long passthrough_before = -1;
    long reject_rc = -1;
    long patch_rc = -1;
    long release_rc = -1;
    long trigger_rc = -1;
    long passthrough_after = -1;
    long hook_status_before_rc = -1;
    long hook_status_after_rc = -1;
    long hook_clear_rc = -1;
    long inspect_rc = -1;
    long clear_rc = -1;
    long cleared_rc = -1;
    int normal_value = -1;
    int shadow_value = -1;
    int patch_value = -1;
    int release_value = -1;
    int resume_value = -1;
    int restored_value = -1;
    int reject_errno = 0;
    int handler_installed = 0;
    int passthrough_thread_started = 0;
    int passthrough_thread_rc = -1;
    int passthrough_join_rc = -1;
    int failures = 0;

    if (r0lab_parse_token(token_text, &token)) {
        snprintf(output, output_size,
                 "rc=-22 error=invalid raw prctl read cycle token");
        return -1;
    }
    page_size = (size_t)sysconf(_SC_PAGESIZE);
    if (page_size != R0LAB_M3_PAGE_SIZE) {
        snprintf(output, output_size,
                 "rc=-38 error=unsupported page size=%zu", page_size);
        return -1;
    }
    page = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) {
        snprintf(output, output_size,
                 "rc=-12 error=raw prctl read cycle page allocation errno=%d",
                 errno);
        return -1;
    }
    code = page;
    code[0] = R0LAB_M3_CODE_MOV_W0_42;
    code[1] = R0LAB_M3_CODE_RET;
    __builtin___clear_cache((char *)page, (char *)page + page_size);
    if (mprotect(page, page_size, PROT_READ | PROT_EXEC)) {
        snprintf(output, output_size,
                 "rc=-1 error=initial raw prctl read cycle mprotect errno=%d",
                 errno);
        munmap(page, page_size);
        return -1;
    }

    readable_code = (volatile uint32_t *)page;
    word_before = readable_code[0];
    normal_value = ((int (*)(void))page)();

    snprintf(command, sizeof(command), "raw arm 0x%llx 0x%llx",
             (unsigned long long)token, (unsigned long long)(uintptr_t)page);
    arm_rc = r0lab_control_raw(command, reply, sizeof(reply));
    if (arm_rc < 0)
        goto finish;
    ready_rc = r0lab_m3_wait_for("raw ready", token);
    if (ready_rc < 0)
        goto clear;

    g_r0lab_raw_handler_faults = 0;
    g_r0lab_raw_signal_page = page;
    g_r0lab_raw_signal_page_size = page_size;
    action.sa_sigaction = r0lab_raw_signal_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_SIGINFO;
    if (sigaction(SIGSEGV, &action, &previous_action))
        goto clear;
    handler_installed = 1;

    shadow_value = ((int (*)(void))page)();
    word_shadow = readable_code[0];

    snprintf(command, sizeof(command), "raw observed 0x%llx",
             (unsigned long long)token);
    observed_rc = r0lab_control_raw(command, reply, sizeof(reply));
    if (observed_rc < 0 ||
        sscanf(reply, "raw_observed activations=%u state=%lu",
               &activations, &state) != 2)
        ++failures;

    snprintf(command, sizeof(command),
             "raw prctl hook arm 0x%llx",
             (unsigned long long)token);
    hook_arm_rc = r0lab_control_raw(command, hook_reply, sizeof(hook_reply));
    if (hook_arm_rc >= 0) {
        passthrough_before =
            syscall(__NR_prctl, R0LAB_PRCTL_GET_DUMPABLE, 0, 0, 0, 0);
        errno = 0;
        reject_rc = syscall(__NR_prctl, R0LAB_PRCTL_MAGIC,
                            (unsigned long)(token ^ 1ULL),
                            R0LAB_PRCTL_OP_PATCH_WORD,
                            (unsigned long)(uintptr_t)page,
                            R0LAB_CODE_MOV_W0_77);
        reject_errno = errno;
        shadow_after_reject = readable_code[0];
        patch_rc = syscall(__NR_prctl, R0LAB_PRCTL_MAGIC,
                           (unsigned long)token,
                           R0LAB_PRCTL_OP_PATCH_WORD,
                           (unsigned long)(uintptr_t)page,
                           R0LAB_CODE_MOV_W0_77);
        word_after_patch = readable_code[0];
        patch_value = ((int (*)(void))page)();
        release_rc = syscall(__NR_prctl, R0LAB_PRCTL_MAGIC,
                             (unsigned long)token,
                             R0LAB_PRCTL_OP_RELEASE_PATCH,
                             (unsigned long)(uintptr_t)page, 0);
        word_after_release = readable_code[0];
        release_value = ((int (*)(void))page)();
        trigger_rc = syscall(__NR_prctl, R0LAB_PRCTL_MAGIC,
                             (unsigned long)token,
                             R0LAB_PRCTL_OP_READ_CYCLE, 0, 0);
        passthrough_after =
            syscall(__NR_prctl, R0LAB_PRCTL_GET_DUMPABLE, 0, 0, 0, 0);
        original_read_word = readable_code[0];
    }

    snprintf(command, sizeof(command),
             "raw prctl hook status 0x%llx",
             (unsigned long long)token);
    hook_status_before_rc = r0lab_control_raw(command, hook_status_before,
                                              sizeof(hook_status_before));

    if (hook_arm_rc >= 0) {
        resume_value = ((int (*)(void))page)();
        word_after_resume = readable_code[0];
    }

    snprintf(command, sizeof(command),
             "raw prctl hook status 0x%llx",
             (unsigned long long)token);
    hook_status_after_rc = r0lab_control_raw(command, hook_status_after,
                                             sizeof(hook_status_after));

    snprintf(command, sizeof(command), "raw inspect 0x%llx",
             (unsigned long long)token);
    inspect_rc = r0lab_control_raw(command, inspect_reply,
                                   sizeof(inspect_reply));
    if (hook_arm_rc < 0 || passthrough_before < 0 ||
        reject_rc != -1 || reject_errno != EPERM ||
        shadow_after_reject != R0LAB_M4_CODE_MOV_W0_99 ||
        patch_rc != 0 || patch_value != 77 ||
        word_after_patch != R0LAB_CODE_MOV_W0_77 ||
        release_rc != 0 || release_value != 99 ||
        word_after_release != R0LAB_M4_CODE_MOV_W0_99 ||
        passthrough_after != passthrough_before || trigger_rc != 0 ||
        hook_status_before_rc < 0 || hook_status_after_rc < 0 ||
        inspect_rc < 0 ||
        !strstr(hook_reply, "raw_prctl_hook_ready") ||
        !strstr(hook_reply, "symbol=prctl") ||
        !strstr(hook_reply, "abi=prctl_magic") ||
        !strstr(hook_reply, "option=0x52304c42") ||
        !strstr(hook_reply, "operations=1,2,3,4,5") ||
        !strstr(hook_reply, "read_cycle_op=1") ||
        !strstr(hook_reply, "patch_word_op=2") ||
        !strstr(hook_reply, "release_patch_op=3") ||
        !strstr(hook_reply, "patch_range_op=4") ||
        !strstr(hook_reply, "release_range_op=5") ||
        !strstr(hook_reply, "lab_uid_scoped=1") ||
        !strstr(hook_reply, "target_mm_scoped=1") ||
        !strstr(hook_reply, "token_scoped=1") ||
        !strstr(hook_reply, "patch_scope=single_page_ranges") ||
        !strstr(hook_reply, "patch_capacity=1024") ||
        !strstr(hook_reply, "overlap=version_last_write_wins") ||
        !strstr(hook_reply, "rebuild=original_seed_active_records") ||
        !strstr(hook_reply, "release=exact_start") ||
        !strstr(hook_reply, "dirty_tracking=byte_bitmap") ||
        !strstr(hook_reply, "user_copy=copy_from_user_nofault") ||
        !strstr(hook_reply, "cache_sync=sync_icache_aliases") ||
        !strstr(hook_reply, "read_cycle=uxn_original_exec_resume") ||
        !strstr(hook_reply, "observe_only=0") ||
        !strstr(hook_reply, "pte_switch=1") ||
        !strstr(hook_reply, "data_fault=absent") ||
        !strstr(hook_reply, "passthrough=nonmagic") ||
        !strstr(hook_status_before, "installed=1") ||
        !strstr(hook_status_before, "operations=1,2,3,4,5") ||
        !strstr(hook_status_before, "hit_events=3") ||
        !strstr(hook_status_before, "read_cycle_events=1") ||
        !strstr(hook_status_before, "patch_events=1") ||
        !strstr(hook_status_before, "release_events=1") ||
        !strstr(hook_status_before, "reject_events=1") ||
        !strstr(hook_status_before, "patch_active=0") ||
        !strstr(hook_status_before, "patch_record_slots=1") ||
        !strstr(hook_status_before, "patch_active_count=0") ||
        !strstr(hook_status_before, "patch_dirty_bytes=0") ||
        !strstr(hook_status_before, "exec_resume=pending") ||
        !strstr(hook_status_before, "failures=0") ||
        !strstr(hook_status_after, "installed=1") ||
        !strstr(hook_status_after, "operations=1,2,3,4,5") ||
        !strstr(hook_status_after, "hit_events=3") ||
        !strstr(hook_status_after, "read_cycle_events=1") ||
        !strstr(hook_status_after, "patch_events=1") ||
        !strstr(hook_status_after, "release_events=1") ||
        !strstr(hook_status_after, "reject_events=1") ||
        !strstr(hook_status_after, "patch_active=0") ||
        !strstr(hook_status_after, "patch_record_slots=1") ||
        !strstr(hook_status_after, "patch_active_count=0") ||
        !strstr(hook_status_after, "patch_dirty_bytes=0") ||
        !strstr(hook_status_after, "exec_resume=proven") ||
        !strstr(hook_status_after, "failures=0") ||
        !strstr(inspect_reply, "active_kind=shadow_rx") ||
        !strstr(inspect_reply, "read_cycle=uxn_original_exec_resume") ||
        !strstr(inspect_reply, "read_cycle_begin_events=1") ||
        !strstr(inspect_reply, "read_cycle_finish_events=1") ||
        !strstr(inspect_reply, "read_cycle_exec_resume=proven"))
        ++failures;

    passthrough_stress.expected = passthrough_before;
    if (passthrough_before >= 0) {
        passthrough_thread_rc =
            pthread_create(&passthrough_thread, NULL,
                           r0lab_prctl_passthrough_stress_thread,
                           &passthrough_stress);
        if (!passthrough_thread_rc) {
            unsigned int wait_attempt;

            passthrough_thread_started = 1;
            for (wait_attempt = 0;
                 wait_attempt < 1000 &&
                 atomic_load_explicit(&passthrough_stress.iterations,
                                      memory_order_relaxed) < 1000;
                 ++wait_attempt)
                usleep(1000);
        }
    }

    snprintf(command, sizeof(command),
             "raw prctl hook clear 0x%llx",
             (unsigned long long)token);
    hook_clear_rc = r0lab_control_raw(command, hook_clear_reply,
                                      sizeof(hook_clear_reply));
    if (passthrough_thread_started) {
        atomic_store_explicit(&passthrough_stress.stop, true,
                              memory_order_relaxed);
        passthrough_join_rc = pthread_join(passthrough_thread, NULL);
    }

clear:
    r0lab_raw_clear(token, &clear_rc, &cleared_rc);
    if (handler_installed)
        sigaction(SIGSEGV, &previous_action, NULL);
    g_r0lab_raw_signal_page = NULL;
    g_r0lab_raw_signal_page_size = 0;
    if (cleared_rc >= 0) {
        word_after_clear = readable_code[0];
        restored_value = ((int (*)(void))page)();
    }

finish:
    if (normal_value != 42 || shadow_value != 99 ||
        patch_value != 77 || release_value != 99 ||
        resume_value != 99 || restored_value != 42 ||
        word_before != R0LAB_M3_CODE_MOV_W0_42 ||
        word_shadow != R0LAB_M4_CODE_MOV_W0_99 ||
        shadow_after_reject != R0LAB_M4_CODE_MOV_W0_99 ||
        word_after_patch != R0LAB_CODE_MOV_W0_77 ||
        word_after_release != R0LAB_M4_CODE_MOV_W0_99 ||
        original_read_word != R0LAB_M3_CODE_MOV_W0_42 ||
        word_after_resume != R0LAB_M4_CODE_MOV_W0_99 ||
        word_after_clear != R0LAB_M3_CODE_MOV_W0_42 ||
        arm_rc < 0 || ready_rc < 0 || observed_rc < 0 ||
        hook_arm_rc < 0 || passthrough_before < 0 ||
        reject_rc != -1 || reject_errno != EPERM ||
        patch_rc != 0 || release_rc != 0 ||
        passthrough_after != passthrough_before || trigger_rc != 0 ||
        hook_status_before_rc < 0 || hook_status_after_rc < 0 ||
        hook_clear_rc < 0 || inspect_rc < 0 || clear_rc < 0 ||
        cleared_rc < 0 || activations != 1 || state != 3 ||
        passthrough_thread_rc || !passthrough_thread_started ||
        passthrough_join_rc ||
        atomic_load_explicit(&passthrough_stress.iterations,
                             memory_order_relaxed) < 1000 ||
        atomic_load_explicit(&passthrough_stress.failures,
                             memory_order_relaxed) ||
        g_r0lab_raw_handler_faults)
        ++failures;
    snprintf(output, output_size,
             "raw mode=prctl-dispatch failures=%d trigger=prctl_magic option=%08x operations=%u,%u,%u,%u,%u dispatch=patch_word,release_patch,patch_range,release_range,read_cycle read_cycle=uxn_original_exec_resume patch_scope=single_page_ranges patch_capacity=1024 overlap=version_last_write_wins rebuild=original_seed_active_records release=exact_start dirty_tracking=byte_bitmap user_copy=copy_from_user_nofault cache_sync=sync_icache_aliases data_fault=absent pte_switch=1 exec_resume=1 normal_value=%d shadow_value=%d reject_rc=%ld reject_errno=%d shadow_after_reject=%08x patch_rc=%ld patch_value=%d patch_word=%08x release_rc=%ld release_value=%d released_word=%08x trigger_rc=%ld original_read_word=%08x resume_value=%d shadow_after_resume=%08x restored_value=%d words=%08x/%08x/%08x/%08x/%08x/%08x/%08x activations=%u state=%lu passthrough_before=%ld passthrough_after=%ld passthrough_stress_iterations=%lu passthrough_stress_failures=%lu passthrough_thread_rc=%d passthrough_join_rc=%d arm_rc=%ld ready_rc=%ld observed_rc=%ld hook_arm_rc=%ld hook_status_before_rc=%ld hook_status_after_rc=%ld hook_clear_rc=%ld inspect_rc=%ld clear_rc=%ld cleared_rc=%ld handler_faults=%d hook=\"%s\" status_before=\"%s\" status_after=\"%s\" hook_clear=\"%s\" inspect=\"%s\"",
             failures, R0LAB_PRCTL_MAGIC, R0LAB_PRCTL_OP_READ_CYCLE,
             R0LAB_PRCTL_OP_PATCH_WORD, R0LAB_PRCTL_OP_RELEASE_PATCH,
             R0LAB_PRCTL_OP_PATCH_RANGE, R0LAB_PRCTL_OP_RELEASE_RANGE,
             normal_value, shadow_value, reject_rc, reject_errno,
             shadow_after_reject, patch_rc, patch_value, word_after_patch,
             release_rc, release_value, word_after_release, trigger_rc,
             original_read_word, resume_value, word_after_resume,
             restored_value, word_before, word_shadow,
             shadow_after_reject, word_after_patch, word_after_release,
             original_read_word, word_after_clear, activations, state,
             passthrough_before, passthrough_after,
             atomic_load_explicit(&passthrough_stress.iterations,
                                  memory_order_relaxed),
             atomic_load_explicit(&passthrough_stress.failures,
                                  memory_order_relaxed),
             passthrough_thread_rc,
             passthrough_join_rc, arm_rc, ready_rc, observed_rc, hook_arm_rc,
             hook_status_before_rc, hook_status_after_rc, hook_clear_rc,
             inspect_rc, clear_rc, cleared_rc,
             (int)g_r0lab_raw_handler_faults, hook_reply,
             hook_status_before, hook_status_after, hook_clear_reply,
             inspect_reply);
    munmap(page, page_size);
    return failures ? -1 : 0;
}

static int r0lab_raw_prctl_patch_records_run(const char *token_text,
                                             char *output,
                                             size_t output_size)
{
    static const uint8_t patch_a[8] =
        {0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18};
    static const uint8_t patch_b[8] =
        {0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28};
    static const uint8_t patch_c[4] = {0x31, 0x32, 0x33, 0x34};
    static const uint8_t expected_after_b[12] =
        {0x11, 0x12, 0x13, 0x14, 0x21, 0x22, 0x23, 0x24,
         0x25, 0x26, 0x27, 0x28};
    static const uint8_t expected_after_release_b[12] =
        {0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
         0x00, 0x00, 0x00, 0x00};
    static const uint8_t expected_after_update_a[12] =
        {0x31, 0x32, 0x33, 0x34, 0x00, 0x00, 0x00, 0x00,
         0x00, 0x00, 0x00, 0x00};
    static const uint8_t expected_original[12] = {0};
    struct sigaction action = {0};
    struct sigaction previous_action = {0};
    struct r0lab_prctl_patch_request request = {0};
    uint8_t snapshot_after_a[12] = {0};
    uint8_t snapshot_after_b[12] = {0};
    uint8_t snapshot_after_release_b[12] = {0};
    uint8_t snapshot_after_update_a[12] = {0};
    uint8_t snapshot_after_invalid[12] = {0};
    uint8_t snapshot_after_release_a[12] = {0};
    uint32_t *code;
    uint64_t token;
    void *page = MAP_FAILED;
    size_t page_size;
    char command[128];
    char reply[256] = {0};
    char hook_reply[1024] = {0};
    char status_a[1024] = {0};
    char status_b[1024] = {0};
    char status_release_b[1024] = {0};
    char status_update_a[1024] = {0};
    char status_final[1024] = {0};
    char status_cleanup[1024] = {0};
    char status_capacity[1024] = {0};
    long arm_rc = -1;
    long ready_rc = -1;
    long observed_rc = -1;
    long hook_arm_rc = -1;
    long apply_a_rc = -1;
    long apply_b_rc = -1;
    long release_b_rc = -1;
    long update_a_rc = -1;
    long invalid_rc = -1;
    long release_a_rc = -1;
    long cleanup_patch_rc = -1;
    long status_a_rc = -1;
    long status_b_rc = -1;
    long status_release_b_rc = -1;
    long status_update_a_rc = -1;
    long status_final_rc = -1;
    long status_cleanup_rc = -1;
    long status_capacity_rc = -1;
    long hook_clear_rc = -1;
    long clear_rc = -1;
    long cleared_rc = -1;
    long capacity_fill_failures = 0;
    long capacity_overflow_rc = -2;
    unsigned int capacity_fill_records = 0;
    unsigned int capacity_index;
    int invalid_errno = 0;
    int capacity_overflow_errno = 0;
    int normal_value = -1;
    int shadow_value = -1;
    int restored_value = -1;
    int handler_installed = 0;
    int failures = 0;

    if (r0lab_parse_token(token_text, &token)) {
        snprintf(output, output_size,
                 "rc=-22 error=invalid raw prctl patch records token");
        return -1;
    }
    page_size = (size_t)sysconf(_SC_PAGESIZE);
    if (page_size != R0LAB_M3_PAGE_SIZE) {
        snprintf(output, output_size,
                 "rc=-38 error=unsupported page size=%zu", page_size);
        return -1;
    }
    page = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) {
        snprintf(output, output_size,
                 "rc=-12 error=raw prctl patch records mmap errno=%d", errno);
        return -1;
    }
    code = page;
    code[0] = R0LAB_M3_CODE_MOV_W0_42;
    code[1] = R0LAB_M3_CODE_RET;
    __builtin___clear_cache((char *)page, (char *)page + page_size);
    if (mprotect(page, page_size, PROT_READ | PROT_EXEC)) {
        snprintf(output, output_size,
                 "rc=-1 error=raw prctl patch records mprotect errno=%d",
                 errno);
        munmap(page, page_size);
        return -1;
    }
    normal_value = ((int (*)(void))page)();

    snprintf(command, sizeof(command), "raw arm 0x%llx 0x%llx",
             (unsigned long long)token, (unsigned long long)(uintptr_t)page);
    arm_rc = r0lab_control_raw(command, reply, sizeof(reply));
    if (arm_rc < 0)
        goto finish;
    ready_rc = r0lab_m3_wait_for("raw ready", token);
    if (ready_rc < 0)
        goto clear;

    g_r0lab_raw_handler_faults = 0;
    g_r0lab_raw_signal_page = page;
    g_r0lab_raw_signal_page_size = page_size;
    action.sa_sigaction = r0lab_raw_signal_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_SIGINFO;
    if (sigaction(SIGSEGV, &action, &previous_action))
        goto clear;
    handler_installed = 1;
    shadow_value = ((int (*)(void))page)();

    snprintf(command, sizeof(command), "raw observed 0x%llx",
             (unsigned long long)token);
    observed_rc = r0lab_control_raw(command, reply, sizeof(reply));
    snprintf(command, sizeof(command), "raw prctl hook arm 0x%llx",
             (unsigned long long)token);
    hook_arm_rc = r0lab_control_raw(command, hook_reply, sizeof(hook_reply));
    if (hook_arm_rc < 0)
        goto clear;

    request.address = (uint64_t)(uintptr_t)((uint8_t *)page + 64);
    request.length = sizeof(patch_a);
    memcpy(request.data, patch_a, sizeof(patch_a));
    apply_a_rc = syscall(__NR_prctl, R0LAB_PRCTL_MAGIC,
                         (unsigned long)token, R0LAB_PRCTL_OP_PATCH_RANGE,
                         (unsigned long)(uintptr_t)&request,
                         sizeof(request.address) + sizeof(request.length) +
                             sizeof(request.flags) + request.length);
    memcpy(snapshot_after_a, (uint8_t *)page + 64, sizeof(snapshot_after_a));
    snprintf(command, sizeof(command), "raw prctl hook status 0x%llx",
             (unsigned long long)token);
    status_a_rc = r0lab_control_raw(command, status_a, sizeof(status_a));

    memset(&request, 0, sizeof(request));
    request.address = (uint64_t)(uintptr_t)((uint8_t *)page + 68);
    request.length = sizeof(patch_b);
    memcpy(request.data, patch_b, sizeof(patch_b));
    apply_b_rc = syscall(__NR_prctl, R0LAB_PRCTL_MAGIC,
                         (unsigned long)token, R0LAB_PRCTL_OP_PATCH_RANGE,
                         (unsigned long)(uintptr_t)&request,
                         sizeof(request.address) + sizeof(request.length) +
                             sizeof(request.flags) + request.length);
    memcpy(snapshot_after_b, (uint8_t *)page + 64, sizeof(snapshot_after_b));
    status_b_rc = r0lab_control_raw(command, status_b, sizeof(status_b));

    release_b_rc = syscall(__NR_prctl, R0LAB_PRCTL_MAGIC,
                           (unsigned long)token,
                           R0LAB_PRCTL_OP_RELEASE_RANGE,
                           (unsigned long)(uintptr_t)((uint8_t *)page + 68), 0);
    memcpy(snapshot_after_release_b, (uint8_t *)page + 64,
           sizeof(snapshot_after_release_b));
    status_release_b_rc =
        r0lab_control_raw(command, status_release_b, sizeof(status_release_b));

    memset(&request, 0, sizeof(request));
    request.address = (uint64_t)(uintptr_t)((uint8_t *)page + 64);
    request.length = sizeof(patch_c);
    memcpy(request.data, patch_c, sizeof(patch_c));
    update_a_rc = syscall(__NR_prctl, R0LAB_PRCTL_MAGIC,
                          (unsigned long)token, R0LAB_PRCTL_OP_PATCH_RANGE,
                          (unsigned long)(uintptr_t)&request,
                          sizeof(request.address) + sizeof(request.length) +
                              sizeof(request.flags) + request.length);
    memcpy(snapshot_after_update_a, (uint8_t *)page + 64,
           sizeof(snapshot_after_update_a));
    status_update_a_rc =
        r0lab_control_raw(command, status_update_a, sizeof(status_update_a));

    memset(&request, 0, sizeof(request));
    request.address = (uint64_t)(uintptr_t)((uint8_t *)page + page_size - 2);
    request.length = sizeof(patch_c);
    memcpy(request.data, patch_c, sizeof(patch_c));
    errno = 0;
    invalid_rc = syscall(__NR_prctl, R0LAB_PRCTL_MAGIC,
                         (unsigned long)token, R0LAB_PRCTL_OP_PATCH_RANGE,
                         (unsigned long)(uintptr_t)&request,
                         sizeof(request.address) + sizeof(request.length) +
                             sizeof(request.flags) + request.length);
    invalid_errno = errno;
    memcpy(snapshot_after_invalid, (uint8_t *)page + 64,
           sizeof(snapshot_after_invalid));

    release_a_rc = syscall(__NR_prctl, R0LAB_PRCTL_MAGIC,
                           (unsigned long)token,
                           R0LAB_PRCTL_OP_RELEASE_RANGE,
                           (unsigned long)(uintptr_t)((uint8_t *)page + 64), 0);
    memcpy(snapshot_after_release_a, (uint8_t *)page + 64,
           sizeof(snapshot_after_release_a));
    status_final_rc =
        r0lab_control_raw(command, status_final, sizeof(status_final));

    if (apply_a_rc || memcmp(snapshot_after_a, patch_a, sizeof(patch_a)) ||
        memcmp(snapshot_after_a + sizeof(patch_a), expected_original, 4) ||
        status_a_rc < 0 || !strstr(status_a, "patch_record_slots=1") ||
        !strstr(status_a, "patch_active_count=1") ||
        !strstr(status_a, "patch_dirty_bytes=8") ||
        apply_b_rc ||
        memcmp(snapshot_after_b, expected_after_b, sizeof(expected_after_b)) ||
        status_b_rc < 0 || !strstr(status_b, "patch_record_slots=2") ||
        !strstr(status_b, "patch_active_count=2") ||
        !strstr(status_b, "patch_dirty_bytes=12") ||
        release_b_rc ||
        memcmp(snapshot_after_release_b, expected_after_release_b,
               sizeof(expected_after_release_b)) ||
        status_release_b_rc < 0 ||
        !strstr(status_release_b, "patch_record_slots=2") ||
        !strstr(status_release_b, "patch_active_count=1") ||
        !strstr(status_release_b, "patch_dirty_bytes=8") ||
        update_a_rc ||
        memcmp(snapshot_after_update_a, expected_after_update_a,
               sizeof(expected_after_update_a)) ||
        status_update_a_rc < 0 ||
        !strstr(status_update_a, "patch_record_slots=2") ||
        !strstr(status_update_a, "patch_active_count=1") ||
        !strstr(status_update_a, "patch_dirty_bytes=4") ||
        invalid_rc != -1 || invalid_errno != EINVAL ||
        memcmp(snapshot_after_invalid, expected_after_update_a,
               sizeof(expected_after_update_a)) ||
        release_a_rc ||
        memcmp(snapshot_after_release_a, expected_original,
               sizeof(snapshot_after_release_a)) ||
        status_final_rc < 0 ||
        !strstr(status_final, "hit_events=5") ||
        !strstr(status_final, "patch_events=3") ||
        !strstr(status_final, "release_events=2") ||
        !strstr(status_final, "failures=1") ||
        !strstr(status_final, "patch_record_slots=2") ||
        !strstr(status_final, "patch_active_count=0") ||
        !strstr(status_final, "patch_dirty_bytes=0") ||
        !strstr(status_final, "patch_capacity=1024") ||
        !strstr(status_final, "overlap=version_last_write_wins") ||
        !strstr(status_final, "rebuild=original_seed_active_records") ||
        !strstr(status_final, "release=exact_start") ||
        !strstr(status_final, "dirty_tracking=byte_bitmap") ||
        !strstr(status_final, "user_copy=copy_from_user_nofault"))
        ++failures;

    memset(&request, 0, sizeof(request));
    request.address = (uint64_t)(uintptr_t)((uint8_t *)page + 80);
    request.length = sizeof(patch_c);
    memcpy(request.data, patch_c, sizeof(patch_c));
    cleanup_patch_rc = syscall(
        __NR_prctl, R0LAB_PRCTL_MAGIC, (unsigned long)token,
        R0LAB_PRCTL_OP_PATCH_RANGE, (unsigned long)(uintptr_t)&request,
        sizeof(request.address) + sizeof(request.length) +
            sizeof(request.flags) + request.length);
    status_cleanup_rc =
        r0lab_control_raw(command, status_cleanup, sizeof(status_cleanup));
    if (cleanup_patch_rc || status_cleanup_rc < 0 ||
        !strstr(status_cleanup, "patch_record_slots=2") ||
        !strstr(status_cleanup, "patch_active_count=1") ||
        !strstr(status_cleanup, "patch_dirty_bytes=4"))
        ++failures;

    for (capacity_index = 0;
         capacity_index < R0LAB_PATCH_RECORD_CAPACITY - 1U;
         ++capacity_index) {
        memset(&request, 0, sizeof(request));
        request.address =
            (uint64_t)(uintptr_t)((uint8_t *)page + 1024U + capacity_index);
        request.length = 1;
        request.data[0] = (uint8_t)(0x80U | (capacity_index & 0x7fU));
        if (syscall(__NR_prctl, R0LAB_PRCTL_MAGIC, (unsigned long)token,
                    R0LAB_PRCTL_OP_PATCH_RANGE,
                    (unsigned long)(uintptr_t)&request,
                    sizeof(request.address) + sizeof(request.length) +
                        sizeof(request.flags) + request.length)) {
            ++capacity_fill_failures;
            break;
        }
        ++capacity_fill_records;
    }
    memset(&request, 0, sizeof(request));
    request.address =
        (uint64_t)(uintptr_t)((uint8_t *)page + 1024U + capacity_fill_records);
    request.length = 1;
    request.data[0] = 0x7fU;
    errno = 0;
    capacity_overflow_rc = syscall(
        __NR_prctl, R0LAB_PRCTL_MAGIC, (unsigned long)token,
        R0LAB_PRCTL_OP_PATCH_RANGE, (unsigned long)(uintptr_t)&request,
        sizeof(request.address) + sizeof(request.length) +
            sizeof(request.flags) + request.length);
    capacity_overflow_errno = errno;
    status_capacity_rc =
        r0lab_control_raw(command, status_capacity, sizeof(status_capacity));
    if (capacity_fill_records != R0LAB_PATCH_RECORD_CAPACITY - 1U ||
        capacity_fill_failures || capacity_overflow_rc != -1 ||
        capacity_overflow_errno != ENOSPC || status_capacity_rc < 0 ||
        !strstr(status_capacity, "patch_record_slots=1024") ||
        !strstr(status_capacity, "patch_active_count=1024") ||
        !strstr(status_capacity, "patch_dirty_bytes=1027") ||
        !strstr(status_capacity, "patch_capacity=1024"))
        ++failures;

    snprintf(command, sizeof(command), "raw prctl hook clear 0x%llx",
             (unsigned long long)token);
    hook_clear_rc = r0lab_control_raw(command, reply, sizeof(reply));

clear:
    r0lab_raw_clear(token, &clear_rc, &cleared_rc);
    if (handler_installed)
        sigaction(SIGSEGV, &previous_action, NULL);
    g_r0lab_raw_signal_page = NULL;
    g_r0lab_raw_signal_page_size = 0;
    if (cleared_rc >= 0)
        restored_value = ((int (*)(void))page)();

finish:
    if (normal_value != 42 || shadow_value != 99 || restored_value != 42 ||
        arm_rc < 0 || ready_rc < 0 || observed_rc < 0 || hook_arm_rc < 0 ||
        apply_a_rc || apply_b_rc || release_b_rc || update_a_rc ||
        invalid_rc != -1 || invalid_errno != EINVAL || release_a_rc ||
        cleanup_patch_rc ||
        status_a_rc < 0 || status_b_rc < 0 || status_release_b_rc < 0 ||
        status_update_a_rc < 0 || status_final_rc < 0 ||
        status_cleanup_rc < 0 || status_capacity_rc < 0 ||
        capacity_fill_records != R0LAB_PATCH_RECORD_CAPACITY - 1U ||
        capacity_fill_failures || capacity_overflow_rc != -1 ||
        capacity_overflow_errno != ENOSPC ||
        hook_clear_rc < 0 || clear_rc < 0 || cleared_rc < 0 ||
        g_r0lab_raw_handler_faults)
        ++failures;
    snprintf(output, output_size,
             "raw mode=prctl-patch-records failures=%d operations=4,5 patch_scope=single_page_ranges patch_capacity=1024 overlap=version_last_write_wins rebuild=original_seed_active_records release=exact_start dirty_tracking=byte_bitmap user_copy=copy_from_user_nofault cache_sync=sync_icache_aliases apply_a_rc=%ld apply_b_rc=%ld release_b_rc=%ld update_a_rc=%ld invalid_rc=%ld invalid_errno=%d release_a_rc=%ld cleanup_patch_rc=%ld capacity_fill_records=%u capacity_fill_failures=%ld capacity_overflow_rc=%ld capacity_overflow_errno=%d capacity_boundary=%s overlap_after_b=%s release_b_rebuild=%s shrink_rebuild=%s invalid_preserved=%s final_original=%s cleanup_active_before_clear=%s active_progress=1,2,1,1,0,1,1024 dirty_progress=8,12,8,4,0,4,1027 slots_progress=1,2,2,2,2,2,1024 normal_value=%d shadow_value=%d restored_value=%d arm_rc=%ld ready_rc=%ld observed_rc=%ld hook_arm_rc=%ld status_a_rc=%ld status_b_rc=%ld status_release_b_rc=%ld status_update_a_rc=%ld status_final_rc=%ld status_cleanup_rc=%ld status_capacity_rc=%ld hook_clear_rc=%ld clear_rc=%ld cleared_rc=%ld handler_faults=%d final_status=\"%s\" cleanup_status=\"%s\"",
             failures, apply_a_rc, apply_b_rc, release_b_rc, update_a_rc,
             invalid_rc, invalid_errno, release_a_rc, cleanup_patch_rc,
             capacity_fill_records, capacity_fill_failures,
             capacity_overflow_rc, capacity_overflow_errno,
             capacity_fill_records == R0LAB_PATCH_RECORD_CAPACITY - 1U &&
                     !capacity_fill_failures && capacity_overflow_rc == -1 &&
                     capacity_overflow_errno == ENOSPC &&
                     status_capacity_rc >= 0 &&
                     strstr(status_capacity, "patch_record_slots=1024") &&
                     strstr(status_capacity, "patch_active_count=1024") &&
                     strstr(status_capacity, "patch_dirty_bytes=1027") ?
                 "pass" : "fail",
             memcmp(snapshot_after_b, expected_after_b,
                    sizeof(expected_after_b)) ? "fail" : "pass",
             memcmp(snapshot_after_release_b, expected_after_release_b,
                    sizeof(expected_after_release_b)) ? "fail" : "pass",
             memcmp(snapshot_after_update_a, expected_after_update_a,
                    sizeof(expected_after_update_a)) ? "fail" : "pass",
             memcmp(snapshot_after_invalid, expected_after_update_a,
                    sizeof(expected_after_update_a)) ? "fail" : "pass",
             memcmp(snapshot_after_release_a, expected_original,
                    sizeof(snapshot_after_release_a)) ? "fail" : "pass",
             status_cleanup_rc < 0 ||
                     !strstr(status_cleanup, "patch_active_count=1") ||
                     !strstr(status_cleanup, "patch_dirty_bytes=4") ?
                 "fail" : "pass",
             normal_value, shadow_value, restored_value, arm_rc, ready_rc,
             observed_rc, hook_arm_rc, status_a_rc, status_b_rc,
             status_release_b_rc, status_update_a_rc, status_final_rc,
             status_cleanup_rc, status_capacity_rc, hook_clear_rc, clear_rc, cleared_rc,
             (int)g_r0lab_raw_handler_faults, status_final, status_cleanup);
    munmap(page, page_size);
    return failures ? -1 : 0;
}

static int r0lab_raw_exit_hook_hold(const char *token_text, char *output,
                                    size_t output_size)
{
    struct sigaction action = {0};
    struct sigaction previous_action = {0};
    uint32_t *code;
    volatile uint32_t *readable_code;
    uint64_t token;
    void *page = MAP_FAILED;
    size_t page_size;
    char command[96];
    char reply[256] = {0};
    char hook_reply[512] = {0};
    char hook_status[512] = {0};
    char inspect_reply[2048] = {0};
    unsigned int activations = 0;
    unsigned long state = 0;
    uint32_t word_before = 0;
    uint32_t word_shadow = 0;
    long arm_rc = -1;
    long ready_rc = -1;
    long observed_rc = -1;
    long hook_arm_rc = -1;
    long hook_status_rc = -1;
    long inspect_rc = -1;
    int normal_value = -1;
    int shadow_value = -1;
    int handler_installed = 0;
    int failures = 0;
    int success = 0;

    if (r0lab_parse_token(token_text, &token)) {
        snprintf(output, output_size,
                 "rc=-22 error=invalid raw exit hook token");
        return -1;
    }
    page_size = (size_t)sysconf(_SC_PAGESIZE);
    if (page_size != R0LAB_M3_PAGE_SIZE) {
        snprintf(output, output_size,
                 "rc=-38 error=unsupported page size=%zu", page_size);
        return -1;
    }
    if (g_r0lab_m5_hold.armed) {
        snprintf(output, output_size, "rc=-16 error=hold already active");
        return -1;
    }
    page = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) {
        snprintf(output, output_size,
                 "rc=-12 error=raw exit hook page allocation errno=%d",
                 errno);
        return -1;
    }
    code = page;
    code[0] = R0LAB_M3_CODE_MOV_W0_42;
    code[1] = R0LAB_M3_CODE_RET;
    __builtin___clear_cache((char *)page, (char *)page + page_size);
    if (mprotect(page, page_size, PROT_READ | PROT_EXEC)) {
        snprintf(output, output_size,
                 "rc=-1 error=initial raw exit hook mprotect errno=%d",
                 errno);
        munmap(page, page_size);
        return -1;
    }

    readable_code = (volatile uint32_t *)page;
    word_before = readable_code[0];
    normal_value = ((int (*)(void))page)();

    snprintf(command, sizeof(command), "raw arm 0x%llx 0x%llx",
             (unsigned long long)token, (unsigned long long)(uintptr_t)page);
    arm_rc = r0lab_control_raw(command, reply, sizeof(reply));
    if (arm_rc < 0)
        goto finish;
    ready_rc = r0lab_m3_wait_for("raw ready", token);
    if (ready_rc < 0)
        goto finish;

    g_r0lab_raw_handler_faults = 0;
    g_r0lab_raw_signal_page = page;
    g_r0lab_raw_signal_page_size = page_size;
    action.sa_sigaction = r0lab_raw_signal_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_SIGINFO;
    if (sigaction(SIGSEGV, &action, &previous_action))
        goto finish;
    handler_installed = 1;

    shadow_value = ((int (*)(void))page)();
    word_shadow = readable_code[0];

    snprintf(command, sizeof(command), "raw observed 0x%llx",
             (unsigned long long)token);
    observed_rc = r0lab_control_raw(command, reply, sizeof(reply));
    if (observed_rc < 0 ||
        sscanf(reply, "raw_observed activations=%u state=%lu",
               &activations, &state) != 2)
        ++failures;

    if (handler_installed)
        sigaction(SIGSEGV, &previous_action, NULL);
    handler_installed = 0;
    g_r0lab_raw_signal_page = NULL;
    g_r0lab_raw_signal_page_size = 0;

    snprintf(command, sizeof(command), "raw exit hook arm 0x%llx",
             (unsigned long long)token);
    hook_arm_rc = r0lab_control_raw(command, hook_reply, sizeof(hook_reply));

    snprintf(command, sizeof(command), "raw exit hook status 0x%llx",
             (unsigned long long)token);
    hook_status_rc = r0lab_control_raw(command, hook_status,
                                       sizeof(hook_status));

    snprintf(command, sizeof(command), "raw inspect 0x%llx",
             (unsigned long long)token);
    inspect_rc = r0lab_control_raw(command, inspect_reply,
                                   sizeof(inspect_reply));
    if (normal_value != 42 || shadow_value != 99 ||
        word_before != R0LAB_M3_CODE_MOV_W0_42 ||
        word_shadow != R0LAB_M4_CODE_MOV_W0_99 ||
        arm_rc < 0 || ready_rc < 0 || observed_rc < 0 ||
        hook_arm_rc < 0 || hook_status_rc < 0 || inspect_rc < 0 ||
        activations != 1 || state != 3 || g_r0lab_raw_handler_faults ||
        !strstr(hook_reply, "raw_exit_hook_ready") ||
        !strstr(hook_reply, "symbol=exit_mmap") ||
        !strstr(hook_reply, "target_mm_scoped=1") ||
        !strstr(hook_reply, "observe_only=1") ||
        !strstr(hook_reply, "pte_switch=0") ||
        !strstr(hook_reply, "cleanup=monitor") ||
        !strstr(hook_status, "installed=1") ||
        !strstr(hook_status, "hit_events=0") ||
        !strstr(hook_status, "failures=0") ||
        !strstr(inspect_reply, "active_kind=shadow_rx") ||
        !strstr(inspect_reply, "exit_hook_installed=1"))
        ++failures;

    if (!failures) {
        g_r0lab_m5_hold.source_page = page;
        g_r0lab_m5_hold.clone_page = NULL;
        g_r0lab_m5_hold.page_size = page_size;
        g_r0lab_m5_hold.token = token;
        g_r0lab_m5_hold.mode = 8;
        g_r0lab_m5_hold.armed = 1;
        success = 1;
    }

finish:
    if (handler_installed)
        sigaction(SIGSEGV, &previous_action, NULL);
    g_r0lab_raw_signal_page = NULL;
    g_r0lab_raw_signal_page_size = 0;
    if (!success && arm_rc >= 0) {
        long clear_rc = -1;
        long cleared_rc = -1;

        snprintf(command, sizeof(command), "raw exit hook clear 0x%llx",
                 (unsigned long long)token);
        (void)r0lab_control_raw(command, reply, sizeof(reply));
        r0lab_raw_clear(token, &clear_rc, &cleared_rc);
    }
    if (!success)
        munmap(page, page_size);
    snprintf(output, output_size,
             "raw mode=exit-hook-hold failures=%d observe_only=1 target_mm_scoped=1 pte_switch=0 cleanup=monitor normal_value=%d shadow_value=%d words=%08x/%08x activations=%u state=%lu arm_rc=%ld ready_rc=%ld observed_rc=%ld hook_arm_rc=%ld hook_status_rc=%ld inspect_rc=%ld handler_faults=%d source=%llx hook=\"%s\" status=\"%s\" inspect=\"%s\"",
             failures, normal_value, shadow_value, word_before, word_shadow,
             activations, state, arm_rc, ready_rc, observed_rc, hook_arm_rc,
             hook_status_rc, inspect_rc, (int)g_r0lab_raw_handler_faults,
             (unsigned long long)(uintptr_t)page, hook_reply, hook_status,
             inspect_reply);
    return success ? 0 : -1;
}

static int r0lab_raw_fault_data_probe_run(const char *token_text,
                                          char *output, size_t output_size)
{
    struct sigaction action = {0};
    struct sigaction previous_action = {0};
    uint32_t *code;
    volatile uint32_t *readable_code;
    volatile uint32_t *probe_words;
    uint64_t token;
    void *page = MAP_FAILED;
    void *probe_page = MAP_FAILED;
    size_t page_size;
    char command[128];
    char reply[256] = {0};
    char hook_reply[512] = {0};
    char hook_status[512] = {0};
    char probe_reply[512] = {0};
    char probe_reader_reply[512] = {0};
    char probe_status[512] = {0};
    char probe_clear_reply[512] = {0};
    char hook_clear_reply[512] = {0};
    char inspect_reply[2048] = {0};
    char reader_tgid_text[64] = {0};
    unsigned int activations = 0;
    unsigned long state = 0;
    uint32_t word_before = 0;
    uint32_t word_shadow = 0;
    uint32_t word_after_clear = 0;
    uint32_t probe_word_before = 0;
    uint32_t probe_word_remote = 0;
    long arm_rc = -1;
    long ready_rc = -1;
    long observed_rc = -1;
    long hook_arm_rc = -1;
    long probe_arm_rc = -1;
    long probe_reader_rc = -1;
    long madvise_rc = -1;
    long hook_status_rc = -1;
    long probe_status_rc = -1;
    long probe_clear_rc = -1;
    long hook_clear_rc = -1;
    long inspect_rc = -1;
    long clear_rc = -1;
    long cleared_rc = -1;
    int madvise_errno = 0;
    int normal_value = -1;
    int shadow_value = -1;
    int restored_value = -1;
    int handler_installed = 0;
    int ready_pipe[2] = {-1, -1};
    int start_pipe[2] = {-1, -1};
    int result_pipe[2] = {-1, -1};
    pid_t child = -1;
    pid_t waited = -1;
    int child_status = -1;
    int child_exit = -1;
    ssize_t ready_read = -1;
    ssize_t start_write = -1;
    ssize_t result_read = -1;
    int pipe_errno = 0;
    struct r0lab_gup_reader_result reader_result = {
        .rc = -1,
        .err = 0,
        .word = 0,
    };
    int failures = 0;

    if (r0lab_parse_token(token_text, &token)) {
        snprintf(output, output_size,
                 "rc=-22 error=invalid raw fault data probe token");
        return -1;
    }
    page_size = (size_t)sysconf(_SC_PAGESIZE);
    if (page_size != R0LAB_M3_PAGE_SIZE) {
        snprintf(output, output_size,
                 "rc=-38 error=unsupported page size=%zu", page_size);
        return -1;
    }
    page = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) {
        snprintf(output, output_size,
                 "rc=-12 error=raw fault data probe page allocation errno=%d",
                 errno);
        return -1;
    }
    code = page;
    code[0] = R0LAB_M3_CODE_MOV_W0_42;
    code[1] = R0LAB_M3_CODE_RET;
    __builtin___clear_cache((char *)page, (char *)page + page_size);
    if (mprotect(page, page_size, PROT_READ | PROT_EXEC)) {
        snprintf(output, output_size,
                 "rc=-1 error=initial raw fault data probe mprotect errno=%d",
                 errno);
        munmap(page, page_size);
        return -1;
    }
    probe_page = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (probe_page == MAP_FAILED) {
        snprintf(output, output_size,
                 "rc=-12 error=normal probe page allocation errno=%d", errno);
        munmap(page, page_size);
        return -1;
    }

    readable_code = (volatile uint32_t *)page;
    probe_words = (volatile uint32_t *)probe_page;
    probe_words[0] = R0LAB_FAULT_PROBE_WORD;
    probe_word_before = probe_words[0];
    word_before = readable_code[0];
    normal_value = ((int (*)(void))page)();

    snprintf(command, sizeof(command), "raw arm 0x%llx 0x%llx",
             (unsigned long long)token, (unsigned long long)(uintptr_t)page);
    arm_rc = r0lab_control_raw(command, reply, sizeof(reply));
    if (arm_rc < 0)
        goto finish;
    ready_rc = r0lab_m3_wait_for("raw ready", token);
    if (ready_rc < 0)
        goto clear;

    g_r0lab_raw_handler_faults = 0;
    g_r0lab_raw_signal_page = page;
    g_r0lab_raw_signal_page_size = page_size;
    action.sa_sigaction = r0lab_raw_signal_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_SIGINFO;
    if (sigaction(SIGSEGV, &action, &previous_action))
        goto clear;
    handler_installed = 1;

    shadow_value = ((int (*)(void))page)();
    word_shadow = readable_code[0];

    snprintf(command, sizeof(command), "raw observed 0x%llx",
             (unsigned long long)token);
    observed_rc = r0lab_control_raw(command, reply, sizeof(reply));
    if (observed_rc < 0 ||
        sscanf(reply, "raw_observed activations=%u state=%lu",
               &activations, &state) != 2)
        ++failures;

    snprintf(command, sizeof(command), "raw fault hook arm 0x%llx",
             (unsigned long long)token);
    hook_arm_rc = r0lab_control_raw(command, hook_reply, sizeof(hook_reply));
    if (hook_arm_rc >= 0) {
        snprintf(command, sizeof(command),
                 "raw fault probe arm 0x%llx 0x%llx",
                 (unsigned long long)token,
                 (unsigned long long)(uintptr_t)probe_page);
        probe_arm_rc = r0lab_control_raw(command, probe_reply,
                                         sizeof(probe_reply));
    }
    if (probe_arm_rc >= 0) {
        errno = 0;
        madvise_rc = madvise(probe_page, page_size, MADV_DONTNEED);
        madvise_errno = errno;
        if (!madvise_rc) {
            if (pipe(ready_pipe) || pipe(start_pipe) || pipe(result_pipe)) {
                pipe_errno = errno;
            } else {
                pid_t parent_pid = getpid();

                child = fork();
                if (child == 0) {
                    struct r0lab_gup_reader_result child_result = {
                        .rc = -1,
                        .err = 0,
                        .word = 0,
                    };
                    uint8_t sync_byte = 0x52;
                    int child_exit_code = 64;

                    close(ready_pipe[0]);
                    close(start_pipe[1]);
                    close(result_pipe[0]);
                    if (write(ready_pipe[1], &sync_byte,
                              sizeof(sync_byte)) !=
                        (ssize_t)sizeof(sync_byte)) {
                        child_result.err = errno;
                        child_exit_code = 65;
                    } else if (read(start_pipe[0], &sync_byte,
                                    sizeof(sync_byte)) !=
                               (ssize_t)sizeof(sync_byte)) {
                        child_result.err = errno ? errno : EPIPE;
                        child_exit_code = 66;
                    } else {
#ifdef __NR_process_vm_readv
                        struct iovec local_iov = {
                            &child_result.word,
                            sizeof(child_result.word),
                        };
                        struct iovec remote_iov = {
                            probe_page,
                            sizeof(child_result.word),
                        };

                        errno = 0;
                        child_result.rc =
                            (int32_t)syscall(__NR_process_vm_readv, parent_pid,
                                             &local_iov, 1, &remote_iov, 1, 0);
                        child_result.err = errno;
                        child_exit_code =
                            child_result.rc ==
                                    (int32_t)sizeof(child_result.word)
                                ? 0
                                : 64;
#else
                        child_result.err = ENOSYS;
                        child_exit_code = 64;
#endif
                    }
                    (void)write(result_pipe[1], &child_result,
                                sizeof(child_result));
                    close(ready_pipe[1]);
                    close(start_pipe[0]);
                    close(result_pipe[1]);
                    _exit(child_exit_code);
                }
                if (child > 0) {
                    uint8_t sync_byte = 0;

                    close(ready_pipe[1]);
                    ready_pipe[1] = -1;
                    close(start_pipe[0]);
                    start_pipe[0] = -1;
                    close(result_pipe[1]);
                    result_pipe[1] = -1;
                    ready_read = read(ready_pipe[0], &sync_byte,
                                      sizeof(sync_byte));
                    close(ready_pipe[0]);
                    ready_pipe[0] = -1;
                    if (ready_read == (ssize_t)sizeof(sync_byte)) {
                        snprintf(command, sizeof(command),
                                 "raw fault probe reader 0x%llx %d",
                                 (unsigned long long)token, child);
                        probe_reader_rc = r0lab_control_raw(
                            command, probe_reader_reply,
                            sizeof(probe_reader_reply));
                        snprintf(reader_tgid_text, sizeof(reader_tgid_text),
                                 "reader_tgid=%d", child);
                    }
                    sync_byte = 0x24;
                    start_write = write(start_pipe[1], &sync_byte,
                                        sizeof(sync_byte));
                    close(start_pipe[1]);
                    start_pipe[1] = -1;
                    result_read = read(result_pipe[0], &reader_result,
                                       sizeof(reader_result));
                    close(result_pipe[0]);
                    result_pipe[0] = -1;
                    waited = waitpid(child, &child_status, 0);
                    if (waited == child && WIFEXITED(child_status))
                        child_exit = WEXITSTATUS(child_status);
                    probe_word_remote = reader_result.word;
                } else {
                    pipe_errno = errno;
                }
            }
        }
    }
    if (ready_pipe[0] >= 0)
        close(ready_pipe[0]);
    if (ready_pipe[1] >= 0)
        close(ready_pipe[1]);
    if (start_pipe[0] >= 0)
        close(start_pipe[0]);
    if (start_pipe[1] >= 0)
        close(start_pipe[1]);
    if (result_pipe[0] >= 0)
        close(result_pipe[0]);
    if (result_pipe[1] >= 0)
        close(result_pipe[1]);

    snprintf(command, sizeof(command), "raw fault hook status 0x%llx",
             (unsigned long long)token);
    hook_status_rc = r0lab_control_raw(command, hook_status,
                                       sizeof(hook_status));
    snprintf(command, sizeof(command), "raw fault probe status 0x%llx",
             (unsigned long long)token);
    probe_status_rc = r0lab_control_raw(command, probe_status,
                                        sizeof(probe_status));
    snprintf(command, sizeof(command), "raw inspect 0x%llx",
             (unsigned long long)token);
    inspect_rc = r0lab_control_raw(command, inspect_reply,
                                   sizeof(inspect_reply));
    if (hook_arm_rc < 0 || probe_arm_rc < 0 || madvise_rc < 0 ||
        hook_status_rc < 0 || probe_status_rc < 0 || inspect_rc < 0 ||
        !strstr(hook_reply, "raw_fault_hook_ready") ||
        !strstr(hook_reply, "symbol=handle_mm_fault") ||
        !strstr(hook_reply, "target_mm_scoped=1") ||
        !strstr(hook_reply, "observe_only=1") ||
        !strstr(hook_reply, "pte_switch=0") ||
        !strstr(probe_reply, "raw_fault_probe_ready") ||
        !strstr(probe_reply, "source=normal_anon_remote_gup") ||
        !strstr(probe_reader_reply, "raw_fault_probe_reader_ready") ||
        !strstr(probe_reader_reply, "remote_only=1") ||
        (child > 0 && !strstr(probe_reader_reply, reader_tgid_text)) ||
        !strstr(hook_status, "installed=1") ||
        !strstr(hook_status, "hit_events=0") ||
        !strstr(hook_status, "failures=0") ||
        !strstr(probe_status, "installed=1") ||
        !strstr(probe_status, "armed=1") ||
        (child > 0 && !strstr(probe_status, reader_tgid_text)) ||
        !strstr(probe_status, "read_events=1") ||
        !strstr(probe_status, "write_events=0") ||
        !strstr(probe_status, "exec_events=0") ||
        !strstr(probe_status, "hit_events=1") ||
        !strstr(probe_status, "failures=0") ||
        !strstr(inspect_reply, "fault_hook_installed=1") ||
        !strstr(inspect_reply, "fault_hook_read_events=0") ||
        !strstr(inspect_reply, "fault_probe_armed=1") ||
        (child > 0 && !strstr(inspect_reply, reader_tgid_text)) ||
        !strstr(inspect_reply, "fault_probe_read_events=1") ||
        !strstr(inspect_reply, "fault_probe_write_events=0") ||
        !strstr(inspect_reply, "fault_probe_exec_events=0") ||
        !strstr(inspect_reply, "fault_probe=normal_anon_remote_gup") ||
        !strstr(inspect_reply, "fault_hook=observe_only"))
        ++failures;

    snprintf(command, sizeof(command), "raw fault probe clear 0x%llx",
             (unsigned long long)token);
    probe_clear_rc = r0lab_control_raw(command, probe_clear_reply,
                                       sizeof(probe_clear_reply));
    snprintf(command, sizeof(command), "raw fault hook clear 0x%llx",
             (unsigned long long)token);
    hook_clear_rc = r0lab_control_raw(command, hook_clear_reply,
                                      sizeof(hook_clear_reply));

clear:
    r0lab_raw_clear(token, &clear_rc, &cleared_rc);
    if (handler_installed)
        sigaction(SIGSEGV, &previous_action, NULL);
    g_r0lab_raw_signal_page = NULL;
    g_r0lab_raw_signal_page_size = 0;
    if (cleared_rc >= 0) {
        word_after_clear = readable_code[0];
        restored_value = ((int (*)(void))page)();
    }

finish:
    if (normal_value != 42 || shadow_value != 99 || restored_value != 42 ||
        word_before != R0LAB_M3_CODE_MOV_W0_42 ||
        word_shadow != R0LAB_M4_CODE_MOV_W0_99 ||
        word_after_clear != R0LAB_M3_CODE_MOV_W0_42 ||
        probe_word_before != R0LAB_FAULT_PROBE_WORD ||
        arm_rc < 0 || ready_rc < 0 || observed_rc < 0 ||
        hook_arm_rc < 0 || probe_arm_rc < 0 || probe_reader_rc < 0 ||
        madvise_rc < 0 || child <= 0 ||
        ready_read != (ssize_t)sizeof(uint8_t) ||
        start_write != (ssize_t)sizeof(uint8_t) ||
        result_read != (ssize_t)sizeof(reader_result) ||
        waited != child || child_exit != 0 ||
        reader_result.rc != (int32_t)sizeof(reader_result.word) ||
        reader_result.err != 0 ||
        hook_status_rc < 0 || probe_status_rc < 0 || probe_clear_rc < 0 ||
        hook_clear_rc < 0 || inspect_rc < 0 || clear_rc < 0 ||
        cleared_rc < 0 || activations != 1 || state != 3 ||
        g_r0lab_raw_handler_faults)
        ++failures;
    snprintf(output, output_size,
             "raw mode=fault-data-probe failures=%d observe_only=1 target_mm_scoped=1 remote_only=1 pte_switch=0 data_fault_source=normal_anon_remote_gup normal_value=%d shadow_value=%d restored_value=%d raw_words=%08x/%08x/%08x probe_words=%08x/%08x madvise_rc=%ld madvise_errno=%d reader_tgid=%d reader_rc=%d reader_errno=%d reader_word=%08x child_exit=%d ready_read=%zd start_write=%zd result_read=%zd pipe_errno=%d activations=%u state=%lu arm_rc=%ld ready_rc=%ld observed_rc=%ld hook_arm_rc=%ld probe_arm_rc=%ld probe_reader_rc=%ld hook_status_rc=%ld probe_status_rc=%ld probe_clear_rc=%ld hook_clear_rc=%ld inspect_rc=%ld clear_rc=%ld cleared_rc=%ld handler_faults=%d hook=\"%s\" probe=\"%s\" reader=\"%s\" status=\"%s\" probe_status=\"%s\" probe_clear=\"%s\" hook_clear=\"%s\" inspect=\"%s\"",
             failures, normal_value, shadow_value, restored_value, word_before,
             word_shadow, word_after_clear, probe_word_before,
             probe_word_remote, madvise_rc, madvise_errno, child,
             reader_result.rc, reader_result.err, reader_result.word,
             child_exit, ready_read, start_write, result_read, pipe_errno,
             activations, state, arm_rc, ready_rc, observed_rc, hook_arm_rc,
             probe_arm_rc, probe_reader_rc, hook_status_rc,
             probe_status_rc, probe_clear_rc, hook_clear_rc, inspect_rc,
             clear_rc, cleared_rc, (int)g_r0lab_raw_handler_faults,
             hook_reply, probe_reply, probe_reader_reply, hook_status,
             probe_status, probe_clear_reply, hook_clear_reply, inspect_reply);
    munmap(probe_page, page_size);
    munmap(page, page_size);
    return failures ? -1 : 0;
}

static int r0lab_raw_xom_run(const char *token_text, char *output,
                             size_t output_size)
{
    struct sigaction action = {0};
    struct sigaction previous_action = {0};
    volatile uint32_t *readable_code;
    uint32_t *code;
    uint64_t token;
    void *page = MAP_FAILED;
    size_t page_size;
    char command[96];
    char reply[256] = {0};
    char inspect_reply[512] = {0};
    unsigned int activations = 0;
    unsigned long state = 0;
    uint32_t word_before = 0;
    uint32_t word_during = 0;
    uint32_t word_after_clear = 0;
    uint32_t read_after_xom = 0;
    long arm_rc = -1;
    long ready_rc = -1;
    long observed_rc = -1;
    long inspect_rc = -1;
    long clear_rc = -1;
    long cleared_rc = -1;
    int normal_value = -1;
    int shadow_value = -1;
    int restored_value = -1;
    int handler_installed = 0;
    int admission_proven = 0;
    int failures = 0;

    if (r0lab_parse_token(token_text, &token)) {
        snprintf(output, output_size, "rc=-22 error=invalid raw xom token");
        return -1;
    }
    page_size = (size_t)sysconf(_SC_PAGESIZE);
    if (page_size != R0LAB_M3_PAGE_SIZE) {
        snprintf(output, output_size,
                 "rc=-38 error=unsupported page size=%zu", page_size);
        return -1;
    }
    page = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) {
        snprintf(output, output_size,
                 "rc=-12 error=raw xom page allocation errno=%d", errno);
        return -1;
    }
    code = page;
    code[0] = R0LAB_M3_CODE_MOV_W0_42;
    code[1] = R0LAB_M3_CODE_RET;
    __builtin___clear_cache((char *)page, (char *)page + page_size);
    if (mprotect(page, page_size, PROT_READ | PROT_EXEC)) {
        snprintf(output, output_size,
                 "rc=-1 error=initial raw xom mprotect errno=%d", errno);
        munmap(page, page_size);
        return -1;
    }

    readable_code = (volatile uint32_t *)page;
    word_before = readable_code[0];
    normal_value = ((int (*)(void))page)();

    snprintf(command, sizeof(command), "raw xom arm 0x%llx 0x%llx",
             (unsigned long long)token, (unsigned long long)(uintptr_t)page);
    arm_rc = r0lab_control_raw(command, reply, sizeof(reply));
    if (arm_rc < 0)
        goto finish;
    ready_rc = r0lab_m3_wait_for("raw ready", token);
    if (ready_rc < 0)
        goto clear;
    word_during = readable_code[0];

    g_r0lab_xom_page = page;
    g_r0lab_xom_page_size = page_size;
    g_r0lab_xom_stage = 0;
    g_r0lab_xom_exec_faults = 0;
    g_r0lab_xom_read_faults = 0;
    action.sa_sigaction = r0lab_xom_signal_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_SIGINFO;
    if (sigaction(SIGSEGV, &action, &previous_action))
        goto clear;
    handler_installed = 1;

    if (!sigsetjmp(g_r0lab_xom_jump, 1)) {
        g_r0lab_xom_stage = 1;
        shadow_value = ((int (*)(void))page)();
    }
    if (!sigsetjmp(g_r0lab_xom_jump, 1)) {
        g_r0lab_xom_stage = 2;
        read_after_xom = readable_code[0];
    }
    g_r0lab_xom_stage = 0;

    memset(reply, 0, sizeof(reply));
    snprintf(command, sizeof(command), "raw observed 0x%llx",
             (unsigned long long)token);
    observed_rc = r0lab_control_raw(command, reply, sizeof(reply));
    if (observed_rc < 0 ||
        sscanf(reply, "raw_observed activations=%u state=%lu",
               &activations, &state) != 2)
        ++failures;

    snprintf(command, sizeof(command), "raw inspect 0x%llx",
             (unsigned long long)token);
    inspect_rc = r0lab_control_raw(command, inspect_reply,
                                   sizeof(inspect_reply));
    admission_proven = shadow_value == 99 &&
        g_r0lab_xom_exec_faults == 0 &&
        g_r0lab_xom_read_faults == 1 &&
        inspect_rc >= 0 &&
        strstr(inspect_reply, "active_kind=shadow_xom") != NULL;

clear:
    r0lab_raw_clear(token, &clear_rc, &cleared_rc);
    if (handler_installed)
        sigaction(SIGSEGV, &previous_action, NULL);
    g_r0lab_xom_page = NULL;
    g_r0lab_xom_page_size = 0;
    g_r0lab_xom_stage = 0;
    if (cleared_rc >= 0) {
        word_after_clear = readable_code[0];
        restored_value = ((int (*)(void))page)();
    }

finish:
    if (normal_value != 42 || restored_value != 42 ||
        word_before != R0LAB_M3_CODE_MOV_W0_42 ||
        word_during != R0LAB_M3_CODE_MOV_W0_42 ||
        word_after_clear != R0LAB_M3_CODE_MOV_W0_42 ||
        arm_rc < 0 || ready_rc < 0 || observed_rc < 0 || inspect_rc < 0 ||
        clear_rc < 0 || cleared_rc < 0 || activations != 1 ||
        restored_value != 42)
        ++failures;
    snprintf(output, output_size,
             "raw mode=xom-admission failures=%d admission=%s normal_value=%d shadow_value=%d restored_value=%d activations=%u state=%lu exec_faults=%d read_faults=%d read_after_xom=%08x words=%08x/%08x/%08x arm_rc=%ld ready_rc=%ld observed_rc=%ld inspect_rc=%ld clear_rc=%ld cleared_rc=%ld inspect=\"%s\"",
             failures, admission_proven ? "proven" : "blocked",
             normal_value, shadow_value, restored_value, activations, state,
             (int)g_r0lab_xom_exec_faults, (int)g_r0lab_xom_read_faults,
             read_after_xom, word_before, word_during, word_after_clear,
             arm_rc, ready_rc, observed_rc, inspect_rc, clear_rc, cleared_rc,
             inspect_reply);
    munmap(page, page_size);
    return failures ? -1 : 0;
}

static int r0lab_m5_map_code_page(void **page_out, uint32_t first_word)
{
    uint32_t *code;
    void *page;
    size_t page_size = (size_t)sysconf(_SC_PAGESIZE);

    if (page_size != R0LAB_M3_PAGE_SIZE)
        return -1;
    page = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED)
        return -1;
    code = page;
    code[0] = first_word;
    code[1] = R0LAB_M3_CODE_RET;
    __builtin___clear_cache((char *)page, (char *)page + page_size);
    if (mprotect(page, page_size, PROT_READ | PROT_EXEC)) {
        munmap(page, page_size);
        return -1;
    }
    *page_out = page;
    return 0;
}

static int r0lab_s4_map_brk_page(void **page_out)
{
    uint32_t *code;
    void *page;
    size_t page_size = (size_t)sysconf(_SC_PAGESIZE);

    if (page_size != R0LAB_M3_PAGE_SIZE)
        return -1;
    page = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED)
        return -1;
    code = page;
    code[0] = R0LAB_S4_CODE_BRK_7;
    code[1] = R0LAB_M3_CODE_MOV_W0_42;
    code[2] = R0LAB_M3_CODE_RET;
    __builtin___clear_cache((char *)page, (char *)page + page_size);
    if (mprotect(page, page_size, PROT_READ | PROT_EXEC)) {
        munmap(page, page_size);
        return -1;
    }
    *page_out = page;
    return 0;
}

static int r0lab_s4_map_raw_step_page(void **page_out)
{
    uint32_t *code;
    void *page;
    size_t page_size = (size_t)sysconf(_SC_PAGESIZE);

    if (page_size != R0LAB_M3_PAGE_SIZE)
        return -1;
    page = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED)
        return -1;
    code = page;
    code[0] = R0LAB_M3_CODE_MOV_W0_42;
    code[1] = R0LAB_M3_CODE_MOV_W0_42;
    code[2] = R0LAB_M3_CODE_RET;
    __builtin___clear_cache((char *)page, (char *)page + page_size);
    if (mprotect(page, page_size, PROT_READ | PROT_EXEC)) {
        munmap(page, page_size);
        return -1;
    }
    *page_out = page;
    return 0;
}

static void r0lab_s4_clear(uint64_t token, long *clear_rc, long *cleared_rc)
{
    char command[96];
    char reply[128] = {0};

    *clear_rc = -1;
    *cleared_rc = -1;
    snprintf(command, sizeof(command), "s4 brk clear 0x%llx",
             (unsigned long long)token);
    *clear_rc = r0lab_control_raw(command, reply, sizeof(reply));
    if (*clear_rc >= 0)
        *cleared_rc = r0lab_wait_for_reply("s4 brk cleared", token, reply,
                                           sizeof(reply));
}

static int r0lab_s4_brk_run(const char *token_text, char *output,
                            size_t output_size)
{
    struct sigaction action = {0};
    struct sigaction previous_action = {0};
    uint64_t token;
    void *page = MAP_FAILED;
    char command[128];
    char arm_reply[192] = {0};
    char observed_reply[192] = {0};
    long arm_rc = -1;
    long observed_rc = -1;
    long clear_rc = -1;
    long cleared_rc = -1;
    int handler_installed = 0;
    int value = -1;
    int failures = 0;
    size_t page_size = (size_t)sysconf(_SC_PAGESIZE);

    if (r0lab_parse_token(token_text, &token) || page_size != R0LAB_M3_PAGE_SIZE) {
        snprintf(output, output_size, "rc=-22 error=invalid S4 BRK request");
        return -1;
    }
    if (r0lab_s4_map_brk_page(&page)) {
        snprintf(output, output_size, "rc=-12 error=s4 brk page allocation errno=%d",
                 errno);
        return -1;
    }

    g_r0lab_s4_brk_page = page;
    g_r0lab_s4_brk_traps = 0;
    g_r0lab_s4_step_traps = 0;
    g_r0lab_s4_brk_unexpected = 0;
    g_r0lab_s4_last_signal = 0;
    g_r0lab_s4_last_si_code = 0;
    g_r0lab_s4_last_pc = 0;
    action.sa_sigaction = r0lab_s4_brk_signal_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_SIGINFO;
    if (!sigaction(SIGTRAP, &action, &previous_action))
        handler_installed = 1;
    if (!handler_installed) {
        failures = 1;
        goto done;
    }

    snprintf(command, sizeof(command), "s4 brk arm 0x%llx 0x%llx",
             (unsigned long long)token, (unsigned long long)(uintptr_t)page);
    arm_rc = r0lab_control_raw(command, arm_reply, sizeof(arm_reply));
    if (arm_rc < 0) {
        failures = 1;
        goto done;
    }

    g_r0lab_s4_stage = 1;
    if (!sigsetjmp(g_r0lab_s4_jump, 1))
        value = ((int (*)(void))page)();
    else
        failures = 1;
    g_r0lab_s4_stage = 0;

    snprintf(command, sizeof(command), "s4 brk observed 0x%llx",
             (unsigned long long)token);
    observed_rc = r0lab_control_raw(command, observed_reply,
                                    sizeof(observed_reply));
    r0lab_s4_clear(token, &clear_rc, &cleared_rc);

    if (value != 42 || g_r0lab_s4_brk_traps != 1 ||
        g_r0lab_s4_step_traps != 0 ||
        g_r0lab_s4_brk_unexpected != 0 || observed_rc < 0 ||
        !strstr(observed_reply, "brk_events=1") ||
        !strstr(observed_reply, "state=brk_observed") ||
        !strstr(observed_reply, "skip_origin=0") ||
        !strstr(observed_reply, "single_step=0") ||
        clear_rc < 0 || cleared_rc < 0)
        failures = 1;

done:
    if (arm_rc >= 0 && clear_rc < 0)
        r0lab_s4_clear(token, &clear_rc, &cleared_rc);
    if (handler_installed)
        sigaction(SIGTRAP, &previous_action, NULL);
    g_r0lab_s4_brk_page = NULL;
    if (page != MAP_FAILED)
        munmap(page, page_size);
    snprintf(output, output_size,
             "s4 mode=brk-only failures=%d value=%d traps=%d step_traps=%d unexpected=%d last_pc=%llx last_signal=%d last_si_code=%d arm_rc=%ld observed_rc=%ld clear_rc=%ld cleared_rc=%ld target=%llx arm=\"%s\" observed=\"%s\"",
             failures, value, (int)g_r0lab_s4_brk_traps,
             (int)g_r0lab_s4_step_traps, (int)g_r0lab_s4_brk_unexpected,
             (unsigned long long)g_r0lab_s4_last_pc,
             (int)g_r0lab_s4_last_signal, (int)g_r0lab_s4_last_si_code,
             arm_rc, observed_rc, clear_rc, cleared_rc,
             (unsigned long long)(uintptr_t)page, arm_reply, observed_reply);
    return failures ? -1 : 0;
}

static int r0lab_s4_step_run(const char *token_text, char *output,
                             size_t output_size)
{
    struct sigaction action = {0};
    struct sigaction previous_action = {0};
    uint64_t token;
    void *page = MAP_FAILED;
    char command[128];
    char arm_reply[192] = {0};
    char observed_reply[256] = {0};
    long arm_rc = -1;
    long observed_rc = -1;
    long clear_rc = -1;
    long cleared_rc = -1;
    int handler_installed = 0;
    int value = -1;
    int failures = 0;
    size_t page_size = (size_t)sysconf(_SC_PAGESIZE);

    if (r0lab_parse_token(token_text, &token) || page_size != R0LAB_M3_PAGE_SIZE) {
        snprintf(output, output_size, "rc=-22 error=invalid S4 step request");
        return -1;
    }
    if (r0lab_s4_map_brk_page(&page)) {
        snprintf(output, output_size, "rc=-12 error=s4 step page allocation errno=%d",
                 errno);
        return -1;
    }

    g_r0lab_s4_brk_page = page;
    g_r0lab_s4_brk_traps = 0;
    g_r0lab_s4_step_traps = 0;
    g_r0lab_s4_brk_unexpected = 0;
    g_r0lab_s4_last_signal = 0;
    g_r0lab_s4_last_si_code = 0;
    g_r0lab_s4_last_pc = 0;
    action.sa_sigaction = r0lab_s4_brk_signal_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_SIGINFO;
    if (!sigaction(SIGTRAP, &action, &previous_action))
        handler_installed = 1;
    if (!handler_installed) {
        failures = 1;
        goto done;
    }

    snprintf(command, sizeof(command), "s4 step arm 0x%llx 0x%llx",
             (unsigned long long)token, (unsigned long long)(uintptr_t)page);
    arm_rc = r0lab_control_raw(command, arm_reply, sizeof(arm_reply));
    if (arm_rc < 0) {
        failures = 1;
        goto done;
    }

    g_r0lab_s4_stage = 1;
    if (!sigsetjmp(g_r0lab_s4_jump, 1))
        value = ((int (*)(void))page)();
    else
        failures = 1;
    g_r0lab_s4_stage = 0;

    snprintf(command, sizeof(command), "s4 step observed 0x%llx",
             (unsigned long long)token);
    observed_rc = r0lab_control_raw(command, observed_reply,
                                    sizeof(observed_reply));
    r0lab_s4_clear(token, &clear_rc, &cleared_rc);

    if (value != 42 || g_r0lab_s4_brk_traps != 0 ||
        g_r0lab_s4_step_traps != 0 ||
        g_r0lab_s4_brk_unexpected != 0 || observed_rc < 0 ||
        !strstr(observed_reply, "brk_events=1") ||
        !strstr(observed_reply, "step_events=1") ||
        !strstr(observed_reply, "enable_events=1") ||
        !strstr(observed_reply, "disable_events=1") ||
        !strstr(observed_reply, "state=step_observed") ||
        !strstr(observed_reply, "brk_skip_origin=1") ||
        !strstr(observed_reply, "step_skip_origin=1") ||
        !strstr(observed_reply, "single_step=1") ||
        !strstr(observed_reply, "pte_switch=0") ||
        clear_rc < 0 || cleared_rc < 0)
        failures = 1;

done:
    if (arm_rc >= 0 && clear_rc < 0)
        r0lab_s4_clear(token, &clear_rc, &cleared_rc);
    if (handler_installed)
        sigaction(SIGTRAP, &previous_action, NULL);
    g_r0lab_s4_brk_page = NULL;
    if (page != MAP_FAILED)
        munmap(page, page_size);
    snprintf(output, output_size,
             "s4 mode=step-only failures=%d value=%d brk_traps=%d step_traps=%d unexpected=%d last_pc=%llx last_signal=%d last_si_code=%d arm_rc=%ld observed_rc=%ld clear_rc=%ld cleared_rc=%ld target=%llx arm=\"%s\" observed=\"%s\"",
             failures, value, (int)g_r0lab_s4_brk_traps,
             (int)g_r0lab_s4_step_traps, (int)g_r0lab_s4_brk_unexpected,
             (unsigned long long)g_r0lab_s4_last_pc,
             (int)g_r0lab_s4_last_signal, (int)g_r0lab_s4_last_si_code,
             arm_rc, observed_rc, clear_rc, cleared_rc,
             (unsigned long long)(uintptr_t)page, arm_reply, observed_reply);
    return failures ? -1 : 0;
}

static int r0lab_s4_raw_step_run(const char *token_text, char *output,
                                 size_t output_size)
{
    struct sigaction action = {0};
    struct sigaction previous_action = {0};
    uint64_t token;
    void *page = MAP_FAILED;
    char command[128];
    char arm_reply[256] = {0};
    char observed_reply[320] = {0};
    long arm_rc = -1;
    long observed_rc = -1;
    long clear_rc = -1;
    long cleared_rc = -1;
    int handler_installed = 0;
    int normal_value = -1;
    int hook_value = -1;
    int restored_value = -1;
    int failures = 0;
    size_t page_size = (size_t)sysconf(_SC_PAGESIZE);

    if (r0lab_parse_token(token_text, &token) || page_size != R0LAB_M3_PAGE_SIZE) {
        snprintf(output, output_size, "rc=-22 error=invalid S4 raw-step request");
        return -1;
    }
    if (r0lab_s4_map_raw_step_page(&page)) {
        snprintf(output, output_size,
                 "rc=-12 error=s4 raw-step page allocation errno=%d", errno);
        return -1;
    }
    normal_value = ((int (*)(void))page)();

    g_r0lab_s4_brk_page = page;
    g_r0lab_s4_brk_traps = 0;
    g_r0lab_s4_step_traps = 0;
    g_r0lab_s4_brk_unexpected = 0;
    g_r0lab_s4_last_signal = 0;
    g_r0lab_s4_last_si_code = 0;
    g_r0lab_s4_last_pc = 0;
    action.sa_sigaction = r0lab_s4_brk_signal_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_SIGINFO;
    if (!sigaction(SIGTRAP, &action, &previous_action))
        handler_installed = 1;
    if (!handler_installed) {
        failures = 1;
        goto done;
    }

    snprintf(command, sizeof(command), "s4 raw-step arm 0x%llx 0x%llx",
             (unsigned long long)token, (unsigned long long)(uintptr_t)page);
    arm_rc = r0lab_control_raw(command, arm_reply, sizeof(arm_reply));
    if (arm_rc < 0) {
        failures = 1;
        goto done;
    }

    g_r0lab_s4_stage = 1;
    if (!sigsetjmp(g_r0lab_s4_jump, 1))
        hook_value = ((int (*)(void))page)();
    else
        failures = 1;
    g_r0lab_s4_stage = 0;

    snprintf(command, sizeof(command), "s4 step observed 0x%llx",
             (unsigned long long)token);
    observed_rc = r0lab_control_raw(command, observed_reply,
                                    sizeof(observed_reply));
    r0lab_s4_clear(token, &clear_rc, &cleared_rc);
    if (cleared_rc >= 0)
        restored_value = ((int (*)(void))page)();

    if (normal_value != 42 || hook_value != 99 || restored_value != 42 ||
        g_r0lab_s4_brk_traps != 0 || g_r0lab_s4_step_traps != 0 ||
        g_r0lab_s4_brk_unexpected != 0 || observed_rc < 0 ||
        !strstr(arm_reply, "s4_raw_step_ready") ||
        !strstr(arm_reply, "pte_switch=1") ||
        !strstr(observed_reply, "brk_events=1") ||
        !strstr(observed_reply, "step_events=1") ||
        !strstr(observed_reply, "enable_events=1") ||
        !strstr(observed_reply, "disable_events=1") ||
        !strstr(observed_reply, "pte_begin_events=1") ||
        !strstr(observed_reply, "pte_finish_events=1") ||
        !strstr(observed_reply, "state=step_observed") ||
        !strstr(observed_reply, "mode=raw_step") ||
        !strstr(observed_reply, "pte_switch=1") ||
        clear_rc < 0 || cleared_rc < 0)
        failures = 1;

done:
    if (arm_rc >= 0 && clear_rc < 0)
        r0lab_s4_clear(token, &clear_rc, &cleared_rc);
    if (handler_installed)
        sigaction(SIGTRAP, &previous_action, NULL);
    g_r0lab_s4_brk_page = NULL;
    if (page != MAP_FAILED)
        munmap(page, page_size);
    snprintf(output, output_size,
             "s4 mode=raw-step failures=%d normal_value=%d hook_value=%d restored_value=%d brk_traps=%d step_traps=%d unexpected=%d last_pc=%llx last_signal=%d last_si_code=%d arm_rc=%ld observed_rc=%ld clear_rc=%ld cleared_rc=%ld target=%llx arm=\"%s\" observed=\"%s\"",
             failures, normal_value, hook_value, restored_value,
             (int)g_r0lab_s4_brk_traps, (int)g_r0lab_s4_step_traps,
             (int)g_r0lab_s4_brk_unexpected,
             (unsigned long long)g_r0lab_s4_last_pc,
             (int)g_r0lab_s4_last_signal, (int)g_r0lab_s4_last_si_code,
             arm_rc, observed_rc, clear_rc, cleared_rc,
             (unsigned long long)(uintptr_t)page, arm_reply, observed_reply);
    return failures ? -1 : 0;
}

static int r0lab_s4_raw_reg_run(const char *token_text, char *output,
                                size_t output_size)
{
    struct sigaction action = {0};
    struct sigaction previous_action = {0};
    uint64_t token;
    void *page = MAP_FAILED;
    char command[128];
    char arm_reply[384] = {0};
    char observed_reply[512] = {0};
    long arm_rc = -1;
    long observed_rc = -1;
    long clear_rc = -1;
    long cleared_rc = -1;
    int handler_installed = 0;
    int normal_value = -1;
    int hook_value = -1;
    int restored_value = -1;
    int failures = 0;
    size_t page_size = (size_t)sysconf(_SC_PAGESIZE);

    if (r0lab_parse_token(token_text, &token) ||
        page_size != R0LAB_M3_PAGE_SIZE) {
        snprintf(output, output_size,
                 "rc=-22 error=invalid S4 raw-reg request");
        return -1;
    }
    if (r0lab_s4_map_raw_step_page(&page)) {
        snprintf(output, output_size,
                 "rc=-12 error=s4 raw-reg page allocation errno=%d", errno);
        return -1;
    }
    normal_value = ((int (*)(void))page)();

    g_r0lab_s4_brk_page = page;
    g_r0lab_s4_brk_traps = 0;
    g_r0lab_s4_step_traps = 0;
    g_r0lab_s4_brk_unexpected = 0;
    g_r0lab_s4_last_signal = 0;
    g_r0lab_s4_last_si_code = 0;
    g_r0lab_s4_last_pc = 0;
    action.sa_sigaction = r0lab_s4_brk_signal_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_SIGINFO;
    if (!sigaction(SIGTRAP, &action, &previous_action))
        handler_installed = 1;
    if (!handler_installed) {
        failures = 1;
        goto done;
    }

    snprintf(command, sizeof(command), "s4 raw-reg arm 0x%llx 0x%llx",
             (unsigned long long)token,
             (unsigned long long)(uintptr_t)page);
    arm_rc = r0lab_control_raw(command, arm_reply, sizeof(arm_reply));
    if (arm_rc < 0) {
        failures = 1;
        goto done;
    }

    g_r0lab_s4_stage = 1;
    if (!sigsetjmp(g_r0lab_s4_jump, 1))
        hook_value = ((int (*)(void))page)();
    else
        failures = 1;
    g_r0lab_s4_stage = 0;

    snprintf(command, sizeof(command), "s4 step observed 0x%llx",
             (unsigned long long)token);
    observed_rc = r0lab_control_raw(command, observed_reply,
                                    sizeof(observed_reply));
    r0lab_s4_clear(token, &clear_rc, &cleared_rc);
    if (cleared_rc >= 0)
        restored_value = ((int (*)(void))page)();

    if (normal_value != 42 || hook_value != R0LAB_S4_RAW_REG_VALUE ||
        restored_value != 42 || g_r0lab_s4_brk_traps != 0 ||
        g_r0lab_s4_step_traps != 0 ||
        g_r0lab_s4_brk_unexpected != 0 || observed_rc < 0 ||
        !strstr(arm_reply, "s4_raw_reg_ready") ||
        !strstr(arm_reply, "mode=raw_reg") ||
        !strstr(arm_reply, "register_edit=1") ||
        !strstr(arm_reply, "register_index=1") ||
        !strstr(arm_reply, "register_value=73") ||
        !strstr(arm_reply, "register_apply=brk_before_single_step") ||
        !strstr(arm_reply, "pte_switch=1") ||
        !strstr(observed_reply, "brk_events=1") ||
        !strstr(observed_reply, "step_events=1") ||
        !strstr(observed_reply, "enable_events=1") ||
        !strstr(observed_reply, "disable_events=1") ||
        !strstr(observed_reply, "pte_begin_events=1") ||
        !strstr(observed_reply, "pte_finish_events=1") ||
        !strstr(observed_reply, "reg_write_events=1") ||
        !strstr(observed_reply, "state=step_observed") ||
        !strstr(observed_reply, "mode=raw_reg") ||
        !strstr(observed_reply, "register_edit=1") ||
        !strstr(observed_reply, "register_index=1") ||
        !strstr(observed_reply, "register_value=73") ||
        !strstr(observed_reply, "register_apply=brk_before_single_step") ||
        !strstr(observed_reply, "pte_switch=1") ||
        clear_rc < 0 || cleared_rc < 0)
        failures = 1;

done:
    if (arm_rc >= 0 && clear_rc < 0)
        r0lab_s4_clear(token, &clear_rc, &cleared_rc);
    if (handler_installed)
        sigaction(SIGTRAP, &previous_action, NULL);
    g_r0lab_s4_brk_page = NULL;
    if (page != MAP_FAILED)
        munmap(page, page_size);
    snprintf(output, output_size,
             "s4 mode=raw-reg failures=%d register_edit=1 register_index=1 register_value=%d register_apply=brk_before_single_step normal_value=%d hook_value=%d restored_value=%d brk_traps=%d step_traps=%d unexpected=%d last_pc=%llx last_signal=%d last_si_code=%d arm_rc=%ld observed_rc=%ld clear_rc=%ld cleared_rc=%ld target=%llx arm=\"%s\" observed=\"%s\"",
             failures, R0LAB_S4_RAW_REG_VALUE, normal_value, hook_value,
             restored_value, (int)g_r0lab_s4_brk_traps,
             (int)g_r0lab_s4_step_traps,
             (int)g_r0lab_s4_brk_unexpected,
             (unsigned long long)g_r0lab_s4_last_pc,
             (int)g_r0lab_s4_last_signal, (int)g_r0lab_s4_last_si_code,
             arm_rc, observed_rc, clear_rc, cleared_rc,
             (unsigned long long)(uintptr_t)page, arm_reply, observed_reply);
    return failures ? -1 : 0;
}

static int r0lab_m5_hold_m3(const char *token_text, char *output,
                             size_t output_size)
{
    uint64_t token;
    void *page = MAP_FAILED;
    char command[96];
    char reply[128] = {0};
    char source_permissions[5] = "????";
    long arm_rc = -1;
    long ready_rc = -1;
    int normal_value = -1;
    int failures = 0;
    size_t page_size = (size_t)sysconf(_SC_PAGESIZE);

    if (r0lab_parse_token(token_text, &token) || page_size != R0LAB_M3_PAGE_SIZE) {
        snprintf(output, output_size, "rc=-22 error=invalid M5 M3 hold request");
        return -1;
    }
    if (g_r0lab_m5_hold.armed) {
        snprintf(output, output_size, "rc=-16 error=hold already active");
        return -1;
    }
    if (r0lab_m5_map_code_page(&page, R0LAB_M3_CODE_MOV_W0_42)) {
        snprintf(output, output_size, "rc=-12 error=hold page allocation errno=%d", errno);
        return -1;
    }
    normal_value = ((int (*)(void))page)();
    snprintf(command, sizeof(command), "m3 arm 0x%llx 0x%llx",
             (unsigned long long)token, (unsigned long long)(uintptr_t)page);
    errno = 0;
    arm_rc = r0lab_control_raw(command, reply, sizeof(reply));
    if (arm_rc < 0)
        goto fail;
    ready_rc = r0lab_m3_wait_for("m3 ready", token);
    if (ready_rc < 0)
        goto fail;
    if (r0lab_m4_map_permissions(page, source_permissions))
        ++failures;
    if (normal_value != 42 || strcmp(source_permissions, "r-xp"))
        ++failures;
    g_r0lab_m5_hold.source_page = page;
    g_r0lab_m5_hold.clone_page = NULL;
    g_r0lab_m5_hold.page_size = page_size;
    g_r0lab_m5_hold.token = token;
    g_r0lab_m5_hold.mode = 3;
    g_r0lab_m5_hold.armed = 1;
    snprintf(output, output_size,
             "m5 mode=m3-hold failures=%d normal_value=%d source_perms=%s arm_rc=%ld ready_rc=%ld source=%llx",
             failures, normal_value, source_permissions, arm_rc, ready_rc,
             (unsigned long long)(uintptr_t)page);
    return failures ? -1 : 0;

fail:
    if (arm_rc >= 0) {
        long clear_rc = -1;
        long cleared_rc = -1;

        r0lab_m3_clear(token, &clear_rc, &cleared_rc);
    }
    munmap(page, page_size);
    snprintf(output, output_size,
             "m5 mode=m3-hold failures=1 normal_value=%d source_perms=%s arm_rc=%ld ready_rc=%ld",
             normal_value, source_permissions, arm_rc, ready_rc);
    return -1;
}

static int r0lab_m5_hold_m4(const char *token_text, char *output,
                             size_t output_size)
{
    uint64_t token;
    void *source_page = MAP_FAILED;
    void *clone_page = MAP_FAILED;
    char command[128];
    char reply[128] = {0};
    char source_permissions[5] = "????";
    char clone_permissions[5] = "????";
    long arm_rc = -1;
    long ready_rc = -1;
    int source_normal_value = -1;
    int clone_normal_value = -1;
    int failures = 0;
    size_t page_size = (size_t)sysconf(_SC_PAGESIZE);

    if (r0lab_parse_token(token_text, &token) || page_size != R0LAB_M3_PAGE_SIZE) {
        snprintf(output, output_size, "rc=-22 error=invalid M5 M4 hold request");
        return -1;
    }
    if (g_r0lab_m5_hold.armed) {
        snprintf(output, output_size, "rc=-16 error=hold already active");
        return -1;
    }
    if (r0lab_m5_map_code_page(&source_page, R0LAB_M3_CODE_MOV_W0_42) ||
        r0lab_m5_map_code_page(&clone_page, R0LAB_M4_CODE_MOV_W0_99)) {
        if (source_page != MAP_FAILED)
            munmap(source_page, page_size);
        snprintf(output, output_size, "rc=-12 error=hold page allocation errno=%d", errno);
        return -1;
    }
    source_normal_value = ((int (*)(void))source_page)();
    clone_normal_value = ((int (*)(void))clone_page)();
    snprintf(command, sizeof(command), "m4 arm 0x%llx 0x%llx 0x%llx",
             (unsigned long long)token, (unsigned long long)(uintptr_t)source_page,
             (unsigned long long)(uintptr_t)clone_page);
    errno = 0;
    arm_rc = r0lab_control_raw(command, reply, sizeof(reply));
    if (arm_rc < 0)
        goto fail;
    ready_rc = r0lab_m3_wait_for("m4 ready", token);
    if (ready_rc < 0)
        goto fail;
    if (r0lab_m4_map_permissions(source_page, source_permissions) ||
        r0lab_m4_map_permissions(clone_page, clone_permissions))
        ++failures;
    if (source_normal_value != 42 || clone_normal_value != 99 ||
        strcmp(source_permissions, "r-xp") || strcmp(clone_permissions, "r-xp"))
        ++failures;
    g_r0lab_m5_hold.source_page = source_page;
    g_r0lab_m5_hold.clone_page = clone_page;
    g_r0lab_m5_hold.page_size = page_size;
    g_r0lab_m5_hold.token = token;
    g_r0lab_m5_hold.mode = 4;
    g_r0lab_m5_hold.armed = 1;
    snprintf(output, output_size,
             "m5 mode=m4-hold failures=%d source_normal=%d clone_normal=%d source_perms=%s clone_perms=%s arm_rc=%ld ready_rc=%ld source=%llx clone=%llx",
             failures, source_normal_value, clone_normal_value,
             source_permissions, clone_permissions, arm_rc, ready_rc,
             (unsigned long long)(uintptr_t)source_page,
             (unsigned long long)(uintptr_t)clone_page);
    return failures ? -1 : 0;

fail:
    if (arm_rc >= 0) {
        long clear_rc = -1;
        long cleared_rc = -1;

        r0lab_m4_clear(token, &clear_rc, &cleared_rc);
    }
    munmap(clone_page, page_size);
    munmap(source_page, page_size);
    snprintf(output, output_size,
             "m5 mode=m4-hold failures=1 source_normal=%d clone_normal=%d source_perms=%s clone_perms=%s arm_rc=%ld ready_rc=%ld",
             source_normal_value, clone_normal_value, source_permissions,
             clone_permissions, arm_rc, ready_rc);
    return -1;
}

static int r0lab_m5_hold_raw(const char *token_text, char *output,
                             size_t output_size)
{
    uint64_t token;
    void *page = MAP_FAILED;
    volatile uint32_t *readable_code;
    char command[96];
    char reply[256] = {0};
    char ready_reply[160] = {0};
    char inspect_reply[512] = {0};
    uint32_t word_during = 0;
    long arm_rc = -1;
    long ready_rc = -1;
    long inspect_rc = -1;
    int normal_value = -1;
    int failures = 0;
    size_t page_size = (size_t)sysconf(_SC_PAGESIZE);

    if (r0lab_parse_token(token_text, &token) || page_size != R0LAB_M3_PAGE_SIZE) {
        snprintf(output, output_size, "rc=-22 error=invalid M5 raw hold request");
        return -1;
    }
    if (g_r0lab_m5_hold.armed) {
        snprintf(output, output_size, "rc=-16 error=hold already active");
        return -1;
    }
    if (r0lab_m5_map_code_page(&page, R0LAB_M3_CODE_MOV_W0_42)) {
        snprintf(output, output_size, "rc=-12 error=hold page allocation errno=%d", errno);
        return -1;
    }
    readable_code = (volatile uint32_t *)page;
    normal_value = ((int (*)(void))page)();
    snprintf(command, sizeof(command), "raw arm 0x%llx 0x%llx",
             (unsigned long long)token, (unsigned long long)(uintptr_t)page);
    errno = 0;
    arm_rc = r0lab_control_raw(command, reply, sizeof(reply));
    if (arm_rc < 0)
        goto fail;
    ready_rc = r0lab_wait_for_reply("raw ready", token, ready_reply,
                                    sizeof(ready_reply));
    if (ready_rc < 0)
        goto fail;
    r0lab_inline_reply(ready_reply);
    word_during = readable_code[0];
    snprintf(command, sizeof(command), "raw inspect 0x%llx",
             (unsigned long long)token);
    inspect_rc = r0lab_control_raw(command, inspect_reply,
                                   sizeof(inspect_reply));
    r0lab_inline_reply(inspect_reply);
    if (normal_value != 42 || word_during != R0LAB_M3_CODE_MOV_W0_42 ||
        inspect_rc < 0 || !strstr(inspect_reply, "active_kind=source_uxn") ||
        !strstr(inspect_reply, "record_backend=raw_two_pfn") ||
        !strstr(inspect_reply, "record_state=source_uxn"))
        ++failures;

    g_r0lab_m5_hold.source_page = page;
    g_r0lab_m5_hold.clone_page = NULL;
    g_r0lab_m5_hold.page_size = page_size;
    g_r0lab_m5_hold.token = token;
    g_r0lab_m5_hold.mode = 5;
    g_r0lab_m5_hold.armed = 1;
    snprintf(output, output_size,
             "m5 mode=raw-hold failures=%d normal_value=%d word=%08x arm_rc=%ld ready_rc=%ld inspect_rc=%ld source=%llx ready=\"%s\" inspect=\"%s\"",
             failures, normal_value, word_during, arm_rc, ready_rc, inspect_rc,
             (unsigned long long)(uintptr_t)page, ready_reply, inspect_reply);
    return failures ? -1 : 0;

fail:
    if (arm_rc >= 0) {
        long clear_rc = -1;
        long cleared_rc = -1;

        r0lab_raw_clear(token, &clear_rc, &cleared_rc);
    }
    munmap(page, page_size);
    snprintf(output, output_size,
             "m5 mode=raw-hold failures=1 normal_value=%d word=%08x arm_rc=%ld ready_rc=%ld inspect_rc=%ld",
             normal_value, word_during, arm_rc, ready_rc, inspect_rc);
    return -1;
}

static int r0lab_m5_hold_s4(const char *token_text, char *output,
                             size_t output_size)
{
    uint64_t token;
    void *page = MAP_FAILED;
    char command[128];
    char ready_reply[192] = {0};
    long arm_rc = -1;
    long ready_rc = -1;
    int failures = 0;
    size_t page_size = (size_t)sysconf(_SC_PAGESIZE);

    if (r0lab_parse_token(token_text, &token) || page_size != R0LAB_M3_PAGE_SIZE) {
        snprintf(output, output_size, "rc=-22 error=invalid M5 S4 hold request");
        return -1;
    }
    if (g_r0lab_m5_hold.armed) {
        snprintf(output, output_size, "rc=-16 error=hold already active");
        return -1;
    }
    if (r0lab_s4_map_brk_page(&page)) {
        snprintf(output, output_size, "rc=-12 error=s4 hold page allocation errno=%d",
                 errno);
        return -1;
    }

    snprintf(command, sizeof(command), "s4 brk arm 0x%llx 0x%llx",
             (unsigned long long)token, (unsigned long long)(uintptr_t)page);
    arm_rc = r0lab_control_raw(command, ready_reply, sizeof(ready_reply));
    if (arm_rc < 0)
        goto fail;
    ready_rc = arm_rc;
    if (!strstr(ready_reply, "s4_brk_ready") ||
        !strstr(ready_reply, "skip_origin=0") ||
        !strstr(ready_reply, "single_step=0") ||
        !strstr(ready_reply, "pte_switch=0"))
        ++failures;

    g_r0lab_m5_hold.source_page = page;
    g_r0lab_m5_hold.clone_page = NULL;
    g_r0lab_m5_hold.page_size = page_size;
    g_r0lab_m5_hold.token = token;
    g_r0lab_m5_hold.mode = 6;
    g_r0lab_m5_hold.armed = 1;
    snprintf(output, output_size,
             "m5 mode=s4-hold failures=%d arm_rc=%ld ready_rc=%ld source=%llx ready=\"%s\"",
             failures, arm_rc, ready_rc, (unsigned long long)(uintptr_t)page,
             ready_reply);
    return failures ? -1 : 0;

fail:
    if (arm_rc >= 0) {
        long clear_rc = -1;
        long cleared_rc = -1;

        r0lab_s4_clear(token, &clear_rc, &cleared_rc);
    }
    munmap(page, page_size);
    snprintf(output, output_size,
             "m5 mode=s4-hold failures=1 arm_rc=%ld ready_rc=%ld ready=\"%s\"",
             arm_rc, ready_rc, ready_reply);
    return -1;
}

static int r0lab_m5_hold_s4_raw_step(const char *token_text, char *output,
                                     size_t output_size)
{
    uint64_t token;
    void *page = MAP_FAILED;
    char command[128];
    char ready_reply[256] = {0};
    long arm_rc = -1;
    long ready_rc = -1;
    int normal_value = -1;
    int failures = 0;
    size_t page_size = (size_t)sysconf(_SC_PAGESIZE);

    if (r0lab_parse_token(token_text, &token) || page_size != R0LAB_M3_PAGE_SIZE) {
        snprintf(output, output_size,
                 "rc=-22 error=invalid M5 S4 raw-step hold request");
        return -1;
    }
    if (g_r0lab_m5_hold.armed) {
        snprintf(output, output_size, "rc=-16 error=hold already active");
        return -1;
    }
    if (r0lab_s4_map_raw_step_page(&page)) {
        snprintf(output, output_size,
                 "rc=-12 error=s4 raw-step hold page allocation errno=%d",
                 errno);
        return -1;
    }
    normal_value = ((int (*)(void))page)();

    snprintf(command, sizeof(command), "s4 raw-step arm 0x%llx 0x%llx",
             (unsigned long long)token, (unsigned long long)(uintptr_t)page);
    arm_rc = r0lab_control_raw(command, ready_reply, sizeof(ready_reply));
    if (arm_rc < 0)
        goto fail;
    ready_rc = arm_rc;
    if (normal_value != 42 ||
        !strstr(ready_reply, "s4_raw_step_ready") ||
        !strstr(ready_reply, "mode=raw_step") ||
        !strstr(ready_reply, "pte_switch=1") ||
        !strstr(ready_reply, "raw_state=shadow_active"))
        ++failures;

    g_r0lab_m5_hold.source_page = page;
    g_r0lab_m5_hold.clone_page = NULL;
    g_r0lab_m5_hold.page_size = page_size;
    g_r0lab_m5_hold.token = token;
    g_r0lab_m5_hold.mode = 7;
    g_r0lab_m5_hold.armed = 1;
    snprintf(output, output_size,
             "m5 mode=s4-raw-step-hold failures=%d normal_value=%d arm_rc=%ld ready_rc=%ld source=%llx ready=\"%s\"",
             failures, normal_value, arm_rc, ready_rc,
             (unsigned long long)(uintptr_t)page, ready_reply);
    return failures ? -1 : 0;

fail:
    if (arm_rc >= 0) {
        long clear_rc = -1;
        long cleared_rc = -1;

        r0lab_s4_clear(token, &clear_rc, &cleared_rc);
    }
    munmap(page, page_size);
    snprintf(output, output_size,
             "m5 mode=s4-raw-step-hold failures=1 normal_value=%d arm_rc=%ld ready_rc=%ld ready=\"%s\"",
             normal_value, arm_rc, ready_rc, ready_reply);
    return -1;
}

static int r0lab_m5_interrupt_m3(const char *token_text, char *output,
                                  size_t output_size)
{
    uint64_t token;
    void *page = MAP_FAILED;
    char command[96];
    char reply[128] = {0};
    char before_permissions[5] = "????";
    char after_permissions[5] = "????";
    long fault_rc = -1;
    long arm_rc = -1;
    long clear_rc = -1;
    long cleared_rc = -1;
    int normal_value = -1;
    int after_value = -1;
    int failures = 0;
    size_t page_size = (size_t)sysconf(_SC_PAGESIZE);

    if (r0lab_parse_token(token_text, &token) || page_size != R0LAB_M3_PAGE_SIZE) {
        snprintf(output, output_size, "rc=-22 error=invalid M5 interrupt request");
        return -1;
    }
    if (r0lab_m5_map_code_page(&page, R0LAB_M3_CODE_MOV_W0_42)) {
        snprintf(output, output_size, "rc=-12 error=interrupt page allocation errno=%d", errno);
        return -1;
    }
    normal_value = ((int (*)(void))page)();
    if (r0lab_m4_map_permissions(page, before_permissions))
        ++failures;
    snprintf(command, sizeof(command), "fault m3 arm-delay 0x%llx",
             (unsigned long long)token);
    errno = 0;
    fault_rc = r0lab_control_raw(command, reply, sizeof(reply));
    if (fault_rc < 0)
        goto finish;
    snprintf(command, sizeof(command), "m3 arm 0x%llx 0x%llx",
             (unsigned long long)token, (unsigned long long)(uintptr_t)page);
    memset(reply, 0, sizeof(reply));
    errno = 0;
    arm_rc = r0lab_control_raw(command, reply, sizeof(reply));
    if (arm_rc >= 0)
        r0lab_m3_clear(token, &clear_rc, &cleared_rc);
    if (r0lab_m4_map_permissions(page, after_permissions))
        ++failures;
    after_value = ((int (*)(void))page)();

finish:
    if (normal_value != 42 || after_value != 42 || strcmp(before_permissions, "r-xp") ||
        strcmp(after_permissions, "r-xp") || fault_rc < 0 || arm_rc < 0 ||
        clear_rc < 0 || cleared_rc < 0)
        ++failures;
    snprintf(output, output_size,
             "m5 mode=m3-interrupt failures=%d normal_value=%d after_value=%d perms=%s/%s fault_rc=%ld arm_rc=%ld clear_rc=%ld cleared_rc=%ld",
             failures, normal_value, after_value, before_permissions, after_permissions,
             fault_rc, arm_rc, clear_rc, cleared_rc);
    munmap(page, page_size);
    return failures ? -1 : 0;
}

static int r0lab_m5_enomem_m3(const char *token_text, char *output,
                               size_t output_size)
{
    uint64_t token;
    void *page = MAP_FAILED;
    char command[96];
    char reply[128] = {0};
    char before_permissions[5] = "????";
    char after_permissions[5] = "????";
    long fault_rc = -1;
    long arm_rc = -1;
    long cleared_rc = -1;
    int arm_errno = 0;
    int normal_value = -1;
    int after_value = -1;
    int failures = 0;
    size_t page_size = (size_t)sysconf(_SC_PAGESIZE);

    if (r0lab_parse_token(token_text, &token) || page_size != R0LAB_M3_PAGE_SIZE) {
        snprintf(output, output_size, "rc=-22 error=invalid M5 ENOMEM request");
        return -1;
    }
    if (r0lab_m5_map_code_page(&page, R0LAB_M3_CODE_MOV_W0_42)) {
        snprintf(output, output_size, "rc=-12 error=ENOMEM page allocation errno=%d", errno);
        return -1;
    }
    normal_value = ((int (*)(void))page)();
    if (r0lab_m4_map_permissions(page, before_permissions))
        ++failures;
    snprintf(command, sizeof(command), "fault m3 arm-enomem 0x%llx",
             (unsigned long long)token);
    errno = 0;
    fault_rc = r0lab_control_raw(command, reply, sizeof(reply));
    if (fault_rc < 0)
        goto finish;
    snprintf(command, sizeof(command), "m3 arm 0x%llx 0x%llx",
             (unsigned long long)token, (unsigned long long)(uintptr_t)page);
    memset(reply, 0, sizeof(reply));
    errno = 0;
    arm_rc = r0lab_control_raw(command, reply, sizeof(reply));
    arm_errno = errno;
    cleared_rc = r0lab_m3_wait_for("m3 cleared", token);
    if (r0lab_m4_map_permissions(page, after_permissions))
        ++failures;
    after_value = ((int (*)(void))page)();

finish:
    if (normal_value != 42 || after_value != 42 || strcmp(before_permissions, "r-xp") ||
        strcmp(after_permissions, "r-xp") || fault_rc < 0 || arm_rc >= 0 ||
        arm_errno != ENOMEM || cleared_rc < 0)
        ++failures;
    snprintf(output, output_size,
             "m5 mode=m3-enomem failures=%d normal_value=%d after_value=%d perms=%s/%s fault_rc=%ld arm_rc=%ld arm_errno=%d cleared_rc=%ld",
             failures, normal_value, after_value, before_permissions, after_permissions,
             fault_rc, arm_rc, arm_errno, cleared_rc);
    munmap(page, page_size);
    return failures ? -1 : 0;
}

static int r0lab_exec_true(void)
{
    char *const argv[] = { "true", NULL };

    return (int)syscall(__NR_execve, "/system/bin/true", argv, environ);
}

static int r0lab_m5_fork_exec_m3(const char *token_text, char *output,
                                  size_t output_size)
{
    uint64_t token;
    void *page = MAP_FAILED;
    char command[96];
    char reply[128] = {0};
    char during_permissions[5] = "????";
    char after_permissions[5] = "????";
    long arm_rc = -1;
    long ready_rc = -1;
    long clear_rc = -1;
    long cleared_rc = -1;
    pid_t child = -1;
    int child_status = -1;
    int child_exit = -1;
    int normal_value = -1;
    int after_value = -1;
    int failures = 0;
    size_t page_size = (size_t)sysconf(_SC_PAGESIZE);

    if (r0lab_parse_token(token_text, &token) || page_size != R0LAB_M3_PAGE_SIZE) {
        snprintf(output, output_size, "rc=-22 error=invalid M5 fork request");
        return -1;
    }
    if (r0lab_m5_map_code_page(&page, R0LAB_M3_CODE_MOV_W0_42)) {
        snprintf(output, output_size, "rc=-12 error=fork page allocation errno=%d", errno);
        return -1;
    }
    normal_value = ((int (*)(void))page)();
    snprintf(command, sizeof(command), "m3 arm 0x%llx 0x%llx",
             (unsigned long long)token, (unsigned long long)(uintptr_t)page);
    errno = 0;
    arm_rc = r0lab_control_raw(command, reply, sizeof(reply));
    if (arm_rc < 0)
        goto finish;
    ready_rc = r0lab_m3_wait_for("m3 ready", token);
    if (ready_rc < 0)
        goto clear;
    child = fork();
    if (!child) {
        (void)r0lab_exec_true();
        _exit(127);
    }
    if (child > 0 && waitpid(child, &child_status, 0) == child &&
        WIFEXITED(child_status))
        child_exit = WEXITSTATUS(child_status);
    if (r0lab_m4_map_permissions(page, during_permissions))
        ++failures;

clear:
    r0lab_m3_clear(token, &clear_rc, &cleared_rc);
    if (cleared_rc >= 0) {
        if (r0lab_m4_map_permissions(page, after_permissions))
            ++failures;
        after_value = ((int (*)(void))page)();
    }

finish:
    if (normal_value != 42 || after_value != 42 || strcmp(during_permissions, "r-xp") ||
        strcmp(after_permissions, "r-xp") || arm_rc < 0 || ready_rc < 0 ||
        child <= 0 || child_exit != 0 || clear_rc < 0 || cleared_rc < 0)
        ++failures;
    snprintf(output, output_size,
             "m5 mode=m3-fork-exec failures=%d normal_value=%d after_value=%d child_pid=%d child_exit=%d perms=%s/%s arm_rc=%ld ready_rc=%ld clear_rc=%ld cleared_rc=%ld",
             failures, normal_value, after_value, child, child_exit,
             during_permissions, after_permissions, arm_rc, ready_rc, clear_rc,
             cleared_rc);
    munmap(page, page_size);
    return failures ? -1 : 0;
}

static int r0lab_m5_exec_m3(const char *token_text, char *output,
                             size_t output_size)
{
    uint64_t token;
    void *page = MAP_FAILED;
    char command[96];
    char reply[128] = {0};
    long arm_rc = -1;
    long ready_rc = -1;
    long clear_rc = -1;
    long cleared_rc = -1;
    int exec_errno;
    size_t page_size = (size_t)sysconf(_SC_PAGESIZE);

    if (r0lab_parse_token(token_text, &token) || page_size != R0LAB_M3_PAGE_SIZE) {
        snprintf(output, output_size, "rc=-22 error=invalid M5 exec request");
        return -1;
    }
    if (r0lab_m5_map_code_page(&page, R0LAB_M3_CODE_MOV_W0_42)) {
        snprintf(output, output_size, "rc=-12 error=exec page allocation errno=%d", errno);
        return -1;
    }
    snprintf(command, sizeof(command), "m3 arm 0x%llx 0x%llx",
             (unsigned long long)token, (unsigned long long)(uintptr_t)page);
    errno = 0;
    arm_rc = r0lab_control_raw(command, reply, sizeof(reply));
    if (arm_rc >= 0)
        ready_rc = r0lab_m3_wait_for("m3 ready", token);
    if (arm_rc >= 0 && ready_rc >= 0)
        (void)r0lab_exec_true();

    exec_errno = errno;
    if (arm_rc >= 0)
        r0lab_m3_clear(token, &clear_rc, &cleared_rc);
    munmap(page, page_size);
    snprintf(output, output_size,
             "m5 mode=m3-exec failures=1 arm_rc=%ld ready_rc=%ld exec_errno=%d clear_rc=%ld cleared_rc=%ld",
             arm_rc, ready_rc, exec_errno, clear_rc, cleared_rc);
    return -1;
}

static void r0lab_describe(char *text, size_t text_size)
{
    Dl_info info = {0};

    dladdr((const void *)&r0lab_marker, &info);
    snprintf(text, text_size,
             "pid=%d tid=%ld uid=%d marker=%p return_site=%p marker_value=%d library=%s",
             getpid(), syscall(__NR_gettid), getuid(),
             (const void *)&r0lab_marker, (const void *)r0lab_marker_return_site,
             r0lab_marker(11), info.dli_fname ? info.dli_fname : "unknown");
}

JNIEXPORT jstring JNICALL
Java_dev_r0hook_lab_MainActivity_nativeDescribe(JNIEnv *env, jobject thiz)
{
    char text[512];

    (void)thiz;
    r0lab_describe(text, sizeof(text));
    return (*env)->NewStringUTF(env, text);
}

JNIEXPORT jstring JNICALL
Java_dev_r0hook_lab_MainActivity_nativeControl(JNIEnv *env, jobject thiz, jstring command)
{
    char reply[4096] = {0};
    char text[4352];
    const char *args;
    long rc;
    int saved_errno;

    (void)thiz;
    if (!command)
        return (*env)->NewStringUTF(env, "rc=-22 error=missing command");
    args = (*env)->GetStringUTFChars(env, command, NULL);
    if (!args)
        return (*env)->NewStringUTF(env, "rc=-12 error=command allocation failed");

    if (!strcmp(args, "describe")) {
        r0lab_describe(reply, sizeof(reply));
        (*env)->ReleaseStringUTFChars(env, command, args);
        return (*env)->NewStringUTF(env, reply);
    }

    if (!strncmp(args, "m2 preflight ", 13)) {
        r0lab_m2_run_preflight(args + 13, reply, sizeof(reply));
        (*env)->ReleaseStringUTFChars(env, command, args);
        return (*env)->NewStringUTF(env, reply);
    }
    if (!strncmp(args, "m2 single ", 10)) {
        r0lab_m2_run_single(args + 10, reply, sizeof(reply));
        (*env)->ReleaseStringUTFChars(env, command, args);
        return (*env)->NewStringUTF(env, reply);
    }
    if (!strncmp(args, "m2 run ", 7)) {
        r0lab_m2_run(args + 7, reply, sizeof(reply));
        (*env)->ReleaseStringUTFChars(env, command, args);
        return (*env)->NewStringUTF(env, reply);
    }
    if (!strncmp(args, "m3 single ", 10)) {
        r0lab_m3_run_single(args + 10, reply, sizeof(reply));
        (*env)->ReleaseStringUTFChars(env, command, args);
        return (*env)->NewStringUTF(env, reply);
    }
    if (!strncmp(args, "m3 multi ", 9)) {
        r0lab_m3_run_multi(args + 9, reply, sizeof(reply));
        (*env)->ReleaseStringUTFChars(env, command, args);
        return (*env)->NewStringUTF(env, reply);
    }
    if (!strncmp(args, "m4 preflight ", 13)) {
        r0lab_m4_run_preflight(args + 13, reply, sizeof(reply));
        (*env)->ReleaseStringUTFChars(env, command, args);
        return (*env)->NewStringUTF(env, reply);
    }
    if (!strncmp(args, "m4 run ", 7)) {
        r0lab_m4_run_redirect(args + 7, reply, sizeof(reply));
        (*env)->ReleaseStringUTFChars(env, command, args);
        return (*env)->NewStringUTF(env, reply);
    }
    if (!strncmp(args, "raw run ", 8)) {
        r0lab_raw_run(args + 8, reply, sizeof(reply));
        (*env)->ReleaseStringUTFChars(env, command, args);
        return (*env)->NewStringUTF(env, reply);
    }
    if (!strncmp(args, "raw gup run ", 12)) {
        r0lab_raw_gup_run(args + 12, reply, sizeof(reply));
        (*env)->ReleaseStringUTFChars(env, command, args);
        return (*env)->NewStringUTF(env, reply);
    }
    if (!strncmp(args, "raw read cycle run ", 19)) {
        r0lab_raw_read_cycle_run(args + 19, reply, sizeof(reply));
        (*env)->ReleaseStringUTFChars(env, command, args);
        return (*env)->NewStringUTF(env, reply);
    }
    if (!strncmp(args, "raw gup hook run ", 17)) {
        r0lab_raw_gup_hook_run(args + 17, reply, sizeof(reply));
        (*env)->ReleaseStringUTFChars(env, command, args);
        return (*env)->NewStringUTF(env, reply);
    }
    if (!strncmp(args, "raw fork hook run ", 18)) {
        r0lab_raw_fork_hook_run(args + 18, reply, sizeof(reply));
        (*env)->ReleaseStringUTFChars(env, command, args);
        return (*env)->NewStringUTF(env, reply);
    }
    if (!strncmp(args, "raw fault data probe run ", 25)) {
        r0lab_raw_fault_data_probe_run(args + 25, reply, sizeof(reply));
        (*env)->ReleaseStringUTFChars(env, command, args);
        return (*env)->NewStringUTF(env, reply);
    }
    if (!strncmp(args, "raw fault hook run ", 19)) {
        r0lab_raw_fault_hook_run(args + 19, reply, sizeof(reply));
        (*env)->ReleaseStringUTFChars(env, command, args);
        return (*env)->NewStringUTF(env, reply);
    }
    if (!strncmp(args, "raw abort probe run ", 20)) {
        r0lab_raw_abort_probe_run(args + 20, reply, sizeof(reply));
        (*env)->ReleaseStringUTFChars(env, command, args);
        return (*env)->NewStringUTF(env, reply);
    }
    if (!strncmp(args, "raw abort write probe run ", 26)) {
        r0lab_raw_abort_write_probe_run(args + 26, reply, sizeof(reply));
        (*env)->ReleaseStringUTFChars(env, command, args);
        return (*env)->NewStringUTF(env, reply);
    }
    if (!strncmp(args, "raw abort write release run ", 28)) {
        r0lab_raw_abort_write_release_run(args + 28, reply, sizeof(reply));
        (*env)->ReleaseStringUTFChars(env, command, args);
        return (*env)->NewStringUTF(env, reply);
    }
    if (!strncmp(args, "raw syscall hook run ", 21)) {
        r0lab_raw_syscall_hook_run(args + 21, reply, sizeof(reply));
        (*env)->ReleaseStringUTFChars(env, command, args);
        return (*env)->NewStringUTF(env, reply);
    }
    if (!strncmp(args, "raw syscall read cycle run ", 27)) {
        r0lab_raw_syscall_read_cycle_run(args + 27, reply, sizeof(reply));
        (*env)->ReleaseStringUTFChars(env, command, args);
        return (*env)->NewStringUTF(env, reply);
    }
    if (!strncmp(args, "raw prctl read cycle run ", 25)) {
        r0lab_raw_prctl_read_cycle_run(args + 25, reply, sizeof(reply));
        (*env)->ReleaseStringUTFChars(env, command, args);
        return (*env)->NewStringUTF(env, reply);
    }
    if (!strncmp(args, "raw prctl patch records run ", 28)) {
        r0lab_raw_prctl_patch_records_run(args + 28, reply, sizeof(reply));
        (*env)->ReleaseStringUTFChars(env, command, args);
        return (*env)->NewStringUTF(env, reply);
    }
    if (!strncmp(args, "raw exit hook hold ", 19)) {
        r0lab_raw_exit_hook_hold(args + 19, reply, sizeof(reply));
        (*env)->ReleaseStringUTFChars(env, command, args);
        return (*env)->NewStringUTF(env, reply);
    }
    if (!strncmp(args, "raw xom run ", 12)) {
        r0lab_raw_xom_run(args + 12, reply, sizeof(reply));
        (*env)->ReleaseStringUTFChars(env, command, args);
        return (*env)->NewStringUTF(env, reply);
    }
    if (!strncmp(args, "s4 brk ", 7)) {
        r0lab_s4_brk_run(args + 7, reply, sizeof(reply));
        (*env)->ReleaseStringUTFChars(env, command, args);
        return (*env)->NewStringUTF(env, reply);
    }
    if (!strncmp(args, "s4 step ", 8)) {
        r0lab_s4_step_run(args + 8, reply, sizeof(reply));
        (*env)->ReleaseStringUTFChars(env, command, args);
        return (*env)->NewStringUTF(env, reply);
    }
    if (!strncmp(args, "s4 raw-step ", 12)) {
        r0lab_s4_raw_step_run(args + 12, reply, sizeof(reply));
        (*env)->ReleaseStringUTFChars(env, command, args);
        return (*env)->NewStringUTF(env, reply);
    }
    if (!strncmp(args, "s4 raw-reg ", 11)) {
        r0lab_s4_raw_reg_run(args + 11, reply, sizeof(reply));
        (*env)->ReleaseStringUTFChars(env, command, args);
        return (*env)->NewStringUTF(env, reply);
    }
    if (!strcmp(args, "xom probe")) {
        r0lab_xom_probe(reply, sizeof(reply));
        (*env)->ReleaseStringUTFChars(env, command, args);
        return (*env)->NewStringUTF(env, reply);
    }
    if (!strncmp(args, "m5 m3-hold ", 11)) {
        r0lab_m5_hold_m3(args + 11, reply, sizeof(reply));
        (*env)->ReleaseStringUTFChars(env, command, args);
        return (*env)->NewStringUTF(env, reply);
    }
    if (!strncmp(args, "m5 m4-hold ", 11)) {
        r0lab_m5_hold_m4(args + 11, reply, sizeof(reply));
        (*env)->ReleaseStringUTFChars(env, command, args);
        return (*env)->NewStringUTF(env, reply);
    }
    if (!strncmp(args, "m5 raw-hold ", 12)) {
        r0lab_m5_hold_raw(args + 12, reply, sizeof(reply));
        (*env)->ReleaseStringUTFChars(env, command, args);
        return (*env)->NewStringUTF(env, reply);
    }
    if (!strncmp(args, "m5 s4-hold ", 11)) {
        r0lab_m5_hold_s4(args + 11, reply, sizeof(reply));
        (*env)->ReleaseStringUTFChars(env, command, args);
        return (*env)->NewStringUTF(env, reply);
    }
    if (!strncmp(args, "m5 s4-raw-step-hold ", 20)) {
        r0lab_m5_hold_s4_raw_step(args + 20, reply, sizeof(reply));
        (*env)->ReleaseStringUTFChars(env, command, args);
        return (*env)->NewStringUTF(env, reply);
    }
    if (!strncmp(args, "m5 m3-interrupt ", 16)) {
        r0lab_m5_interrupt_m3(args + 16, reply, sizeof(reply));
        (*env)->ReleaseStringUTFChars(env, command, args);
        return (*env)->NewStringUTF(env, reply);
    }
    if (!strncmp(args, "m5 m3-enomem ", 13)) {
        r0lab_m5_enomem_m3(args + 13, reply, sizeof(reply));
        (*env)->ReleaseStringUTFChars(env, command, args);
        return (*env)->NewStringUTF(env, reply);
    }
    if (!strncmp(args, "m5 m3-fork-exec ", 16)) {
        r0lab_m5_fork_exec_m3(args + 16, reply, sizeof(reply));
        (*env)->ReleaseStringUTFChars(env, command, args);
        return (*env)->NewStringUTF(env, reply);
    }
    if (!strncmp(args, "m5 m3-exec ", 11)) {
        r0lab_m5_exec_m3(args + 11, reply, sizeof(reply));
        (*env)->ReleaseStringUTFChars(env, command, args);
        return (*env)->NewStringUTF(env, reply);
    }

    errno = 0;
    rc = r0lab_control_raw(args, reply, sizeof(reply));
    saved_errno = errno;
    (*env)->ReleaseStringUTFChars(env, command, args);

    if (rc < 0) {
        snprintf(text, sizeof(text), "rc=%ld errno=%d error=%s", rc, saved_errno,
                 strerror(saved_errno));
    } else {
        snprintf(text, sizeof(text), "rc=%ld\n%s", rc, reply);
    }
    return (*env)->NewStringUTF(env, text);
}
