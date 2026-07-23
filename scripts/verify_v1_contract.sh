#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

fail() {
  printf '%s\n' "v1 contract verification failure: $*" >&2
  exit 1
}

require_file() {
  path=$1
  [ -f "$ROOT/$path" ] || fail "required file is missing: $path"
}

require_text() {
  path=$1
  text=$2
  grep -F -- "$text" "$ROOT/$path" >/dev/null ||
    fail "required contract text is missing from $path: $text"
}

CONTRACT=docs/r0lab-v1-contract.md
VERIFICATION=docs/r0lab-v1-verification.md
SHADOW_PLAN=docs/shadow-page-transition-plan.md
RAW_PLAN=docs/wxshadow-raw-pte-implementation-plan.md
REPLICA_PLAN=docs/wxshadow-replica-plan.md
RAW_COMPAT=docs/pixel7-panther-raw-pte-compatibility.md
S4_PLAN=docs/wxshadow-s4-brk-step-plan.md

require_file "$CONTRACT"
require_file "$VERIFICATION"
require_file "$SHADOW_PLAN"
require_file "$RAW_PLAN"
require_file "$REPLICA_PLAN"
require_file "$RAW_COMPAT"
require_file "$S4_PLAN"
require_file docs/kpm-research-plan.md
require_file docs/kpm-compatibility-matrix.md

require_text "$CONTRACT" 'r0lab v1 Implementation Contract'
require_text "$CONTRACT" 'target_id'
require_text "$CONTRACT" 'workers_shutdown=1'
require_text "$CONTRACT" 'S1: Contract And Regression Gate'
require_text "$CONTRACT" 'S4: BRK/Single-Step ABI Gate'
require_text "$CONTRACT" 'wxshadow-s4-brk-step-plan.md'
require_text "$SHADOW_PLAN" 'UXN-Gated Shadow Page Transition Plan'
require_text "$SHADOW_PLAN" 'result=blocked'
require_text "$SHADOW_PLAN" 'user_xom_read_fault=absent'
require_text "$SHADOW_PLAN" 'boot_reason=kernel_panic'
require_text "$SHADOW_PLAN" 'visible-clone'
require_text "$RAW_PLAN" 'same VA, two PFNs'
require_text "$RAW_PLAN" 'record_backend=raw_two_pfn'
require_text "$REPLICA_PLAN" 'wxshadow Replica Plan For r0lab'
require_text "$REPLICA_PLAN" 'no `shadow_xom` symbols in KPM source'
require_text "$REPLICA_PLAN" 'struct r0lab_page_record'
require_text "$REPLICA_PLAN" 'record_backend=visible_clone'
require_text "$CONTRACT" 'page_records'
require_text "$RAW_COMPAT" 'lab_two_pfn_pass'
require_text "$RAW_COMPAT" 'kpm_locked_target_mm_writer=proven'
require_text "$VERIFICATION" 'scripts/test_v1_device.sh'
require_text "$VERIFICATION" 'scripts/test_raw_device.sh'
require_text "$VERIFICATION" 'scripts/test_raw_read_cycle_device.sh'
require_text "$VERIFICATION" 'scripts/test_raw_exit_hook_device.sh'
require_text "$VERIFICATION" 'scripts/probe_s4_abi_device.sh'
require_text "$VERIFICATION" 'M0_LOOPS=100'
require_text "$VERIFICATION" 'm4_ready'
require_text "$VERIFICATION" 'record_backend=visible_clone'
require_text "$VERIFICATION" 'M5 target-exit lifecycle test for M3, M4, raw two-PFN, S4 BRK-only, and S4 raw-step hold states.'
require_text "$REPLICA_PLAN" 's4 abi'
require_text "$REPLICA_PLAN" 'probe-only'
require_text "$S4_PLAN" 'wxshadow S4 BRK/Single-Step Plan'
require_text "$S4_PLAN" 'BRK-only slice'
require_text "$S4_PLAN" 'must not set'
require_text "$S4_PLAN" 'skip_origin'
require_text "$S4_PLAN" 'arbitrary address breakpoints'
require_text "$S4_PLAN" 'register mutation API'
require_text "$S4_PLAN" 'S4_BRK_OBSERVED'
require_text docs/kpm-research-plan.md 'M0-M5 plus raw two-PFN are complete for this pinned device and Lab App.'
require_text docs/kpm-compatibility-matrix.md 'Trusting `.kpm.exit` to block FolkPatch unload'

SCRIPTS='
scripts/build_kpm.sh
scripts/build_lab_app.sh
scripts/test_m0_environment_device.sh
scripts/test_m0_device.sh
scripts/test_m1_isolation_device.sh
scripts/test_m2_device.sh
scripts/test_m3_device.sh
scripts/test_m4_device.sh
scripts/test_m5_exit_probe_device.sh
scripts/test_m5_lifecycle_device.sh
scripts/test_m5_faults_device.sh
scripts/probe_shadow_page_capabilities_device.sh
scripts/probe_raw_pte_compat_device.sh
scripts/probe_raw_xom_device.sh
scripts/probe_s4_abi_device.sh
scripts/test_s4_brk_device.sh
scripts/test_s4_step_device.sh
scripts/test_s4_raw_step_device.sh
scripts/test_raw_device.sh
scripts/test_raw_read_cycle_device.sh
scripts/test_raw_gup_hide_device.sh
scripts/test_raw_gup_hook_device.sh
scripts/test_raw_fork_hook_device.sh
scripts/test_raw_fault_hook_device.sh
scripts/test_raw_fault_data_probe_device.sh
scripts/test_raw_exit_hook_device.sh
scripts/test_v1_device.sh
'

for script in $SCRIPTS; do
  require_file "$script"
  [ -x "$ROOT/$script" ] || fail "required script is not executable: $script"
  sh -n "$ROOT/$script" || fail "shell syntax is invalid: $script"
done

if grep -E 'pgtable_entry|flush_tlb_all|vmalle1is|[[:space:]]tlbi[[:space:]]' \
  "$ROOT/kpm/r0lab.c" \
  "$ROOT/kpm/r0lab_raw_compat.c" \
  "$ROOT/kpm/r0lab_raw.h" >/dev/null; then
  fail 'r0lab KPM contains a forbidden raw page-table or global-TLB primitive'
fi

if grep -E 'shadow_xom|SHADOW_XOM|xom_mode|activate_shadow_xom' \
  "$ROOT/kpm/r0lab.c" \
  "$ROOT/kpm/r0lab_raw_compat.c" \
  "$ROOT/kpm/r0lab_raw.h" >/dev/null; then
  fail 'r0lab KPM contains a disabled raw-xom execution path'
fi

if grep -F 'do_mprotect_pkey' "$ROOT/kpm/r0lab.c" >/dev/null; then
  fail 'r0lab KPM still depends on worker-side mprotect(+X)'
fi

require_text kpm/r0lab.c 'struct r0lab_page_record'
require_text kpm/r0lab.c 'record_backend=%s'
require_text kpm/r0lab.c 'R0LAB_PAGE_RECORD_M4_VISIBLE_CLONE'
require_text kpm/r0lab.c 'R0LAB_PAGE_RECORD_RAW_TWO_PFN'
require_text kpm/r0lab.c 's4_abi ready=%s'
require_text kpm/r0lab.c 'g_s4_brk_handler'
require_text kpm/r0lab.c 'g_s4_user_enable_single_step'
require_text kpm/r0lab.c 'R0LAB_EVENT_S4_BRK_OBSERVED'
require_text kpm/r0lab.c 'R0LAB_S4_HOOKED'
require_text kpm/r0lab.c 'next_state=%s'
require_text kpm/r0lab.c 's4_brk_ready target=%llx state=%s mode=brk_only skip_origin=0 single_step=0 pte_switch=0'
require_text kpm/r0lab.c 's4_step_ready target=%llx state=%s mode=step_only brk_skip_origin=1 step_skip_origin=1 single_step=1 pte_switch=0'
require_text kpm/r0lab.c 's4_step_observed target=%llx brk_events=%u step_events=%u enable_events=%u disable_events=%u state=%s mode=step_only brk_skip_origin=1 step_skip_origin=1 single_step=1 pte_switch=0'
require_text kpm/r0lab.c 's4_raw_step_ready target=%llx state=%s mode=raw_step brk_skip_origin=1 step_skip_origin=1 single_step=1 pte_switch=1 raw_state=shadow_active'
require_text kpm/r0lab.c 'pte_begin_events=%u pte_finish_events=%u state=%s mode=raw_step'
require_text kpm/r0lab_raw_compat.c 'int r0lab_raw_begin_stepping(struct r0lab_raw_page *page)'
require_text kpm/r0lab_raw_compat.c 'int r0lab_raw_finish_stepping(struct r0lab_raw_page *page)'
require_text kpm/r0lab_raw_compat.c 'int r0lab_raw_begin_read_cycle(struct r0lab_raw_page *page)'
require_text kpm/r0lab_raw_compat.c 'int r0lab_raw_finish_read_cycle(struct r0lab_raw_page *page)'
require_text kpm/r0lab_raw_compat.c 'int r0lab_raw_begin_gup_hide(struct r0lab_raw_page *page)'
require_text kpm/r0lab_raw_compat.c 'int r0lab_raw_finish_gup_hide(struct r0lab_raw_page *page)'
require_text kpm/r0lab_raw_compat.c 'int r0lab_raw_begin_fork_hide(struct r0lab_raw_page *page, void *oldmm)'
require_text kpm/r0lab_raw_compat.c 'int r0lab_raw_finish_fork_hide(struct r0lab_raw_page *page, void *oldmm)'
require_text kpm/r0lab_raw_compat.c 'int r0lab_raw_vma_matches(const struct r0lab_raw_page *page, void *vma_ptr,'
require_text kpm/r0lab_raw_compat.c 'int r0lab_raw_arm_source_uxn_only(struct r0lab_raw_page *page)'
require_text kpm/r0lab_raw_compat.c 'vma->vm_mm == (struct mm_struct *)page->mm'
require_text kpm/r0lab.c 'g_m3_page.raw.mm = mm'
require_text kpm/r0lab.c 'r0lab_raw_arm_source_uxn_only(&page->raw)'
require_text kpm/r0lab.c 'r0lab_raw_restore_original(&page->raw)'
require_text kpm/r0lab.c 'source_transition=pte_uxn pte_switch=1'
require_text kpm/r0lab_raw.h 'int r0lab_raw_vma_matches(const struct r0lab_raw_page *page, void *vma,'
require_text kpm/r0lab_raw_compat.c 'page->gup_saved_pte = page->shadow_rx_pte'
require_text kpm/r0lab_raw_compat.c 'page->fork_saved_pte = page->shadow_rx_pte'
require_text kpm/r0lab.c 'R0LAB_EVENT_RAW_GUP_BEGIN'
require_text kpm/r0lab.c 'R0LAB_EVENT_RAW_GUP_FINISH'
require_text kpm/r0lab.c 'raw_gup_%s result=%d state=%lu active_kind=%s gup_hide_active=%lu gup_begin_events=%lu gup_finish_events=%lu gup_hide_primitive=proven'
require_text kpm/r0lab.c 'R0LAB_EVENT_RAW_READ_CYCLE_BEGIN'
require_text kpm/r0lab.c 'R0LAB_EVENT_RAW_READ_CYCLE_FINISH'
require_text kpm/r0lab.c 'raw_read_cycle_begin result=%d state=%lu active_kind=%s read_cycle_active=%lu read_cycle_begin_events=%lu read_cycle_finish_events=%lu read_cycle=uxn_original_exec_resume trigger=supercall pte_switch=1 data_fault=absent exec_resume=pending'
require_text kpm/r0lab.c 'raw_read_cycle_status state=%lu active_kind=%s read_cycle_active=%lu read_cycle_begin_events=%lu read_cycle_finish_events=%lu read_cycle=uxn_original_exec_resume trigger=supercall pte_switch=%u data_fault=absent exec_resume=%s'
require_text kpm/r0lab.c 'R0LAB_EVENT_RAW_GUP_HOOK_BEGIN'
require_text kpm/r0lab.c 'R0LAB_EVENT_RAW_GUP_HOOK_FINISH'
require_text kpm/r0lab.c 'g_follow_page_pte = r0lab_lookup_first("follow_page_pte.cfi_jt"'
require_text kpm/r0lab.c 'hook_wrap5(hook_target, r0lab_raw_gup_pte_before'
require_text kpm/r0lab.c 'hook_wrap4(hook_target, r0lab_raw_gup_mask_before'
require_text kpm/r0lab.c 'r0lab_raw_vma_matches(&g_raw_page.raw, vma, address)'
require_text kpm/r0lab.c 'raw_gup_hook_ready symbol=%s follow_page_pte=%s follow_page_mask=%s installed=1 mode=%s target_mm_scoped=1 external_reader=1'
require_text kpm/r0lab.c 'raw_gup_hook_status symbol=%s follow_page_pte=%s follow_page_mask=%s installed=%u hook_begin_events=%u hook_finish_events=%u hook_failures=%u primitive_begin_events=%lu primitive_finish_events=%lu target_mm_scoped=1 external_reader=1'
require_text kpm/r0lab.c 'R0LAB_EVENT_RAW_FORK_HOOK_BEGIN'
require_text kpm/r0lab.c 'R0LAB_EVENT_RAW_FORK_HOOK_FINISH'
require_text kpm/r0lab.c 'g_dup_mmap = r0lab_lookup_first("dup_mmap.cfi_jt", "dup_mmap")'
require_text kpm/r0lab.c 'hook_wrap2(g_dup_mmap, r0lab_raw_fork_before'
require_text kpm/r0lab.c 'r0lab_raw_begin_fork_hide(&g_raw_page.raw, oldmm)'
require_text kpm/r0lab.c 'raw_fork_hook_ready symbol=dup_mmap installed=1 parent_pause=1 child_original_inherit=1 target_mm_scoped=1'
require_text kpm/r0lab.c 'raw_fork_hook_status symbol=%s installed=%u hook_begin_events=%u hook_finish_events=%u hook_failures=%u primitive_begin_events=%lu primitive_finish_events=%lu parent_pause=1 child_original_inherit=1 target_mm_scoped=1'
require_text kpm/r0lab.c 'R0LAB_EVENT_RAW_FAULT_HOOK_HIT'
require_text kpm/r0lab.c 'g_handle_mm_fault = r0lab_lookup_first("handle_mm_fault.cfi_jt"'
require_text kpm/r0lab.c 'hook_wrap4(g_handle_mm_fault, r0lab_raw_fault_before'
require_text kpm/r0lab.c 'raw_fault_hook_ready symbol=handle_mm_fault installed=1 target_mm_scoped=1 observe_only=1 pte_switch=0'
require_text kpm/r0lab.c 'raw_fault_hook_status symbol=%s installed=%u read_events=%u write_events=%u exec_events=%u hit_events=%u failures=%u target_mm_scoped=1 observe_only=1 pte_switch=0'
require_text kpm/r0lab.c 'raw_fault_probe_ready symbol=handle_mm_fault installed=1 armed=1 page=%llx reader_tgid=0 target_mm_scoped=1 observe_only=1 pte_switch=0 source=normal_anon_remote_gup'
require_text kpm/r0lab.c 'raw_fault_probe_reader_ready reader_tgid=%d armed=1 page=%llx target_mm_scoped=1 remote_only=1 observe_only=1 pte_switch=0 source=normal_anon_remote_gup'
require_text kpm/r0lab.c 'raw_fault_probe_status symbol=%s installed=%u armed=%u page=%llx reader_tgid=%d read_events=%u write_events=%u exec_events=%u hit_events=%u failures=%u target_mm_scoped=1 remote_only=1 observe_only=1 pte_switch=0 source=normal_anon_remote_gup'
require_text kpm/r0lab.c 'fault_hook=observe_only'
require_text kpm/r0lab.c 'hook_wrap3(g_s4_brk_handler, r0lab_s4_brk_before, NULL'
require_text kpm/r0lab.c 'hook_wrap3(g_s4_single_step_handler,'
require_text kpm/r0lab.c 'hook_unwrap(g_s4_brk_handler, r0lab_s4_brk_before, NULL)'
require_text kpm/r0lab.c 'hook_unwrap(g_s4_single_step_handler, r0lab_s4_step_before, NULL)'
require_text kpm/r0lab.c 'Step-mode only: consume BRK and run the fixed Lab instruction.'
require_text kpm/r0lab.c 'args->skip_origin = 1'
require_text lab-app/src/main/cpp/labprobe.c 'ready=\"%s\" observed=\"%s\"'
require_text lab-app/src/main/cpp/labprobe.c 'R0LAB_S4_CODE_BRK_7'
require_text lab-app/src/main/cpp/labprobe.c 's4 mode=brk-only failures=%d'
require_text lab-app/src/main/cpp/labprobe.c 's4 mode=step-only failures=%d'
require_text lab-app/src/main/cpp/labprobe.c 's4 mode=raw-step failures=%d'
require_text lab-app/src/main/cpp/labprobe.c 'raw mode=gup-hide failures=%d'
require_text lab-app/src/main/cpp/labprobe.c 'raw mode=read-cycle failures=%d trigger=supercall data_fault=absent pte_switch=1 exec_resume=1'
require_text lab-app/src/main/cpp/labprobe.c 'raw mode=gup-hook failures=%d reader=external'
require_text lab-app/src/main/cpp/labprobe.c 'raw mode=fork-hook failures=%d parent_pause=1 child_original_inherit=1'
require_text lab-app/src/main/cpp/labprobe.c 'raw mode=fault-hook failures=%d observe_only=1 target_mm_scoped=1 pte_switch=0'
require_text lab-app/src/main/cpp/labprobe.c 'raw mode=fault-data-probe failures=%d observe_only=1 target_mm_scoped=1 remote_only=1 pte_switch=0 data_fault_source=normal_anon_remote_gup'
require_text lab-app/src/main/cpp/labprobe.c 'strcmp(source_permissions, "r-xp")'
require_text lab-app/src/main/cpp/labprobe.c 'strcmp(during_permissions, "r-xp")'
require_text lab-app/src/main/cpp/labprobe.c '__NR_process_vm_readv'
require_text lab-app/src/main/cpp/labprobe.c 'child = fork()'
require_text lab-app/src/main/cpp/labprobe.c 'local_result.word = ((volatile uint32_t *)page)[0]'
require_text lab-app/src/main/cpp/labprobe.c 'parent_pid, &local_iov, 1'
require_text lab-app/src/main/cpp/labprobe.c 'raw fault probe reader 0x%llx %d'
require_text lab-app/src/main/cpp/labprobe.c 'r0lab_wait_for_reply("m4 ready"'
require_text lab-app/src/main/cpp/labprobe.c 'snprintf(observed_reply'
require_text lab-app/src/main/cpp/labprobe.c 'm5 mode=raw-hold'
require_text scripts/test_s4_brk_device.sh 's4 mode=brk-only failures=0'
require_text scripts/test_s4_brk_device.sh 'op=24 result=0'
require_text scripts/test_s4_brk_device.sh 'op=26 result=0'
require_text scripts/test_s4_step_device.sh 's4 mode=step-only failures=0'
require_text scripts/test_s4_step_device.sh 'op=25 result=0'
require_text scripts/test_s4_raw_step_device.sh 's4 mode=raw-step failures=0'
require_text scripts/test_s4_raw_step_device.sh 'hook_value=99'
require_text scripts/test_s4_raw_step_device.sh 'pte_begin_events=1'
require_text scripts/test_s4_raw_step_device.sh 'pte_finish_events=1'
require_text scripts/test_raw_gup_hide_device.sh 'raw mode=gup-hide failures=0'
require_text scripts/test_raw_gup_hide_device.sh 'original_view_after_shadow=proven'
require_text scripts/test_raw_gup_hide_device.sh 'gup_hide_primitive=proven'
require_text scripts/test_raw_read_cycle_device.sh 'raw mode=read-cycle failures=0'
require_text scripts/test_raw_read_cycle_device.sh 'original_read_word=52800540'
require_text scripts/test_raw_read_cycle_device.sh 'shadow_after_resume=52800c60'
require_text scripts/test_raw_read_cycle_device.sh 'read_cycle=uxn_original_exec_resume'
require_text scripts/test_raw_read_cycle_device.sh 'read_cycle_exec_resume=proven'
require_text scripts/test_raw_read_cycle_device.sh 'read_cycle_data_fault=absent'
require_text scripts/test_raw_read_cycle_device.sh 'op=36 result=0'
require_text scripts/test_raw_read_cycle_device.sh 'op=37 result=0'
require_text scripts/test_raw_gup_hook_device.sh 'raw mode=gup-hook failures=0'
require_text scripts/test_raw_gup_hook_device.sh 'reader=external'
require_text scripts/test_raw_gup_hook_device.sh 'gup_read_word=52800540'
require_text scripts/test_raw_gup_hook_device.sh 'target_mm_scoped=1'
require_text scripts/test_raw_gup_hook_device.sh 'external_reader=1'
require_text scripts/test_raw_gup_hook_device.sh 'child_exit=0'
require_text scripts/test_raw_gup_hook_device.sh 'hook_begin_events=1'
require_text scripts/test_raw_gup_hook_device.sh 'hook_finish_events=1'
require_text scripts/test_raw_gup_hook_device.sh 'op=29 result=0'
require_text scripts/test_raw_gup_hook_device.sh 'op=30 result=0'
require_text scripts/test_raw_fork_hook_device.sh 'raw mode=fork-hook failures=0'
require_text scripts/test_raw_fork_hook_device.sh 'parent_pause=1'
require_text scripts/test_raw_fork_hook_device.sh 'child_original_inherit=1'
require_text scripts/test_raw_fork_hook_device.sh 'child_word=52800540'
require_text scripts/test_raw_fork_hook_device.sh 'child_value=42'
require_text scripts/test_raw_fork_hook_device.sh 'fork_hide_primitive=proven'
require_text scripts/test_raw_fork_hook_device.sh 'op=31 result=0'
require_text scripts/test_raw_fork_hook_device.sh 'op=32 result=0'
require_text scripts/test_raw_fault_hook_device.sh 'raw mode=fault-hook failures=0'
require_text scripts/test_raw_fault_hook_device.sh 'observe_only=1'
require_text scripts/test_raw_fault_hook_device.sh 'target_mm_scoped=1'
require_text scripts/test_raw_fault_hook_device.sh 'pte_switch=0'
require_text scripts/test_raw_fault_hook_device.sh 'op=33 result=0'
require_text scripts/test_raw_fault_data_probe_device.sh 'raw mode=fault-data-probe failures=0'
require_text scripts/test_raw_fault_data_probe_device.sh 'data_fault_source=normal_anon_remote_gup'
require_text scripts/test_raw_fault_data_probe_device.sh 'raw_fault_probe_reader_ready'
require_text scripts/test_raw_fault_data_probe_device.sh 'target_mm_scoped=1'
require_text scripts/test_raw_fault_data_probe_device.sh 'remote_only=1'
require_text scripts/test_raw_fault_data_probe_device.sh 'fault_probe_read_events=1'
require_text scripts/test_raw_fault_data_probe_device.sh 'op=33 result=0'
require_text scripts/test_raw_exit_hook_device.sh 'raw mode=exit-hook-hold failures=0'
require_text scripts/test_raw_exit_hook_device.sh 'symbol=exit_mmap'
require_text scripts/test_raw_exit_hook_device.sh 'exit_hook_installed=1'
require_text scripts/test_raw_exit_hook_device.sh 'op=34 result=0'
require_text scripts/test_raw_exit_hook_device.sh 'op=22 result=0'
require_text scripts/test_v1_device.sh 'run_phase s4_step scripts/test_s4_step_device.sh'
require_text scripts/test_v1_device.sh 'run_phase s4_raw_step scripts/test_s4_raw_step_device.sh'
require_text scripts/test_v1_device.sh 'run_phase raw_gup_hide scripts/test_raw_gup_hide_device.sh'
require_text scripts/test_v1_device.sh 'run_phase raw_gup_hook scripts/test_raw_gup_hook_device.sh'
require_text scripts/test_v1_device.sh 'run_phase raw_fork_hook scripts/test_raw_fork_hook_device.sh'
require_text scripts/test_v1_device.sh 'run_phase raw_fault_hook scripts/test_raw_fault_hook_device.sh'
require_text scripts/test_v1_device.sh 'run_phase raw_fault_data_probe scripts/test_raw_fault_data_probe_device.sh'
require_text scripts/test_m5_lifecycle_device.sh 'm5_target_exit_phases=m3,m4,raw,s4'
require_text scripts/test_m5_lifecycle_device.sh "run_phase raw \"\$RAW_TOKEN\" 'm5 raw-hold' 'raw-hold' 'raw_slots' 22"
require_text scripts/test_m5_lifecycle_device.sh "run_phase s4 \"\$S4_TOKEN\" 'm5 s4-hold' 's4-hold' 's4_slots' 26"
require_text scripts/test_m5_lifecycle_device.sh "run_dual_phase s4_raw_step \"\$S4_RAW_STEP_TOKEN\" 'm5 s4-raw-step-hold'"
require_text scripts/test_m3_device.sh 'op=14 result=0'
require_text scripts/test_m4_device.sh 'record_backend=visible_clone record_state=source_uxn'
require_text scripts/test_m4_device.sh 'source_perms=r-xp/r-xp/r-xp clone_perms=r-xp/r-xp/r-xp'
require_text lab-app/src/main/cpp/labprobe.c 'm5 mode=s4-hold failures=%d'
require_text lab-app/src/main/cpp/labprobe.c 'm5 mode=s4-raw-step-hold failures=%d'
require_text lab-app/src/main/cpp/labprobe.c 'raw mode=exit-hook-hold failures=%d'
require_text lab-app/src/main/cpp/labprobe.c 'strncmp(args, "raw exit hook hold ", 19)'
require_text lab-app/src/main/cpp/labprobe.c 'strncmp(args, "m5 s4-raw-step-hold ", 20)'
require_text kpm/r0lab.c 'r0lab_s4_monitor_worker'
require_text kpm/r0lab.c 'r0lab_session_has_slots_locked'
require_text kpm/r0lab.c 'r0lab_session_monitor_loop'
require_text kpm/r0lab.c 'r0lab_raw_exit_mmap_before'
require_text kpm/r0lab.c 'g_raw_page.clearing && !g_raw_page.target_exiting'
require_text kpm/r0lab.c 'exit_hook=exit_mmap_observe'

printf '%s\n' 'v1_contract=pass scripts=27 raw_pte_kpm=lab_two_pfn s4_brk=brk_only s4_step=raw_pte_step raw_gup_hide=primitive raw_read_cycle=uxn_original_exec_resume raw_gup_hook=target_mm_external_reader raw_fork_hook=dup_mmap_parent_pause raw_fault_hook=handle_mm_fault_observe_only raw_fault_data_probe=normal_anon_remote_gup raw_exit_hook=exit_mmap_observe result=pass'
