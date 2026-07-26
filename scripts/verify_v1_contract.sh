#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
KPM_SOURCE="$ROOT/kpm/r0lab.c"
RAW_HEADER="$ROOT/kpm/r0lab_raw.h"
RAW_SOURCE="$ROOT/kpm/r0lab_raw_compat.c"
LAB_SOURCE="$ROOT/lab-app/src/main/cpp/labprobe.c"

fail() {
  printf '%s\n' "v1 contract verification failure: $*" >&2
  exit 1
}

require_file() {
  path=$1
  [ -f "$ROOT/$path" ] || fail "required file is missing: $path"
}

require_executable() {
  path=$1
  [ -x "$ROOT/$path" ] || fail "required script is not executable: $path"
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

function_body() {
  path=$1
  function_name=$2
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
  ' "$path"
}

require_function_text() {
  path=$1
  function_name=$2
  text=$3
  function_body "$path" "$function_name" | grep -F -- "$text" >/dev/null ||
    fail "required text is missing from ${path#"$ROOT/"} function $function_name: $text"
}

reject_function_text() {
  path=$1
  function_name=$2
  text=$3
  if function_body "$path" "$function_name" | grep -F -- "$text" >/dev/null; then
    fail "forbidden text is present in ${path#"$ROOT/"} function $function_name: $text"
  fi
}

require_line_before() {
  path=$1
  first=$2
  second=$3
  first_line=$(grep -n -F -- "$first" "$path" | head -n 1 | cut -d: -f1)
  second_line=$(grep -n -F -- "$second" "$path" | head -n 1 | cut -d: -f1)
  [ -n "$first_line" ] ||
    fail "first ordering text is missing from ${path#"$ROOT/"}: $first"
  [ -n "$second_line" ] ||
    fail "second ordering text is missing from ${path#"$ROOT/"}: $second"
  [ "$first_line" -lt "$second_line" ] ||
    fail "ordering mismatch in ${path#"$ROOT/"}: $first must appear before $second"
}

require_function_line_before() {
  path=$1
  function_name=$2
  first=$3
  second=$4
  body=$(function_body "$path" "$function_name")
  first_line=$(printf '%s\n' "$body" | grep -n -F -- "$first" | head -n 1 | cut -d: -f1)
  second_line=$(printf '%s\n' "$body" | grep -n -F -- "$second" | head -n 1 | cut -d: -f1)
  [ -n "$first_line" ] ||
    fail "first ordering text is missing from ${path#"$ROOT/"} function $function_name: $first"
  [ -n "$second_line" ] ||
    fail "second ordering text is missing from ${path#"$ROOT/"} function $function_name: $second"
  [ "$first_line" -lt "$second_line" ] ||
    fail "ordering mismatch in ${path#"$ROOT/"} function $function_name: $first must appear before $second"
}

check_script_syntax() {
  script=$1
  first_line=$(sed -n '1p' "$script")
  case "$first_line" in
    *bash*)
      bash -n "$script" ||
        fail "invalid bash syntax: ${script#"$ROOT/"}"
      ;;
    *)
      sh -n "$script" ||
        fail "invalid shell syntax: ${script#"$ROOT/"}"
      ;;
  esac
}

require_no_private_doc_dependency() {
  private_path_pattern=d'ocs/'
  matches=$(grep -R -n -F -- "$private_path_pattern" "$ROOT/scripts" 2>/dev/null || true)
  [ -z "$matches" ] ||
    fail "script still references removed private documentation: $matches"
}

REQUIRED_FILES="
kpm/Makefile
kpm/r0lab.c
kpm/r0lab.lds
kpm/r0lab_raw.h
kpm/r0lab_raw_compat.c
lab-app/AndroidManifest.xml
lab-app/src/main/cpp/Android.mk
lab-app/src/main/cpp/Application.mk
lab-app/src/main/cpp/labprobe.c
lab-app/src/main/java/dev/r0hook/lab/MainActivity.java
tools/r0lab_nonlab_control_probe.c
scripts/build_kpm.sh
scripts/build_lab_app.sh
scripts/test_v1_device.sh
scripts/probe_shadow_page_capabilities_device.sh
scripts/probe_raw_xom_device.sh
scripts/probe_raw_xom_preflight_device.sh
scripts/test_raw_xom_read_fault_device.sh
scripts/test_raw_abort_read_cycle_device.sh
scripts/test_raw_r3o_lifetime_device.sh
scripts/test_s4_descriptor_routing_device.sh
scripts/test_s4_descriptor_negative_device.sh
scripts/verify_d4_r3o_reference_lifetime.sh
scripts/verify_wxshadow_reference_source.sh
scripts/verify_wxshadow_reference_inventory.sh
scripts/test_wxshadow_reference_inventory_host.sh
"

for required in $REQUIRED_FILES; do
  require_file "$required"
done

script_count=0
for script in "$ROOT"/scripts/*.sh; do
  script_count=$((script_count + 1))
  require_executable "${script#"$ROOT/"}"
  check_script_syntax "$script"
done

require_no_private_doc_dependency

require_text "$KPM_SOURCE" 'struct r0lab_page_record'
require_text "$KPM_SOURCE" 'record_backend=%s'
require_text "$KPM_SOURCE" 'R0LAB_PAGE_RECORD_M4_VISIBLE_CLONE'
require_text "$KPM_SOURCE" 'R0LAB_PAGE_RECORD_RAW_TWO_PFN'
require_text "$KPM_SOURCE" '#define R0LAB_RAW_PAGE_SLOT_CAPACITY 2U'
require_text "$KPM_SOURCE" 'R0LAB_RAW_HOOK_ROUTE_ALLOW_SHADOW_XOM'
require_text "$KPM_SOURCE" 'struct r0lab_raw_page_table'
require_text "$KPM_SOURCE" 'r0lab_raw_page_slot_reset_locked'
require_text "$KPM_SOURCE" 'r0lab_raw_selected_page_locked'
require_text "$KPM_SOURCE" 'r0lab_raw_page_table_active_count_locked'
require_text "$KPM_SOURCE" 'r0lab_raw_slot_arm'
require_text "$KPM_SOURCE" 'raw slot arm '
require_text "$KPM_SOURCE" 'raw_slot_ready slot=%u'
require_text "$KPM_SOURCE" 'raw_slot_observed slot=%u'
require_text "$KPM_SOURCE" 'raw_slot_inspect slot=%u'
require_text "$KPM_SOURCE" 'raw_slot_patch_check_ok slot=%u'
require_text "$KPM_SOURCE" 'r0lab_raw_abort_hook_users_locked'
require_text "$KPM_SOURCE" 'raw_page_table_slots=%u raw_page_table_active=%u raw_selected_slot=%u'
require_text "$KPM_SOURCE" 'raw_slot_backend=%s raw_slot_state=%s raw_slot_source=%llx raw_slot_source_pfn=%llx raw_slot_shadow_pfn=%llx'
require_text "$KPM_SOURCE" 's4_abi ready=%s'
require_text "$KPM_SOURCE" 'g_s4_brk_handler'
require_text "$KPM_SOURCE" 'g_s4_user_enable_single_step'
require_text "$KPM_SOURCE" 'R0LAB_EVENT_S4_BRK_OBSERVED'
require_text "$KPM_SOURCE" 'R0LAB_S4_HOOKED'
require_text "$KPM_SOURCE" 'next_state=%s'
require_text "$KPM_SOURCE" 'enum r0lab_s4_descriptor_state'
require_text "$KPM_SOURCE" 'enum r0lab_s4_descriptor_mode'
require_text "$KPM_SOURCE" 'struct r0lab_s4_descriptor'
require_text "$KPM_SOURCE" 'r0lab_s4_descriptor_prepare_locked(&g_raw_page, raw_mm,'
require_text "$KPM_SOURCE" 'r0lab_s4_descriptor_brk_matches_locked'
require_text "$KPM_SOURCE" 'r0lab_s4_descriptor_step_matches_locked'
require_text "$KPM_SOURCE" 'uint32_t pte_begin_events;'
require_text "$KPM_SOURCE" 'uint32_t pte_finish_events;'
require_text "$KPM_SOURCE" 'r0lab_s4_descriptor_find_brk_locked'
require_text "$KPM_SOURCE" 'r0lab_s4_descriptor_find_step_locked'
require_text "$KPM_SOURCE" 'r0lab_s4_descriptor_has_armed_locked'
require_text "$KPM_SOURCE" 'r0lab_s4_descriptor_clear_step_tid_locked'
require_text "$KPM_SOURCE" 'r0lab_s4_restore_descriptor_pages'
require_text "$KPM_SOURCE" 'r0lab_s4_descriptor_routing_arm'
require_text "$KPM_SOURCE" 'r0lab_s4_descriptor_routing_observed'
require_text "$KPM_SOURCE" 'r0lab_s4_descriptor_routing_clear'
require_text "$KPM_SOURCE" 'r0lab_s4_descriptor_negative_probe'
require_text "$KPM_SOURCE" 'descriptor->slot_id != page->slot_id'
require_text "$KPM_SOURCE" 's4_descriptor_negative_observed'
require_text "$KPM_SOURCE" 'bad_register_index_rejected'
require_text "$KPM_SOURCE" 'wrong_step_tid_rejected'
require_text "$KPM_SOURCE" 's4 descriptor routing arm '
require_text "$KPM_SOURCE" 's4 descriptor routing observed '
require_text "$KPM_SOURCE" 's4 descriptor routing clear '
require_text "$KPM_SOURCE" 's4 descriptor negative probe '

require_text "$RAW_HEADER" 'struct r0lab_raw_live_pte_snapshot'
require_text "$RAW_HEADER" 'identity_match'
require_text "$RAW_HEADER" 'r0lab_raw_saved_identity_matches'
require_text "$RAW_HEADER" 'r0lab_raw_snapshot_live_pte'
require_function_text "$RAW_SOURCE" r0lab_raw_saved_identity_matches 'page->source_pfn'
require_function_text "$RAW_SOURCE" r0lab_raw_saved_identity_matches 'page->shadow_pfn'
require_function_text "$RAW_SOURCE" r0lab_raw_saved_identity_matches 'r0lab_raw_active_pte_identity_matches'
require_function_text "$RAW_SOURCE" r0lab_raw_snapshot_live_pte 'mmap_read_lock(mm);'
require_function_text "$RAW_SOURCE" r0lab_raw_snapshot_live_pte 'r0lab_raw_walk_locked'
require_function_text "$RAW_SOURCE" r0lab_raw_snapshot_live_pte 'live_pte = READ_ONCE(*ptep);'
require_function_text "$RAW_SOURCE" r0lab_raw_snapshot_live_pte 'vma->vm_flags'
require_function_text "$RAW_SOURCE" r0lab_raw_snapshot_live_pte 'source_identity_match'
require_function_text "$RAW_SOURCE" r0lab_raw_snapshot_live_pte 'shadow_identity_match'
require_function_text "$RAW_SOURCE" r0lab_raw_snapshot_live_pte 'identity_match'
reject_function_text "$RAW_SOURCE" r0lab_raw_snapshot_live_pte 'r0lab_raw_replace_locked'
reject_function_text "$RAW_SOURCE" r0lab_raw_snapshot_live_pte 'flush_tlb'
reject_function_text "$RAW_SOURCE" r0lab_raw_snapshot_live_pte 'set_pte_at'
reject_function_text "$RAW_SOURCE" r0lab_raw_snapshot_live_pte 'memset'

require_text "$LAB_SOURCE" 'r0lab_s4_descriptor_routing_run'
require_text "$LAB_SOURCE" 'r0lab_s4_descriptor_negative_run'
require_text "$LAB_SOURCE" '"s4 descriptor routing arm 0x%llx 0x%llx 0x%llx"'
require_text "$LAB_SOURCE" '"s4 descriptor negative probe 0x%llx"'
require_text "$LAB_SOURCE" '"s4 descriptor routing observed 0x%llx"'
require_text "$LAB_SOURCE" '"s4 descriptor routing clear 0x%llx"'
require_text "$LAB_SOURCE" 'hook1 = ((int (*)(void))page1)()'
require_text "$LAB_SOURCE" 'hook0 = ((int (*)(void))page0)()'
require_text "$LAB_SOURCE" 'r0lab_raw_hold_lifetime'
require_text "$LAB_SOURCE" 'raw raw-hold lifetime '
require_text "$LAB_SOURCE" 'raw mode=raw-hold-lifetime failures=%d'
require_text "$LAB_SOURCE" 'target_state=%s slots=%u raw_slots=%u page_records=%u'
require_text "$LAB_SOURCE" 'r0lab_raw_hold_live_pte'
require_text "$LAB_SOURCE" 'raw raw-hold live-pte '
require_text "$LAB_SOURCE" 'raw mode=raw-hold-live-pte failures=%d'
require_text "$LAB_SOURCE" 'identity_match=1'

require_text "$ROOT/scripts/test_s4_descriptor_routing_device.sh" 's4 descriptor routing $TOKEN'
require_text "$ROOT/scripts/test_s4_descriptor_routing_device.sh" 'slot0_pte_begin_events=1'
require_text "$ROOT/scripts/test_s4_descriptor_routing_device.sh" 'slot1_pte_finish_events=1'
require_text "$ROOT/scripts/test_s4_descriptor_negative_device.sh" 's4 descriptor negative $TOKEN'
require_text "$ROOT/scripts/test_s4_descriptor_negative_device.sh" 'reject_checks=11'
require_text "$ROOT/scripts/test_s4_descriptor_negative_device.sh" 'wrong_brk_slot_rejected=1'
require_text "$ROOT/scripts/test_s4_descriptor_negative_device.sh" 'bad_register_value_rejected=1'
require_text "$ROOT/scripts/probe_shadow_page_capabilities_device.sh" 'ordinary_xom_read_path=blocked reason=user_xom_read_fault_absent'
require_text "$ROOT/scripts/probe_shadow_page_capabilities_device.sh" 'raw_xom_permission_read_path=blocked reason=disabled_after_kernel_panic_requires_separate_preflight'
require_text "$ROOT/scripts/probe_shadow_page_capabilities_device.sh" 'controlled_dabt_read_cycle=separate_smoke scripts/test_raw_abort_read_cycle_device.sh'
require_text "$ROOT/scripts/probe_shadow_page_capabilities_device.sh" 'result=blocked reason=hardware_xom_route_unavailable'

require_text "$ROOT/scripts/verify_wxshadow_reference_source.sh" 'WXSHADOW_REFERENCE_SOURCE_not_set'
require_text "$ROOT/scripts/verify_wxshadow_reference_inventory.sh" 'reference_inventory_coverage_not_set'
require_text "$ROOT/scripts/test_wxshadow_reference_inventory_host.sh" 'duplicate-function'
require_text "$ROOT/scripts/test_wxshadow_reference_inventory_host.sh" 'missing-function'
require_text "$ROOT/scripts/test_wxshadow_reference_inventory_host.sh" 'unknown-family'
require_text "$ROOT/scripts/test_wxshadow_reference_inventory_host.sh" 'missing-matrix-family'

require_function_line_before "$KPM_SOURCE" r0lab_raw_arm_worker \
  'r0lab_raw_abort_hook_acquire(page);' \
  'r0lab_raw_arm_source_uxn(&page->raw);'
reject_text "$KPM_SOURCE" 'raw_xom_unsafe'

if grep -E 'flush_tlb_all|vmalle1is|[[:space:]]tlbi[[:space:]]' \
  "$KPM_SOURCE" "$RAW_SOURCE" >/dev/null; then
  fail "raw path must not use global TLB invalidation"
fi

printf 'v1_contract=pass public_contract=1 scripts=%s private_doc_dependency=0 result=pass\n' \
  "$script_count"
