#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
KPM_SOURCE="$ROOT/kpm/r0lab.c"
RAW_SOURCE="$ROOT/kpm/r0lab_raw_compat.c"
RAW_HEADER="$ROOT/kpm/r0lab_raw.h"
LAB_SOURCE="$ROOT/lab-app/src/main/cpp/labprobe.c"
PLAN="$ROOT/docs/wxshadow-f4.6-d4-r3o-reference-lifetime-parity-fix-plan.md"
DEVICE_SCRIPT="$ROOT/scripts/test_raw_r3o_lifetime_device.sh"
NDK_TOOLCHAIN=/Users/ivory/Library/Android/sdk/ndk/29.0.14206865/toolchains/llvm/prebuilt/darwin-x86_64/bin
NM="$NDK_TOOLCHAIN/llvm-nm"
READELF="$NDK_TOOLCHAIN/llvm-readelf"
MODULE="$ROOT/kpm/build/r0lab-m1.kpm"

fail() {
  printf '%s\n' "D4-R3o verification failure: $*" >&2
  exit 1
}

require_text() {
  path=$1
  text=$2
  grep -F -- "$text" "$path" >/dev/null ||
    fail "required text is missing from ${path#"$ROOT/"}: $text"
}

reject_text() {
  path=$1
  text=$2
  if grep -F -- "$text" "$path" >/dev/null; then
    fail "forbidden text is present in ${path#"$ROOT/"}: $text"
  fi
}

require_function_text() {
  path=$1
  function_name=$2
  text=$3
  awk -v function_name="$function_name" '
    !candidate && !in_function && index($0, function_name "(") {
      candidate = 1
    }
    candidate && index($0, ";") && !index($0, "{") {
      candidate = 0
      next
    }
    candidate && index($0, "{") {
      candidate = 0
      in_function = 1
    }
    in_function { print }
    in_function && /^}/ { exit }
  ' "$path" | grep -F -- "$text" >/dev/null ||
    fail "required text is missing from ${path#"$ROOT/"} function $function_name: $text"
}

reject_function_text() {
  path=$1
  function_name=$2
  text=$3
  if awk -v function_name="$function_name" '
    !candidate && !in_function && index($0, function_name "(") {
      candidate = 1
    }
    candidate && index($0, ";") && !index($0, "{") {
      candidate = 0
      next
    }
    candidate && index($0, "{") {
      candidate = 0
      in_function = 1
    }
    in_function { print }
    in_function && /^}/ { exit }
  ' "$path" | grep -F -- "$text" >/dev/null; then
    fail "forbidden text is present in ${path#"$ROOT/"} function $function_name: $text"
  fi
}

require_function_sequence3() {
  path=$1
  function_name=$2
  first=$3
  second=$4
  third=$5
  awk -v function_name="$function_name" -v first="$first" \
      -v second="$second" -v third="$third" '
    !candidate && !in_function && index($0, function_name "(") {
      candidate = 1
    }
    candidate && index($0, ";") && !index($0, "{") {
      candidate = 0
      next
    }
    candidate && index($0, "{") {
      candidate = 0
      in_function = 1
    }
    in_function && state == 0 && index($0, first) { state = 1; next }
    in_function && state == 1 && index($0, second) { state = 2; next }
    in_function && state == 2 && index($0, third) { state = 3 }
    in_function && /^}/ { exit }
    END { exit state == 3 ? 0 : 1 }
  ' "$path" ||
    fail "required sequence is missing from ${path#"$ROOT/"} function $function_name"
}

[ -f "$PLAN" ] || fail "R3o plan is missing"
[ -f "$KPM_SOURCE" ] || fail "KPM source is missing"
[ -f "$RAW_SOURCE" ] || fail "raw compatibility source is missing"
[ -f "$RAW_HEADER" ] || fail "raw compatibility header is missing"
[ -f "$LAB_SOURCE" ] || fail "Lab native source is missing"
[ -f "$DEVICE_SCRIPT" ] || fail "R3o device ladder is missing"

require_text "$PLAN" 'R3o uses the kernel'
require_text "$PLAN" 'Status: complete.'
require_text "$PLAN" 'mmgrab(mm)'
require_text "$PLAN" 'mmdrop(mm)'
require_text "$PLAN" 'before the first source-UXN PTE transition'
require_text "$PLAN" '__get_free_pages(GFP_KERNEL, 0)'
require_text "$PLAN" 'synchronizes the full page after the initial copy/seed'
require_text "$PLAN" 'the final full-abort callback resident'
require_text "$PLAN" 'ordinary owner-process DABTs'
require_text "$PLAN" 'R3O_STAGE4_HOLD_SECONDS'
require_text "$PLAN" 'source-tagged six-stage ladder passed'
require_text "$PLAN" \
  'wxshadow-v2-f46-d4-r3o-stage4-runtime-gate-candidate-20260724'
require_text "$PLAN" \
  'build/evidence/raw-r3o-stage-5-20260724-195753.log'
require_text "$PLAN" 'Stage 6 ended with an empty FolkPatch module list.'
require_text "$PLAN" 'raw raw-hold clear <token>'
require_text "$PLAN" 'r0lab-r3o:'
require_text "$PLAN" 'scripts/test_raw_r3o_lifetime_device.sh'

require_text "$KPM_SOURCE" 'bool mm_count_owned;'
require_text "$KPM_SOURCE" 'static hook_chain3_callback g_raw_abort_resident_callback;'
require_text "$KPM_SOURCE" 'static bool g_raw_exit_hook_resident;'
require_text "$KPM_SOURCE" 'raw_abort_resident=%u raw_exit_resident=%u'
require_text "$KPM_SOURCE" 'mm_ref=%s exit_hook_installed=%u'

require_function_sequence3 "$KPM_SOURCE" r0lab_raw_slot_arm \
  'r0lab_raw_mmgrab(mm);' 'slot->raw.mm = mm;' 'g_mmput(mm);'
require_function_text "$KPM_SOURCE" r0lab_raw_slot_arm \
  'slot->mm_count_owned = true;'
require_function_text "$KPM_SOURCE" r0lab_raw_slot_arm \
  'r0lab-r3o: slot_owned'

require_function_sequence3 "$KPM_SOURCE" r0lab_raw_arm_worker \
  'r0lab_raw_exit_hook_acquire(page);' \
  'r0lab_raw_abort_hook_acquire(page);' \
  'r0lab_raw_arm_source_uxn(&page->raw);'
require_function_text "$KPM_SOURCE" r0lab_raw_arm_worker \
  'r0lab-r3o: exit_protection_ready'
require_function_text "$KPM_SOURCE" r0lab_raw_arm_worker \
  'r0lab-r3o: source_uxn_installed'
require_function_text "$KPM_SOURCE" r0lab_raw_arm_worker \
  'r0lab-r3o: arm_complete'

require_function_text "$KPM_SOURCE" r0lab_raw_prepare_shadow \
  'g_get_free_pages('
require_function_text "$KPM_SOURCE" r0lab_raw_prepare_shadow \
  'r0lab_raw_abi_gfp_kernel()'
require_function_text "$KPM_SOURCE" r0lab_raw_prepare_shadow \
  'r0lab_runtime_sync_icache_aliases('
require_function_text "$KPM_SOURCE" r0lab_raw_prepare_shadow \
  'r0lab-r3o: shadow_ready'
reject_function_text "$KPM_SOURCE" r0lab_raw_prepare_shadow 'g_vmalloc'
reject_function_text "$KPM_SOURCE" r0lab_raw_prepare_shadow 'g_vfree'

require_function_text "$KPM_SOURCE" r0lab_raw_free_shadow_page \
  'g_free_pages((unsigned long)shadow_kaddr, 0);'
require_function_text "$KPM_SOURCE" r0lab_raw_free_shadow_page \
  'r0lab-r3o: shadow_free'
require_function_text "$KPM_SOURCE" r0lab_raw_release_mm_ref \
  'r0lab_raw_mmdrop(mm);'
require_function_text "$KPM_SOURCE" r0lab_raw_release_mm_ref 'g_mmput(mm);'
require_function_text "$KPM_SOURCE" r0lab_raw_release_mm_ref \
  'r0lab-r3o: mm_release'
require_function_text "$KPM_SOURCE" r0lab_raw_reset_exited_page \
  'r0lab_raw_release_mm_ref(mm, mm_count_owned, mm_users_owned);'
reject_function_text "$KPM_SOURCE" r0lab_raw_cleanup_exited_mm 'g_mmput(mm);'

require_function_text "$KPM_SOURCE" r0lab_raw_exit_hook_release \
  'page->exit_hook_installed = false;'
reject_function_text "$KPM_SOURCE" r0lab_raw_exit_hook_release \
  'r0lab_hook_detach'
require_function_text "$KPM_SOURCE" r0lab_raw_abort_hook_release \
  'callback != r0lab_raw_before_abort_compact;'
require_function_text "$KPM_SOURCE" r0lab_raw_abort_hook_callback \
  'return r0lab_raw_before_abort_compact;'
require_function_text "$KPM_SOURCE" r0lab_raw_before_abort_compact \
  'r0lab_raw_before_abort_full_iabt(args, udata);'
require_function_text "$KPM_SOURCE" r0lab_raw_before_abort_compact \
  'r0lab_raw_dabt_route_armed_unlocked()'
require_function_text "$KPM_SOURCE" r0lab_raw_before_abort_compact \
  'r0lab_raw_before_abort(args, udata);'
require_function_text "$KPM_SOURCE" r0lab_raw_dabt_route_armed_unlocked \
  'page->abort_read_cycle_armed'
require_function_text "$KPM_SOURCE" r0lab_raw_dabt_route_armed_unlocked \
  'page->abort_write_release_armed'
require_function_text "$KPM_SOURCE" r0lab_raw_dabt_route_armed_unlocked \
  'page->abort_probe_armed'
require_function_text "$KPM_SOURCE" r0lab_raw_before_abort \
  '!g_initialized'
require_function_text "$KPM_SOURCE" r0lab_raw_exit_mmap_before \
  '!g_initialized'
require_function_text "$KPM_SOURCE" r0lab_raw_exit_mmap_before \
  'r0lab-r3o: exit_restore_begin'
require_function_text "$KPM_SOURCE" r0lab_raw_exit_mmap_before \
  'r0lab-r3o: exit_restore_end'
require_function_text "$KPM_SOURCE" r0lab_raw_clear_worker \
  'r0lab-r3o: clear_begin'
require_function_text "$KPM_SOURCE" r0lab_raw_clear_worker \
  'r0lab-r3o: clear_end'
reject_function_text "$KPM_SOURCE" r0lab_raw_before_abort 'r0lab-r3o:'
require_function_text "$KPM_SOURCE" r0lab_exit \
  'g_initialized = false;'
require_function_text "$KPM_SOURCE" r0lab_exit \
  'r0lab_hook_detach(g_do_mem_abort, abort_callback, NULL);'
require_function_text "$KPM_SOURCE" r0lab_exit \
  'r0lab_hook_detach(g_exit_mmap, r0lab_raw_exit_mmap_before, NULL);'

require_text "$RAW_HEADER" 'unsigned long r0lab_raw_abi_gfp_kernel(void);'
require_text "$RAW_HEADER" 'void r0lab_raw_mmgrab(void *mm);'
require_text "$RAW_HEADER" 'void r0lab_raw_mmdrop(void *mm);'
require_function_text "$RAW_SOURCE" r0lab_raw_shadow_pfn_from_kaddr \
  'virt_to_page(page->shadow_kaddr)'
reject_function_text "$RAW_SOURCE" r0lab_raw_shadow_pfn_from_kaddr \
  'virt_addr_valid'
reject_function_text "$RAW_SOURCE" r0lab_raw_shadow_pfn_from_kaddr \
  'vmalloc_to_page'
require_function_sequence3 "$RAW_SOURCE" r0lab_raw_replace_locked \
  'ptep_get_and_clear(mm, address, ptep);' \
  'flush_tlb_page(vma, address);' \
  'set_pte_at(mm, address, ptep, replacement);'
require_function_text "$RAW_SOURCE" r0lab_raw_replace_locked \
  'r0lab_runtime_sync_icache_aliases('

require_function_text "$LAB_SOURCE" r0lab_raw_hold_lifetime_common \
  'exit_hook_installed[index] != 1'
require_function_text "$LAB_SOURCE" r0lab_raw_hold_lifetime_common \
  'exit_hook_installed=%d/%d'
require_function_sequence3 "$LAB_SOURCE" r0lab_raw_hold_clear \
  'r0lab_raw_slot_clear(token, index' \
  'mprotect(pages[index], page_size,' \
  'restored[index] = ((int (*)(void))pages[index])();'
require_function_sequence3 "$LAB_SOURCE" r0lab_raw_hold_clear \
  'restored[index] = ((int (*)(void))pages[index])();' \
  'close_rc = r0lab_control_raw(command, reply, sizeof(reply));' \
  'munmap(pages[0], page_size);'
require_function_text "$LAB_SOURCE" r0lab_raw_hold_clear \
  'memset(&g_r0lab_m5_hold, 0, sizeof(g_r0lab_m5_hold));'
require_text "$LAB_SOURCE" 'raw raw-hold clear '

sh -n "$DEVICE_SCRIPT" || fail "R3o device ladder has invalid shell syntax"
require_text "$DEVICE_SCRIPT" 'R3O_STAGE=1..6 is required'
require_text "$DEVICE_SCRIPT" 'R3O_NEXT_STAGE=$next_stage'
require_text "$DEVICE_SCRIPT" 'source commit changed after stage 1'
require_text "$DEVICE_SCRIPT" 'boot ID changed:'
require_text "$DEVICE_SCRIPT" 'warn_count changed:'
require_text "$DEVICE_SCRIPT" 'wait_for_failure_device'
require_text "$DEVICE_SCRIPT" 'failure_pstore_console_begin'
require_text "$DEVICE_SCRIPT" 'R3O_LOG_START_UPTIME=$(read_uptime)'
require_text "$DEVICE_SCRIPT" \
  'LC_ALL=C awk -v start="$R3O_LOG_START_UPTIME"'
reject_text "$DEVICE_SCRIPT" 'R3O_LOG_BEFORE_COUNT'
require_text "$DEVICE_SCRIPT" 'raw raw-hold clear $TOKEN_2'
require_text "$DEVICE_SCRIPT" "'states=2/0'"
require_text "$DEVICE_SCRIPT" 'require_exit_only_callbacks_status'
require_text "$DEVICE_SCRIPT" 'raw_abort_resident=0'
require_text "$DEVICE_SCRIPT" 'raw raw-hold clear $TOKEN_3'
require_text "$DEVICE_SCRIPT" 'raw raw-hold clear $TOKEN_4'
require_text "$DEVICE_SCRIPT" 'STAGE4_HOLD_SECONDS=${R3O_STAGE4_HOLD_SECONDS:-60}'
require_text "$DEVICE_SCRIPT" 'poll_runtime_continuity "$STAGE4_HOLD_SECONDS"'
require_text "$DEVICE_SCRIPT" 'system_server PID changed during runtime poll'
require_text "$DEVICE_SCRIPT" 'Lab PID changed during runtime poll'
require_text "$DEVICE_SCRIPT" \
  'crash buffer became non-empty during active raw hold'
require_text "$DEVICE_SCRIPT" 'exit_restore_begin slot='
require_text "$DEVICE_SCRIPT" 'count_contains "$EVENTS" '\''op=34 result=0'\'' 2'
require_text "$DEVICE_SCRIPT" 'unload_module || fail "stage 6 first unload failed"'
require_text "$DEVICE_SCRIPT" 'load_module || fail "stage 6 reload failed"'
reject_text "$DEVICE_SCRIPT" 'adb reboot'
reject_text "$DEVICE_SCRIPT" 'adb_device reboot'

"$ROOT/scripts/build_kpm.sh" >/dev/null
[ -f "$MODULE" ] || fail "KPM build did not produce $MODULE"
"$ROOT/scripts/build_lab_app.sh" >/dev/null
[ -f "$ROOT/build/lab-app/r0lab-debug.apk" ] ||
  fail "Lab build did not produce r0lab-debug.apk"

for section in .kpm.info .kpm.init .kpm.ctl0 .kpm.exit; do
  "$READELF" -S "$MODULE" | grep -F -- "$section" >/dev/null ||
    fail "missing KPM section: $section"
done

"$NM" -u "$MODULE" | awk '{ print $2 }' | while IFS= read -r symbol; do
  case "$symbol" in
    compat_copy_to_user | current_uid | hook_unwrap_remove | hook_wrap | \
    kallsyms_lookup_name | kf_memcpy | kf_memset | kf_snprintf | \
    kf_strchr | kf_strcmp | kf_strlen | kf_strncmp | kf_strnlen | \
    kf_strstr | printk | sp_el0_is_current | \
    sp_el0_is_thread_info | task_in_thread_info_offset | thread_info_in_task | \
    thread_size)
      ;;
    *)
      fail "unexpected undefined symbol in KPM: $symbol"
      ;;
  esac
done

"$NM" --defined-only "$MODULE" | grep -F ' T __mmdrop' >/dev/null ||
  fail "KPM does not provide the __mmdrop bridge"
"$NM" --defined-only "$MODULE" |
  grep -F ' T r0lab_runtime_sync_icache_aliases' >/dev/null ||
  fail "KPM does not provide the cache-sync bridge"

printf '%s\n' \
  'D4-R3o reference-lifetime host verification passed: ownership, hook order, compact IABT/DABT admission, direct shadow page, cache sync, BBM, bounded logs, 60-second PID/crash gate, Lab clear, staged ladder, builds, sections, and symbols'
