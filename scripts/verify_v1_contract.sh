#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

fail() {
  printf '%s\n' "v1 contract verification failure: $*" >&2
  exit 1
}

verify_d4_r3j_plan_packet_paths() {
  changed=$(
    (
      git -C "$ROOT" diff --name-only
      git -C "$ROOT" diff --cached --name-only
      git -C "$ROOT" ls-files --others --exclude-standard
    ) | LC_ALL=C sort -u
  ) || fail "could not enumerate D4-R3j plan-packet changes"

  for path in $changed; do
    case "$path" in
      docs/wxshadow-development-sequence.md | \
      docs/wxshadow-f4.6-d4-r3j-inflight-accounting-plan.md | \
      docs/wxshadow-final-experiment-roadmap.md | \
      docs/wxshadow-reference-function-coverage.md | \
      scripts/verify_d4_r3j_plan_packet.sh | \
      scripts/verify_v1_contract.sh)
        ;;
      *)
        fail "D4-R3j plan packet contains a forbidden changed path: $path"
        ;;
    esac
  done
}

verify_d4_r3k_plan_packet_paths() {
  changed=$(
    (
      git -C "$ROOT" diff --name-only
      git -C "$ROOT" diff --cached --name-only
      git -C "$ROOT" ls-files --others --exclude-standard
    ) | LC_ALL=C sort -u
  ) || fail "could not enumerate D4-R3k plan-packet changes"

  for path in $changed; do
    case "$path" in
      docs/wxshadow-development-sequence.md | \
      docs/wxshadow-f4.6-d4-r3k-visible-inflight-lifetime-plan.md | \
      docs/wxshadow-final-experiment-roadmap.md | \
      docs/wxshadow-reference-function-coverage.md | \
      scripts/verify_d4_r3k_plan_packet.sh | \
      scripts/verify_v1_contract.sh)
        ;;
      *)
        fail "D4-R3k plan packet contains a forbidden changed path: $path"
        ;;
    esac
  done
}

verify_d4_r3l_plan_packet_paths() {
  changed=$(
    (
      git -C "$ROOT" diff --name-only
      git -C "$ROOT" diff --cached --name-only
      git -C "$ROOT" ls-files --others --exclude-standard
    ) | LC_ALL=C sort -u
  ) || fail "could not enumerate D4-R3l plan-packet changes"

  for path in $changed; do
    case "$path" in
      docs/wxshadow-development-sequence.md | \
      docs/wxshadow-f4.6-d4-r3l-readonly-iabt-route-plan.md | \
      docs/wxshadow-final-experiment-roadmap.md | \
      docs/wxshadow-reference-function-coverage.md | \
      scripts/verify_d4_r3l_plan_packet.sh | \
      scripts/verify_v1_contract.sh)
        ;;
      *)
        fail "D4-R3l plan packet contains a forbidden changed path: $path"
        ;;
    esac
  done
}

verify_d4_r3m_plan_packet_paths() {
  changed=$(
    (
      git -C "$ROOT" diff --name-only
      git -C "$ROOT" diff --cached --name-only
      git -C "$ROOT" ls-files --others --exclude-standard
    ) | LC_ALL=C sort -u
  ) || fail "could not enumerate D4-R3m plan-packet changes"

  for path in $changed; do
    case "$path" in
      docs/wxshadow-development-sequence.md | \
      docs/wxshadow-f4.6-d4-r3m-iabt-transition-restore-abi-plan.md | \
      docs/wxshadow-final-experiment-roadmap.md | \
      docs/wxshadow-reference-function-coverage.md | \
      scripts/verify_d4_r3m_plan_packet.sh | \
      scripts/verify_v1_contract.sh)
        ;;
      *)
        fail "D4-R3m plan packet contains a forbidden changed path: $path"
        ;;
    esac
  done
}

verify_d4_r3n_plan_packet_paths() {
  changed=$(
    (
      git -C "$ROOT" diff --name-only
      git -C "$ROOT" diff --cached --name-only
      git -C "$ROOT" ls-files --others --exclude-standard
    ) | LC_ALL=C sort -u
  ) || fail "could not enumerate D4-R3n plan-packet changes"

  for path in $changed; do
    case "$path" in
      docs/wxshadow-development-sequence.md | \
      docs/wxshadow-f4.6-d4-r3n-full-abort-lifecycle-plan.md | \
      docs/wxshadow-final-experiment-roadmap.md | \
      docs/wxshadow-reference-function-coverage.md | \
      scripts/verify_d4_r3n_plan_packet.sh | \
      scripts/verify_v1_contract.sh)
        ;;
      *)
        fail "D4-R3n plan packet contains a forbidden changed path: $path"
        ;;
    esac
  done
}

if [ "${D4_R3J_PLAN_PACKET_STRICT:-0}" = 1 ]; then
  verify_d4_r3j_plan_packet_paths
fi

if [ "${D4_R3K_PLAN_PACKET_STRICT:-0}" = 1 ]; then
  verify_d4_r3k_plan_packet_paths
fi

if [ "${D4_R3L_PLAN_PACKET_STRICT:-0}" = 1 ]; then
  verify_d4_r3l_plan_packet_paths
fi

if [ "${D4_R3M_PLAN_PACKET_STRICT:-0}" = 1 ]; then
  verify_d4_r3m_plan_packet_paths
fi

if [ "${D4_R3N_PLAN_PACKET_STRICT:-0}" = 1 ]; then
  verify_d4_r3n_plan_packet_paths
fi

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

reject_text() {
  path=$1
  text=$2
  if grep -F -- "$text" "$ROOT/$path" >/dev/null; then
    fail "forbidden contract text is present in $path: $text"
  fi
}

require_function_text() {
  path=$1
  function_name=$2
  text=$3
  awk -v function_name="$function_name" '
    index($0, function_name "(") { in_function = 1 }
    in_function { print }
    in_function && /^}/ { exit }
  ' "$ROOT/$path" | grep -F -- "$text" >/dev/null ||
    fail "required contract text is missing from $path function $function_name: $text"
}

reject_function_text() {
  path=$1
  function_name=$2
  text=$3
  if awk -v function_name="$function_name" '
    index($0, function_name "(") { in_function = 1 }
    in_function { print }
    in_function && /^}/ { exit }
  ' "$ROOT/$path" | grep -F -- "$text" >/dev/null; then
    fail "forbidden contract text is present in $path function $function_name: $text"
  fi
}

require_function_count() {
  path=$1
  function_name=$2
  text=$3
  expected=$4
  actual=$(
    awk -v function_name="$function_name" -v text="$text" '
      index($0, function_name "(") { in_function = 1 }
      in_function {
        line = $0
        while ((position = index(line, text)) != 0) {
          ++count
          line = substr(line, position + length(text))
        }
      }
      in_function && /^}/ {
        print count + 0
        exit
      }
    ' "$ROOT/$path"
  )
  [ "$actual" = "$expected" ] ||
    fail "unexpected occurrence count in $path function $function_name: $text expected=$expected actual=${actual:-missing}"
}

require_function_text_before() {
  path=$1
  function_name=$2
  first=$3
  second=$4
  positions=$(
    awk -v function_name="$function_name" -v first="$first" \
        -v second="$second" '
      index($0, function_name "(") { in_function = 1 }
      in_function && !first_line && index($0, first) {
        first_line = NR
      }
      in_function && !second_line && index($0, second) {
        second_line = NR
      }
      in_function && /^}/ {
        print first_line + 0, second_line + 0
        exit
      }
    ' "$ROOT/$path"
  )
  first_line=${positions%% *}
  second_line=${positions#* }
  [ "${first_line:-0}" -gt 0 ] ||
    fail "required ordered text is missing from $path function $function_name: $first"
  [ "${second_line:-0}" -gt 0 ] ||
    fail "required ordered text is missing from $path function $function_name: $second"
  [ "$first_line" -lt "$second_line" ] ||
    fail "required text order is invalid in $path function $function_name: $first before $second"
}

require_file_sha256() {
  path=$1
  expected=$2

  if command -v shasum >/dev/null 2>&1; then
    actual=$(shasum -a 256 "$ROOT/$path" | awk '{ print $1 }')
  elif command -v sha256sum >/dev/null 2>&1; then
    actual=$(sha256sum "$ROOT/$path" | awk '{ print $1 }')
  else
    fail "neither shasum nor sha256sum is available"
  fi
  [ "$actual" = "$expected" ] ||
    fail "SHA-256 mismatch for $path: expected=$expected actual=$actual"
}

require_function_sha256() {
  path=$1
  function_name=$2
  expected=$3

  if command -v shasum >/dev/null 2>&1; then
    actual=$(
      awk -v function_name="$function_name" '
        index($0, function_name "(") { in_function = 1 }
        in_function { print }
        in_function && /^}/ { exit }
      ' "$ROOT/$path" | shasum -a 256 | awk '{ print $1 }'
    )
  elif command -v sha256sum >/dev/null 2>&1; then
    actual=$(
      awk -v function_name="$function_name" '
        index($0, function_name "(") { in_function = 1 }
        in_function { print }
        in_function && /^}/ { exit }
      ' "$ROOT/$path" | sha256sum | awk '{ print $1 }'
    )
  else
    fail "neither shasum nor sha256sum is available"
  fi
  [ "$actual" = "$expected" ] ||
    fail "function SHA-256 mismatch for $path $function_name: expected=$expected actual=$actual"
}

require_tag_target() {
  tag=$1
  expected=$2

  actual=$(
    git -C "$ROOT" rev-parse --verify "refs/tags/${tag}^{commit}" 2>/dev/null
  ) ||
    fail "required Git tag is missing or does not resolve to a commit: $tag"
  [ "$actual" = "$expected" ] ||
    fail "Git tag target mismatch for $tag: expected=$expected actual=$actual"
}

require_line_before() {
  path=$1
  first=$2
  second=$3
  first_line=$(
    grep -n -F -x -- "$first" "$ROOT/$path" | sed -n '1s/:.*//p'
  )
  second_line=$(
    grep -n -F -x -- "$second" "$ROOT/$path" | sed -n '1s/:.*//p'
  )
  [ -n "$first_line" ] ||
    fail "required ordered line is missing from $path: $first"
  [ -n "$second_line" ] ||
    fail "required ordered line is missing from $path: $second"
  [ "$first_line" -lt "$second_line" ] ||
    fail "required line order is invalid in $path: $first before $second"
}

CONTRACT=docs/r0lab-v1-contract.md
VERIFICATION=docs/r0lab-v1-verification.md
SHADOW_PLAN=docs/shadow-page-transition-plan.md
RAW_PLAN=docs/wxshadow-raw-pte-implementation-plan.md
REPLICA_PLAN=docs/wxshadow-replica-plan.md
RAW_COMPAT=docs/pixel7-panther-raw-pte-compatibility.md
KPM_COMPAT_MATRIX=docs/kpm-compatibility-matrix.md
S4_PLAN=docs/wxshadow-s4-brk-step-plan.md
FINAL_ROADMAP=docs/wxshadow-final-experiment-roadmap.md
F5_DECISION_PLAN=docs/wxshadow-f5-hidden-read-decision-plan.md
F6_BRK_STEP_PLAN=docs/wxshadow-f6-brk-step-descriptor-abi-plan.md
F6_D3_PLAN=docs/wxshadow-f6-d3-two-slot-descriptor-routing-plan.md
F6_D4_PLAN=docs/wxshadow-f6-d4-negative-descriptor-controls-plan.md
F7_STRESS_PLAN=docs/wxshadow-f7-final-lifecycle-stress-plan.md
F2_PLAN=docs/wxshadow-f2-two-page-lab-harness-plan.md
F3_PLAN=docs/wxshadow-f3-page-local-patch-records-plan.md
F4_PLAN=docs/wxshadow-f4-hook-routing-by-page-record-plan.md
F42_POSITIVE_PLAN=docs/wxshadow-f4.2-positive-trigger-plan.md
F43_GUP_PLAN=docs/wxshadow-f4.3-gup-hook-routing-plan.md
F44_FORK_PLAN=docs/wxshadow-f4.4-fork-hook-routing-plan.md
F45_SYSCALL_PRCTL_PLAN=docs/wxshadow-f4.5-syscall-prctl-routing-plan.md
F45_PRCTL_CHECKPOINT=docs/wxshadow-f4.5-prctl-routing-source-checkpoint.md
F46_EXIT_PLAN=docs/wxshadow-f4.6-exit-hook-routing-plan.md
F46_PANIC_DIAGNOSIS=docs/wxshadow-f4.6-panic-diagnosis.md
F46_D4_R3A_PLAN=docs/wxshadow-f4.6-d4-r3a-diagnostic-split-plan.md
F46_D4_R3B_PLAN=docs/wxshadow-f4.6-d4-r3b-raw-hold-split-plan.md
F46_D4_R3C_PLAN=docs/wxshadow-f4.6-d4-r3c-status-reader-split-plan.md
F46_D4_R3D_PLAN=docs/wxshadow-f4.6-d4-r3d-status-transport-split-plan.md
F46_D4_R3E_PLAN=docs/wxshadow-f4.6-d4-r3e-raw-hold-lifetime-plan.md
F46_D4_R3E_L2_PLAN=docs/wxshadow-f4.6-d4-r3e-l2-live-pte-plan.md
F46_D4_R3E_L3_PLAN=docs/wxshadow-f4.6-d4-r3e-l3-observer-perturbation-plan.md
F46_D4_R3F_PLAN=docs/wxshadow-f4.6-d4-r3f-abort-hook-exposure-plan.md
F46_D4_R3G_PLAN=docs/wxshadow-f4.6-d4-r3g-passthrough-wrapper-plan.md
F46_D4_R3H_PLAN=docs/wxshadow-f4.6-d4-r3h-mm-reference-plan.md
F46_D4_R3I_PLAN=docs/wxshadow-f4.6-d4-r3i-lock-exposure-plan.md
F46_D4_R3J_PLAN=docs/wxshadow-f4.6-d4-r3j-inflight-accounting-plan.md
F46_D4_R3K_PLAN=docs/wxshadow-f4.6-d4-r3k-visible-inflight-lifetime-plan.md
F46_D4_R3L_PLAN=docs/wxshadow-f4.6-d4-r3l-readonly-iabt-route-plan.md
F46_D4_R3M_PLAN=docs/wxshadow-f4.6-d4-r3m-iabt-transition-restore-abi-plan.md
F46_D4_R3N_PLAN=docs/wxshadow-f4.6-d4-r3n-full-abort-lifecycle-plan.md
F46_D4_R3N_MM_FIX_PLAN=docs/wxshadow-f4.6-d4-r3n-owner-exit-mmput-fix-plan.md
F46_D4_R3O_PLAN=docs/wxshadow-f4.6-d4-r3o-reference-lifetime-parity-fix-plan.md
DEVELOPMENT_SEQUENCE=docs/wxshadow-development-sequence.md
FOLKPATCH_REFERENCE=docs/folkpatch-runtime-reference.md
REFERENCE_REVIEW=docs/wxshadow-reference-review.md
REFERENCE_COVERAGE=docs/wxshadow-reference-function-coverage.md
REFERENCE_INVENTORY=docs/wxshadow-reference-function-inventory.txt

require_file "$CONTRACT"
require_file "$VERIFICATION"
require_file "$SHADOW_PLAN"
require_file "$RAW_PLAN"
require_file "$REPLICA_PLAN"
require_file "$RAW_COMPAT"
require_file "$KPM_COMPAT_MATRIX"
require_file "$S4_PLAN"
require_file "$FINAL_ROADMAP"
require_file "$F5_DECISION_PLAN"
require_file "$F6_BRK_STEP_PLAN"
require_file "$F6_D3_PLAN"
require_file "$F6_D4_PLAN"
require_file "$F7_STRESS_PLAN"
require_file "$F2_PLAN"
require_file "$F3_PLAN"
require_file "$F4_PLAN"
require_file "$F42_POSITIVE_PLAN"
require_file "$F43_GUP_PLAN"
require_file "$F44_FORK_PLAN"
require_file "$F45_SYSCALL_PRCTL_PLAN"
require_file "$F45_PRCTL_CHECKPOINT"
require_file "$F46_EXIT_PLAN"
require_file "$F46_PANIC_DIAGNOSIS"
require_file "$F46_D4_R3A_PLAN"
require_file "$F46_D4_R3B_PLAN"
require_file "$F46_D4_R3C_PLAN"
require_file "$F46_D4_R3D_PLAN"
require_file "$F46_D4_R3E_PLAN"
require_file "$F46_D4_R3E_L2_PLAN"
require_file "$F46_D4_R3E_L3_PLAN"
require_file "$F46_D4_R3F_PLAN"
require_file "$F46_D4_R3G_PLAN"
require_file "$F46_D4_R3H_PLAN"
require_file "$F46_D4_R3I_PLAN"
require_file "$F46_D4_R3J_PLAN"
require_file "$F46_D4_R3K_PLAN"
require_file "$F46_D4_R3L_PLAN"
require_file "$F46_D4_R3M_PLAN"
require_file "$F46_D4_R3N_PLAN"
require_file "$F46_D4_R3N_MM_FIX_PLAN"
require_file "$F46_D4_R3O_PLAN"
require_file "$DEVELOPMENT_SEQUENCE"
require_file "$FOLKPATCH_REFERENCE"
require_file "$REFERENCE_REVIEW"
require_file "$REFERENCE_COVERAGE"
require_file "$REFERENCE_INVENTORY"
require_file scripts/verify_wxshadow_reference_source.sh
require_file scripts/verify_wxshadow_reference_inventory.sh
require_file scripts/test_wxshadow_reference_inventory_host.sh
require_file scripts/test_raw_exit_hook_routing_device.sh
require_file scripts/test_raw_exit_hook_routing_diagnostics_device.sh
require_file scripts/test_raw_exit_hook_cleanup_isolation_device.sh
require_file scripts/test_raw_exit_hook_preclear_hold_split_device.sh
require_file scripts/test_raw_exit_hook_raw_hold_split_device.sh
require_file scripts/test_raw_exit_hook_raw_hold_idle_device.sh
require_file scripts/test_raw_hold_lifetime_matrix_device.sh
require_file scripts/test_raw_live_pte_snapshot_device.sh
require_file scripts/test_raw_observer_perturbation_device.sh
require_file scripts/classify_raw_observer_perturbation_evidence.sh
require_file scripts/test_raw_observer_aggregate_host.sh
require_file scripts/test_raw_abort_hook_exposure_device.sh
require_file scripts/test_raw_abort_mmget_passthrough_device.sh
require_file scripts/test_raw_abort_lock_passthrough_device.sh
require_file scripts/test_raw_abort_inflight_passthrough_device.sh
require_file scripts/verify_d4_r3j_disassembly.sh
require_file scripts/verify_d4_r3j_plan_packet.sh
require_file scripts/verify_d4_r3k_plan_packet.sh
require_file scripts/verify_d4_r3l_plan_packet.sh
require_file scripts/verify_d4_r3m_plan_packet.sh
require_file scripts/test_raw_abort_visible_inflight_device.sh
require_file scripts/verify_d4_r3k_disassembly.sh
require_file scripts/test_raw_abort_iabt_transition_device.sh
require_file scripts/verify_d4_r3m_disassembly.sh
require_file scripts/test_raw_full_abort_lifecycle_device.sh
require_file scripts/verify_d4_r3n_disassembly.sh
require_file scripts/verify_d4_r3o_reference_lifetime.sh
require_file scripts/test_raw_r3o_lifetime_device.sh
require_file scripts/test_s4_descriptor_routing_device.sh
require_file scripts/test_s4_descriptor_negative_device.sh
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
require_text "$REPLICA_PLAN" 'Future work must'
require_text "$REPLICA_PLAN" 'struct r0lab_page_record'
require_text "$REPLICA_PLAN" 'record_backend=visible_clone'
require_text "$FINAL_ROADMAP" 'wxshadow Final Experiment Roadmap'
require_text "$FINAL_ROADMAP" 'Raw Page Table Skeleton'
require_text "$FINAL_ROADMAP" 'Two-Page Lab Harness'
require_text "$FINAL_ROADMAP" 'Page-Local Patch Records'
require_text "$FINAL_ROADMAP" 'Hook Routing By Page Record'
require_text "$FINAL_ROADMAP" 'Controlled Hidden-Read Decision Gate'
require_text "$FINAL_ROADMAP" 'Do not re-enable `shadow_xom`'
require_text "$FINAL_ROADMAP" 'Do not add arbitrary process or arbitrary address support.'
require_text "$FINAL_ROADMAP" 'raw_page_table_slots'
require_text "$FINAL_ROADMAP" 'Slot 1 is intentionally inert until F2'
require_text "$FINAL_ROADMAP" 'docs/wxshadow-f2-two-page-lab-harness-plan.md'
require_text "$FINAL_ROADMAP" 'docs/wxshadow-f3-page-local-patch-records-plan.md'
require_text "$FINAL_ROADMAP" 'docs/wxshadow-f4-hook-routing-by-page-record-plan.md'
require_text "$FINAL_ROADMAP" 'docs/wxshadow-f4.2-positive-trigger-plan.md'
require_text "$FINAL_ROADMAP" 'docs/wxshadow-f4.3-gup-hook-routing-plan.md'
require_text "$FINAL_ROADMAP" 'docs/wxshadow-f4.4-fork-hook-routing-plan.md'
require_text "$FINAL_ROADMAP" 'docs/wxshadow-development-sequence.md'
require_text "$FINAL_ROADMAP" 'Plan checkpoints are docs/contract only.'
require_text "$FINAL_ROADMAP" 'No hook-family migration starts unless'
require_text "$FINAL_ROADMAP" 'scripts/test_raw_exit_hook_device.sh'
require_text "$FINAL_ROADMAP" 'scripts/test_raw_hook_routing_device.sh'
require_text "$FINAL_ROADMAP" 'Completed F4.4 gate'
require_text "$FINAL_ROADMAP" 'build/evidence/raw-fork-hook-routing-20260724-031442.log'
require_text "$FINAL_ROADMAP" 'status_source=cross_clear'
require_text "$FINAL_ROADMAP" 'docs/wxshadow-f4.5-syscall-prctl-routing-plan.md'
require_text "$FINAL_ROADMAP" 'docs/wxshadow-f4.6-exit-hook-routing-plan.md'
require_text "$FINAL_ROADMAP" 'scripts/test_raw_fork_hook_routing_device.sh'
require_text "$DEVELOPMENT_SEQUENCE" 'wxshadow Planned Development Sequence'
require_text "$DEVELOPMENT_SEQUENCE" 'This document turns the roadmap into a fixed development queue.'
require_text "$DEVELOPMENT_SEQUENCE" 'F4.1: Abort Hook Routing'
require_text "$DEVELOPMENT_SEQUENCE" 'F4.2: Fault Hook Routing'
require_text "$DEVELOPMENT_SEQUENCE" 'F4.3: GUP Hook Routing'
require_text "$DEVELOPMENT_SEQUENCE" 'F4.4: Fork Hook Routing'
require_text "$DEVELOPMENT_SEQUENCE" 'F4.5: Syscall And Prctl Routing'
require_text "$DEVELOPMENT_SEQUENCE" 'F4.6: Exit Hook Routing'
require_text "$DEVELOPMENT_SEQUENCE" 'F4.7: F4 Integration Gate'
require_text "$DEVELOPMENT_SEQUENCE" 'Planning Gate'
require_text "$DEVELOPMENT_SEQUENCE" 'Only one source slice may be active at a time.'
require_text "$DEVELOPMENT_SEQUENCE" 'Source Work Packet Requirement'
require_text "$DEVELOPMENT_SEQUENCE" 'Do not turn an inspection finding directly into a patch.'
require_text "$DEVELOPMENT_SEQUENCE" 'scripts/test_raw_hook_routing_device.sh'
require_text "$DEVELOPMENT_SEQUENCE" 'scripts/test_raw_fault_hook_routing_device.sh'
require_text "$DEVELOPMENT_SEQUENCE" 'scripts/test_raw_fault_hook_positive_preflight_device.sh'
require_text "$DEVELOPMENT_SEQUENCE" 'docs/wxshadow-f4.2-positive-trigger-plan.md'
require_text "$DEVELOPMENT_SEQUENCE" 'docs/wxshadow-f4.3-gup-hook-routing-plan.md'
require_text "$DEVELOPMENT_SEQUENCE" 'docs/wxshadow-f4.4-fork-hook-routing-plan.md'
require_text "$DEVELOPMENT_SEQUENCE" 'docs/wxshadow-f4.5-syscall-prctl-routing-plan.md'
require_text "$DEVELOPMENT_SEQUENCE" 'docs/wxshadow-f4.6-exit-hook-routing-plan.md'
require_text "$DEVELOPMENT_SEQUENCE" 'F4.2a verification ladder'
require_text "$DEVELOPMENT_SEQUENCE" 'scripts/test_raw_gup_hook_routing_device.sh'
require_text "$DEVELOPMENT_SEQUENCE" 'scripts/test_raw_fork_hook_device.sh'
require_text "$DEVELOPMENT_SEQUENCE" 'scripts/test_raw_fork_hook_routing_device.sh'
require_text "$DEVELOPMENT_SEQUENCE" 'file-backed RX plus shadow-PTE access-flag clear'
require_text "$DEVELOPMENT_SEQUENCE" 'If a failed positive smoke exposes a new development gap, classify it'
require_text "$DEVELOPMENT_SEQUENCE" 'Do not pull these behaviors into F4 source checkpoints.'
require_text "$DEVELOPMENT_SEQUENCE" 'Development Control Loop'
require_text "$DEVELOPMENT_SEQUENCE" 'D1 source scaffold has static/build evidence.'
require_text "$DEVELOPMENT_SEQUENCE" 'build/evidence/raw-fork-hook-20260724-031422.log'
require_text "$DEVELOPMENT_SEQUENCE" 'build/evidence/raw-fork-hook-routing-20260724-031442.log'
require_text "$DEVELOPMENT_SEQUENCE" 'The next source checkpoint is'
require_text "$DEVELOPMENT_SEQUENCE" 'D0 plan gate is complete.'
require_text "$DEVELOPMENT_SEQUENCE" 'F4.6-D4 script work packet'
require_text "$DEVELOPMENT_SEQUENCE" 'D1-D3 syscall/getpid read-cycle routing is implemented and gate-passed'
require_text "$DEVELOPMENT_SEQUENCE" 'D4-D6 Lab-private prctl dispatch/patch routing is implemented and'
require_text "$DEVELOPMENT_SEQUENCE" 'build/evidence/raw-prctl-hook-routing-20260724-035944.log'
require_text "$DEVELOPMENT_SEQUENCE" 'build/evidence/raw-syscall-hook-routing-20260724-033233.log'
require_text "$F2_PLAN" 'raw slot arm <token> <slot> <page>'
require_text "$F2_PLAN" 'raw page table run <token>'
require_text "$F2_PLAN" 'scripts/test_raw_page_table_device.sh'
require_text "$F3_PLAN" 'wxshadow F3 Page-Local Patch Records Plan'
require_text "$F3_PLAN" 'Status: implemented'
require_text "$F3_PLAN" 'plan-only checkpoint'
require_text "$F3_PLAN" 'raw slot patch byte <token> <slot> <generation> <offset> <value>'
require_text "$F3_PLAN" 'raw slot patch word <token> <slot> <generation> <offset> <word>'
require_text "$F3_PLAN" 'raw slot patch release <token> <slot> <generation> <offset>'
require_text "$F3_PLAN" 'raw slot patch status <token> <slot>'
require_text "$F3_PLAN" 'patch_capacity=1024'
require_text "$F3_PLAN" 'raw page table patch records run <token>'
require_text "$F3_PLAN" 'scripts/test_raw_page_table_patch_records_device.sh'
require_text "$F3_PLAN" 'slot0_patch_record_slots=1024'
require_text "$F3_PLAN" 'slot1_after_slot0_capacity_value=88'
require_text "$F3_PLAN" 'scripts/test_raw_prctl_patch_records_device.sh'
require_text "$F3_PLAN" 'scripts/test_raw_page_table_device.sh'
require_text "$F3_PLAN" 'build/evidence/raw-page-table-patch-records-20260724-003931.log'
require_text "$F4_PLAN" 'wxshadow F4 Hook Routing By Page Record Plan'
require_text "$F4_PLAN" 'Status: F4.1 abort-routing checkpoint implemented and targeted gate-passed.'
require_text "$F4_PLAN" 'allowed files: this document'
require_text "$F4_PLAN" 'disallowed files: `kpm/r0lab.c`, `kpm/r0lab_raw_compat.c`'
require_text "$F4_PLAN" 'target mm plus page VA lookup'
require_text "$F4_PLAN" 'Current source-helper checkpoint:'
require_text "$F4_PLAN" 'Current F4.1 abort-routing checkpoint:'
require_text "$F4_PLAN" 'Active F4.2 fault-routing preflight:'
require_text "$F4_PLAN" 'docs/wxshadow-f4.2-positive-trigger-plan.md'
require_text "$F4_PLAN" 'fault-hook hits and final `42/42` behavior'
require_text "$F4_PLAN" 'reject RWX write as a trigger'
require_text "$F4_PLAN" 'file-backed RX plus shadow-PTE access-flag clear'
require_text "$F4_PLAN" 'r0lab_raw_page_find_by_mm_addr_locked'
require_text "$F4_PLAN" 'r0lab_raw_page_find_by_fault_locked'
require_text "$F4_PLAN" 'r0lab_raw_page_find_for_hook_locked'
require_text "$F4_PLAN" 'struct r0lab_raw_hook_page_token'
require_text "$F4_PLAN" 'raw hook route status <token> <slot>'
require_text "$F4_PLAN" 'hook callbacks must not free page memory'
require_text "$F4_PLAN" 'do_mem_abort'
require_text "$F4_PLAN" 'handle_mm_fault'
require_text "$F4_PLAN" 'follow_page_pte'
require_text "$F4_PLAN" 'follow_page_mask'
require_text "$F4_PLAN" 'dup_mmap'
require_text "$F4_PLAN" 'exit_mmap'
require_text "$F4_PLAN" '__arm64_sys_getpid'
require_text "$F4_PLAN" '__arm64_sys_prctl'
require_text "$F4_PLAN" 'raw_hook_route_status slot=<slot>'
require_text "$F4_PLAN" 'page_record_routed=1'
require_text "$F4_PLAN" 'scripts/test_raw_hook_routing_device.sh'
require_text "$F4_PLAN" 'arbitrary process, arbitrary `mm`, or arbitrary address support'
require_text "$F4_PLAN" 'raw-XOM, permission-fault hidden-read experiments, or `PTE_USER` changes'
require_text "$F42_POSITIVE_PLAN" 'wxshadow F4.2 Positive Trigger Plan'
require_text "$F42_POSITIVE_PLAN" 'Status: F4.2a source-checkpoint execution gate; Pixel 7 device outcome is'
require_text "$F42_POSITIVE_PLAN" 'F4.2a device outcome:'
require_text "$F42_POSITIVE_PLAN" 'build/evidence/raw-fault-hook-positive-preflight-20260724-020034.log'
require_text "$F42_POSITIVE_PLAN" 'Fixed development ladder'
require_text "$F42_POSITIVE_PLAN" 'F4.2a-D0 plan lock'
require_text "$F42_POSITIVE_PLAN" 'F4.2a-D1 source scaffold'
require_text "$F42_POSITIVE_PLAN" 'F4.2a-D2 negative regression'
require_text "$F42_POSITIVE_PLAN" 'F4.2a-D3 positive preflight'
require_text "$F42_POSITIVE_PLAN" 'F4.2a-D4 promotion or block'
require_text "$F42_POSITIVE_PLAN" 'PROT_NONE` raw-page read is blocked before `handle_mm_fault`'
require_text "$F42_POSITIVE_PLAN" 'File-backed RX shadow PTE access-flag clear'
require_text "$F42_POSITIVE_PLAN" 'First positive candidate.'
require_text "$F42_POSITIVE_PLAN" 'raw_fault_hook_positive_preflight=pass'
require_text "$F42_POSITIVE_PLAN" 'positive_route=1'
require_text "$F42_POSITIVE_PLAN" 'op=33 result=0'
require_text "$F42_POSITIVE_PLAN" 'handler_faults=0'
require_text "$F42_POSITIVE_PLAN" 'normal=42/42`, `shadow=99/99`, and final cleanup returns `42/42`'
require_text "$F42_POSITIVE_PLAN" 'RWX write trigger strings do not appear in the positive script or static'
require_text "$F42_POSITIVE_PLAN" 'If file-backed executable mapping cannot be created safely in the Lab App'
require_text "$F42_POSITIVE_PLAN" 'handle_mm_fault_positive_blocked'
require_text "$F42_POSITIVE_PLAN" 'Failure Classification'
require_text "$F42_POSITIVE_PLAN" 'Do not patch from a failed smoke directly.'
require_text "$F43_GUP_PLAN" 'wxshadow F4.3 GUP Hook Routing Plan'
require_text "$F43_GUP_PLAN" 'D2/D3 device-gate passed'
require_text "$F43_GUP_PLAN" 'build/evidence/raw-gup-hook-20260724-024012.log'
require_text "$F43_GUP_PLAN" 'build/evidence/raw-gup-hook-routing-20260724-024036.log'
require_text "$F43_GUP_PLAN" 'build/evidence/raw-hook-routing-20260724-024107.log'
require_text "$F43_GUP_PLAN" 'If a test exposes a missing behavior, do not patch source first.'
require_text "$F43_GUP_PLAN" 'D2 Recovery Protocol'
require_text "$F43_GUP_PLAN" 'D3 Evidence Protocol'
require_text "$F43_GUP_PLAN" "after slot 0's external read, slot 1 inspect still shows zero GUP hook"
require_text "$F43_GUP_PLAN" 'Fixed Development Ladder'
require_text "$F43_GUP_PLAN" 'F4.3-D0 plan lock'
require_text "$F43_GUP_PLAN" 'F4.3-D1 source scaffold'
require_text "$F43_GUP_PLAN" 'F4.3-D2 slot-0 regression'
require_text "$F43_GUP_PLAN" 'F4.3-D3 two-slot routing'
require_text "$F43_GUP_PLAN" 'F4.3-D4 commit/tag'
require_text "$F43_GUP_PLAN" 'GUP before hook resolves the page by `vma->vm_mm` plus GUP address.'
require_text "$F43_GUP_PLAN" 'The before hook stores slot and generation in `hook_local_t`.'
require_text "$F43_GUP_PLAN" 'Legacy `raw gup hook arm/status/clear <token>` remains slot-0 compatible.'
require_text "$F43_GUP_PLAN" 'scripts/test_raw_gup_hook_routing_device.sh'
require_text "$F43_GUP_PLAN" 'Wrong-page routing'
require_text "$F44_FORK_PLAN" 'wxshadow F4.4 Fork Hook Routing Plan'
require_text "$F44_FORK_PLAN" 'Status: implemented on the pinned Pixel 7.'
require_text "$F44_FORK_PLAN" 'Outcome Evidence'
require_text "$F44_FORK_PLAN" 'build/evidence/raw-fork-hook-routing-20260724-031442.log'
require_text "$F44_FORK_PLAN" 'status_source=cross_clear'
require_text "$F44_FORK_PLAN" 'shadow_pause_parent_pages(mm)'
require_text "$F44_FORK_PLAN" 'shadow_resume_parent_pages(mm)'
require_text "$F44_FORK_PLAN" 'Fixed Development Ladder'
require_text "$F44_FORK_PLAN" 'F4.4-D1 Work Packet'
require_text "$F44_FORK_PLAN" 'Allowed files:'
require_text "$F44_FORK_PLAN" 'Disallowed files:'
require_text "$F44_FORK_PLAN" 'New `raw slot fork hook arm/status/clear <token> <slot>` commands are'
require_text "$F44_FORK_PLAN" 'F4.4-D0 plan lock'
require_text "$F44_FORK_PLAN" 'F4.4-D1 source scaffold'
require_text "$F44_FORK_PLAN" 'F4.4-D2 slot-0 regression'
require_text "$F44_FORK_PLAN" 'F4.4-D3 two-slot fork routing'
require_text "$F44_FORK_PLAN" 'F4.4-D4 commit/tag'
require_text "$F44_FORK_PLAN" 'The before hook stores a bounded paused-slot set plus slot generations in'
require_text "$F44_FORK_PLAN" 'Legacy `raw fork hook arm/status/clear <token>` remains slot-0 compatible.'
require_text "$F44_FORK_PLAN" 'scripts/test_raw_fork_hook_routing_device.sh'
require_text "$F44_FORK_PLAN" 'F4.4-D2 Regression Triage Plan'
require_text "$F44_FORK_PLAN" 'raw slot fork hook status <token> 0'
require_text "$F44_FORK_PLAN" 'Do not change `dup_mmap` callbacks for this case.'
require_text "$F44_FORK_PLAN" 'Partial pause mismatch'
require_text "$F45_SYSCALL_PRCTL_PLAN" 'wxshadow F4.5 Syscall And Prctl Routing Plan'
require_text "$F45_SYSCALL_PRCTL_PLAN" 'Status: D1-D3 syscall routing and D4-D6 prctl routing are implemented'
require_text "$F45_SYSCALL_PRCTL_PLAN" 'build/evidence/raw-prctl-hook-routing-20260724-035944.log'
require_text "$F45_SYSCALL_PRCTL_PLAN" 'F4.5-D0 plan lock'
require_text "$F45_SYSCALL_PRCTL_PLAN" 'F4.5-D1 syscall source scaffold'
require_text "$F45_SYSCALL_PRCTL_PLAN" 'F4.5-D2 syscall slot-0 regression'
require_text "$F45_SYSCALL_PRCTL_PLAN" 'F4.5-D3 syscall two-slot routing'
require_text "$F45_SYSCALL_PRCTL_PLAN" 'F4.5-D4 prctl source scaffold'
require_text "$F45_SYSCALL_PRCTL_PLAN" 'F4.5-D5 prctl slot-0 regression'
require_text "$F45_SYSCALL_PRCTL_PLAN" 'F4.5-D6 prctl two-slot routing'
require_text "$F45_SYSCALL_PRCTL_PLAN" 'F4.5-D7 commit/tag'
require_text "$F45_SYSCALL_PRCTL_PLAN" 'docs/wxshadow-f4.5-prctl-routing-source-checkpoint.md'
require_text "$F45_SYSCALL_PRCTL_PLAN" 'Legacy `raw syscall ... <token>` and `raw prctl ... <token>` commands remain'
require_text "$F45_SYSCALL_PRCTL_PLAN" '<token> <slot> <generation>'
require_text "$F45_SYSCALL_PRCTL_PLAN" 'raw slot syscall read cycle hook select <token> <slot> <generation>'
require_text "$F45_SYSCALL_PRCTL_PLAN" 'routes through selected slot plus generation'
require_text "$F45_SYSCALL_PRCTL_PLAN" 'route through `current_mm + address` or `current_mm + request.address`'
require_text "$F45_SYSCALL_PRCTL_PLAN" 'Patch-record capacity remains `1024` per page record.'
require_text "$F45_SYSCALL_PRCTL_PLAN" 'scripts/test_raw_syscall_read_cycle_device.sh'
require_text "$F45_SYSCALL_PRCTL_PLAN" 'scripts/test_raw_syscall_hook_routing_device.sh'
require_text "$F45_SYSCALL_PRCTL_PLAN" 'scripts/test_raw_prctl_patch_records_device.sh'
require_text "$F45_SYSCALL_PRCTL_PLAN" 'scripts/test_raw_prctl_hook_routing_device.sh'
require_text "$F45_SYSCALL_PRCTL_PLAN" 'Stale generation accepted'
require_text "$F45_SYSCALL_PRCTL_PLAN" 'build/evidence/raw-syscall-hook-routing-20260724-033233.log'
require_text "$F45_PRCTL_CHECKPOINT" 'wxshadow F4.5 D4-D6 Prctl Routing Source Checkpoint'
require_text "$F45_PRCTL_CHECKPOINT" 'Status: D4-D6 implemented and gate-passed on Pixel 7'
require_text "$F45_PRCTL_CHECKPOINT" 'D4-D7 Execution Contract'
require_text "$F45_PRCTL_CHECKPOINT" 'No development step is allowed to discover a new scope hole'
require_text "$F45_PRCTL_CHECKPOINT" 'r0lab_raw_prctl_before()'
require_text "$F45_PRCTL_CHECKPOINT" 'r0lab_raw_prctl_hook_users_locked()'
require_text "$F45_PRCTL_CHECKPOINT" 'raw slot prctl hook arm <token> <slot> <generation>'
require_text "$F45_PRCTL_CHECKPOINT" 'raw slot prctl hook select <token> <slot> <generation>'
require_text "$F45_PRCTL_CHECKPOINT" 'raw slot prctl hook status <token> <slot>'
require_text "$F45_PRCTL_CHECKPOINT" 'raw slot prctl hook clear <token> <slot> <generation>'
require_text "$F45_PRCTL_CHECKPOINT" 'event-ring truncation fix'
require_text "$F45_PRCTL_CHECKPOINT" 'build/evidence/raw-prctl-hook-routing-20260724-035944.log'
require_text "$F45_PRCTL_CHECKPOINT" 'build/evidence/raw-prctl-read-cycle-20260724-040015.log'
require_text "$F45_PRCTL_CHECKPOINT" 'build/evidence/raw-prctl-patch-records-20260724-040036.log'
require_text "$F45_PRCTL_CHECKPOINT" 'build/evidence/raw-syscall-hook-routing-20260724-040056.log'
require_text "$F45_PRCTL_CHECKPOINT" 'R0LAB_PRCTL_OP_READ_CYCLE` has no address argument'
require_text "$F45_PRCTL_CHECKPOINT" 'route by `current_mm + address`'
require_text "$F45_PRCTL_CHECKPOINT" 'route by `current_mm + request.address`'
require_text "$F45_PRCTL_CHECKPOINT" 'r0lab_raw_hook_page_token_acquire_locked()'
require_text "$F45_PRCTL_CHECKPOINT" 'R0LAB_RAW_HOOK_PRCTL'
require_text "$F45_PRCTL_CHECKPOINT" 'raw_slot_prctl_hook_ready slot=%u generation=%llu symbol=prctl installed=1 selected=1 page_record_routed=1'
require_text "$F45_PRCTL_CHECKPOINT" 'raw mode=prctl-routing failures=0 trigger=prctl_magic route=selected_slot,address page_record_routed=1'
require_text "$F45_PRCTL_CHECKPOINT" 'scripts/test_raw_prctl_hook_routing_device.sh'
require_text "$F46_EXIT_PLAN" 'wxshadow F4.6 Exit Hook Routing Plan'
require_text "$F46_EXIT_PLAN" 'Status: D4 first-smoke evidence is captured, D4-R0 classified the reboot'
require_text "$F46_EXIT_PLAN" 'D4-R2 explicit-clear cleanup'
require_text "$F46_EXIT_PLAN" 'current gate is D4-R3 source-packet planning'
require_text "$F46_EXIT_PLAN" 'docs/wxshadow-f4.6-panic-diagnosis.md'
require_text "$F46_EXIT_PLAN" 'hook callbacks may mark and record matching page records,'
require_text "$F46_EXIT_PLAN" 'but they must not directly free page memory'
require_text "$F46_EXIT_PLAN" 'No KPM'
require_text "$F46_EXIT_PLAN" 'or Lab source edit may start from the D4 first-smoke timing, D4-R1 cleanup'
require_text "$F46_EXIT_PLAN" 'reentry, or D4-R2 exit-hook-clear evidence alone.'
require_text "$F46_EXIT_PLAN" 'D2 Recovery Record'
require_text "$F46_EXIT_PLAN" 'build/evidence/raw-exit-hook-20260724-043815.log'
require_text "$F46_EXIT_PLAN" 'Completed Recovery Plan'
require_text "$F46_EXIT_PLAN" 'R1 source edit is an isolation slice'
require_text "$F46_EXIT_PLAN" 'defer the new target-exiting marker'
require_text "$F46_EXIT_PLAN" 'F4.6-D0 plan lock'
require_text "$F46_EXIT_PLAN" 'F4.6-D1 KPM source scaffold'
require_text "$F46_EXIT_PLAN" 'F4.6-D2 slot-0 regression'
require_text "$F46_EXIT_PLAN" 'F4.6-R0/R1 blocker recovery'
require_text "$F46_EXIT_PLAN" 'F4.6-D3 Lab two-slot hold source'
require_text "$F46_EXIT_PLAN" 'F4.6-D4 two-slot owner-exit smoke'
require_text "$F46_EXIT_PLAN" 'F4.6-D4-R0 failure classification'
require_text "$F46_EXIT_PLAN" 'F4.6-D4-R1 reboot-source decoupling'
require_text "$F46_EXIT_PLAN" 'F4.6-D5 lifecycle regression'
require_text "$F46_EXIT_PLAN" 'F4.6-D6 commit/tag'
require_text "$F46_EXIT_PLAN" 'F4.6-D3 Source Work Packet'
require_text "$F46_EXIT_PLAN" 'F4.6-D3 Local Result'
require_text "$F46_EXIT_PLAN" 'F4.6-D4 Script Work Packet'
require_text "$F46_EXIT_PLAN" 'D3-a command dispatch'
require_text "$F46_EXIT_PLAN" 'D3-e compact result and failure cleanup'
require_text "$F46_EXIT_PLAN" 'D4-c owner exit'
require_text "$F46_EXIT_PLAN" 'F4.6-D4 First Smoke Result'
require_text "$F46_EXIT_PLAN" 'build/evidence/raw-exit-hook-routing-20260724-045308.log'
require_text "$F46_EXIT_PLAN" 'build/evidence/raw-exit-hook-routing-20260724-045308.console-ramoops-0.txt'
require_text "$F46_EXIT_PLAN" 'boringssl_self_test_apex64'
require_text "$F46_EXIT_PLAN" 'system-reboot-after-owner-exit'
require_text "$F46_EXIT_PLAN" 'F4.6-D4-R0 Diagnosis Work Packet'
require_text "$F46_EXIT_PLAN" 'F4.6-D4-R1 Reboot-Source Decoupling Work Packet'
require_text "$F46_EXIT_PLAN" 'phase=no_kpm_force_stop result=pass boot_stable=1'
require_text "$F46_EXIT_PLAN" 'phase=kpm_loaded_no_session_force_stop result=pass boot_stable=1'
require_text "$F46_EXIT_PLAN" 'phase=kpm_session_no_exit_hook_force_stop result=pass boot_stable=1 cleanup=monitor'
require_text "$F46_EXIT_PLAN" 'target_exit_event=not_required_without_page_records'
require_text "$F46_EXIT_PLAN" 'phase=d3_hold_explicit_clear_then_force_stop result=pass boot_stable=1 explicit_clear=1 exit_mmap_owner_cleanup=not_exercised'
require_text "$F46_EXIT_PLAN" 'r0lab_raw_exit_mmap_before()'
require_text "$F46_EXIT_PLAN" 'g_raw_page.exit_hook_installed'
require_text "$F46_EXIT_PLAN" 'D4-R2 is diagnostic-only'
require_text "$F46_EXIT_PLAN" 'D4-R1 First Harness Result'
require_text "$F46_EXIT_PLAN" 'build/evidence/raw-exit-hook-routing-diagnostics-20260724-051021.log'
require_text "$F46_EXIT_PLAN" 'D4-R1-harness-anchor-too-strong'
require_text "$F46_EXIT_PLAN" 'D4-R1 Second Diagnostic Result'
require_text "$F46_EXIT_PLAN" 'build/evidence/raw-exit-hook-routing-diagnostics-20260724-051219.log'
require_text "$F46_EXIT_PLAN" 'D4-R1-explicit-clear-cleanup-reentry-panic'
require_text "$F46_EXIT_PLAN" 'F4.6-D4-R2 Explicit-Clear Cleanup Isolation Work Packet'
require_text "$F46_EXIT_PLAN" 'D4-R2-c non-reentrant slot0 clear'
require_text "$F46_EXIT_PLAN" 'scripts/test_raw_exit_hook_cleanup_isolation_device.sh'
require_text "$F46_EXIT_PLAN" 'phase=d4_r2_exit_hook_clear_only result=pass boot_stable=1 raw_slots=2 page_records=2 installed=0/0 cleanup_reentry=0'
require_text "$F46_EXIT_PLAN" 'phase=d4_r2_slot0_clear_non_reentrant result=pass boot_stable=1 raw_slot_cleared=0 cleanup_reentry=0 raw_slots=1 page_records=1'
require_text "$F46_EXIT_PLAN" 'phase=d4_r2_slot1_clear_non_reentrant result=pass boot_stable=1 raw_slot_cleared=1 cleanup_reentry=0 raw_slots=0 page_records=0'
require_text "$F46_EXIT_PLAN" 'raw_exit_hook_cleanup_isolation=pass phases=4'
require_text "$F46_EXIT_PLAN" 'D4-R2 First Device Result'
require_text "$F46_EXIT_PLAN" 'D4-R2-exit-hook-clear-only-panic'
require_text "$F46_EXIT_PLAN" 'No `raw slot clear 0x729249` command appears'
require_text "$F46_EXIT_PLAN" 'F4.6-D4-R3 Exit-Hook Release Source Packet Gate'
require_text "$F46_EXIT_PLAN" 'Status: current source packet'
require_text "$F46_EXIT_PLAN" 'Allowed source files after this packet is committed'
require_text "$F46_EXIT_PLAN" 'Source facts from inspection'
require_text "$F46_EXIT_PLAN" 'D4-R3 source delta'
require_text "$F46_EXIT_PLAN" 'g_raw_exit_hook_wrapped'
require_text "$F46_EXIT_PLAN" 'r0lab_raw_page_table_active_count_locked() == 0'
require_text "$F46_EXIT_PLAN" 'r0lab_raw_exit_hook_arm_common()'
require_text "$F46_EXIT_PLAN" 'r0lab_raw_exit_hook_release()'
require_text "$F46_EXIT_PLAN" 'r0lab_raw_exit_unhook_primary_legacy()'
require_text "$F46_EXIT_PLAN" 'r0lab_raw_reset_final_page()'
require_text "$F46_EXIT_PLAN" 'Migrate `r0lab_raw_exit_mmap_before()` away from the singleton'
require_text "$F46_EXIT_PLAN" 'The callback must not'
require_text "$F46_EXIT_PLAN" 'scripts/test_raw_exit_hook_device.sh'
require_text "$F46_EXIT_PLAN" 'scripts/test_raw_exit_hook_cleanup_isolation_device.sh'
require_text "$F46_EXIT_PLAN" 'D4-R3 stop rules'
require_text "$F46_EXIT_PLAN" 'If D4-R3 passes, queue D4-R4'
require_text "$F46_EXIT_PLAN" 'Current D4-R3 implementation result'
require_text "$F46_EXIT_PLAN" 'D4-R3-pre-clear-hold-state-init-sigsegv'
require_text "$F46_EXIT_PLAN" 'The next gate is D4-R3a'
require_text "$F46_EXIT_PLAN" 'Open D4-R3a diagnostic questions'
require_text "$F46_EXIT_PLAN" 'docs/wxshadow-f4.6-d4-r3a-diagnostic-split-plan.md'
require_text "$F46_EXIT_PLAN" 'scripts/test_raw_exit_hook_preclear_hold_split_device.sh'
require_text "$F46_EXIT_PLAN" 'phase=d4_r3a_hold_quiet_no_boot_reader result=pass'
require_text "$F46_EXIT_PLAN" 'phase=d4_r3a_boot_id_reader result=pass boot_stable=1'
require_text "$F46_EXIT_PLAN" 'raw_exit_hook_preclear_hold_split=pass phases=5'
require_text "$F46_EXIT_PLAN" 'D4-R3a first device result'
require_text "$F46_EXIT_PLAN" 'raw-exit-hook-preclear-hold-split-20260724-055545.log'
require_text "$F46_EXIT_PLAN" 'D4-R3a-boot-id-reader-unstable'
require_text "$F46_EXIT_PLAN" 'F4.6-D4-R3b Lab App-only raw-hold split packet'
require_text "$F46_EXIT_PLAN" 'F4.6-D4-R3b plan-lock packet'
require_text "$F46_EXIT_PLAN" 'docs/wxshadow-f4.6-d4-r3b-raw-hold-split-plan.md'
require_text "$F46_EXIT_PLAN" 'raw raw-hold routing hold <token>'
require_text "$F46_EXIT_PLAN" 'scripts/test_raw_exit_hook_raw_hold_split_device.sh'
require_text "$F46_EXIT_PLAN" 'raw mode=raw-hold-routing-hold failures=0'
require_text "$F46_EXIT_PLAN" 'exit_mmap_armed=0'
require_text "$F46_EXIT_PLAN" 'D4-R3b-L local source result'
require_text "$F46_EXIT_PLAN" 'r0lab_raw_hold_routing_hold()'
require_text "$F46_EXIT_PLAN" 'If the command cannot emit the D3 anchor without changing KPM behavior'
require_text "$F46_EXIT_PLAN" 'raw slot exit hook arm <token> <slot> <generation>'
require_text "$F46_EXIT_PLAN" 'raw slot exit hook status <token> <slot>'
require_text "$F46_EXIT_PLAN" 'raw slot exit hook clear <token> <slot> <generation>'
require_text "$F46_EXIT_PLAN" 'raw exit hook routing hold <token>'
require_text "$F46_EXIT_PLAN" 'scripts/test_raw_exit_hook_routing_device.sh'
require_text "$F46_EXIT_PLAN" 'scripts/test_raw_exit_hook_routing_diagnostics_device.sh'
require_text "$F46_EXIT_PLAN" 'target_mm_scoped=1 page_record_routed=1 observe_only=0 cleanup=monitor'
require_text "$F46_EXIT_PLAN" 'verify at least two `op=34 result=0` exit-mmap hit events'
require_text "$F46_EXIT_PLAN" 'Singleton residue'
require_text "$F46_EXIT_PLAN" 'sh -n scripts/test_raw_exit_hook_routing_diagnostics_device.sh'
require_text lab-app/src/main/cpp/labprobe.c 'r0lab_raw_exit_hook_routing_hold'
require_text lab-app/src/main/cpp/labprobe.c 'raw exit hook routing hold '
require_text lab-app/src/main/cpp/labprobe.c 'raw mode=exit-hook-routing-hold failures=%d'
require_text lab-app/src/main/cpp/labprobe.c 'g_r0lab_m5_hold.mode = 9'
require_text lab-app/src/main/cpp/labprobe.c 'raw slot exit hook arm 0x%llx 0 0x%llx'
require_text lab-app/src/main/cpp/labprobe.c 'raw slot exit hook status 0x%llx 0'
require_text lab-app/src/main/cpp/labprobe.c 'raw slot exit hook clear 0x%llx 0 0x%llx'
require_text lab-app/src/main/cpp/labprobe.c 'r0lab_raw_hold_routing_hold'
require_text lab-app/src/main/cpp/labprobe.c 'raw raw-hold routing hold '
require_text lab-app/src/main/cpp/labprobe.c 'raw mode=raw-hold-routing-hold failures=%d'
require_text lab-app/src/main/cpp/labprobe.c 'exit_mmap_armed=0'
require_text lab-app/src/main/cpp/labprobe.c 'hook_arm_rc=%ld/%ld'
require_text lab-app/src/main/cpp/labprobe.c 'hook_status_rc=%ld/%ld'
require_text lab-app/src/main/cpp/labprobe.c 'exit_hook_installed=%s/%s'
require_text lab-app/src/main/cpp/labprobe.c '"not_queried", "not_queried"'
require_text lab-app/src/main/cpp/labprobe.c 'g_r0lab_m5_hold.mode = 10'
require_text "$F46_PANIC_DIAGNOSIS" 'wxshadow F4.6 Panic Diagnosis'
require_text "$F46_PANIC_DIAGNOSIS" 'Status: D2 recovery complete, D3 Lab two-slot hold built, D4 first-smoke'
require_text "$F46_PANIC_DIAGNOSIS" 'D2 slot-0 compatibility blocker'
require_text "$F46_PANIC_DIAGNOSIS" 'raw exit hook clear 0x729206'
require_text "$F46_PANIC_DIAGNOSIS" 'raw clear 0x729206'
require_text "$F46_PANIC_DIAGNOSIS" 'page_lock_anon_vma_read'
require_text "$F46_PANIC_DIAGNOSIS" 'R1 Planned Source Slice'
require_text "$F46_PANIC_DIAGNOSIS" 'Candidate deltas to isolate'
require_text "$F46_PANIC_DIAGNOSIS" 'direct slot-0 legacy wrappers'
require_text "$F46_PANIC_DIAGNOSIS" 'R1a Result'
require_text "$F46_PANIC_DIAGNOSIS" 'R1b Planned Source Slice'
require_text "$F46_PANIC_DIAGNOSIS" 'full legacy slot-0 exit-hook parity'
require_text "$F46_PANIC_DIAGNOSIS" 'R1b Device Rerun Result'
require_text "$F46_PANIC_DIAGNOSIS" 'R1c Planned Harness Slice'
require_text "$F46_PANIC_DIAGNOSIS" 'command_timeout command=<requested command>'
require_text "$F46_PANIC_DIAGNOSIS" 'R1c Device Rerun Result'
require_text "$F46_PANIC_DIAGNOSIS" 'R1d Planned Harness Slice'
require_text "$F46_PANIC_DIAGNOSIS" 'R1d Device Rerun Result'
require_text "$F46_PANIC_DIAGNOSIS" 'D4 First Smoke Result'
require_text "$F46_PANIC_DIAGNOSIS" 'D4-R0 Planned Diagnosis Slice'
require_text "$F46_PANIC_DIAGNOSIS" 'D4-R1 Planned Diagnosis Slice'
require_text "$F46_PANIC_DIAGNOSIS" 'no_kpm_force_stop'
require_text "$F46_PANIC_DIAGNOSIS" 'kpm_loaded_no_session_force_stop'
require_text "$F46_PANIC_DIAGNOSIS" 'kpm_session_no_exit_hook_force_stop'
require_text "$F46_PANIC_DIAGNOSIS" 'd3_hold_explicit_clear_then_force_stop'
require_text "$F46_PANIC_DIAGNOSIS" 'Candidate source clue, still unproven'
require_text "$F46_PANIC_DIAGNOSIS" 'D4-R1 First Harness Result'
require_text "$F46_PANIC_DIAGNOSIS" 'D4-R1-harness-anchor-too-strong'
require_text "$F46_PANIC_DIAGNOSIS" 'target-exit event when no page records existed'
require_text "$F46_PANIC_DIAGNOSIS" 'D4-R1 Second Diagnostic Result'
require_text "$F46_PANIC_DIAGNOSIS" 'D4-R1-explicit-clear-cleanup-reentry-panic'
require_text "$F46_PANIC_DIAGNOSIS" 'D4-R2 Planned Diagnosis Slice'
require_text "$F46_PANIC_DIAGNOSIS" 'scripts/test_raw_exit_hook_cleanup_isolation_device.sh'
require_text "$F46_PANIC_DIAGNOSIS" 'does not query or clear slot0 exit-hook state after slot0 has reported'
require_text "$F46_PANIC_DIAGNOSIS" 'D4-R2 First Device Result'
require_text "$F46_PANIC_DIAGNOSIS" 'D4-R2-exit-hook-clear-only-panic'
require_text "$F46_PANIC_DIAGNOSIS" 'No `raw slot clear 0x729249` command appears before panic'
require_text "$F46_PANIC_DIAGNOSIS" 'D4-R3 Planned Source Packet Gate'
require_text "$F46_PANIC_DIAGNOSIS" 'D4-R3 is now a named source packet'
require_text "$F46_PANIC_DIAGNOSIS" 'g_raw_exit_hook_wrapped'
require_text "$F46_PANIC_DIAGNOSIS" 'r0lab_raw_page_table_active_count_locked() == 0'
require_text "$F46_PANIC_DIAGNOSIS" 'migrate `r0lab_raw_exit_mmap_before()` to scan'
require_text "$F46_PANIC_DIAGNOSIS" 'If D4-R3 passes, queue D4-R4'
require_text "$F46_PANIC_DIAGNOSIS" 'docs/wxshadow-f4.6-d4-r3a-diagnostic-split-plan.md'
require_text "$F46_PANIC_DIAGNOSIS" 'scripts/test_raw_exit_hook_preclear_hold_split_device.sh'
require_text "$F46_PANIC_DIAGNOSIS" 'D4-R3a-hold-self-unstable'
require_text "$F46_PANIC_DIAGNOSIS" 'D4-R3a-boot-id-reader-unstable'
require_text "$F46_PANIC_DIAGNOSIS" 'D4-R3a First Device Result'
require_text "$F46_PANIC_DIAGNOSIS" 'phase=d4_r3a_boot_id_reader boot_id_reader_rc=134'
require_text "$F46_PANIC_DIAGNOSIS" 'F4.6-D4-R3b Lab App-only raw-hold split packet'
require_text "$F46_PANIC_DIAGNOSIS" 'D4-R3b plan-lock and Lab App implementation result'
require_text "$F46_PANIC_DIAGNOSIS" 'docs/wxshadow-f4.6-d4-r3b-raw-hold-split-plan.md'
require_text "$F46_PANIC_DIAGNOSIS" 'raw raw-hold routing hold <token>'
require_text "$F46_PANIC_DIAGNOSIS" 'scripts/test_raw_exit_hook_raw_hold_split_device.sh'
require_text "$F46_PANIC_DIAGNOSIS" 'raw mode=raw-hold-routing-hold failures=0'
require_text "$F46_PANIC_DIAGNOSIS" 'exit_mmap_armed=0'
require_text "$F46_PANIC_DIAGNOSIS" 'D4-R3b-L local source result'
require_text "$F46_PANIC_DIAGNOSIS" 'D4-R3b-raw-hold-anchor-too-strong'
require_text "$F46_PANIC_DIAGNOSIS" 'exit_hook_installed=not_queried/not_queried'
require_text "$F46_PANIC_DIAGNOSIS" 'D4-R3b-post-hold-status-reader-unstable'
require_text "$F46_PANIC_DIAGNOSIS" 'raw-exit-hook-raw-hold-split-20260724-061838.log'
require_text "$F46_PANIC_DIAGNOSIS" 'HOLD_ACTIVE=1'
require_text "$F46_PANIC_DIAGNOSIS" 'F4.6-D4-R3c status-reader split plan'
require_text "$F46_PANIC_DIAGNOSIS" 'docs/wxshadow-f4.6-d4-r3c-status-reader-split-plan.md'
require_text "$F46_PANIC_DIAGNOSIS" 'boringssl_self_test_apex64'
require_text "$F46_PANIC_DIAGNOSIS" 'Current classification: `system-reboot-after-owner-exit`.'
require_text "$F46_PANIC_DIAGNOSIS" 'warn_count` stayed `2 -> 2`'
require_text "$F46_PANIC_DIAGNOSIS" 'LC_ALL=C grep'
require_text "$F46_PANIC_DIAGNOSIS" 'R1 stop rule'
require_text "$F46_PANIC_DIAGNOSIS" 'F4.6-R0'
require_text "$F46_PANIC_DIAGNOSIS" 'F4.6-R1'
require_text "$F46_PANIC_DIAGNOSIS" 'Do not patch D4 before D4-R3e-L2 live-PTE planning converts'
require_text "$F46_PANIC_DIAGNOSIS" 'D4-R3c-status-logcat-timeout-kernel-panic'
require_text "$F46_PANIC_DIAGNOSIS" 'docs/wxshadow-f4.6-d4-r3d-status-transport-split-plan.md'
require_text "$F46_PANIC_DIAGNOSIS" 'D4-R3d-raw-hold-self-unstable'
require_text "$F46_PANIC_DIAGNOSIS" 'docs/wxshadow-f4.6-d4-r3e-raw-hold-lifetime-plan.md'
require_text "$F46_PANIC_DIAGNOSIS" 'docs/wxshadow-f4.6-d4-r3e-l2-live-pte-plan.md'
require_text "$F46_PANIC_DIAGNOSIS" 'D4-R3e-L1-single-source-uxn-unstable'
require_text "$F46_PANIC_DIAGNOSIS" 'raw-hold-lifetime-matrix-20260724-071931.log'
require_text "$DEVELOPMENT_SEQUENCE" 'Active Slice Board'
require_text "$DEVELOPMENT_SEQUENCE" 'F4.6-R1 slot-0 compatibility isolation'
require_text "$DEVELOPMENT_SEQUENCE" 'R1 is not a feature slice'
require_text "$DEVELOPMENT_SEQUENCE" 'Planning-First Development Gate'
require_text "$DEVELOPMENT_SEQUENCE" 'Finding missing behavior in source review is not enough to edit source'
require_text "$DEVELOPMENT_SEQUENCE" 'If the first targeted smoke fails, update the diagnosis'
require_text "$DEVELOPMENT_SEQUENCE" 'R1b was the only earlier KPM source slice'
require_text "$DEVELOPMENT_SEQUENCE" 'D4-R3h then added only the full callback'
require_text "$DEVELOPMENT_SEQUENCE" 'F4.6-D4-R2 explicit-clear cleanup isolation'
require_text "$DEVELOPMENT_SEQUENCE" 'Passed through R1d'
require_text "$DEVELOPMENT_SEQUENCE" 'F4.6-D3 Lab two-slot hold source | Passed locally'
require_text "$DEVELOPMENT_SEQUENCE" 'F4.6-D4 two-slot owner-exit smoke | Failed evidence captured'
require_text "$DEVELOPMENT_SEQUENCE" 'F4.6-D4-R0 failure classification | Passed'
require_text "$DEVELOPMENT_SEQUENCE" 'over-strong no-exit-hook `op=18` event anchor'
require_text "$DEVELOPMENT_SEQUENCE" 'F4.6-D4-R1 reboot-source decoupling | Failed/classified'
require_text "$DEVELOPMENT_SEQUENCE" 'F4.6-D4-R2 explicit-clear cleanup isolation | Failed/classified'
require_text "$DEVELOPMENT_SEQUENCE" 'F4.6-D4-R3 exit-hook release source packet | Attempted/blocked'
require_text "$DEVELOPMENT_SEQUENCE" 'F4.6-D4-R3a pre-clear hold-state split | Failed/classified'
require_text "$DEVELOPMENT_SEQUENCE" 'F4.6-D4-R3b Lab App-only raw-hold split | Failed/classified'
require_text "$DEVELOPMENT_SEQUENCE" 'F4.6-D4-R3c status-reader split | Failed/classified'
require_text "$DEVELOPMENT_SEQUENCE" 'F4.6-D4-R3d status-transport split | Failed/classified'
require_text "$DEVELOPMENT_SEQUENCE" 'F4.6-D4-R3e-L2 live-PTE snapshot | Device evidence captured'
require_text "$DEVELOPMENT_SEQUENCE" 'F4.6-D4-R3e-L3 observer perturbation | Complete/classified postmortem'
require_text "$DEVELOPMENT_SEQUENCE" 'F4.6-D4-R3f abort-hook exposure | Complete/classified stable'
require_text "$DEVELOPMENT_SEQUENCE" 'D4-R3d-P | Plan/docs/contract only'
require_text "$DEVELOPMENT_SEQUENCE" 'D4-R3d-L1 | Raw-hold idle diagnostic script only'
require_text "$DEVELOPMENT_SEQUENCE" 'docs/wxshadow-f4.6-d4-r3a-diagnostic-split-plan.md'
require_text "$DEVELOPMENT_SEQUENCE" 'docs/wxshadow-f4.6-d4-r3b-raw-hold-split-plan.md'
require_text "$DEVELOPMENT_SEQUENCE" 'docs/wxshadow-f4.6-d4-r3c-status-reader-split-plan.md'
require_text "$DEVELOPMENT_SEQUENCE" 'scripts/test_raw_exit_hook_preclear_hold_split_device.sh'
require_text "$DEVELOPMENT_SEQUENCE" 'scripts/test_raw_exit_hook_raw_hold_split_device.sh'
require_text "$DEVELOPMENT_SEQUENCE" 'D4-R3a-boot-id-reader-unstable'
require_text "$DEVELOPMENT_SEQUENCE" 'D4-R3b-raw-hold-anchor-too-strong'
require_text "$DEVELOPMENT_SEQUENCE" 'D4-R3b-post-hold-status-reader-unstable'
require_text "$DEVELOPMENT_SEQUENCE" 'Fixed Next Work Queue'
require_text "$DEVELOPMENT_SEQUENCE" 'classify_status_after_raw_hold()'
require_text "$DEVELOPMENT_SEQUENCE" 'D4-R3c-status-logcat-timeout-kernel-panic'
require_text "$DEVELOPMENT_SEQUENCE" 'clean-source device row was stable'
require_text "$DEVELOPMENT_SEQUENCE" 'The read-only audit selected D4-R3g as the'
require_text "$DEVELOPMENT_SEQUENCE" 'scripts/test_raw_exit_hook_raw_hold_idle_device.sh'
require_text "$DEVELOPMENT_SEQUENCE" 'raw-exit-hook-raw-hold-idle-20260724-065137.log'
require_text "$DEVELOPMENT_SEQUENCE" 'classification is `D4-R3d-raw-hold-self-unstable`'
require_text "$F46_D4_R3A_PLAN" 'wxshadow F4.6 D4-R3a Diagnostic Split Plan'
require_text "$F46_D4_R3A_PLAN" 'Status: current diagnostic packet'
require_text "$F46_D4_R3A_PLAN" 'This packet does not unlock a KPM behavior edit'
require_text "$F46_D4_R3A_PLAN" 'scripts/test_raw_exit_hook_preclear_hold_split_device.sh'
require_text "$F46_D4_R3A_PLAN" 'phase=d4_r3a_hold_quiet_no_boot_reader result=pass'
require_text "$F46_D4_R3A_PLAN" 'phase=d4_r3a_boot_id_reader result=pass boot_stable=1'
require_text "$F46_D4_R3A_PLAN" 'raw_exit_hook_preclear_hold_split=pass phases=5'
require_text "$F46_D4_R3A_PLAN" 'First Device Result'
require_text "$F46_D4_R3A_PLAN" 'phase=d4_r3a_boot_id_reader boot_id_reader_rc=134'
require_text "$F46_D4_R3A_PLAN" 'D4-R3a-boot-id-reader-unstable'
require_text "$F46_D4_R3A_PLAN" 'F4.6-D4-R3b Lab App-only raw-hold split packet'
require_text "$F46_D4_R3A_PLAN" 'Lab App-only raw-hold split packet'
require_text "$F46_D4_R3B_PLAN" 'wxshadow F4.6 D4-R3b Raw-Hold Split Plan'
require_text "$F46_D4_R3B_PLAN" 'Status: plan-lock committed'
require_text "$F46_D4_R3B_PLAN" 'D4-R3b-P plan lock'
require_text "$F46_D4_R3B_PLAN" 'D4-R3b-L Lab App raw-hold implementation'
require_text "$F46_D4_R3B_PLAN" 'Finding missing behavior during source review is not enough to edit source'
require_text "$F46_D4_R3B_PLAN" 'raw raw-hold routing hold <token>'
require_text "$F46_D4_R3B_PLAN" 'scripts/test_raw_exit_hook_raw_hold_split_device.sh'
require_text "$F46_D4_R3B_PLAN" 'kpm/r0lab.c` behavior changes'
require_text "$F46_D4_R3B_PLAN" 'exit_mmap_armed=0'
require_text "$F46_D4_R3B_PLAN" 'phase=d4_r3b_raw_hold_established result=pass exit_mmap_armed=0 raw_slots=2 page_records=2'
require_text "$F46_D4_R3B_PLAN" 'D4-R3b-boot-id-reader-unstable'
require_text "$F46_D4_R3B_PLAN" 'scripts/verify_v1_contract.sh'
require_text "$F46_D4_R3B_PLAN" 'D4-R3b-L Local Source Result'
require_text "$F46_D4_R3B_PLAN" 'r0lab_raw_hold_routing_hold()'
require_text "$F46_D4_R3B_PLAN" 'Full device reader verification did not reach the shell/getprop or boot-id'
require_text "$F46_D4_R3B_PLAN" 'D4-R3b First Device Attempt'
require_text "$F46_D4_R3B_PLAN" 'raw-exit-hook-raw-hold-split-20260724-061455.log'
require_text "$F46_D4_R3B_PLAN" 'D4-R3b-raw-hold-anchor-too-strong'
require_text "$F46_D4_R3B_PLAN" 'exit_hook_installed=not_queried/not_queried'
require_text "$F46_D4_R3B_PLAN" 'D4-R3b Second Device Attempt'
require_text "$F46_D4_R3B_PLAN" 'raw-exit-hook-raw-hold-split-20260724-061838.log'
require_text "$F46_D4_R3B_PLAN" 'D4-R3b-post-hold-status-reader-unstable'
require_text "$F46_D4_R3B_PLAN" 'HOLD_ACTIVE=1'
require_text "$F46_D4_R3B_PLAN" 'docs/wxshadow-f4.6-d4-r3c-status-reader-split-plan.md'
require_text "$F46_D4_R3C_PLAN" 'wxshadow F4.6 D4-R3c Status-Reader Split Plan'
require_text "$F46_D4_R3C_PLAN" 'Status: plan-lock committed; D4-R3c-L harness evidence patch implemented'
require_text "$F46_D4_R3C_PLAN" 'D4-R3b-post-hold-status-reader-unstable'
require_text "$F46_D4_R3C_PLAN" 'D4-R3c-status-logcat-timeout-kernel-panic'
require_text "$F46_D4_R3C_PLAN" 'raw-exit-hook-raw-hold-split-20260724-061838.log'
require_text "$F46_D4_R3C_PLAN" 'raw-exit-hook-raw-hold-split-20260724-062956.log'
require_text "$F46_D4_R3C_PLAN" 'raw-exit-hook-raw-hold-split-20260724-062956-postreboot.console-ramoops-0.txt'
require_text "$F46_D4_R3C_PLAN" 'D4-R3c-L harness evidence patch'
require_text "$F46_D4_R3C_PLAN" 'Append `STATUS_HELD` to the evidence file before any `require_contains`'
require_text "$F46_D4_R3C_PLAN" 'D4-R3c-L Local Harness Result'
require_text "$F46_D4_R3C_PLAN" 'D4-R3c-D Device Result'
require_text "$F46_D4_R3C_PLAN" 'classify_status_after_raw_hold()'
require_text "$F46_D4_R3C_PLAN" 'phase=d4_r3c_status_after_raw_hold evidence=begin'
require_text "$F46_D4_R3C_PLAN" 'D4-R3c-status-logcat-timeout'
require_text "$F46_D4_R3C_PLAN" 'D4-R3c-status-empty-or-reset-state'
require_text "$F46_D4_R3C_PLAN" 'D4-R3c-status-malformed-output'
require_text "$F46_D4_R3C_PLAN" 'D4-R3c-status-healthy-boot-id-pending'
require_text "$F46_D4_R3C_PLAN" 'KPM behavior, Lab raw-hold behavior, cleanup ownership, reader order'
require_text "$F46_D4_R3C_PLAN" 'Local verification passed'
require_text "$F46_D4_R3C_PLAN" 'scripts/build_lab_app.sh'
require_text "$F46_D4_R3C_PLAN" 'command_timeout command=status wait_ms=10000'
require_text "$F46_D4_R3C_PLAN" 'Kernel panic - not syncing: Attempted to kill init! exitcode=0x0000000b'
require_text "$F46_D4_R3C_PLAN" 'show_map_vma'
require_text "$F46_D4_R3C_PLAN" 'docs/wxshadow-f4.6-d4-r3d-status-transport-split-plan.md'
require_text "$F46_D4_R3C_PLAN" 'Do not use D4-R3c to add arbitrary target support'
require_text "$F46_D4_R3D_PLAN" 'wxshadow F4.6 D4-R3d Status-Transport Split Plan'
require_text "$F46_D4_R3D_PLAN" 'D4-R3d-L1 raw-hold idle diagnostic'
require_text "$F46_D4_R3D_PLAN" 'D4-R3c-status-logcat-timeout-kernel-panic'
require_text "$F46_D4_R3D_PLAN" 'command_timeout command=status wait_ms=10000'
require_text "$F46_D4_R3D_PLAN" 'show_map_vma'
require_text "$F46_D4_R3D_PLAN" 'seq_read_iter'
require_text "$F46_D4_R3D_PLAN" 'raw-hold idle split'
require_text "$F46_D4_R3D_PLAN" 'status transport split'
require_text "$F46_D4_R3D_PLAN" 'maps reader split'
require_text "$F46_D4_R3D_PLAN" 'D4-R3d-raw-hold-self-unstable'
require_text "$F46_D4_R3D_PLAN" 'D4-R3d-app-logcat-transport-timeout'
require_text "$F46_D4_R3D_PLAN" 'D4-R3d-kpm-status-supercall-blocks'
require_text "$F46_D4_R3D_PLAN" 'D4-R3d-proc-maps-reader-panic'
require_text "$F46_D4_R3D_PLAN" 'Do not change KPM source during D4-R3d'
require_text "$F46_D4_R3D_PLAN" 'scripts/test_raw_exit_hook_raw_hold_idle_device.sh'
require_text "$F46_D4_R3D_PLAN" 'raw-exit-hook-raw-hold-idle-20260724-065137.log'
require_text "$F46_D4_R3D_PLAN" 'D4-R3d-L2 status transport and D4-R3d-L3 maps'
require_text "$F46_D4_R3D_PLAN" 'docs/wxshadow-f4.6-d4-r3e-raw-hold-lifetime-plan.md'
require_text "$F46_D4_R3D_PLAN" 'post_hold_status=not_used'
require_text "$F46_D4_R3D_PLAN" 'boot_id_reader=not_used'
require_text "$F46_D4_R3D_PLAN" 'cleanup=preserve'
require_text "$F46_D4_R3E_PLAN" 'wxshadow F4.6 D4-R3e Raw-Hold Lifetime Plan'
require_text "$F46_D4_R3E_PLAN" 'D4-R3d-raw-hold-self-unstable'
require_text "$F46_D4_R3E_PLAN" 'raw-exit-hook-raw-hold-idle-20260724-065137.log'
require_text "$F46_D4_R3E_PLAN" 'PTE locking, TLB invalidation, data/instruction cache maintenance, and'
require_text "$F46_D4_R3E_PLAN" 'Read-only audit result: `raw-hold-routing-hold` establishes two active'
require_text "$F46_D4_R3E_PLAN" 'raw_slot_inspect` reports the stored `active_pte` and record state'
require_text "$F46_D4_R3E_PLAN" 'No KPM C source is unlocked by D4-R3e-L1'
require_text "$F46_D4_R3E_PLAN" 'raw raw-hold lifetime source single <token>'
require_text "$F46_D4_R3E_PLAN" 'scripts/test_raw_hold_lifetime_matrix_device.sh'
require_text "$F46_D4_R3E_PLAN" 'D4-R3e-L1-two-shadow-rx-unstable'
require_text "$F46_D4_R3E_PLAN" 'Implementation status: D4-R3e-L1 adds the Lab App command dispatcher'
require_text "$F46_D4_R3E_PLAN" 'post_hold_status=not_used boot_id_reader=not_used proc_maps=not_used'
require_text "$F46_D4_R3E_PLAN" 'D4-R3e-L1 Clean-Source Device Result'
require_text "$F46_D4_R3E_PLAN" 'raw-hold-lifetime-matrix-20260724-071931.log'
require_text "$F46_D4_R3E_PLAN" 'D4-R3e-L1-single-source-uxn-unstable'
require_text "$F46_D4_R3E_PLAN" 'single retained `SOURCE_UXN` raw PTE'
require_text "$F46_D4_R3E_PLAN" 'D4-R3e-L2 Planning Gate'
require_text "$F46_D4_R3E_PLAN" 'D4-R3e-L2-P'
require_text "$F46_D4_R3E_PLAN" 'docs/wxshadow-f4.6-d4-r3e-l2-live-pte-plan.md'
require_text "$F46_D4_R3E_PLAN" 'live-PTE snapshot after `source_uxn` arm'
require_text "$F46_D4_R3E_PLAN" 'D4-R3e-A | Read-only source audit'
require_text "$F46_D4_R3E_PLAN" 'D4-R3e-L1 | Lab App lifetime matrix only'
require_text "$F46_D4_R3E_PLAN" 'Do not continue D4-R3d-L2 status transport or L3 maps reader work'
require_text "$F46_D4_R3E_L2_PLAN" 'wxshadow F4.6 D4-R3e-L2 Live PTE Snapshot Plan'
require_text "$F46_D4_R3E_L2_PLAN" 'Status: the narrow source packet is implemented and builds locally.'
require_text "$F46_D4_R3E_L2_PLAN" 'D4-R3e-L1-single-source-uxn-unstable'
require_text "$F46_D4_R3E_L2_PLAN" 'D4-R3e-L2-live-pte-divergence'
require_text "$F46_D4_R3E_L2_PLAN" 'no PTE replacement ordering changes'
require_text "$F46_D4_R3E_L2_PLAN" 'no `SHADOW_RX` activation'
require_text "$F46_D4_R3E_L2_PLAN" 'raw mode=raw-hold-live-pte'
require_text "$F46_D4_R3E_L2_PLAN" 'live_pte=<hex> expected_pte=<hex>'
require_text "$F46_D4_R3E_L2_PLAN" 'D4-R3e-L2-live-pte-match-then-unstable'
require_text "$F46_D4_R3E_L2_PLAN" 'scripts/test_raw_live_pte_snapshot_device.sh'
require_text "$F46_D4_R3E_L2_PLAN" 'raw slot live pte <token> <slot>'
require_text "$F46_D4_R3E_L2_PLAN" 'raw raw-hold live-pte <token>'
require_text "$F46_D4_R3E_L2_PLAN" 'Clean-Source Device Result'
require_text "$F46_D4_R3E_L2_PLAN" 'raw-live-pte-snapshot-20260724-080322.log'
require_text "$F46_D4_R3E_L2_PLAN" 'live_pte=e00009f9bfffc3 expected_pte=e00009f9bfffc3'
require_text "$F46_D4_R3E_L2_PLAN" 'D4-R3e-L2-live-pte-stable'
require_text "$F46_D4_R3E_L2_PLAN" 'repeat/lower-intrusion observation'
require_text "$F46_D4_R3E_L2_PLAN" 'docs/wxshadow-f4.6-d4-r3e-l3-observer-perturbation-plan.md'
require_text "$F46_D4_R3E_L3_PLAN" 'wxshadow F4.6 D4-R3e-L3 Observer Perturbation Plan'
require_text "$F46_D4_R3E_L3_PLAN" 'Status: all four L3-A device attempts are captured.'
require_text "$F46_D4_R3E_L3_PLAN" 'postmortem-recovered unstable row'
require_text "$F46_D4_R3E_L3_PLAN" 'D4-R3e-L3-observer-perturbation'
require_text "$F46_D4_R3E_L3_PLAN" 'D4-R3e-L3-B0-baseline-unstable'
require_text "$F46_D4_R3E_L3_PLAN" 'D4-R3e-L3-B1-external-snapshot-stable'
require_text "$F46_D4_R3E_L3_PLAN" 'D4-R3e-L3-observer-correlated'
require_text "$F46_D4_R3E_L3_PLAN" 'No majority vote is allowed.'
require_text "$F46_D4_R3E_L3_PLAN" 'snapshot_stage=after_set_same_pte_lock'
require_text "$F46_D4_R3E_L3_PLAN" 'inline_walk=0 extra_pte_lock=0'
require_text "$F46_D4_R3E_L3_PLAN" 'No KPM or Lab source is allowed in L3-A.'
require_text "$F46_D4_R3E_L3_PLAN" 'RAW_OBSERVER_CLEAN_BOOT_CONFIRMED=1'
require_text "$F46_D4_R3E_L3_PLAN" 'RAW_OBSERVER_PAIRED_RUN=1..4'
require_text "$F46_D4_R3E_L3_PLAN" 'scripts/test_raw_observer_perturbation_device.sh'
require_text "$F46_D4_R3E_L3_PLAN" 'D4-R3e-L3-B1-live-pte-walk-failed'
require_text "$F46_D4_R3E_L3_PLAN" 'D4-R3e-L3-B1-live-pte-mismatch'
require_text "$F46_D4_R3E_L3_PLAN" 'D4-R3e-L3-B0-baseline-unstable'
require_text "$F46_D4_R3E_L3_PLAN" 'raw-observer-perturbation-run1-baseline-20260724-095302.log'
require_text "$F46_D4_R3E_L3_PLAN" 'poll 12 returned `device not found`'
require_text "$F46_D4_R3E_L3_PLAN" '87a8af2453bbd7753511a58d7ca0925edd6bc5bb61a8f3bdcb5de01fa5ef853b'
require_text "$F46_D4_R3E_L3_PLAN" 'raw-observer-perturbation-run2-external-20260724-100506.log'
require_text "$F46_D4_R3E_L3_PLAN" 'D4-R3e-L3-B1-external-snapshot-unstable'
require_text "$F46_D4_R3E_L3_PLAN" 'transport failure occurred after that established row, at idle second 1'
require_text "$F46_D4_R3E_L3_PLAN" '3639e9817b27d6edf919b306dc92fbe3a8f58b949131cc15f09b4eee106c1959'
require_text "$F46_D4_R3E_L3_PLAN" 'raw-observer-perturbation-run3-baseline-20260724-101331.log'
require_text "$F46_D4_R3E_L3_PLAN" 'D4-R3e-L3-B0-baseline-stable'
require_text "$F46_D4_R3E_L3_PLAN" 'all 15 transport-only polls returned `device`'
require_text "$F46_D4_R3E_L3_PLAN" '5e90385beec75fcf916284bf88d79f28228762cdaf1b549123b1d4028898b84f'
require_text "$F46_D4_R3E_L3_PLAN" 'raw-observer-perturbation-run4-external-20260724-101821.log'
require_text "$F46_D4_R3E_L3_PLAN" '6188f0ed62ee0cf31a0fc35365229a1c81fb8dbfd31898109a6c963868362c5e'
require_text "$F46_D4_R3E_L3_PLAN" 'Attempted to kill init'
require_text "$F46_D4_R3E_L3_PLAN" 'The runtime classification is'
require_text "$F46_D4_R3E_L3_PLAN" 'strict offline classifier intentionally continues to reject'
require_text "$F46_D4_R3E_L3_PLAN" 'docs/wxshadow-f4.6-d4-r3f-abort-hook-exposure-plan.md'
require_text "$F46_D4_R3E_L3_PLAN" 'R0LAB_DEBUG_KEYSTORE='
require_text scripts/build_lab_app.sh 'R0LAB_DEBUG_KEYSTORE'
require_text scripts/build_lab_app.sh 'explicit Lab keystore is missing'
require_text "$F46_D4_R3E_L3_PLAN" 'L3-A Offline Aggregate Classifier'
require_text "$F46_D4_R3E_L3_PLAN" 'mutually exclusive precedence'
require_text "$F46_D4_R3E_L3_PLAN" 'D4-R3e-L3-environment-drift'
require_text "$F46_D4_R3E_L3_PLAN" 'D4-R3e-L3-observer-not-correlated'
require_text "$F46_D4_R3E_L3_PLAN" 'D4-R3e-L3-repeat-nondeterministic'
require_text "$F46_D4_R3E_L3_PLAN" 'RAW_OBSERVER_HISTORICAL_BASELINE_LOG'
require_text "$F46_D4_R3E_L3_PLAN" 'RAW_OBSERVER_HISTORICAL_EXTERNAL_LOG'
require_text "$F46_D4_R3E_L3_PLAN" 'accepted historical harness artifact'
require_text "$F46_D4_R3E_L3_PLAN" 'scripts/classify_raw_observer_perturbation_evidence.sh'
require_text "$F46_D4_R3E_L3_PLAN" 'scripts/test_raw_observer_aggregate_host.sh'
require_text "$F46_D4_R3E_L3_PLAN" 'The local host test passes all four valid aggregate classifications'
require_text "$F46_D4_R3E_L3_PLAN" 'four rejected-input cases'
require_text "$F46_D4_R3E_L3_PLAN" 'metadata-mismatch rejection cases'
require_text "$F46_D4_R3E_L3_PLAN" 'real historical anchors'
require_text "$F46_D4_R3F_PLAN" 'wxshadow F4.6 D4-R3f Abort-Hook Exposure Plan'
require_text "$F46_D4_R3F_PLAN" 'D4-R3f-global-abort-hook-exposure'
require_text "$F46_D4_R3F_PLAN" 'raw slot arm no-abort <token> <slot> <page>'
require_text "$F46_D4_R3F_PLAN" 'raw raw-hold no-abort <token>'
require_text "$F46_D4_R3F_PLAN" 'abort_hook_installed=0 abort_hook_suppressed=1'
require_text "$F46_D4_R3F_PLAN" 'scripts/test_raw_abort_hook_exposure_device.sh'
require_text "$F46_D4_R3F_PLAN" 'D4-R3f-source-uxn-no-abort-stable'
require_text "$F46_D4_R3F_PLAN" 'D4-R3f-source-uxn-no-abort-unstable'
require_text "$F46_D4_R3F_PLAN" 'This is a causality split, not a production fix.'
require_text "$F46_D4_R3F_PLAN" 'PTE descriptor bits or replacement ordering'
require_text "$F46_D4_R3F_PLAN" 'Every stable or unstable row requires a separate physical reboot'
require_text "$F46_D4_R3F_PLAN" 'raw-abort-hook-exposure-20260724-105834.log'
require_text "$F46_D4_R3F_PLAN" '7c1a706e5b96930508201a36db9e11304516866db84eaa9fd4751a554673826c'
require_text "$F46_D4_R3F_PLAN" 'classification=D4-R3f-source-uxn-no-abort-stable'
require_text "$F46_D4_R3F_PLAN" 'one stable short-window sample'
require_text "$F46_D4_R3G_PLAN" 'wxshadow F4.6 D4-R3g Passthrough Wrapper Plan'
require_text "$F46_D4_R3G_PLAN" 'D4-R3g-global-abort-trampoline-exposure'
require_text "$F46_D4_R3G_PLAN" 'raw slot arm abort-passthrough <token> <slot> <page>'
require_text "$F46_D4_R3G_PLAN" 'raw raw-hold abort-passthrough <token>'
require_text "$F46_D4_R3G_PLAN" 'abort_hook_installed=1 abort_hook_suppressed=0 abort_hook_passthrough=1'
require_text "$F46_D4_R3G_PLAN" 'r0lab_raw_before_abort_passthrough'
require_text "$F46_D4_R3G_PLAN" 'No per-fault passthrough counter is permitted'
require_text "$F46_D4_R3G_PLAN" 'scripts/test_raw_abort_wrapper_passthrough_device.sh'
require_text "$F46_D4_R3G_PLAN" 'D4-R3g-source-uxn-abort-passthrough-stable'
require_text "$F46_D4_R3G_PLAN" 'D4-R3g-source-uxn-abort-passthrough-unstable'
require_text "$F46_D4_R3G_PLAN" 'Every stable or unstable row requires a separate physical reboot'
require_text "$F46_D4_R3G_PLAN" 'KernelPatch source or hook-chain implementation'
require_text "$F46_D4_R3G_PLAN" 'raw-abort-wrapper-passthrough-20260724-112342.log'
require_text "$F46_D4_R3G_PLAN" '7e0e8a7917fd6a4aa50fce41fee8bea2e57a71d8e1d6d1f10568a2099a99d21f'
require_text "$F46_D4_R3G_PLAN" 'classification=D4-R3g-source-uxn-abort-passthrough-stable'
require_text "$F46_D4_R3G_PLAN" '15 of 15 online transport'
require_text "$F46_D4_R3H_PLAN" 'wxshadow F4.6 D4-R3h Global MM Reference Plan'
require_text "$F46_D4_R3H_PLAN" 'D4-R3h-global-current-mm-reference-exposure'
require_text "$F46_D4_R3H_PLAN" 'raw slot arm abort-mmget <token> <slot> <page>'
require_text "$F46_D4_R3H_PLAN" 'raw raw-hold abort-mmget <token>'
require_text "$F46_D4_R3H_PLAN" 'r0lab_raw_before_abort_mmget_passthrough'
require_text "$F46_D4_R3H_PLAN" 'g_get_task_mm(current)'
require_text "$F46_D4_R3H_PLAN" 'call `g_mmput()` exactly'
require_text "$F46_D4_R3H_PLAN" 'abort_hook_installed=1 abort_hook_suppressed=0 abort_hook_passthrough=0 abort_hook_mmget=1'
require_text "$F46_D4_R3H_PLAN" 'No callback counter is permitted'
require_text "$F46_D4_R3H_PLAN" 'scripts/test_raw_abort_mmget_passthrough_device.sh'
require_text "$F46_D4_R3H_PLAN" 'D4-R3h-source-uxn-abort-mmget-stable'
require_text "$F46_D4_R3H_PLAN" 'D4-R3h-source-uxn-abort-mmget-unstable'
require_text "$F46_D4_R3H_PLAN" 'Every stable or unstable row requires a separate physical reboot'
require_text "$F46_D4_R3H_PLAN" 'The absence of a callback counter'
require_text "$F46_D4_R3H_PLAN" 'KernelPatch or FolkPatch source and hook-chain implementation'
require_text "$F46_D4_R3H_PLAN" 'do not dereference hook arguments'
require_text "$F46_D4_R3H_PLAN" 'do not read `g_session`, owner tgid'
require_text "$F46_D4_R3H_PLAN" 'do not take an r0lab lock'
require_text "$F46_D4_R3H_PLAN" 'same pointer'
require_text "$F46_D4_R3H_PLAN" 'for rollback detach'
require_text "$F46_D4_R3H_PLAN" 'store the requested callback mode in the reserved page slot'
require_text "$F46_D4_R3H_PLAN" 'commit only `hook_installed` after global wrapper installation succeeds'
require_text "$F46_D4_R3H_PLAN" 'capture the immutable requested mode before clearing `hook_installed`'
require_text "$F46_D4_R3H_PLAN" 'After the hold is active, the script may run only 15 bounded'
require_text "$F46_D4_R3H_PLAN" 'must not issue post-hold status'
require_text "$F46_D4_R3H_PLAN" 'PTE descriptor bits or replacement ordering'
require_text "$F46_D4_R3H_PLAN" 'TLB or cache maintenance'
require_text "$F46_D4_R3H_PLAN" 'restore ABI or restore ordering'
require_text "$F46_D4_R3H_PLAN" 'Status: clean committed-source device evidence is classified stable.'
require_text "$F46_D4_R3H_PLAN" '## Local Source Checkpoint'
require_text "$F46_D4_R3H_PLAN" 'missing-clean-boot and dirty-source rejection before any ADB command'
require_text "$F46_D4_R3H_PLAN" 'existing full'
require_text "$F46_D4_R3H_PLAN" '`r0lab_raw_before_abort()` callback is unchanged'
require_text "$F46_D4_R3H_PLAN" 'static SHA-256 guards'
require_text "$F46_D4_R3H_PLAN" '## Clean Committed Source'
require_text "$F46_D4_R3H_PLAN" 'bbdfa6382fae716e486fbcb7b76430352ea6df23'
require_text "$F46_D4_R3H_PLAN" '331cd4249afdbc6e1f3f18e5b3a44149ac95b4dc7a4a30745632f6f8328e7336'
require_text "$F46_D4_R3H_PLAN" 'b4053bfd338761e78f38b9688c298adaf7c2f50346d16678396c5cbe33744d4a'
require_text "$F46_D4_R3H_PLAN" 'raw-abort-mmget-passthrough-20260724-120316.log'
require_text "$F46_D4_R3H_PLAN" 'bf7480aaf387e051007906bb70d4d9a1fe4c89263dde976c032351e738ef888c'
require_text "$F46_D4_R3H_PLAN" 'All 15 transport-only polls returned `device`.'
require_text "$F46_D4_R3H_PLAN" 'active_hold=1'
require_text "$F46_D4_R3H_PLAN" 'not a measured number of callback'
require_text "$F46_D4_R3I_PLAN" 'wxshadow F4.6 D4-R3i Abort-Lock Exposure Plan'
require_text "$F46_D4_R3I_PLAN" 'Status: device row classified stable; physical reboot closure complete.'
require_text "$F46_D4_R3I_PLAN" 'bbdfa6382fae716e486fbcb7b76430352ea6df23'
require_text "$F46_D4_R3I_PLAN" 'bf7480aaf387e051007906bb70d4d9a1fe4c89263dde976c032351e738ef888c'
require_text "$F46_D4_R3I_PLAN" 'wxshadow-v2-f46-d4-r3h-mm-reference-stable-20260724'
require_text "$F46_D4_R3I_PLAN" 'physical_reboot_confirmed=1'
require_text "$F46_D4_R3I_PLAN" 'post_reboot_boot_id=c5084d25-1e0f-4de4-9042-a972cd0170a8'
require_text "$F46_D4_R3I_PLAN" 'folkpatch_module_list=empty'
require_text "$F46_D4_R3I_PLAN" 'result_tag_target=e6ee7c080ec5760f4b2c43bb0062724009ab440f'
require_tag_target wxshadow-v2-f46-d4-r3h-mm-reference-stable-20260724 \
  e6ee7c080ec5760f4b2c43bb0062724009ab440f
require_text "$F46_D4_R3I_PLAN" 'base_commit=93601f7dc0166ce4559b80a79669b8905f8e4dc6'
require_text "$F46_D4_R3I_PLAN" 'base_tag=wxshadow-v2-f46-d4-r3i-lock-exposure-plan-20260724'
require_text "$F46_D4_R3I_PLAN" 'source_commit=65679740eb5253c2779bbd0a0c4dce89ebdd5529'
require_text "$F46_D4_R3I_PLAN" 'source_tag=wxshadow-v2-f46-d4-r3i-lock-exposure-source-20260724'
require_text "$F46_D4_R3I_PLAN" 'kpm_sha256=dfc4411b50f9ff233bb93905e5eca3da2bd8260dfc33d8a786f43fc67dfebb50'
require_text "$F46_D4_R3I_PLAN" 'apk_lib_entry=lib/arm64-v8a/liblabprobe.so'
require_text "$F46_D4_R3I_PLAN" 'apk_lib_entry_sha256=cad0df4c7bf406f6c48dc319b41864f0f7a7b667c17b7b41f040d56b1b82dcd4'
require_text "$F46_D4_R3I_PLAN" 'apk_dex_entry=classes.dex'
require_text "$F46_D4_R3I_PLAN" 'apk_dex_entry_sha256=325e8a54bd306ef4da230de9919d0da46dec112f97efa9fbf42646dc7dd7ec79'
require_text "$F46_D4_R3I_PLAN" 'whole-APK hash is not a reproducibility gate'
require_text "$F46_D4_R3I_PLAN" '73f1e2d251423909f33bfc7573580bd096834b57f680d5edb6e68655b1f903dd'
require_tag_target wxshadow-v2-f46-d4-r3i-lock-exposure-plan-20260724 \
  93601f7dc0166ce4559b80a79669b8905f8e4dc6
require_tag_target wxshadow-v2-f46-d4-r3i-lock-exposure-source-20260724 \
  65679740eb5253c2779bbd0a0c4dce89ebdd5529
require_text "$F46_D4_R3I_PLAN" 'INSTALL_FAILED_UPDATE_INCOMPATIBLE'
require_text "$F46_D4_R3I_PLAN" 'raw-abort-lock-passthrough-20260724-132524.log'
require_text "$F46_D4_R3I_PLAN" '7714473555eaecb40dcb28351b4986894d4d06bcd36a0480b54d645f778b9938'
require_text "$F46_D4_R3I_PLAN" 'classification=D4-R3i-source-uxn-abort-lock-stable'
require_text "$F46_D4_R3I_PLAN" 'exactly 15 transport-only polls, all returning `device`'
require_text "$F46_D4_R3I_PLAN" '`active_hold=1`'
require_text "$F46_D4_R3I_PLAN" 'does not count callback invocations'
require_text "$F46_D4_R3I_PLAN" 'previous_boot_id=c5084d25-1e0f-4de4-9042-a972cd0170a8'
require_text "$F46_D4_R3I_PLAN" 'post_reboot_boot_id=0a73bd18-ba4a-4bdf-87c6-a99f98d9d039'
require_text "$F46_D4_R3I_PLAN" 'boot_id_changed=1'
require_text "$F46_D4_R3I_PLAN" 'raw-abort-lock-reboot-closure-20260724-133350.log'
require_text "$F46_D4_R3I_PLAN" '6ca72573234f38b4006f58789d8a5af8ca742062460ef8693185bed64b7d9ee2'
require_text "$F46_D4_R3I_PLAN" 'result_tag=wxshadow-v2-f46-d4-r3i-lock-exposure-stable-20260724'
require_text "$F46_D4_R3I_PLAN" 'result_tag_target=5d7ec82d926c30856ffd650376dd4df00c9bee04'
require_tag_target wxshadow-v2-f46-d4-r3i-lock-exposure-stable-20260724 \
  5d7ec82d926c30856ffd650376dd4df00c9bee04
require_text "$F46_D4_R3I_PLAN" 'D4-R3i-global-abort-r0lab-lock-exposure'
require_text "$F46_D4_R3I_PLAN" 'raw slot arm abort-lock <token> <slot> <page>'
require_text "$F46_D4_R3I_PLAN" 'raw raw-hold abort-lock <token>'
require_text "$F46_D4_R3I_PLAN" 'r0lab_raw_before_abort_lock_passthrough'
require_text "$F46_D4_R3I_PLAN" 'g_get_task_mm(current)'
require_text "$F46_D4_R3I_PLAN" 'call `g_mmput()` exactly once'
require_text "$F46_D4_R3I_PLAN" 'call `r0lab_lock()` exactly once'
require_text "$F46_D4_R3I_PLAN" 'immediately call `r0lab_unlock(flags)`'
require_text "$F46_D4_R3I_PLAN" 'do not use trylock, retry, timeout, logging, counters, atomics, or events'
require_text "$F46_D4_R3I_PLAN" 'do not dereference hook arguments'
require_text "$F46_D4_R3I_PLAN" 'do not read `g_session`, owner tgid'
require_text "$F46_D4_R3I_PLAN" 'do not increment `g_raw_inflight`'
require_text "$F46_D4_R3I_PLAN" 'route a page, mutate `skip_origin`'
require_text "$F46_D4_R3I_PLAN" 'reject mixing normal, suppressed, pure-passthrough, mmget-passthrough, and'
require_text "$F46_D4_R3I_PLAN" 'store the requested callback mode in the reserved page slot before its arm'
require_text "$F46_D4_R3I_PLAN" 'select the callback pointer exactly once from that immutable mode'
require_text "$F46_D4_R3I_PLAN" 'use the same pointer for `hook_wrap3()` and rollback detach'
require_text "$F46_D4_R3I_PLAN" 'compare the requested'
require_text "$F46_D4_R3I_PLAN" 'reject a mismatch before sharing the wrapper'
require_text "$F46_D4_R3I_PLAN" 'commit only `hook_installed` after global wrapper installation succeeds'
require_text "$F46_D4_R3I_PLAN" 'capture the immutable requested mode before clearing'
require_text "$F46_D4_R3I_PLAN" 'detach the exact callback'
require_text "$F46_D4_R3I_PLAN" 'PTE/TLB/cache behavior'
require_text "$F46_D4_R3I_PLAN" 'abort_hook_lock=1'
require_text "$F46_D4_R3I_PLAN" 'scripts/test_raw_abort_lock_passthrough_device.sh'
require_text "$F46_D4_R3I_PLAN" 'RAW_ABORT_LOCK_CLEAN_BOOT_CONFIRMED=1'
require_text "$F46_D4_R3I_PLAN" 'separately confirmed physical reboot after D4-R3h'
require_text "$F46_D4_R3I_PLAN" 'fixed to source/single with no'
require_text "$F46_D4_R3I_PLAN" 'UXN execution after arm'
require_text "$F46_D4_R3I_PLAN" 'After the hold is active, the script may run only 15 bounded'
require_text "$F46_D4_R3I_PLAN" 'must not issue post-hold status'
require_text "$F46_D4_R3I_PLAN" 'D4-R3i-source-uxn-abort-lock-stable'
require_text "$F46_D4_R3I_PLAN" 'D4-R3i-source-uxn-abort-lock-unstable'
require_text "$F46_D4_R3I_PLAN" 'Every stable or unstable row requires a separate physical reboot'
require_text "$F46_D4_R3I_PLAN" 'it does not prove unique root cause'
require_text "$F46_D4_R3I_PLAN" 'not a measured invocation count'
require_text "$F46_D4_R3I_PLAN" 'D4-R3i source symbols, Lab command, and device script are now required.'
require_text "$F46_D4_R3I_PLAN" 'function SHA-256 guards'
require_function_sha256 kpm/r0lab.c r0lab_raw_before_abort_mmget_passthrough \
  780ffc1e7ea492813c8776aed6ff5ee5530c425f4e89316b42ca0806f8155e7c
require_function_sha256 kpm/r0lab.c \
  r0lab_raw_before_abort_lock_passthrough \
  be53f3e27caa976d597375ce599bd626404c10e9aad630240acb5e4e4c11aa92
require_function_sha256 kpm/r0lab.c r0lab_raw_abort_hook_callback \
  21385cee16f278490a57beda8702055f314875516c7758f987188b66526675fd
require_function_sha256 kpm/r0lab.c \
  r0lab_raw_abort_hook_installed_callback_locked \
  d74bd88f1daf89464f7bb26a8db384ecbd6600e6ede9388c3a3f8c8f0b5a7180
require_function_sha256 kpm/r0lab.c r0lab_raw_abort_hook_acquire \
  7547ebe1c19a6654cebcb7aa1ec58dce8861772f237e614bf6423b56d2e94676
require_function_sha256 kpm/r0lab.c r0lab_raw_abort_hook_release \
  141e4bf9e15aaee8b08558784c957ba4dcd58dd3a5bab2d3b8e5bd22ca8426e3
require_function_sha256 kpm/r0lab.c r0lab_raw_slot_arm \
  f116231d20d16d97d51ca27467c54cf7f61708397f8d8c75b25573a6b9641589
require_function_sha256 kpm/r0lab.c r0lab_control0 \
  14bc6f0ae0e9902f9ac78d861a9e77a999c00f09324799565b3276e2157d6a7b
require_function_sha256 lab-app/src/main/cpp/labprobe.c \
  r0lab_raw_hold_lifetime_common \
  56ec74766dd25f01f58650aaffa7c147f21f7f46ce1050b86f0958859e882302
require_function_sha256 lab-app/src/main/cpp/labprobe.c \
  r0lab_raw_hold_abort_lock \
  d9b68a753737d4d3f8d6c247f1fb0f90b9edd387d3b8f88e6addf0967565a25e
require_function_sha256 lab-app/src/main/cpp/labprobe.c \
  Java_dev_r0hook_lab_MainActivity_nativeControl \
  b027291c6fff57ceac9bdf43eb944f0e9643e6b3347f0a26ab62d8aed5d6233a
require_text "$F46_D4_R3J_PLAN" 'wxshadow F4.6 D4-R3j Adjacent Inflight-Accounting Plan'
require_text "$F46_D4_R3J_PLAN" 'Status: device row stable and physical-reboot closure verified.'
require_text "$F46_D4_R3J_PLAN" '65679740eb5253c2779bbd0a0c4dce89ebdd5529'
require_text "$F46_D4_R3J_PLAN" '7714473555eaecb40dcb28351b4986894d4d06bcd36a0480b54d645f778b9938'
require_text "$F46_D4_R3J_PLAN" '0a73bd18-ba4a-4bdf-87c6-a99f98d9d039'
require_text "$F46_D4_R3J_PLAN" '6ca72573234f38b4006f58789d8a5af8ca742062460ef8693185bed64b7d9ee2'
require_text "$F46_D4_R3J_PLAN" 'wxshadow-v2-f46-d4-r3i-lock-exposure-stable-20260724'
require_text "$F46_D4_R3J_PLAN" 'result_tag_target=5d7ec82d926c30856ffd650376dd4df00c9bee04'
require_text "$F46_D4_R3J_PLAN" 'closure_commit=ef991c20d57783dc2a8fc654ec52f42625343350'
require_text "$F46_D4_R3J_PLAN" 'atomic_add_wrapper()'
require_text "$F46_D4_R3J_PLAN" 'atomic_sub_wrapper()'
require_text "$F46_D4_R3J_PLAN" 'D4-R3j-global-abort-adjacent-inflight-accounting'
require_text "$F46_D4_R3J_PLAN" 'raw slot arm abort-inflight <token> <slot> <page>'
require_text "$F46_D4_R3J_PLAN" 'raw raw-hold abort-inflight <token>'
require_text "$F46_D4_R3J_PLAN" 'r0lab_raw_before_abort_inflight_passthrough'
require_text "$F46_D4_R3J_PLAN" 'increment `g_raw_inflight` exactly once'
require_text "$F46_D4_R3J_PLAN" 'call compiler-only `barrier()` exactly once'
require_text "$F46_D4_R3J_PLAN" 'immediately decrement `g_raw_inflight` exactly once'
require_text "$F46_D4_R3J_PLAN" 'exactly one lock/unlock pair remains'
require_text "$F46_D4_R3J_PLAN" 'no unlocked interval exposes a nonzero count'
require_text "$F46_D4_R3J_PLAN" 'preserve the local lock-protected counter model'
require_text "$F46_D4_R3J_PLAN" 'do not use atomics, exclusive-load/store loops, hardware memory barriers'
require_text "$F46_D4_R3J_PLAN" 'do not use `READ_ONCE`, `WRITE_ONCE`, volatile counter casts'
require_text "$F46_D4_R3J_PLAN" 'do not emit `dmb`, `dsb`, or `isb`'
require_text "$F46_D4_R3J_PLAN" 'optimized arm64 KPM callback disassembly'
require_text "$F46_D4_R3J_PLAN" 'without it, Clang removes both adjacent operations'
require_text "$F46_D4_R3J_PLAN" 'empty volatile compiler barrier with a memory clobber'
require_text "$F46_D4_R3J_PLAN" 'do not add a callback invocation counter'
require_text "$F46_D4_R3J_PLAN" 'abort_hook_inflight=1'
require_text "$F46_D4_R3J_PLAN" 'abort_hook_lock=0'
require_text "$F46_D4_R3J_PLAN" 'scripts/test_raw_abort_inflight_passthrough_device.sh'
require_text "$F46_D4_R3J_PLAN" 'RAW_ABORT_INFLIGHT_CLEAN_BOOT_CONFIRMED=1'
require_text "$F46_D4_R3J_PLAN" 'After the hold is active, the script may run only 15 bounded'
require_text "$F46_D4_R3J_PLAN" 'D4-R3j-source-uxn-abort-inflight-stable'
require_text "$F46_D4_R3J_PLAN" 'D4-R3j-source-uxn-abort-inflight-unstable'
require_text "$F46_D4_R3J_PLAN" 'Both stable and unstable terminal paths preserve `active_hold=1`.'
require_text "$F46_D4_R3J_PLAN" 'No D4-R3j source symbol, Lab command, or device script may exist'
require_text "$F46_D4_R3J_PLAN" 'scripts/verify_d4_r3j_plan_packet.sh'
require_text "$F46_D4_R3J_PLAN" 'rejects every changed path outside the'
require_text "$F46_D4_R3J_PLAN" '## Source Implementation Checkpoint'
require_text "$F46_D4_R3J_PLAN" 'increment=1'
require_text "$F46_D4_R3J_PLAN" 'decrement=1'
require_text "$F46_D4_R3J_PLAN" 'counter_stores=2'
require_text "$F46_D4_R3J_PLAN" 'hardware_barriers=0'
require_text "$F46_D4_R3J_PLAN" '## Device Result'
require_text "$F46_D4_R3J_PLAN" 'raw-abort-inflight-passthrough-20260724-145803.log'
require_text "$F46_D4_R3J_PLAN" '5ea6e6ed4cc3f47018e7ff710d031390a1b637defbc46d44abb17bed67c75929'
require_text "$F46_D4_R3J_PLAN" '## Physical Reboot Closure'
require_text "$F46_D4_R3J_PLAN" '4ef34205-59b2-4003-8e1e-80728f6158ec'
require_text "$F46_D4_R3J_PLAN" 'folkpatch_module_list=empty'
require_text "$F46_D4_R3J_PLAN" 'bec44c190aa98909921b2236d582cc5bf3e3773f2a3bf1cfd94f995efc010ec1'
require_text "$F46_D4_R3J_PLAN" 'kpm_sha256=018c8e96023692102f2941f4884b5be36913047ab80a59fe96f8e5458c775cfc'
require_text "$F46_D4_R3J_PLAN" 'apk_lib_entry_sha256=f927140d79d9298d8c55a22cecd2021d664c320557ec74854eb39a90af1e37cb'
require_text "$F46_D4_R3J_PLAN" 'apk_dex_entry_sha256=325e8a54bd306ef4da230de9919d0da46dec112f97efa9fbf42646dc7dd7ec79'
require_text "$F46_D4_R3J_PLAN" 'source_tag=wxshadow-v2-f46-d4-r3j-inflight-accounting-source-20260724'
require_text "$F46_D4_R3J_PLAN" 'device_access=not_run'
require_text scripts/verify_d4_r3j_plan_packet.sh \
  'D4_R3J_PLAN_PACKET_STRICT=1'
require_text scripts/verify_v1_contract.sh \
  'D4-R3j plan packet contains a forbidden changed path'
require_text "$F46_D4_R3K_PLAN" 'wxshadow F4.6 D4-R3k Visible Inflight-Lifetime Plan'
require_text "$F46_D4_R3K_PLAN" 'Status: device row stable and physical-reboot closure verified.'
require_text "$F46_D4_R3K_PLAN" 'D4-R3k-global-abort-visible-inflight-lifetime'
require_text "$F46_D4_R3K_PLAN" 'The R3k target ordering is:'
require_text "$F46_D4_R3K_PLAN" 'keeps the acquired'
require_text "$F46_D4_R3K_PLAN" 'The decrement remains'
require_text "$F46_D4_R3K_PLAN" 'two `r0lab_lock()` calls and two matched `r0lab_unlock()` calls'
require_text "$F46_D4_R3K_PLAN" 'no lock is held across `g_mmput`'
require_text "$F46_D4_R3K_PLAN" 'must not add an'
require_text "$F46_D4_R3K_PLAN" '`abort_hook_lifetime` flag'
require_text "$F46_D4_R3K_PLAN" 'must not execute the UXN page after arm'
require_text "$F46_D4_R3K_PLAN" 'D4-R3k-source-uxn-abort-visible-inflight-stable'
require_text "$F46_D4_R3K_PLAN" 'D4-R3k-source-uxn-abort-visible-inflight-unstable'
require_text "$F46_D4_R3K_PLAN" 'D4-R3k-setup-blocked'
require_text "$F46_D4_R3K_PLAN" 'scripts/test_raw_abort_visible_inflight_device.sh'
require_text "$F46_D4_R3K_PLAN" 'scripts/verify_d4_r3k_disassembly.sh'
require_text "$F46_D4_R3K_PLAN" 'lab-app/src/main/cpp/labprobe.c'
require_text "$F46_D4_R3K_PLAN" 'Unrelated main-worktree exit-hook changes must not enter its KPM hash.'
require_text "$F46_D4_R3K_PLAN" '## Source Implementation Checkpoint'
require_text "$F46_D4_R3K_PLAN" 'first lock -> ++g_raw_inflight -> unlock'
require_text "$F46_D4_R3K_PLAN" 'second lock -> --g_raw_inflight -> unlock'
require_text "$F46_D4_R3K_PLAN" 'D4-R3k disassembly=pass'
require_text "$F46_D4_R3K_PLAN" 'increment=1'
require_text "$F46_D4_R3K_PLAN" 'decrement=1'
require_text "$F46_D4_R3K_PLAN" 'counter_stores=2'
require_text "$F46_D4_R3K_PLAN" 'intervening_calls=3'
require_text "$F46_D4_R3K_PLAN" 'hardware_barriers=0'
require_text "$F46_D4_R3K_PLAN" 'kpm_sha256=7f83c71160629f12db6d4e752b076ec42c102aeb9228972001736e06755468cc'
require_text "$F46_D4_R3K_PLAN" 'apk_lib_entry_sha256=f927140d79d9298d8c55a22cecd2021d664c320557ec74854eb39a90af1e37cb'
require_text "$F46_D4_R3K_PLAN" 'apk_dex_entry_sha256=325e8a54bd306ef4da230de9919d0da46dec112f97efa9fbf42646dc7dd7ec79'
require_text "$F46_D4_R3K_PLAN" 'signer_cert_sha256=73f1e2d251423909f33bfc7573580bd096834b57f680d5edb6e68655b1f903dd'
require_text "$F46_D4_R3K_PLAN" 'source_tag=wxshadow-v2-f46-d4-r3k-visible-inflight-source-20260724'
require_text "$F46_D4_R3K_PLAN" 'device_access=not_run'
require_text "$F46_D4_R3K_PLAN" '## Device Runtime And Reboot Closure'
require_text "$F46_D4_R3K_PLAN" 'source_commit=45880fe13aa3b49d7d10993e2e6e49d78f3da5e3'
require_text "$F46_D4_R3K_PLAN" 'pre_hold_boot_id=4ef34205-59b2-4003-8e1e-80728f6158ec'
require_text "$F46_D4_R3K_PLAN" 'runtime_evidence=build/evidence/raw-abort-visible-inflight-20260724-151559.log'
require_text "$F46_D4_R3K_PLAN" 'runtime_sha256=26fc9ef00390834d0f08b26ff529138f0b4272ec31bf6c1d5b739834f275994c'
require_text "$F46_D4_R3K_PLAN" 'classification=D4-R3k-source-uxn-abort-visible-inflight-stable'
require_text "$F46_D4_R3K_PLAN" 'adb_polls=15/15 device'
require_text "$F46_D4_R3K_PLAN" 'post_reboot_boot_id=eb7c7501-5c05-430e-9bee-d1ee2b5d718e'
require_text "$F46_D4_R3K_PLAN" 'folkpatch_module_list=empty'
require_text "$F46_D4_R3K_PLAN" 'warn_delta=0'
require_text "$F46_D4_R3K_PLAN" 'reboot_evidence=build/evidence/raw-abort-visible-inflight-reboot-20260724-151809.log'
require_text "$F46_D4_R3K_PLAN" 'reboot_evidence_sha256=9720c568a591d282dfc37acbae9cadb2c54a2ee0d6adb3fb09b204f38be8b9ca'
require_text scripts/verify_d4_r3k_plan_packet.sh \
  'D4_R3K_PLAN_PACKET_STRICT=1'
require_text scripts/verify_v1_contract.sh \
  'D4-R3k plan packet contains a forbidden changed path'
require_text "$F46_D4_R3L_PLAN" 'wxshadow F4.6 D4-R3l Read-Only IABT Admission And Route Plan'
require_text "$F46_D4_R3L_PLAN" 'Status: device row stable and physical-reboot closure verified.'
require_text "$F46_D4_R3L_PLAN" 'source_tag=wxshadow-v2-f46-d4-r3l-readonly-iabt-route-source-20260724'
require_text "$F46_D4_R3L_PLAN" 'device_access=not_run'
require_text "$F46_D4_R3L_PLAN" 'contract_script_count=55'
require_text "$F46_D4_R3L_PLAN" 'kpm_sha256=a424b2aa99e2808d1bcefde1efa553a11ef6d3bdfd29848c54bceaaceb4d69bf'
require_text "$F46_D4_R3L_PLAN" 'kpm_object_sha256=cfb9cca1be9ba59506d5f1932a7ecc28a685e927ee52988f3382654c579ef33b'
require_text "$F46_D4_R3L_PLAN" 'apk_lib_entry_sha256=673d4a54aed6f2c0c68969d3d9cff5bdd7d311bc902f7af967a76d08da0063e8'
require_text "$F46_D4_R3L_PLAN" 'apk_dex_entry_sha256=325e8a54bd306ef4da230de9919d0da46dec112f97efa9fbf42646dc7dd7ec79'
require_text "$F46_D4_R3L_PLAN" 'signer_cert_sha256=73f1e2d251423909f33bfc7573580bd096834b57f680d5edb6e68655b1f903dd'
require_text "$F46_D4_R3L_PLAN" 'hardware_barriers:0'
require_text "$F46_D4_R3L_PLAN" 'source_commit=dc12f7ebec5ade927a64acc78a7ff696ca11c3f0'
require_text "$F46_D4_R3L_PLAN" 'pre_hold_boot_id=eb7c7501-5c05-430e-9bee-d1ee2b5d718e'
require_text "$F46_D4_R3L_PLAN" 'runtime_evidence=build/evidence/raw-abort-iabt-route-20260724-155647.log'
require_text "$F46_D4_R3L_PLAN" 'runtime_sha256=4fe617db55f8c105e0866838fe27df44e3a7b033cd3103498fdf5e73171c1858'
require_text "$F46_D4_R3L_PLAN" 'classification=D4-R3l-source-uxn-abort-iabt-route-stable'
require_text "$F46_D4_R3L_PLAN" 'adb_polls=15/15 device'
require_text "$F46_D4_R3L_PLAN" 'post_reboot_closure=pass'
require_text "$F46_D4_R3L_PLAN" 'post_reboot_boot_id=922ad5c1-a406-4993-8c76-8b51d6d1a546'
require_text "$F46_D4_R3L_PLAN" 'folkpatch_module_list=empty'
require_text "$F46_D4_R3L_PLAN" 'warn_delta=0'
require_text "$F46_D4_R3L_PLAN" 'reboot_evidence=build/evidence/raw-abort-iabt-route-reboot-20260724-155857.log'
require_text "$F46_D4_R3L_PLAN" 'reboot_evidence_sha256=b0bdd7ca6f4f38c670e20b4380dc52a22d8b2fb1b8a70a188fdaa3ec3a602671'
require_text "$F46_D4_R3L_PLAN" 'result_tag=wxshadow-v2-f46-d4-r3l-readonly-iabt-route-stable-20260724'
require_text "$F46_D4_R3L_PLAN" 'D4-R3l-global-abort-readonly-iabt-route'
require_text "$F46_D4_R3L_PLAN" 'abort_hook_iabt_route'
require_text "$F46_D4_R3L_PLAN" 'raw slot arm abort-iabt-route <token> <slot> <page>'
require_text "$F46_D4_R3L_PLAN" 'raw raw-hold abort-iabt-route <token>'
require_text "$F46_D4_R3L_PLAN" 'r0lab_raw_before_abort_iabt_route'
require_text "$F46_D4_R3L_PLAN" 'r0lab_raw_hook_page_token_acquire_readonly_locked'
require_text "$F46_D4_R3L_PLAN" 'The existing `r0lab_raw_hook_page_token_acquire_locked()` is not permitted'
require_text "$F46_D4_R3L_PLAN" 'call `r0lab_raw_hook_route_reject_locked()`'
require_text "$F46_D4_R3L_PLAN" 'increment `route_hits`, `route_rejects`, or any other counter'
require_text "$F46_D4_R3L_PLAN" 'matched token release with clear_transition=false'
require_text "$F46_D4_R3L_PLAN" 'page_address <= regs->pc < page_address + R0LAB_RAW_PAGE_SIZE'
require_text "$F46_D4_R3L_PLAN" '## Compiler Witness'
require_text "$F46_D4_R3L_PLAN" 'r0lab_raw_iabt_route_compiler_witness(page, generation, pc)'
require_text "$F46_D4_R3L_PLAN" 'empty `asm volatile` with page pointer, generation, and'
require_text "$F46_D4_R3L_PLAN" 'A plain local boolean,'
require_text "$F46_D4_R3L_PLAN" 'It never sets'
require_text "$F46_D4_R3L_PLAN" '`args->skip_origin`'
require_text "$F46_D4_R3L_PLAN" 'No older diagnostic command or mode may change semantics.'
require_text "$F46_D4_R3L_PLAN" 'source checkpoint must start from exact R3k stable commit `d1c3e23`'
require_text "$F46_D4_R3L_PLAN" 'scripts/test_raw_abort_iabt_route_device.sh'
require_text "$F46_D4_R3L_PLAN" 'scripts/verify_d4_r3l_disassembly.sh'
require_text "$F46_D4_R3L_PLAN" 'RAW_ABORT_IABT_ROUTE_CLEAN_BOOT_CONFIRMED=1'
require_text "$F46_D4_R3L_PLAN" 'D4-R3l-source-uxn-abort-iabt-route-stable'
require_text "$F46_D4_R3L_PLAN" 'D4-R3l-source-uxn-abort-iabt-route-unstable'
require_text "$F46_D4_R3L_PLAN" 'D4-R3l-setup-blocked'
require_text "$F46_D4_R3L_PLAN" 'R3l is closed. R3m may enter planning'
require_text scripts/verify_d4_r3l_plan_packet.sh \
  'D4_R3L_PLAN_PACKET_STRICT=1'
require_text scripts/verify_v1_contract.sh \
  'D4-R3l plan packet contains a forbidden changed path'
require_text "$F46_D4_R3M_PLAN" 'wxshadow F4.6 D4-R3m IABT Transition And Restore-ABI Plan'
require_text "$F46_D4_R3M_PLAN" 'Status: retry1 device row stable and physical-reboot closure verified.'
require_text "$F46_D4_R3M_PLAN" 'D4-R3m-global-abort-iabt-transition-restore-abi'
require_text "$F46_D4_R3M_PLAN" 'abort_hook_iabt_transition'
require_text "$F46_D4_R3M_PLAN" 'raw slot arm abort-iabt-transition <token> <slot> <page>'
require_text "$F46_D4_R3M_PLAN" 'raw raw-hold abort-iabt-transition <token>'
require_text "$F46_D4_R3M_PLAN" 'r0lab_raw_before_abort_iabt_transition'
require_text "$F46_D4_R3M_PLAN" 'r0lab_raw_hook_page_token_acquire_iabt_transition_locked'
require_text "$F46_D4_R3M_PLAN" 'r0lab_raw_activate_shadow()'
require_text "$F46_D4_R3M_PLAN" 'r0lab_raw_finish_read_cycle()'
require_text "$F46_D4_R3M_PLAN" 'args->skip_origin=1 and args->ret=0'
require_text "$F46_D4_R3M_PLAN" 'g_r0lab_raw_signal_jump_on_fault=1'
require_text "$F46_D4_R3M_PLAN" 'never call'
require_text "$F46_D4_R3M_PLAN" '`mprotect` from the handler'
require_text "$F46_D4_R3M_PLAN" 'source checkpoint must start from exact R3l stable commit `b655fcd`'
require_text "$F46_D4_R3M_PLAN" 'scripts/test_raw_abort_iabt_transition_device.sh'
require_text "$F46_D4_R3M_PLAN" 'scripts/verify_d4_r3m_disassembly.sh'
require_text "$F46_D4_R3M_PLAN" 'RAW_ABORT_IABT_TRANSITION_CLEAN_BOOT_CONFIRMED=1'
require_text "$F46_D4_R3M_PLAN" 'D4-R3m-source-uxn-iabt-transition-stable'
require_text "$F46_D4_R3M_PLAN" 'D4-R3m-source-uxn-iabt-transition-unstable'
require_text "$F46_D4_R3M_PLAN" 'D4-R3m-setup-blocked'
require_text "$F46_D4_R3M_PLAN" 'R3m is closed. R3n may enter planning'
require_text "$F46_D4_R3M_PLAN" 'kpm_sha256=c29b114970e94e4b6d559728a1e72dfaadc441f9333c906de67554379c121feb'
require_text "$F46_D4_R3M_PLAN" 'kpm_object_sha256=b4812d16328f433036a81a78f693cf6dca0e3831686afdb2891dc33c9482da3a'
require_text "$F46_D4_R3M_PLAN" 'apk_lib_entry_sha256=8f3e8256b023514b379c2ebdd6d6d043b36d53e2d1fca1d2d6d8b56861d947f3'
require_text "$F46_D4_R3M_PLAN" 'apk_dex_entry_sha256=325e8a54bd306ef4da230de9919d0da46dec112f97efa9fbf42646dc7dd7ec79'
require_text "$F46_D4_R3M_PLAN" 'signer_cert_sha256=73f1e2d251423909f33bfc7573580bd096834b57f680d5edb6e68655b1f903dd'
require_text "$F46_D4_R3M_PLAN" 'device_script_sha256=02bd98414c29c07e78cc39695d6dc6b2e268bf6b87fd604c3fbbf2e1413c9295'
require_text "$F46_D4_R3M_PLAN" 'disassembly_script_sha256=1c4deb9a5a6b3e54e1262ac352e62234873191c07696b38315bfa2021d3872ab'
require_text "$F46_D4_R3M_PLAN" 'contract_script_count=58'
require_text "$F46_D4_R3M_PLAN" 'source_commit=614c977f598cded206475b4a778cbdf37d52e42a'
require_text "$F46_D4_R3M_PLAN" 'runtime_evidence=build/evidence/raw-abort-iabt-transition-20260724-162705.log'
require_text "$F46_D4_R3M_PLAN" 'runtime_sha256=ee3305a49f251067aad653c8369264e4f0c05b0d270f872770b7cf5112d5fbda'
require_text "$F46_D4_R3M_PLAN" 'classification=D4-R3m-source-uxn-iabt-transition-unstable'
require_text "$F46_D4_R3M_PLAN" 'ready_rc=320'
require_text "$F46_D4_R3M_PLAN" 'abort_hook_iabt_transition=-1'
require_text "$F46_D4_R3M_PLAN" 'physical_reboot=confirmed'
require_text "$F46_D4_R3M_PLAN" 'post_reboot_boot_id=f5d754c7-b25c-4375-894f-b5a2e374cbf8'
require_text "$F46_D4_R3M_PLAN" 'folkpatch_module_list=empty'
require_text "$F46_D4_R3M_PLAN" 'reboot_evidence=build/evidence/raw-abort-iabt-transition-reboot-20260724-163522.log'
require_text "$F46_D4_R3M_PLAN" 'reboot_evidence_sha256=9cfc5e0af0626e0caddab16d3487f890712f94ec170ec70cea5aa3dc73d66012'
require_text "$F46_D4_R3M_PLAN" 'retry_source_tag=wxshadow-v2-f46-d4-r3m-iabt-transition-retry1-source-20260724'
require_text "$F46_D4_R3M_PLAN" 'retry_kpm_sha256=c29b114970e94e4b6d559728a1e72dfaadc441f9333c906de67554379c121feb'
require_text "$F46_D4_R3M_PLAN" 'retry_apk_lib_entry_sha256=48a6317362355be5e8070ecfa31179cc163f777bf46a865404986255da127a9d'
require_text "$F46_D4_R3M_PLAN" 'retry_device_script_sha256=34b5edfe7081f87afefe99c06078f34f515aeb946d01d424b7ab664fd5486c02'
require_text "$F46_D4_R3M_PLAN" 'retry_pre_hold_boot_id=f5d754c7-b25c-4375-894f-b5a2e374cbf8'
require_text "$F46_D4_R3M_PLAN" 'device_access=ready_after_physical_reboot_closure'
require_text "$F46_D4_R3M_PLAN" 'retry_source_commit=ce3fc3da1c0d6f551cf14a2e15f9d50e03b72b95'
require_text "$F46_D4_R3M_PLAN" 'retry_runtime_evidence=build/evidence/raw-abort-iabt-transition-20260724-164024.log'
require_text "$F46_D4_R3M_PLAN" 'retry_runtime_sha256=7e3321901530be4927e856c67a8d3fcb0c0a26d50c6a0b303d144c0200b3ce18'
require_text "$F46_D4_R3M_PLAN" 'classification=D4-R3m-source-uxn-iabt-transition-stable'
require_text "$F46_D4_R3M_PLAN" 'ready_rc=344'
require_text "$F46_D4_R3M_PLAN" 'abort_hook_iabt_transition=1'
require_text "$F46_D4_R3M_PLAN" 'idle_polls=15/15'
require_text "$F46_D4_R3M_PLAN" 'post_reboot_boot_id=30ea3346-dc2b-486a-a811-602005e79e43'
require_text "$F46_D4_R3M_PLAN" 'reboot_evidence=build/evidence/raw-abort-iabt-transition-retry1-reboot-20260724-164256.log'
require_text "$F46_D4_R3M_PLAN" 'reboot_evidence_sha256=a04e5838c3adc27c9de05a9dc78d8f3801d72b445b7dfbd0ace2e6f225a0ef0a'
require_text "$F46_D4_R3M_PLAN" 'stable_tag=wxshadow-v2-f46-d4-r3m-iabt-transition-stable-20260724'
require_text scripts/verify_d4_r3m_plan_packet.sh \
  'D4_R3M_PLAN_PACKET_STRICT=1'
require_text scripts/verify_v1_contract.sh \
  'D4-R3m plan packet contains a forbidden changed path'
require_text "$F46_D4_R3N_PLAN" 'wxshadow F4.6 D4-R3n Full Abort And No-Reboot Lifecycle Plan'
require_text "$F46_D4_R3N_PLAN" 'Status: plan locked; source implementation and device acceptance pending.'
require_text "$F46_D4_R3N_PLAN" 'D4-R3n-full-abort-two-slot-no-reboot-lifecycle'
require_text "$F46_D4_R3N_PLAN" 'r0lab_raw_hook_page_token_acquire_full_abort_locked'
require_text "$F46_D4_R3N_PLAN" 'abort_hook_full=1'
require_text "$F46_D4_R3N_PLAN" 'args->skip_origin=1'
require_text "$F46_D4_R3N_PLAN" 'args->ret=0'
require_text "$F46_D4_R3N_PLAN" 'This branch never sets `skip_origin`'
require_text "$F46_D4_R3N_PLAN" 'r0lab_raw_exit_mmap_before()'
require_text "$F46_D4_R3N_PLAN" 'restore happens before page'
require_text "$F46_D4_R3N_PLAN" 'The first slot may not close the'
require_text "$F46_D4_R3N_PLAN" 'raw full abort lifecycle run <token>'
require_text "$F46_D4_R3N_PLAN" 'scripts/test_raw_full_abort_lifecycle_device.sh'
require_text "$F46_D4_R3N_PLAN" 'scripts/verify_d4_r3n_disassembly.sh'
require_text "$F46_D4_R3N_PLAN" 'Phase A, explicit lifecycle'
require_text "$F46_D4_R3N_PLAN" 'Phase B begins immediately on the same boot'
require_text "$F46_D4_R3N_PLAN" 'R3n stable acceptance does not include a reboot.'
require_text "$F46_D4_R3N_PLAN" 'D4-R3n-full-abort-lifecycle-stable'
require_text "$F46_D4_R3N_PLAN" 'D4-R3n-abort-routing-unstable'
require_text "$F46_D4_R3N_PLAN" 'D4-R3n-explicit-cleanup-unstable'
require_text "$F46_D4_R3N_PLAN" 'D4-R3n-no-reboot-reload-unstable'
require_text "$F46_D4_R3N_PLAN" 'D4-R3n-owner-exit-cleanup-unstable'
require_text "$F46_D4_R3N_PLAN" 'D4-R3n-setup-blocked'
require_text "$F46_D4_R3N_PLAN" 'R3n is the final F4.6 gate.'
require_text "$F46_D4_R3N_MM_FIX_PLAN" \
  'F4.6 D4-R3n Owner-Exit `mmput` Fix Plan'
require_text "$F46_D4_R3N_MM_FIX_PLAN" \
  'zero `RAW_EXIT_MMAP_HIT` events (`op=34`)'
require_text "$F46_D4_R3N_MM_FIX_PLAN" \
  'The final `mmput()` must enter the still-installed'
require_text "$F46_D4_R3N_MM_FIX_PLAN" \
  'No PTE primitive runs after the final `mmput()` returns.'
require_text "$F46_D4_R3N_MM_FIX_PLAN" \
  'two successful `op=34` events'
require_text "$F46_D4_R3N_MM_FIX_PLAN" \
  'boot_reason=reboot,userrequested'
require_text "$F46_D4_R3N_MM_FIX_PLAN" \
  'candidate_kpm_loaded=0'
require_text scripts/verify_d4_r3n_plan_packet.sh \
  'D4_R3N_PLAN_PACKET_STRICT=1'
require_text scripts/verify_v1_contract.sh \
  'D4-R3n plan packet contains a forbidden changed path'
require_function_sha256 kpm/r0lab.c \
  r0lab_raw_hook_page_token_acquire_full_abort_locked \
  4e408d41443fae25faa27af8ba70a3e9a79db9a5c6e6c07dd94b0f7129e4fdb8
require_function_sha256 kpm/r0lab.c r0lab_raw_before_abort \
  dfa2fef78a90a38c427a82041164acbfa3fab5b00b594976edd09d965f0fea61
require_function_sha256 kpm/r0lab.c r0lab_raw_exit_mmap_before \
  c93284b7ea39e8923cd5161ea1871dcdc3106d51486596496746d385088fcca3
require_function_sha256 kpm/r0lab.c r0lab_raw_unhook_page \
  1f1fbf1e47db51d7a7665660a2857d0fe14cab14170b82d17abc95226ce03145
require_function_sha256 kpm/r0lab.c r0lab_raw_reset_final_page \
  a5528dc7e0ee2e931a56faa88e750916a50d7847856d1ac4a4a09210991feaeb
require_function_sha256 kpm/r0lab.c r0lab_raw_monitor_worker \
  a3b60e02adb98313cf591eca4b3a86fae7c3104d37ebb720074ceea2cd4917e8
require_function_sha256 kpm/r0lab.c r0lab_raw_reset_exited_page \
  29412d371baba0dd2de734292395a715d8152f99be08f7213c60d52f8c690705
require_function_sha256 kpm/r0lab.c r0lab_raw_wait_for_owner_task_exit \
  723a423f9019563f2d6d0cda6a28d47f88383942602a99e7c70a48d92519a8db
require_function_sha256 kpm/r0lab.c r0lab_raw_cleanup_exited_mm \
  5781bfc212b1f0d2396c605b56df57aa501af3de1748cea52e4ee2618dd8fbee
require_function_sha256 kpm/r0lab.c r0lab_close_exited_session \
  f8c8d6d4669cfe52fc937abf3cbfd3599c905aca10cf81da2d43b7ccbc02bf9d
require_function_sha256 kpm/r0lab.c r0lab_status \
  a3fae779f6b10185d3e63b769f4415a8efcc72334387a77f5093d97bca64fa1e
require_function_sha256 kpm/r0lab.c r0lab_raw_slot_ready \
  78992e7036b97f888201719227717da581112a44d325033698a413ec0facbb7b
require_function_sha256 kpm/r0lab.c r0lab_raw_arm_worker \
  874f8f12422ff744204799582b402d2541eab4e5447662f0a1eb4c377ad6e9a2
require_function_sha256 lab-app/src/main/cpp/labprobe.c \
  r0lab_raw_full_abort_lifecycle_run \
  3c7edade83c1550b3a82c89cae9defe38489243e3db36263c3c2202b8aa68657
require_function_sha256 lab-app/src/main/cpp/labprobe.c \
  r0lab_raw_exit_hook_routing_hold \
  5b9238169c269c21876d210041596d84b27988948a9285946ca1448fd48f3a4e
require_function_text kpm/r0lab.c r0lab_raw_before_abort \
  'if (!g_initialized || !args || !g_get_task_mm || !g_mmput)'
require_function_count kpm/r0lab.c r0lab_raw_before_abort \
  'g_get_task_mm(current)' 1
require_function_count kpm/r0lab.c r0lab_raw_before_abort \
  'g_mmput(current_mm);' 1
require_function_count kpm/r0lab.c r0lab_raw_before_abort \
  '++g_raw_inflight;' 1
require_function_count kpm/r0lab.c r0lab_raw_before_abort \
  '--g_raw_inflight;' 1
require_function_count kpm/r0lab.c r0lab_raw_before_abort \
  'args->skip_origin = 1;' 2
require_function_count kpm/r0lab.c r0lab_raw_before_abort \
  'args->ret = 0;' 2
for primitive in \
  r0lab_raw_activate_shadow \
  r0lab_raw_finish_read_cycle \
  r0lab_raw_begin_fault_read_cycle \
  r0lab_raw_restore_original
do
  require_function_count kpm/r0lab.c r0lab_raw_before_abort \
    "$primitive(" 1
done
for forbidden in \
  'WRITE_ONCE(*ptep' \
  'dmb ' \
  'dsb ' \
  'isb ' \
  'tlbi '
do
  reject_function_text kpm/r0lab.c r0lab_raw_before_abort "$forbidden"
done
reject_function_text kpm/r0lab.c \
  r0lab_raw_hook_page_token_acquire_full_abort_locked 'r0lab_record'
reject_function_text kpm/r0lab.c \
  r0lab_raw_hook_page_token_acquire_full_abort_locked \
  'page->transitioning ='
reject_function_text kpm/r0lab.c \
  r0lab_raw_hook_page_token_acquire_full_abort_locked \
  'abort_probe_read_events'
reject_function_text kpm/r0lab.c \
  r0lab_raw_hook_page_token_acquire_full_abort_locked \
  'abort_probe_write_events'
require_function_text kpm/r0lab.c r0lab_raw_exit_mmap_before \
  'tokens[R0LAB_RAW_PAGE_SLOT_CAPACITY]'
require_function_count kpm/r0lab.c r0lab_raw_exit_mmap_before \
  '++g_raw_inflight;' 1
require_function_count kpm/r0lab.c r0lab_raw_exit_mmap_before \
  '--g_raw_inflight;' 1
require_function_count kpm/r0lab.c r0lab_raw_exit_mmap_before \
  'r0lab_raw_restore_original(&page->raw)' 1
for forbidden in \
  'r0lab_raw_unhook' \
  'r0lab_raw_wait' \
  'r0lab_raw_reset' \
  'r0lab_close' \
  'g_mmput' \
  'g_vfree' \
  'g_vmalloc'
do
  reject_function_text kpm/r0lab.c r0lab_raw_exit_mmap_before "$forbidden"
done
require_function_text_before kpm/r0lab.c r0lab_raw_reset_final_page \
  'r0lab_raw_drain_patch_buffers_page' \
  'r0lab_raw_page_slot_reset_locked'
require_function_text_before kpm/r0lab.c r0lab_raw_reset_final_page \
  'r0lab_raw_free_shadow_page(shadow_kaddr);' \
  'r0lab_raw_release_mm_ref(mm, mm_count_owned, mm_users_owned);'
reject_function_text kpm/r0lab.c r0lab_raw_reset_final_page \
  'g_mmput(mm);'
require_function_text kpm/r0lab.c r0lab_raw_wait_for_owner_task_exit \
  'if (!r0lab_target_task_live(owner_tgid))'
require_function_text_before kpm/r0lab.c \
  r0lab_raw_cleanup_exited_mm \
  'r0lab_raw_drain_patch_buffers_page' 'r0lab_raw_exit_hook_release'
require_function_text_before kpm/r0lab.c \
  r0lab_raw_cleanup_exited_mm \
  'r0lab_raw_exit_hook_release' 'r0lab_raw_reset_exited_page'
reject_function_text kpm/r0lab.c r0lab_raw_cleanup_exited_mm \
  'g_mmput(mm);'
require_function_text kpm/r0lab.c r0lab_raw_cleanup_exited_mm \
  '!slot->exit_hook_events_before'
require_function_text kpm/r0lab.c r0lab_raw_cleanup_exited_mm \
  'page->exit_hook_events != slot->exit_hook_events_before'
for forbidden in \
  'g_mmput' \
  'r0lab_raw_restore_original' \
  'r0lab_raw_activate_shadow' \
  'r0lab_raw_begin_fault_read_cycle' \
  'r0lab_raw_finish_read_cycle'
do
  reject_function_text kpm/r0lab.c r0lab_raw_reset_exited_page "$forbidden"
done
require_function_text_before kpm/r0lab.c r0lab_raw_monitor_worker \
  'r0lab_raw_wait_for_owner_task_exit' 'r0lab_raw_cleanup_exited_mm'
for forbidden in \
  'g_mmput' \
  'r0lab_raw_restore_original' \
  'r0lab_raw_unhook_page' \
  'r0lab_raw_reset_final_page'
do
  reject_function_text kpm/r0lab.c r0lab_raw_monitor_worker "$forbidden"
done
require_text kpm/r0lab.c '!r0lab_session_has_slots_locked()'
require_function_text kpm/r0lab.c r0lab_raw_slot_ready \
  'abort_hook_full=%u'
require_function_text kpm/r0lab.c r0lab_status 'raw_inflight=%u'
require_function_text lab-app/src/main/cpp/labprobe.c \
  r0lab_raw_full_abort_lifecycle_run \
  'raw mode=full-abort-lifecycle failures=%d'
for field in \
  'slot0_read_cycle=%s' \
  'slot1_write_signal=%d' \
  'slot1_write_release_result=%d' \
  'handler_repairs=0' \
  'restored=%d/%d' \
  'session_closed=%d'
do
  require_function_text lab-app/src/main/cpp/labprobe.c \
    r0lab_raw_full_abort_lifecycle_run "$field"
done
require_function_text lab-app/src/main/cpp/labprobe.c \
  r0lab_raw_exit_hook_routing_hold 'abort_hook_full=%d/%d'
require_function_text lab-app/src/main/cpp/labprobe.c \
  Java_dev_r0hook_lab_MainActivity_nativeControl \
  'raw full abort lifecycle run '
require_text scripts/test_raw_full_abort_lifecycle_device.sh \
  'EXPECTED_SERIAL=32250DLH2000Z3'
require_text scripts/test_raw_full_abort_lifecycle_device.sh \
  'EXPECTED_BOOT_ID=85ea557e-0182-4cc8-8509-0586bdaaab04'
require_text scripts/test_raw_full_abort_lifecycle_device.sh \
  'SOURCE_TAG=wxshadow-v2-f46-d4-r3n-owner-exit-mmput-fix-retry1-source-20260724'
require_text scripts/test_raw_full_abort_lifecycle_device.sh \
  'PHASE=phase-a'
require_text scripts/test_raw_full_abort_lifecycle_device.sh \
  'PHASE=phase-b'
require_text scripts/test_raw_full_abort_lifecycle_device.sh \
  'reload_without_reboot=1'
require_text scripts/test_raw_full_abort_lifecycle_device.sh \
  'EXPECTED_KPM_SHA=318c9a86e57d1301c34aa357fa9d8da7c6de11df7fdfdfa1dedd442dc582cc68'
require_text scripts/test_raw_full_abort_lifecycle_device.sh \
  'EXPECTED_LABPROBE_SHA=ce787040a378c18e6619f2e43d07d703da9929303176853bce42a6824b9109b4'
require_text scripts/test_raw_full_abort_lifecycle_device.sh \
  'EXPECTED_CLASSES_DEX_SHA=325e8a54bd306ef4da230de9919d0da46dec112f97efa9fbf42646dc7dd7ec79'
reject_text scripts/test_raw_full_abort_lifecycle_device.sh \
  'R3N_KPM_SHA256'
reject_text scripts/test_raw_full_abort_lifecycle_device.sh \
  'R3N_LABPROBE_SHA256'
reject_text scripts/test_raw_full_abort_lifecycle_device.sh \
  'R3N_CLASSES_DEX_SHA256'
require_text scripts/test_raw_full_abort_lifecycle_device.sh \
  'cleanup_rc=$?'
reject_text scripts/test_raw_full_abort_lifecycle_device.sh \
  'status=$?'
reject_text scripts/test_raw_full_abort_lifecycle_device.sh 'adb reboot'
reject_text scripts/test_raw_full_abort_lifecycle_device.sh \
  'adb_device reboot'
require_text scripts/verify_d4_r3n_disassembly.sh \
  'D4-R3n disassembly passed: primitives=4 transition_sets=3 transition_clears=3 skip_origin_stores=2 ret_zero_stores=2 exit_restore_relocations=3 token_helper_calls=0 hardware_cache_tlb=0'
require_text "$F46_D4_R3O_PLAN" \
  'F4.6 D4-R3o Reference Lifetime Parity Fix Plan'
require_text "$F46_D4_R3O_PLAN" \
  'Status: complete.'
require_text "$F46_D4_R3O_PLAN" 'mmgrab(mm)'
require_text "$F46_D4_R3O_PLAN" 'mmdrop(mm)'
require_text "$F46_D4_R3O_PLAN" \
  'before the first source-UXN PTE transition'
require_text "$F46_D4_R3O_PLAN" '__get_free_pages(GFP_KERNEL, 0)'
require_text "$F46_D4_R3O_PLAN" \
  'synchronizes the full page after the initial copy/seed'
require_text "$F46_D4_R3O_PLAN" \
  'the final full-abort callback resident'
require_text "$F46_D4_R3O_PLAN" \
  'Owner exit relies on natural kernel `mmput()`/`exit_mmap`'
require_text "$F46_D4_R3O_PLAN" \
  'Only after stages 1-5 pass, run one unload/reload-without-reboot cycle.'
require_text "$DEVELOPMENT_SEQUENCE" \
  '| F4.6-D4-R3o reference lifetime parity | Complete; source-tagged Stage 1-6 ladder passed |'
require_text "$DEVELOPMENT_SEQUENCE" \
  '| 28 | D4-R3o | Reference lifetime parity and compact abort admission |'
require_text "$FINAL_ROADMAP" \
  'Current superseding status: the F4.6 queue is complete.'
require_text "$FINAL_ROADMAP" \
  'Allowing every DABT into the full callback first killed `system_server`'
require_text "$REFERENCE_COVERAGE" \
  'one `mm_count` reference each'
require_text "$REFERENCE_COVERAGE" \
  'matched `__get_free_pages(GFP_KERNEL, 0)`/`free_pages(..., 0)` pair'
require_text kpm/r0lab.c 'bool mm_count_owned;'
require_text kpm/r0lab.c \
  'raw_abort_resident=%u raw_exit_resident=%u'
require_text kpm/r0lab.c \
  'static bool r0lab_raw_dabt_route_armed_unlocked(void)'
require_text kpm/r0lab.c \
  'callback != r0lab_raw_before_abort_compact'
require_text kpm/r0lab.c 'mm_ref=%s exit_hook_installed=%u'
require_text kpm/r0lab_raw.h \
  'void r0lab_raw_mmdrop(void *mm);'
require_text scripts/verify_d4_r3o_reference_lifetime.sh \
  'require_function_sequence3 "$KPM_SOURCE" r0lab_raw_slot_arm'
require_text scripts/verify_d4_r3o_reference_lifetime.sh \
  'require_function_sequence3 "$KPM_SOURCE" r0lab_raw_arm_worker'
require_text scripts/verify_d4_r3o_reference_lifetime.sh \
  'require_function_sequence3 "$RAW_SOURCE" r0lab_raw_replace_locked'
require_text scripts/verify_d4_r3o_reference_lifetime.sh \
  'require_function_sequence3 "$LAB_SOURCE" r0lab_raw_hold_clear'
require_text scripts/test_raw_r3o_lifetime_device.sh \
  'R3O_STAGE=1..6 is required'
require_text scripts/test_raw_r3o_lifetime_device.sh \
  'raw raw-hold clear $TOKEN_4'
require_text scripts/test_raw_r3o_lifetime_device.sh \
  'STAGE4_HOLD_SECONDS=${R3O_STAGE4_HOLD_SECONDS:-60}'
require_text scripts/test_raw_r3o_lifetime_device.sh \
  'poll_runtime_continuity "$STAGE4_HOLD_SECONDS"'
require_text scripts/test_raw_r3o_lifetime_device.sh \
  'wait_for_failure_device'
require_text kpm/r0lab.c 'r0lab-r3o: exit_protection_ready'
require_text kpm/r0lab.c 'r0lab-r3o: exit_restore_begin'
require_text lab-app/src/main/cpp/labprobe.c 'raw raw-hold clear '
require_file scripts/test_raw_abort_iabt_route_device.sh
require_file scripts/verify_d4_r3l_disassembly.sh
require_text kpm/r0lab.c 'bool abort_hook_iabt_route;'
require_text kpm/r0lab.c 'raw slot arm abort-iabt-route '
require_function_sha256 kpm/r0lab.c \
  r0lab_raw_hook_page_token_acquire_readonly_locked \
  f94e6f0b68312093a3b853261ebd6c648dcdaa3fd67c5754bbc470fd9cc73588
require_function_sha256 kpm/r0lab.c \
  r0lab_raw_iabt_route_compiler_witness \
  e7b4a45526d6cfeec9c5167ecfa7f19f9db93fc19df6e33331dc5b8482faa115
require_function_sha256 kpm/r0lab.c r0lab_raw_before_abort_iabt_route \
  921744701a349a15a5e0f20d2d86db825ecfa39c17edd2f638a4221e3df8ce49
require_function_sha256 kpm/r0lab.c r0lab_raw_arm \
  d25e5b45a82d00a18b2e8cbf26e1ae62b4ab5038725564b5543e4a5e9b5f9dc3
require_function_sha256 lab-app/src/main/cpp/labprobe.c \
  r0lab_raw_hold_lifetime \
  5f1ca60e2aa03f62e3971e9bd57aff6ac860a8c835611c3b1e9d4d9d0ddbd8a5
require_function_sha256 lab-app/src/main/cpp/labprobe.c \
  r0lab_raw_hold_live_pte \
  f64d6a9d28cf7118c9240c21e155c4a12761708c8b6fb0f1c0936ddf3131519d
require_function_sha256 lab-app/src/main/cpp/labprobe.c \
  r0lab_raw_hold_no_abort \
  7cb3c2f9d23a19b5b9946b58d1f3d43ecda19aae71a4918cb5317d3a522903db
require_function_sha256 lab-app/src/main/cpp/labprobe.c \
  r0lab_raw_hold_abort_passthrough \
  a757d0138155dc071da87590760094a0b89bab4d25bf177ae77ba505122b9b36
require_function_sha256 lab-app/src/main/cpp/labprobe.c \
  r0lab_raw_hold_abort_mmget \
  10f55337b313465b9ae4adb6e8b4806bc234d9adfe00345002e4d16382ebb531
require_function_sha256 lab-app/src/main/cpp/labprobe.c \
  r0lab_raw_hold_abort_iabt_route \
  6bae8add63260797b3c9e8ab2e5cd5932304320111edabb8ec7760306f0bf7cf
require_function_text kpm/r0lab.c \
  r0lab_raw_hook_page_token_acquire_readonly_locked \
  '(route_flags & R0LAB_RAW_HOOK_ROUTE_MUTATING)'
require_function_text kpm/r0lab.c \
  r0lab_raw_hook_page_token_acquire_readonly_locked \
  'page = r0lab_raw_page_find_by_mm_addr_locked(mm, address);'
require_function_text kpm/r0lab.c \
  r0lab_raw_hook_page_token_acquire_readonly_locked \
  'if (!page->abort_hook_iabt_route)'
require_function_text kpm/r0lab.c \
  r0lab_raw_hook_page_token_acquire_readonly_locked \
  'if (page->transitioning)'
require_function_text kpm/r0lab.c \
  r0lab_raw_hook_page_token_acquire_readonly_locked \
  'token->generation = page->generation;'
reject_function_text kpm/r0lab.c \
  r0lab_raw_hook_page_token_acquire_readonly_locked \
  'r0lab_raw_hook_route_reject_locked'
reject_function_text kpm/r0lab.c \
  r0lab_raw_hook_page_token_acquire_readonly_locked 'route_hits'
reject_function_text kpm/r0lab.c \
  r0lab_raw_hook_page_token_acquire_readonly_locked 'route_rejects'
reject_function_text kpm/r0lab.c \
  r0lab_raw_hook_page_token_acquire_readonly_locked \
  'page->transitioning ='
reject_function_text kpm/r0lab.c \
  r0lab_raw_hook_page_token_acquire_readonly_locked 'r0lab_record'
require_function_text kpm/r0lab.c \
  r0lab_raw_iabt_route_compiler_witness \
  'asm volatile("" : : "r"(page), "r"(generation), "r"(pc) : "memory");'
require_function_text kpm/r0lab.c r0lab_raw_before_abort_iabt_route \
  'far = (unsigned long)args->arg0;'
require_function_text kpm/r0lab.c r0lab_raw_before_abort_iabt_route \
  'esr = (unsigned int)args->arg1;'
require_function_text kpm/r0lab.c r0lab_raw_before_abort_iabt_route \
  'regs = (struct pt_regs *)(unsigned long)args->arg2;'
require_function_text kpm/r0lab.c r0lab_raw_before_abort_iabt_route \
  'g_session.owner_tgid == r0lab_current_tgid()'
require_function_text kpm/r0lab.c r0lab_raw_before_abort_iabt_route \
  'r0lab_raw_hook_page_token_acquire_readonly_locked('
require_function_text kpm/r0lab.c r0lab_raw_before_abort_iabt_route \
  'r0lab_raw_iabt_route_compiler_witness('
require_function_text kpm/r0lab.c r0lab_raw_before_abort_iabt_route \
  '&route_token, false);'
require_function_count kpm/r0lab.c r0lab_raw_before_abort_iabt_route \
  'g_get_task_mm(' 1
require_function_count kpm/r0lab.c r0lab_raw_before_abort_iabt_route \
  'g_mmput(' 1
require_function_count kpm/r0lab.c r0lab_raw_before_abort_iabt_route \
  'r0lab_lock();' 2
require_function_count kpm/r0lab.c r0lab_raw_before_abort_iabt_route \
  'r0lab_unlock(flags);' 2
require_function_count kpm/r0lab.c r0lab_raw_before_abort_iabt_route \
  '++g_raw_inflight;' 1
require_function_count kpm/r0lab.c r0lab_raw_before_abort_iabt_route \
  '--g_raw_inflight;' 1
reject_function_text kpm/r0lab.c r0lab_raw_before_abort_iabt_route \
  'r0lab_raw_hook_page_token_acquire_locked('
reject_function_text kpm/r0lab.c r0lab_raw_before_abort_iabt_route \
  'r0lab_raw_hook_route_reject_locked'
reject_function_text kpm/r0lab.c r0lab_raw_before_abort_iabt_route \
  'transitioning ='
reject_function_text kpm/r0lab.c r0lab_raw_before_abort_iabt_route \
  'r0lab_raw_begin_'
reject_function_text kpm/r0lab.c r0lab_raw_before_abort_iabt_route \
  'r0lab_raw_finish_'
reject_function_text kpm/r0lab.c r0lab_raw_before_abort_iabt_route \
  'r0lab_record'
reject_function_text kpm/r0lab.c r0lab_raw_before_abort_iabt_route \
  'skip_origin'
reject_function_text kpm/r0lab.c r0lab_raw_before_abort_iabt_route \
  'R0LAB_M3_ESR_EC_DABT_LOW'
require_function_text kpm/r0lab.c r0lab_raw_abort_hook_callback \
  'if (iabt_route)'
require_function_text kpm/r0lab.c r0lab_raw_abort_hook_callback \
  'return r0lab_raw_before_abort_iabt_route;'
require_function_text kpm/r0lab.c \
  r0lab_raw_abort_hook_installed_callback_locked \
  'page->abort_hook_iabt_route,'
require_function_text kpm/r0lab.c r0lab_raw_abort_hook_acquire \
  'page->abort_hook_iabt_route, page->abort_hook_iabt_transition);'
require_function_text kpm/r0lab.c r0lab_raw_abort_hook_release \
  'iabt_route = page->abort_hook_iabt_route;'
require_function_text kpm/r0lab.c r0lab_raw_slot_arm \
  '(iabt_route_abort_hook ? 1U : 0U)'
require_function_text kpm/r0lab.c r0lab_raw_slot_arm \
  'active->abort_hook_iabt_route'
require_function_text kpm/r0lab.c r0lab_raw_slot_arm \
  'slot->abort_hook_iabt_route = iabt_route_abort_hook;'
require_function_text kpm/r0lab.c r0lab_raw_slot_ready \
  'abort_hook_iabt_route = page->abort_hook_iabt_route;'
require_function_text kpm/r0lab.c r0lab_raw_slot_ready \
  'abort_hook_inflight=%u abort_hook_iabt_route=%u'
require_line_before kpm/r0lab.c \
  '    if (!strncmp(args, "raw slot arm abort-iabt-route ", 30)) {' \
  '    if (!strncmp(args, "raw slot arm ", 13)) {'
require_text lab-app/src/main/cpp/labprobe.c \
  'raw raw-hold abort-iabt-route '
require_text lab-app/src/main/cpp/labprobe.c \
  'raw mode=raw-hold-abort-iabt-route failures=%d'
require_function_text lab-app/src/main/cpp/labprobe.c \
  r0lab_raw_hold_lifetime_common \
  'error=abort-iabt-route hold requires source single'
require_function_text lab-app/src/main/cpp/labprobe.c \
  r0lab_raw_hold_lifetime_common \
  '(iabt_route_abort_hook ? 1U : 0U)'
require_function_text lab-app/src/main/cpp/labprobe.c \
  r0lab_raw_hold_lifetime_common \
  'abort_hook_iabt_route[index] != 1'
require_function_text lab-app/src/main/cpp/labprobe.c \
  r0lab_raw_hold_lifetime_common \
  'iabt_route_abort_hook ? 19 :'
require_line_before lab-app/src/main/cpp/labprobe.c \
  '    if (!strncmp(args, "raw raw-hold abort-iabt-route ", 30)) {' \
  '    if (!strncmp(args, "raw exit hook hold ", 19)) {'
require_text scripts/test_raw_abort_iabt_route_device.sh \
  'RAW_ABORT_IABT_ROUTE_CLEAN_BOOT_CONFIRMED'
require_text scripts/test_raw_abort_iabt_route_device.sh \
  'EXPECTED_SERIAL=32250DLH2000Z3'
require_text scripts/test_raw_abort_iabt_route_device.sh \
  'IDLE_SECONDS=15'
require_text scripts/test_raw_abort_iabt_route_device.sh \
  'EXPECTED_KPM_SHA=a424b2aa99e2808d1bcefde1efa553a11ef6d3bdfd29848c54bceaaceb4d69bf'
require_text scripts/test_raw_abort_iabt_route_device.sh \
  'EXPECTED_LABPROBE_SHA=673d4a54aed6f2c0c68969d3d9cff5bdd7d311bc902f7af967a76d08da0063e8'
require_text scripts/test_raw_abort_iabt_route_device.sh \
  'wxshadow-v2-f46-d4-r3l-readonly-iabt-route-source-20260724'
require_text scripts/test_raw_abort_iabt_route_device.sh \
  'raw raw-hold abort-iabt-route $TOKEN'
require_text scripts/test_raw_abort_iabt_route_device.sh \
  'D4-R3l-source-uxn-abort-iabt-route-stable'
require_text scripts/test_raw_abort_iabt_route_device.sh \
  'D4-R3l-source-uxn-abort-iabt-route-unstable'
require_text scripts/test_raw_abort_iabt_route_device.sh \
  'D4-R3l-setup-blocked'
require_function_text scripts/test_raw_abort_iabt_route_device.sh \
  poll_adb_transport 'state=$(adb_device get-state 2>&1)'
reject_function_text scripts/test_raw_abort_iabt_route_device.sh \
  poll_adb_transport 'run_app_command'
reject_function_text scripts/test_raw_abort_iabt_route_device.sh \
  poll_adb_transport 'supercmd'
reject_function_text scripts/test_raw_abort_iabt_route_device.sh \
  poll_adb_transport '/proc/'
reject_function_text scripts/test_raw_abort_iabt_route_device.sh \
  poll_adb_transport 'adb_device shell getprop'
require_text scripts/verify_d4_r3l_disassembly.sh \
  'D4-R3l disassembly passed: frame_loads=3 increment=1 decrement=1 nonstack_stores=2 indirect_calls=7 hardware_barriers=0'
require_text scripts/verify_d4_r3l_disassembly.sh \
  "grep -Eq '[[:space:]](dmb|dsb|isb|dc|ic)([[:space:]]|$)'"
require_text kpm/r0lab.c 'bool abort_hook_iabt_transition;'
require_text kpm/r0lab.c 'raw slot arm abort-iabt-transition '
require_function_sha256 kpm/r0lab.c \
  r0lab_raw_hook_page_token_acquire_iabt_transition_locked \
  eb0f1a957c0a3991029f1c2de0cef5440939769a0e980ef8d8be7e258f94874d
require_function_sha256 kpm/r0lab.c r0lab_raw_before_abort_iabt_transition \
  164d791a0a6e0de50e9bf681b9df25700efffe085d17a55ee502ea3da8302083
require_function_sha256 kpm/r0lab.c r0lab_raw_before_abort_full_iabt \
  eddd98592bb6156259c3968b57dca98c4f230a6702f59f1a7877b896133f6385
require_function_sha256 kpm/r0lab.c \
  r0lab_raw_dabt_route_armed_unlocked \
  658d0f2f4eac94449b1b99006772d6212e1da4f8557ceab7d867df40f2e09e92
require_function_sha256 kpm/r0lab.c r0lab_raw_before_abort_compact \
  3a04e6ec6811c586a0aea0923eb6f468860a848240436202aa7c8c1bb61bb662
require_function_sha256 kpm/r0lab.c r0lab_raw_abort_hook_callback \
  21385cee16f278490a57beda8702055f314875516c7758f987188b66526675fd
require_function_sha256 kpm/r0lab.c \
  r0lab_raw_abort_hook_installed_callback_locked \
  d74bd88f1daf89464f7bb26a8db384ecbd6600e6ede9388c3a3f8c8f0b5a7180
require_function_sha256 kpm/r0lab.c r0lab_raw_abort_hook_acquire \
  7547ebe1c19a6654cebcb7aa1ec58dce8861772f237e614bf6423b56d2e94676
require_function_sha256 kpm/r0lab.c r0lab_raw_abort_hook_release \
  141e4bf9e15aaee8b08558784c957ba4dcd58dd3a5bab2d3b8e5bd22ca8426e3
require_function_sha256 kpm/r0lab.c r0lab_raw_slot_arm \
  f116231d20d16d97d51ca27467c54cf7f61708397f8d8c75b25573a6b9641589
require_function_sha256 kpm/r0lab.c r0lab_raw_slot_ready \
  78992e7036b97f888201719227717da581112a44d325033698a413ec0facbb7b
require_function_sha256 lab-app/src/main/cpp/labprobe.c \
  r0lab_raw_hold_lifetime_common \
  56ec74766dd25f01f58650aaffa7c147f21f7f46ce1050b86f0958859e882302
require_function_sha256 lab-app/src/main/cpp/labprobe.c \
  r0lab_raw_hold_abort_iabt_transition \
  12ecd7fadde7f3f38ff129c12e5366f98f1908926c60970fb4ff6b06c97b7826
require_function_text kpm/r0lab.c \
  r0lab_raw_hook_page_token_acquire_iabt_transition_locked \
  'page = r0lab_raw_page_find_by_mm_addr_locked(mm, address);'
require_function_text kpm/r0lab.c \
  r0lab_raw_hook_page_token_acquire_iabt_transition_locked \
  'if (!page->abort_hook_iabt_transition)'
require_function_text kpm/r0lab.c \
  r0lab_raw_hook_page_token_acquire_iabt_transition_locked \
  'page->raw.state != R0LAB_RAW_SOURCE_UXN'
require_function_text kpm/r0lab.c \
  r0lab_raw_hook_page_token_acquire_iabt_transition_locked \
  'page->raw.state != R0LAB_RAW_ORIGINAL_READ'
require_function_text kpm/r0lab.c \
  r0lab_raw_hook_page_token_acquire_iabt_transition_locked \
  '!page->raw.read_cycle_active'
require_function_text kpm/r0lab.c \
  r0lab_raw_hook_page_token_acquire_iabt_transition_locked \
  'token->kind = R0LAB_RAW_HOOK_ABORT;'
reject_function_text kpm/r0lab.c \
  r0lab_raw_hook_page_token_acquire_iabt_transition_locked \
  'r0lab_raw_hook_route_reject_locked'
reject_function_text kpm/r0lab.c \
  r0lab_raw_hook_page_token_acquire_iabt_transition_locked 'route_hits'
reject_function_text kpm/r0lab.c \
  r0lab_raw_hook_page_token_acquire_iabt_transition_locked 'route_rejects'
reject_function_text kpm/r0lab.c \
  r0lab_raw_hook_page_token_acquire_iabt_transition_locked \
  'page->transitioning ='
reject_function_text kpm/r0lab.c \
  r0lab_raw_hook_page_token_acquire_iabt_transition_locked 'r0lab_record'
require_function_text kpm/r0lab.c r0lab_raw_before_abort_iabt_transition \
  'far = (unsigned long)args->arg0;'
require_function_text kpm/r0lab.c r0lab_raw_before_abort_iabt_transition \
  'esr = (unsigned int)args->arg1;'
require_function_text kpm/r0lab.c r0lab_raw_before_abort_iabt_transition \
  'regs = (struct pt_regs *)(unsigned long)args->arg2;'
require_function_text kpm/r0lab.c r0lab_raw_before_abort_iabt_transition \
  'g_session.owner_tgid == r0lab_current_tgid()'
require_function_text kpm/r0lab.c r0lab_raw_before_abort_iabt_transition \
  'r0lab_raw_hook_page_token_acquire_iabt_transition_locked('
require_function_text kpm/r0lab.c r0lab_raw_before_abort_iabt_transition \
  'page->transitioning = true;'
require_function_text kpm/r0lab.c r0lab_raw_before_abort_iabt_transition \
  'result = r0lab_raw_finish_read_cycle(&fault_page->raw);'
require_function_text kpm/r0lab.c r0lab_raw_before_abort_iabt_transition \
  'result = r0lab_raw_activate_shadow(&fault_page->raw);'
require_function_text kpm/r0lab.c r0lab_raw_before_abort_iabt_transition \
  'r0lab_raw_hook_page_token_release_locked(&route_token, true)'
require_function_text kpm/r0lab.c r0lab_raw_before_abort_iabt_transition \
  'fault_page->raw.state == R0LAB_RAW_SHADOW_RX'
require_function_text kpm/r0lab.c r0lab_raw_before_abort_iabt_transition \
  'args->skip_origin = 1;'
require_function_text kpm/r0lab.c r0lab_raw_before_abort_iabt_transition \
  'args->ret = 0;'
require_function_count kpm/r0lab.c r0lab_raw_before_abort_iabt_transition \
  'g_get_task_mm(' 1
require_function_count kpm/r0lab.c r0lab_raw_before_abort_iabt_transition \
  'g_mmput(' 1
require_function_count kpm/r0lab.c r0lab_raw_before_abort_iabt_transition \
  'r0lab_lock();' 3
require_function_count kpm/r0lab.c r0lab_raw_before_abort_iabt_transition \
  'r0lab_unlock(flags);' 3
require_function_count kpm/r0lab.c r0lab_raw_before_abort_iabt_transition \
  '++g_raw_inflight;' 1
require_function_count kpm/r0lab.c r0lab_raw_before_abort_iabt_transition \
  '--g_raw_inflight;' 1
require_function_count kpm/r0lab.c r0lab_raw_before_abort_iabt_transition \
  'page->transitioning = true;' 1
require_function_count kpm/r0lab.c r0lab_raw_before_abort_iabt_transition \
  'r0lab_raw_finish_read_cycle(' 1
require_function_count kpm/r0lab.c r0lab_raw_before_abort_iabt_transition \
  'r0lab_raw_activate_shadow(' 1
require_function_count kpm/r0lab.c r0lab_raw_before_abort_iabt_transition \
  'args->skip_origin = 1;' 1
require_function_count kpm/r0lab.c r0lab_raw_before_abort_iabt_transition \
  'args->ret = 0;' 1
reject_function_text kpm/r0lab.c r0lab_raw_before_abort_iabt_transition \
  'R0LAB_M3_ESR_EC_DABT_LOW'
reject_function_text kpm/r0lab.c r0lab_raw_before_abort_iabt_transition \
  'r0lab_raw_hook_route_reject_locked'
reject_function_text kpm/r0lab.c r0lab_raw_before_abort_iabt_transition \
  'route_hits'
reject_function_text kpm/r0lab.c r0lab_raw_before_abort_iabt_transition \
  'route_rejects'
reject_function_text kpm/r0lab.c r0lab_raw_before_abort_iabt_transition \
  'r0lab_record'
reject_function_text kpm/r0lab.c r0lab_raw_before_abort_iabt_transition \
  'r0lab_raw_replace_locked'
require_function_text kpm/r0lab.c r0lab_raw_abort_hook_callback \
  'if (iabt_transition)'
require_function_text kpm/r0lab.c r0lab_raw_abort_hook_callback \
  'return r0lab_raw_before_abort_iabt_transition;'
require_function_text kpm/r0lab.c \
  r0lab_raw_abort_hook_installed_callback_locked \
  'page->abort_hook_iabt_transition);'
require_function_text kpm/r0lab.c r0lab_raw_abort_hook_acquire \
  'page->abort_hook_iabt_transition);'
require_function_text kpm/r0lab.c r0lab_raw_abort_hook_release \
  'iabt_transition = page->abort_hook_iabt_transition;'
require_function_text kpm/r0lab.c r0lab_raw_slot_arm \
  '(iabt_transition_abort_hook ? 1U : 0U)'
require_function_text kpm/r0lab.c r0lab_raw_slot_arm \
  'active->abort_hook_iabt_transition'
require_function_text kpm/r0lab.c r0lab_raw_slot_arm \
  'slot->abort_hook_iabt_transition = iabt_transition_abort_hook;'
require_function_text kpm/r0lab.c r0lab_raw_slot_ready \
  'abort_hook_iabt_transition = page->abort_hook_iabt_transition;'
require_function_text kpm/r0lab.c r0lab_raw_slot_ready \
  'abort_hook_iabt_route=%u abort_hook_iabt_transition=%u'
require_line_before kpm/r0lab.c \
  '    if (!strncmp(args, "raw slot arm abort-iabt-transition ", 35)) {' \
  '    if (!strncmp(args, "raw slot arm ", 13)) {'
require_text lab-app/src/main/cpp/labprobe.c \
  'raw raw-hold abort-iabt-transition '
require_text lab-app/src/main/cpp/labprobe.c \
  'raw mode=raw-hold-abort-iabt-transition failures=%d'
require_function_text lab-app/src/main/cpp/labprobe.c \
  r0lab_raw_hold_lifetime_common \
  'error=abort-iabt-transition hold requires source single'
require_function_text lab-app/src/main/cpp/labprobe.c \
  r0lab_raw_hold_lifetime_common \
  'expect_shadow = want_shadow || iabt_transition_abort_hook;'
require_function_text lab-app/src/main/cpp/labprobe.c \
  r0lab_raw_hold_lifetime_common \
  'char ready[2][512] = {{0}, {0}};'
require_function_text lab-app/src/main/cpp/labprobe.c \
  r0lab_raw_hold_lifetime_common \
  'iabt_transition_abort_hook ? 1 : 0;'
require_function_text lab-app/src/main/cpp/labprobe.c \
  r0lab_raw_hold_lifetime_common \
  'if (!sigsetjmp(g_r0lab_raw_signal_jump, 1))'
require_function_text lab-app/src/main/cpp/labprobe.c \
  r0lab_raw_hold_lifetime_common \
  'transition_fault_caught = 1;'
require_function_text lab-app/src/main/cpp/labprobe.c \
  r0lab_raw_hold_lifetime_common \
  'g_r0lab_m5_hold.mode = iabt_transition_abort_hook ? 20 :'
require_function_text lab-app/src/main/cpp/labprobe.c \
  r0lab_raw_hold_lifetime_common \
  'restore_abi=skip_origin_ret0'
require_function_text lab-app/src/main/cpp/labprobe.c \
  r0lab_raw_signal_handler \
  'if (g_r0lab_raw_signal_jump_on_fault)'
require_line_before lab-app/src/main/cpp/labprobe.c \
  '    if (g_r0lab_raw_signal_jump_on_fault)' \
  '    (void)syscall(SYS_mprotect, g_r0lab_raw_signal_page,'
require_line_before lab-app/src/main/cpp/labprobe.c \
  '    if (!strncmp(args, "raw raw-hold abort-iabt-transition ", 35)) {' \
  '    if (!strncmp(args, "raw exit hook hold ", 19)) {'
require_text scripts/test_raw_abort_iabt_transition_device.sh \
  'RAW_ABORT_IABT_TRANSITION_CLEAN_BOOT_CONFIRMED'
require_text scripts/test_raw_abort_iabt_transition_device.sh \
  'EXPECTED_SERIAL=32250DLH2000Z3'
require_text scripts/test_raw_abort_iabt_transition_device.sh \
  'EXPECTED_PRE_HOLD_BOOT_ID=f5d754c7-b25c-4375-894f-b5a2e374cbf8'
require_text scripts/test_raw_abort_iabt_transition_device.sh \
  'IDLE_SECONDS=15'
require_text scripts/test_raw_abort_iabt_transition_device.sh \
  'EXPECTED_KPM_SHA=c29b114970e94e4b6d559728a1e72dfaadc441f9333c906de67554379c121feb'
require_text scripts/test_raw_abort_iabt_transition_device.sh \
  'EXPECTED_LABPROBE_SHA=48a6317362355be5e8070ecfa31179cc163f777bf46a865404986255da127a9d'
require_text scripts/test_raw_abort_iabt_transition_device.sh \
  'EXPECTED_CLASSES_DEX_SHA=325e8a54bd306ef4da230de9919d0da46dec112f97efa9fbf42646dc7dd7ec79'
require_text scripts/test_raw_abort_iabt_transition_device.sh \
  'EXPECTED_SIGNER_CERT_SHA=73f1e2d251423909f33bfc7573580bd096834b57f680d5edb6e68655b1f903dd'
require_text scripts/test_raw_abort_iabt_transition_device.sh \
  'wxshadow-v2-f46-d4-r3m-iabt-transition-retry1-source-20260724'
require_text scripts/test_raw_abort_iabt_transition_device.sh \
  'raw raw-hold abort-iabt-transition $TOKEN'
require_text scripts/test_raw_abort_iabt_transition_device.sh \
  'D4-R3m-source-uxn-iabt-transition-stable'
require_text scripts/test_raw_abort_iabt_transition_device.sh \
  'D4-R3m-source-uxn-iabt-transition-unstable'
require_text scripts/test_raw_abort_iabt_transition_device.sh \
  'D4-R3m-setup-blocked'
require_file_sha256 scripts/test_raw_abort_iabt_transition_device.sh \
  34b5edfe7081f87afefe99c06078f34f515aeb946d01d424b7ab664fd5486c02
require_function_text scripts/test_raw_abort_iabt_transition_device.sh \
  poll_adb_transport 'state=$(adb_device get-state 2>&1)'
reject_function_text scripts/test_raw_abort_iabt_transition_device.sh \
  poll_adb_transport 'run_app_command'
reject_function_text scripts/test_raw_abort_iabt_transition_device.sh \
  poll_adb_transport 'supercmd'
reject_function_text scripts/test_raw_abort_iabt_transition_device.sh \
  poll_adb_transport '/proc/'
reject_function_text scripts/test_raw_abort_iabt_transition_device.sh \
  poll_adb_transport 'adb_device shell getprop'
require_text scripts/verify_d4_r3m_disassembly.sh \
  'D4-R3m disassembly passed: frame_loads=3 increments=2 decrement=1 nonstack_stores=10 indirect_calls=10 activate_calls=1 finish_calls=1 transition_stores=2 skip_origin_stores=1 ret_zero_stores=1 hardware_cache_tlb=0'
require_text scripts/verify_d4_r3m_disassembly.sh \
  "grep -Eq '[[:space:]](dmb|dsb|isb|dc|ic|tlbi)([[:space:]]|$)'"
require_file_sha256 scripts/verify_d4_r3m_disassembly.sh \
  1c4deb9a5a6b3e54e1262ac352e62234873191c07696b38315bfa2021d3872ab
require_function_sha256 kpm/r0lab.c r0lab_raw_before_abort \
  dfa2fef78a90a38c427a82041164acbfa3fab5b00b594976edd09d965f0fea61
require_function_sha256 kpm/r0lab.c r0lab_raw_slot_ready \
  78992e7036b97f888201719227717da581112a44d325033698a413ec0facbb7b
require_file scripts/test_raw_abort_inflight_passthrough_device.sh
require_file scripts/verify_d4_r3j_disassembly.sh
require_text kpm/r0lab.c 'bool abort_hook_inflight;'
require_text kpm/r0lab.c 'raw slot arm abort-inflight '
require_function_sha256 kpm/r0lab.c \
  r0lab_raw_before_abort_inflight_passthrough \
  47a891ca29896f4f599b045250dccf90896d1999b41af09df73a5d657f708b7d
require_function_text kpm/r0lab.c \
  r0lab_raw_before_abort_inflight_passthrough \
  'current_mm = g_get_task_mm(current);'
require_function_text kpm/r0lab.c \
  r0lab_raw_before_abort_inflight_passthrough 'flags = r0lab_lock();'
require_function_text kpm/r0lab.c \
  r0lab_raw_before_abort_inflight_passthrough '++g_raw_inflight;'
require_function_text kpm/r0lab.c \
  r0lab_raw_before_abort_inflight_passthrough '--g_raw_inflight;'
require_function_text kpm/r0lab.c \
  r0lab_raw_before_abort_inflight_passthrough 'r0lab_unlock(flags);'
require_function_text kpm/r0lab.c \
  r0lab_raw_before_abort_inflight_passthrough 'g_mmput(current_mm);'
require_function_count kpm/r0lab.c \
  r0lab_raw_before_abort_inflight_passthrough 'g_get_task_mm(' 1
require_function_count kpm/r0lab.c \
  r0lab_raw_before_abort_inflight_passthrough 'r0lab_lock(' 2
require_function_count kpm/r0lab.c \
  r0lab_raw_before_abort_inflight_passthrough '++g_raw_inflight;' 1
require_function_count kpm/r0lab.c \
  r0lab_raw_before_abort_inflight_passthrough '--g_raw_inflight;' 1
require_function_count kpm/r0lab.c \
  r0lab_raw_before_abort_inflight_passthrough 'r0lab_unlock(' 2
require_function_count kpm/r0lab.c \
  r0lab_raw_before_abort_inflight_passthrough 'g_mmput(' 1
reject_function_text kpm/r0lab.c \
  r0lab_raw_before_abort_inflight_passthrough 'barrier();'
reject_function_text kpm/r0lab.c \
  r0lab_raw_before_abort_inflight_passthrough 'args->'
reject_function_text kpm/r0lab.c \
  r0lab_raw_before_abort_inflight_passthrough 'udata->'
reject_function_text kpm/r0lab.c \
  r0lab_raw_before_abort_inflight_passthrough 'g_session'
reject_function_text kpm/r0lab.c \
  r0lab_raw_before_abort_inflight_passthrough 'r0lab_current_tgid'
reject_function_text kpm/r0lab.c \
  r0lab_raw_before_abort_inflight_passthrough 'r0lab_record'
reject_function_text kpm/r0lab.c \
  r0lab_raw_before_abort_inflight_passthrough 'skip_origin'
reject_function_text kpm/r0lab.c \
  r0lab_raw_before_abort_inflight_passthrough 'pte'
reject_function_text kpm/r0lab.c \
  r0lab_raw_before_abort_inflight_passthrough 'atomic'
reject_function_text kpm/r0lab.c \
  r0lab_raw_before_abort_inflight_passthrough 'READ_ONCE'
reject_function_text kpm/r0lab.c \
  r0lab_raw_before_abort_inflight_passthrough 'WRITE_ONCE'
require_function_text kpm/r0lab.c r0lab_raw_abort_hook_callback \
  'if (inflight)'
require_function_text kpm/r0lab.c r0lab_raw_abort_hook_callback \
  'return r0lab_raw_before_abort_inflight_passthrough;'
require_function_text kpm/r0lab.c r0lab_raw_abort_hook_release \
  'inflight = page->abort_hook_inflight;'
require_function_text kpm/r0lab.c r0lab_raw_abort_hook_release \
  'callback = r0lab_raw_abort_hook_callback('
require_function_text kpm/r0lab.c r0lab_raw_slot_arm \
  '(inflight_abort_hook ? 1U : 0U)'
require_function_text kpm/r0lab.c r0lab_raw_slot_arm \
  'active->abort_hook_inflight'
require_function_text kpm/r0lab.c r0lab_raw_slot_arm \
  'slot->abort_hook_inflight = inflight_abort_hook;'
require_function_text kpm/r0lab.c r0lab_raw_slot_ready \
  'abort_hook_inflight = page->abort_hook_inflight;'
require_function_text kpm/r0lab.c r0lab_raw_slot_ready \
  'abort_hook_lock=%u abort_hook_inflight=%u'
require_line_before kpm/r0lab.c \
  '    if (!strncmp(args, "raw slot arm abort-inflight ", 28)) {' \
  '    if (!strncmp(args, "raw slot arm ", 13)) {'
require_function_sha256 lab-app/src/main/cpp/labprobe.c \
  r0lab_raw_hold_abort_inflight \
  98d313f8e46f3bb449f4cef11159096506f9a9d8e7806af0ffa5c474b97998c1
require_text lab-app/src/main/cpp/labprobe.c 'raw raw-hold abort-inflight '
require_text lab-app/src/main/cpp/labprobe.c \
  'raw mode=raw-hold-abort-inflight failures=%d'
require_function_text lab-app/src/main/cpp/labprobe.c \
  r0lab_raw_hold_lifetime_common \
  'error=abort-inflight hold requires source single'
require_function_text lab-app/src/main/cpp/labprobe.c \
  r0lab_raw_hold_lifetime_common \
  'abort_hook_inflight[index] != 1'
require_function_text lab-app/src/main/cpp/labprobe.c \
  r0lab_raw_hold_lifetime_common \
  'inflight_abort_hook ? 18 :'
require_function_text lab-app/src/main/cpp/labprobe.c \
  r0lab_raw_hold_abort_inflight \
  'false, false, false, false, true,'
require_text scripts/test_raw_abort_inflight_passthrough_device.sh \
  'RAW_ABORT_INFLIGHT_CLEAN_BOOT_CONFIRMED'
require_text scripts/test_raw_abort_inflight_passthrough_device.sh \
  'EXPECTED_SERIAL=32250DLH2000Z3'
require_line_before scripts/test_raw_abort_inflight_passthrough_device.sh \
  '[ "$SERIAL" = "$EXPECTED_SERIAL" ] ||' \
  'case "$CLEAN_BOOT_CONFIRMED" in'
require_text scripts/test_raw_abort_inflight_passthrough_device.sh \
  'EXPECTED_KPM_SHA=018c8e96023692102f2941f4884b5be36913047ab80a59fe96f8e5458c775cfc'
require_text scripts/test_raw_abort_inflight_passthrough_device.sh \
  'EXPECTED_LABPROBE_SHA=f927140d79d9298d8c55a22cecd2021d664c320557ec74854eb39a90af1e37cb'
require_text scripts/test_raw_abort_inflight_passthrough_device.sh \
  'raw raw-hold abort-inflight $TOKEN'
require_text scripts/test_raw_abort_inflight_passthrough_device.sh \
  'abort_hook_lock=0'
require_text scripts/test_raw_abort_inflight_passthrough_device.sh \
  'abort_hook_inflight=1'
require_text scripts/test_raw_abort_inflight_passthrough_device.sh \
  'D4-R3j-source-uxn-abort-inflight-stable'
require_text scripts/test_raw_abort_inflight_passthrough_device.sh \
  'D4-R3j-source-uxn-abort-inflight-unstable'
require_text scripts/test_raw_abort_inflight_passthrough_device.sh \
  'D4-R3j-setup-blocked'
require_function_text scripts/test_raw_abort_inflight_passthrough_device.sh \
  poll_adb_transport 'adb_device get-state'
reject_function_text scripts/test_raw_abort_inflight_passthrough_device.sh \
  poll_adb_transport 'run_app_command'
reject_function_text scripts/test_raw_abort_inflight_passthrough_device.sh \
  poll_adb_transport 'supercmd'
reject_function_text scripts/test_raw_abort_inflight_passthrough_device.sh \
  poll_adb_transport '/proc/'
reject_function_text scripts/test_raw_abort_inflight_passthrough_device.sh \
  poll_adb_transport 'adb_device shell getprop'
reject_function_text scripts/test_raw_abort_inflight_passthrough_device.sh \
  poll_adb_transport 'module unload'
require_text scripts/verify_d4_r3j_disassembly.sh \
  'D4-R3j disassembly passed: increment=1 decrement=1 stores=2 hardware_barriers=0'
require_text scripts/verify_d4_r3j_disassembly.sh \
  "grep -Eq '[[:space:]](dmb|dsb|isb)([[:space:]]|$)'"
require_text scripts/test_raw_abort_visible_inflight_device.sh \
  'RAW_ABORT_VISIBLE_INFLIGHT_CLEAN_BOOT_CONFIRMED'
require_text scripts/test_raw_abort_visible_inflight_device.sh \
  'EXPECTED_SERIAL=32250DLH2000Z3'
require_line_before scripts/test_raw_abort_visible_inflight_device.sh \
  '[ "$SERIAL" = "$EXPECTED_SERIAL" ] ||' \
  'case "$CLEAN_BOOT_CONFIRMED" in'
require_text scripts/test_raw_abort_visible_inflight_device.sh \
  'EXPECTED_KPM_SHA=7f83c71160629f12db6d4e752b076ec42c102aeb9228972001736e06755468cc'
require_text scripts/test_raw_abort_visible_inflight_device.sh \
  'EXPECTED_LABPROBE_SHA=f927140d79d9298d8c55a22cecd2021d664c320557ec74854eb39a90af1e37cb'
require_text scripts/test_raw_abort_visible_inflight_device.sh \
  'EXPECTED_CLASSES_DEX_SHA=325e8a54bd306ef4da230de9919d0da46dec112f97efa9fbf42646dc7dd7ec79'
require_text scripts/test_raw_abort_visible_inflight_device.sh \
  'EXPECTED_SIGNER_CERT_SHA=73f1e2d251423909f33bfc7573580bd096834b57f680d5edb6e68655b1f903dd'
require_text scripts/test_raw_abort_visible_inflight_device.sh \
  'wxshadow-v2-f46-d4-r3k-visible-inflight-source-20260724'
require_text scripts/test_raw_abort_visible_inflight_device.sh \
  'source_commit=%s source_tag=%s'
require_text scripts/test_raw_abort_visible_inflight_device.sh \
  'raw raw-hold abort-inflight $TOKEN'
require_text scripts/test_raw_abort_visible_inflight_device.sh \
  'abort_hook_lock=0'
require_text scripts/test_raw_abort_visible_inflight_device.sh \
  'abort_hook_inflight=1'
require_text scripts/test_raw_abort_visible_inflight_device.sh \
  'D4-R3k-source-uxn-abort-visible-inflight-stable'
require_text scripts/test_raw_abort_visible_inflight_device.sh \
  'D4-R3k-source-uxn-abort-visible-inflight-unstable'
require_text scripts/test_raw_abort_visible_inflight_device.sh \
  'D4-R3k-setup-blocked'
require_function_text scripts/test_raw_abort_visible_inflight_device.sh \
  poll_adb_transport 'adb_device get-state'
reject_function_text scripts/test_raw_abort_visible_inflight_device.sh \
  poll_adb_transport 'run_app_command'
reject_function_text scripts/test_raw_abort_visible_inflight_device.sh \
  poll_adb_transport 'supercmd'
reject_function_text scripts/test_raw_abort_visible_inflight_device.sh \
  poll_adb_transport '/proc/'
reject_function_text scripts/test_raw_abort_visible_inflight_device.sh \
  poll_adb_transport 'adb_device shell getprop'
reject_function_text scripts/test_raw_abort_visible_inflight_device.sh \
  poll_adb_transport 'module unload'
require_text scripts/verify_d4_r3k_disassembly.sh \
  'D4-R3k disassembly passed: increment=1 decrement=1 stores=2 intervening_calls=3 hardware_barriers=0'
require_text scripts/verify_d4_r3k_disassembly.sh \
  "grep -Eq '[[:space:]](dmb|dsb|isb)([[:space:]]|$)'"
require_text kpm/r0lab.c 'bool abort_hook_lock;'
require_text kpm/r0lab.c 'static bool g_raw_abort_hook_transitioning;'
require_text kpm/r0lab.c 'raw slot arm abort-lock '
require_function_text kpm/r0lab.c \
  r0lab_raw_before_abort_lock_passthrough '(void)args;'
require_function_text kpm/r0lab.c \
  r0lab_raw_before_abort_lock_passthrough '(void)udata;'
require_function_text kpm/r0lab.c \
  r0lab_raw_before_abort_lock_passthrough \
  'current_mm = g_get_task_mm(current);'
require_function_text kpm/r0lab.c \
  r0lab_raw_before_abort_lock_passthrough 'flags = r0lab_lock();'
require_function_text kpm/r0lab.c \
  r0lab_raw_before_abort_lock_passthrough 'r0lab_unlock(flags);'
require_function_text kpm/r0lab.c \
  r0lab_raw_before_abort_lock_passthrough 'g_mmput(current_mm);'
require_function_count kpm/r0lab.c \
  r0lab_raw_before_abort_lock_passthrough 'g_get_task_mm(' 1
require_function_count kpm/r0lab.c \
  r0lab_raw_before_abort_lock_passthrough 'r0lab_lock(' 1
require_function_count kpm/r0lab.c \
  r0lab_raw_before_abort_lock_passthrough 'r0lab_unlock(' 1
require_function_count kpm/r0lab.c \
  r0lab_raw_before_abort_lock_passthrough 'g_mmput(' 1
reject_function_text kpm/r0lab.c \
  r0lab_raw_before_abort_lock_passthrough 'args->'
reject_function_text kpm/r0lab.c \
  r0lab_raw_before_abort_lock_passthrough 'udata->'
reject_function_text kpm/r0lab.c \
  r0lab_raw_before_abort_lock_passthrough 'g_session'
reject_function_text kpm/r0lab.c \
  r0lab_raw_before_abort_lock_passthrough 'r0lab_current_tgid'
reject_function_text kpm/r0lab.c \
  r0lab_raw_before_abort_lock_passthrough 'g_raw_inflight'
reject_function_text kpm/r0lab.c \
  r0lab_raw_before_abort_lock_passthrough 'r0lab_record'
reject_function_text kpm/r0lab.c \
  r0lab_raw_before_abort_lock_passthrough 'route'
reject_function_text kpm/r0lab.c \
  r0lab_raw_before_abort_lock_passthrough 'skip_origin'
reject_function_text kpm/r0lab.c \
  r0lab_raw_before_abort_lock_passthrough 'pte'
reject_function_text kpm/r0lab.c \
  r0lab_raw_before_abort_lock_passthrough 'tlb'
reject_function_text kpm/r0lab.c \
  r0lab_raw_before_abort_lock_passthrough 'cache'
reject_function_text kpm/r0lab.c \
  r0lab_raw_before_abort_lock_passthrough 'trylock'
reject_function_text kpm/r0lab.c \
  r0lab_raw_before_abort_lock_passthrough 'printk'
require_function_text kpm/r0lab.c r0lab_raw_abort_hook_callback \
  'if (lock)'
require_function_text kpm/r0lab.c r0lab_raw_abort_hook_callback \
  'return r0lab_raw_before_abort_lock_passthrough;'
require_function_text kpm/r0lab.c \
  r0lab_raw_abort_hook_installed_callback_locked \
  'if (page->hook_installed)'
require_function_text kpm/r0lab.c \
  r0lab_raw_abort_hook_installed_callback_locked \
  'page->abort_hook_lock, page->abort_hook_inflight,'
require_function_text kpm/r0lab.c r0lab_raw_abort_hook_acquire \
  'page->abort_hook_lock, page->abort_hook_inflight,'
require_function_text kpm/r0lab.c r0lab_raw_abort_hook_acquire \
  'installed_callback = r0lab_raw_abort_hook_installed_callback_locked();'
require_function_text kpm/r0lab.c r0lab_raw_abort_hook_acquire \
  'if (installed_callback && installed_callback != callback)'
require_function_text kpm/r0lab.c r0lab_raw_abort_hook_acquire \
  'if (g_raw_abort_hook_transitioning)'
require_function_text kpm/r0lab.c r0lab_raw_abort_hook_acquire \
  'g_raw_abort_hook_transitioning = true;'
require_function_text kpm/r0lab.c r0lab_raw_abort_hook_acquire \
  'result = hook_wrap3(g_do_mem_abort, callback, NULL, NULL);'
require_function_text kpm/r0lab.c r0lab_raw_abort_hook_acquire \
  'page->hook_installed = true;'
require_function_text kpm/r0lab.c r0lab_raw_abort_hook_acquire \
  'r0lab_hook_detach(g_do_mem_abort, callback, NULL);'
require_function_text kpm/r0lab.c r0lab_raw_abort_hook_release \
  'lock = page->abort_hook_lock;'
require_function_text kpm/r0lab.c r0lab_raw_abort_hook_release \
  'page->hook_installed = false;'
require_function_text kpm/r0lab.c r0lab_raw_abort_hook_release \
  'g_raw_abort_hook_transitioning = true;'
require_function_text kpm/r0lab.c r0lab_raw_abort_hook_release \
  'g_raw_abort_hook_transitioning = false;'
require_function_text kpm/r0lab.c r0lab_raw_abort_hook_release \
  'callback = r0lab_raw_abort_hook_callback('
require_function_text kpm/r0lab.c r0lab_raw_abort_hook_release \
  'r0lab_hook_detach(g_do_mem_abort, callback, NULL);'
require_function_text kpm/r0lab.c r0lab_raw_slot_arm \
  '(lock_abort_hook ? 1U : 0U)'
require_function_text kpm/r0lab.c r0lab_raw_slot_arm \
  'active->abort_hook_lock'
require_function_text kpm/r0lab.c r0lab_raw_slot_arm \
  'slot->abort_hook_lock = lock_abort_hook;'
require_function_text kpm/r0lab.c r0lab_raw_slot_ready \
  'abort_hook_lock = page->abort_hook_lock;'
require_function_text kpm/r0lab.c r0lab_raw_slot_ready \
  'abort_hook_mmget=%u abort_hook_lock=%u'
require_line_before kpm/r0lab.c \
  '    if (!strncmp(args, "raw slot arm abort-lock ", 24)) {' \
  '    if (!strncmp(args, "raw slot arm ", 13)) {'
require_text lab-app/src/main/cpp/labprobe.c 'r0lab_raw_hold_abort_lock'
require_text lab-app/src/main/cpp/labprobe.c 'raw raw-hold abort-lock '
require_text lab-app/src/main/cpp/labprobe.c \
  'raw mode=raw-hold-abort-lock failures=%d'
require_text lab-app/src/main/cpp/labprobe.c \
  '"raw slot arm abort-lock 0x%llx %u 0x%llx"'
require_function_text lab-app/src/main/cpp/labprobe.c \
  r0lab_raw_hold_lifetime_common \
  'error=abort-lock hold requires source single'
require_function_text lab-app/src/main/cpp/labprobe.c \
  r0lab_raw_hold_lifetime_common \
  '(lock_abort_hook ? 1U : 0U)'
require_function_text lab-app/src/main/cpp/labprobe.c \
  r0lab_raw_hold_lifetime_common \
  'abort_hook_lock[index] != 1'
require_function_text lab-app/src/main/cpp/labprobe.c \
  r0lab_raw_hold_lifetime_common \
  'lock_abort_hook ? 17 :'
require_function_text lab-app/src/main/cpp/labprobe.c \
  r0lab_raw_hold_abort_lock \
  'snprintf(args, sizeof(args), "source single 0x%llx"'
require_function_text lab-app/src/main/cpp/labprobe.c \
  r0lab_raw_hold_abort_lock \
  'false, false, false, true, false,'
require_line_before lab-app/src/main/cpp/labprobe.c \
  '    if (!strncmp(args, "raw raw-hold abort-lock ", 24)) {' \
  '    if (!strncmp(args, "raw exit hook hold ", 19)) {'
require_text scripts/test_raw_abort_lock_passthrough_device.sh \
  'RAW_ABORT_LOCK_CLEAN_BOOT_CONFIRMED'
require_file_sha256 scripts/test_raw_abort_lock_passthrough_device.sh \
  674255c9e4bf0950904d4932dc7c5b6785f0b086cbd755d6d9c774430232327e
require_text scripts/test_raw_abort_lock_passthrough_device.sh \
  'IDLE_SECONDS=15'
require_text scripts/test_raw_abort_lock_passthrough_device.sh \
  'EXPECTED_KPM_SHA=dfc4411b50f9ff233bb93905e5eca3da2bd8260dfc33d8a786f43fc67dfebb50'
require_text scripts/test_raw_abort_lock_passthrough_device.sh \
  'EXPECTED_LABPROBE_SHA=cad0df4c7bf406f6c48dc319b41864f0f7a7b667c17b7b41f040d56b1b82dcd4'
require_text scripts/test_raw_abort_lock_passthrough_device.sh \
  'EXPECTED_CLASSES_DEX_SHA=325e8a54bd306ef4da230de9919d0da46dec112f97efa9fbf42646dc7dd7ec79'
require_text scripts/test_raw_abort_lock_passthrough_device.sh \
  'EXPECTED_SIGNER_CERT_SHA=73f1e2d251423909f33bfc7573580bd096834b57f680d5edb6e68655b1f903dd'
require_text scripts/test_raw_abort_lock_passthrough_device.sh \
  'R0LAB_DEBUG_KEYSTORE="$KEYSTORE" "$ROOT/scripts/build_lab_app.sh"'
require_text scripts/test_raw_abort_lock_passthrough_device.sh \
  'git -C "$ROOT" status --porcelain --untracked-files=no'
require_text scripts/test_raw_abort_lock_passthrough_device.sh \
  'raw raw-hold abort-lock $TOKEN'
require_text scripts/test_raw_abort_lock_passthrough_device.sh \
  'abort_hook_installed=1'
require_text scripts/test_raw_abort_lock_passthrough_device.sh \
  'abort_hook_suppressed=0'
require_text scripts/test_raw_abort_lock_passthrough_device.sh \
  'abort_hook_passthrough=0'
require_text scripts/test_raw_abort_lock_passthrough_device.sh \
  'abort_hook_mmget=0'
require_text scripts/test_raw_abort_lock_passthrough_device.sh \
  'abort_hook_lock=1'
require_text scripts/test_raw_abort_lock_passthrough_device.sh \
  'D4-R3i-source-uxn-abort-lock-stable'
require_text scripts/test_raw_abort_lock_passthrough_device.sh \
  'D4-R3i-source-uxn-abort-lock-unstable'
require_text scripts/test_raw_abort_lock_passthrough_device.sh \
  'D4-R3i-setup-blocked'
require_text scripts/test_raw_abort_lock_passthrough_device.sh \
  'refs/tags/${tag}^{commit}'
require_text scripts/test_raw_abort_lock_passthrough_device.sh \
  'require_tagged_head wxshadow-v2-f46-d4-r3i-lock-exposure-source-20260724'
require_function_text scripts/test_raw_abort_lock_passthrough_device.sh \
  cleanup 'if [ "$HOLD_ACTIVE" -eq 1 ]; then'
require_function_text scripts/test_raw_abort_lock_passthrough_device.sh \
  poll_adb_transport 'adb_device get-state'
require_function_text scripts/test_raw_abort_lock_passthrough_device.sh \
  poll_adb_transport \
  'preserve_active_hold 1 D4-R3i-source-uxn-abort-lock-unstable'
require_function_text scripts/test_raw_abort_lock_passthrough_device.sh \
  poll_adb_transport \
  'preserve_active_hold 0 D4-R3i-source-uxn-abort-lock-stable'
reject_function_text scripts/test_raw_abort_lock_passthrough_device.sh \
  poll_adb_transport 'run_app_command'
reject_function_text scripts/test_raw_abort_lock_passthrough_device.sh \
  poll_adb_transport 'supercmd'
reject_function_text scripts/test_raw_abort_lock_passthrough_device.sh \
  poll_adb_transport 'capture_pstore'
reject_function_text scripts/test_raw_abort_lock_passthrough_device.sh \
  poll_adb_transport 'wait-for-device'
reject_function_text scripts/test_raw_abort_lock_passthrough_device.sh \
  poll_adb_transport '/proc/'
reject_function_text scripts/test_raw_abort_lock_passthrough_device.sh \
  poll_adb_transport 'adb_device shell getprop'
reject_function_text scripts/test_raw_abort_lock_passthrough_device.sh \
  poll_adb_transport 'raw slot clear'
reject_function_text scripts/test_raw_abort_lock_passthrough_device.sh \
  poll_adb_transport 'module unload'
require_line_before scripts/test_raw_abort_lock_passthrough_device.sh \
  'case "$CLEAN_BOOT_CONFIRMED" in' 'ensure_clean_source'
require_line_before scripts/test_raw_abort_lock_passthrough_device.sh \
  'ensure_clean_source' \
  'require_tag_target wxshadow-v2-f46-d4-r3h-mm-reference-stable-20260724 e6ee7c080ec5760f4b2c43bb0062724009ab440f'
require_line_before scripts/test_raw_abort_lock_passthrough_device.sh \
  'require_tag_target wxshadow-v2-f46-d4-r3i-lock-exposure-plan-20260724 93601f7dc0166ce4559b80a79669b8905f8e4dc6' \
  'EXISTING=$(supercmd module list 2>&1) ||'
require_line_before scripts/test_raw_abort_lock_passthrough_device.sh \
  'require_tagged_head wxshadow-v2-f46-d4-r3i-lock-exposure-source-20260724' \
  'EXISTING=$(supercmd module list 2>&1) ||'
require_line_before scripts/test_raw_abort_lock_passthrough_device.sh \
  'require_sha256 signer-cert "$EXPECTED_SIGNER_CERT_SHA" "$SIGNER_CERT_SHA"' \
  'EXISTING=$(supercmd module list 2>&1) ||'
require_line_before scripts/test_raw_abort_lock_passthrough_device.sh \
  'require_sha256 kpm "$EXPECTED_KPM_SHA" "$KPM_SHA"' \
  'EXISTING=$(supercmd module list 2>&1) ||'
require_line_before scripts/test_raw_abort_lock_passthrough_device.sh \
  'require_sha256 labprobe "$EXPECTED_LABPROBE_SHA" "$LABPROBE_SHA"' \
  'EXISTING=$(supercmd module list 2>&1) ||'
require_line_before scripts/test_raw_abort_lock_passthrough_device.sh \
  'require_sha256 classes.dex "$EXPECTED_CLASSES_DEX_SHA" "$CLASSES_DEX_SHA"' \
  'EXISTING=$(supercmd module list 2>&1) ||'
require_text "$DEVELOPMENT_SEQUENCE" 'F4.6-D4-R3d status-transport split'
require_text "$DEVELOPMENT_SEQUENCE" 'D4-R3c-status-logcat-timeout-kernel-panic'
require_text "$DEVELOPMENT_SEQUENCE" 'D4-R3d-raw-hold-self-unstable'
require_text "$DEVELOPMENT_SEQUENCE" 'D4-R3e-P'
require_text "$DEVELOPMENT_SEQUENCE" 'D4-R3e-A'
require_text "$DEVELOPMENT_SEQUENCE" 'Lab App-only lifetime matrix'
require_text "$DEVELOPMENT_SEQUENCE" 'slot count and retained PTE state'
require_text "$DEVELOPMENT_SEQUENCE" 'F4.6-D4-R3e-L2 live-PTE snapshot | Device evidence captured'
require_text "$DEVELOPMENT_SEQUENCE" 'F4.6-D4-R3e-L3 observer perturbation | Complete/classified postmortem'
require_text "$DEVELOPMENT_SEQUENCE" 'F4.6-D4-R3f abort-hook exposure | Complete/classified stable'
require_text "$DEVELOPMENT_SEQUENCE" 'F4.6-D4-R3g passthrough abort wrapper | Complete/classified stable'
require_text "$DEVELOPMENT_SEQUENCE" 'F4.6-D4-R3h global MM reference | Complete/classified stable'
require_text "$DEVELOPMENT_SEQUENCE" 'c5084d25-1e0f-4de4-9042-a972cd0170a8'
require_text "$DEVELOPMENT_SEQUENCE" 'wxshadow-v2-f46-d4-r3h-mm-reference-stable-20260724'
require_text "$DEVELOPMENT_SEQUENCE" '| 20 | D4-R3g |'
require_text "$DEVELOPMENT_SEQUENCE" '| 21 | D4-R3h |'
require_text "$DEVELOPMENT_SEQUENCE" '| 22 | D4-R3i |'
require_text "$DEVELOPMENT_SEQUENCE" '| 23 | D4-R3j |'
require_text "$DEVELOPMENT_SEQUENCE" '| 24 | D4-R3k |'
require_text "$DEVELOPMENT_SEQUENCE" '| 25 | D4-R3l |'
require_text "$DEVELOPMENT_SEQUENCE" '| 26 | D4-R3m |'
require_text "$DEVELOPMENT_SEQUENCE" '| 27 | D4-R3n |'
require_text "$DEVELOPMENT_SEQUENCE" 'F4.6-D4-R3i abort-lock exposure | Complete/classified stable; reboot closed'
require_text "$DEVELOPMENT_SEQUENCE" 'raw-abort-lock-passthrough-20260724-132524.log'
require_text "$DEVELOPMENT_SEQUENCE" '7714473555eaecb40dcb28351b4986894d4d06bcd36a0480b54d645f778b9938'
require_text "$DEVELOPMENT_SEQUENCE" '0a73bd18-ba4a-4bdf-87c6-a99f98d9d039'
require_text "$DEVELOPMENT_SEQUENCE" 'wxshadow-v2-f46-d4-r3i-lock-exposure-stable-20260724'
require_text "$DEVELOPMENT_SEQUENCE" 'D4-R3j | Adjacent inflight-accounting diagnostic'
require_text "$DEVELOPMENT_SEQUENCE" 'F4.6-D4-R3j adjacent inflight accounting | Complete/classified stable; reboot closed'
require_text "$DEVELOPMENT_SEQUENCE" 'raw-abort-inflight-passthrough-20260724-145803.log'
require_text "$DEVELOPMENT_SEQUENCE" '5ea6e6ed4cc3f47018e7ff710d031390a1b637defbc46d44abb17bed67c75929'
require_text "$DEVELOPMENT_SEQUENCE" '4ef34205-59b2-4003-8e1e-80728f6158ec'
require_text "$DEVELOPMENT_SEQUENCE" 'D4-R3k | Visible inflight-lifetime diagnostic'
require_text "$DEVELOPMENT_SEQUENCE" 'Complete/classified stable; reboot closed.'
require_text "$DEVELOPMENT_SEQUENCE" 'Exact source `45880fe`'
require_text "$DEVELOPMENT_SEQUENCE" 'docs/wxshadow-f4.6-d4-r3k-visible-inflight-lifetime-plan.md'
require_text "$DEVELOPMENT_SEQUENCE" 'D4-R3l | Read-only IABT admission and route'
require_text "$DEVELOPMENT_SEQUENCE" 'Complete/classified stable and reboot-closed.'
require_text "$FINAL_ROADMAP" 'R3l is closed and queue progress reached 25 of 27.'
require_text "$REFERENCE_COVERAGE" 'R3o exact source `e58d3de`'
require_text "$REFERENCE_COVERAGE" \
  'R3o Stage 5 restored both active shadow pages'
require_text "$DEVELOPMENT_SEQUENCE" 'docs/wxshadow-f4.6-d4-r3l-readonly-iabt-route-plan.md'
require_text "$DEVELOPMENT_SEQUENCE" 'dedicated no-accounting token acquire/validate/release path'
require_text "$DEVELOPMENT_SEQUENCE" 'docs/wxshadow-f4.6-d4-r3j-inflight-accounting-plan.md'
require_text "$DEVELOPMENT_SEQUENCE" 'scripts/verify_d4_r3j_disassembly.sh'
require_text "$DEVELOPMENT_SEQUENCE" 'docs/wxshadow-f4.6-d4-r3g-passthrough-wrapper-plan.md'
require_text "$DEVELOPMENT_SEQUENCE" 'docs/wxshadow-f4.6-d4-r3h-mm-reference-plan.md'
require_text "$DEVELOPMENT_SEQUENCE" 'docs/wxshadow-f4.6-d4-r3e-l3-observer-perturbation-plan.md'
require_text "$DEVELOPMENT_SEQUENCE" 'three valid samples per variant'
require_text "$DEVELOPMENT_SEQUENCE" 'D4-R3e-L3-A-L | Script-only local implementation'
require_text "$DEVELOPMENT_SEQUENCE" 'D4-R3e-L3-A-C-P | Aggregate classifier plan only'
require_text "$DEVELOPMENT_SEQUENCE" 'D4-R3e-L3-A-C-L | Offline classifier implementation'
require_text "$DEVELOPMENT_SEQUENCE" 'Passed locally: `scripts/classify_raw_observer_perturbation_evidence.sh`'
require_text "$DEVELOPMENT_SEQUENCE" 'D4-R3e-L3-A-D | Four clean-boot device rows'
require_text "$DEVELOPMENT_SEQUENCE" 'raw raw-hold lifetime source|shadow single|double <token>'
require_text "$DEVELOPMENT_SEQUENCE" 'D4-R3e-L2-P'
require_text "$DEVELOPMENT_SEQUENCE" 'D4-R3e-L1-single-source-uxn-unstable'
require_text "$DEVELOPMENT_SEQUENCE" 'docs/wxshadow-f4.6-d4-r3e-l2-live-pte-plan.md'
require_text "$FINAL_ROADMAP" 'F4.6-D4-R3e-L1 raw-hold lifetime matrix'
require_text "$FINAL_ROADMAP" 'source is unlocked by D4-R3e-L1'
require_text "$FINAL_ROADMAP" 'scripts/test_raw_hold_lifetime_matrix_device.sh'
require_text "$FINAL_ROADMAP" 'D4-R3e-L2 planning'
require_text "$FINAL_ROADMAP" 'raw-hold-lifetime-matrix-20260724-071931.log'
require_text "$FINAL_ROADMAP" 'D4-R3e-L2 live-PTE planning'
require_text "$FINAL_ROADMAP" 'raw slot live pte'
require_text "$FINAL_ROADMAP" 'raw raw-hold live-pte <token>'
require_text "$FINAL_ROADMAP" 'raw-live-pte-snapshot-20260724-080322.log'
require_text "$FINAL_ROADMAP" 'docs/wxshadow-f4.6-d4-r3e-l3-observer-perturbation-plan.md'
require_text "$FINAL_ROADMAP" 'strict 3/3 baseline instability versus 3/3 external-snapshot'
require_text "$FINAL_ROADMAP" 'Both variants now contain stable and unstable rows'
require_text "$FINAL_ROADMAP" 'scripts/test_raw_observer_perturbation_device.sh'
require_text "$FINAL_ROADMAP" 'B0-S2, B1-S2, B0-S3, and B1-S3'
require_text "$FINAL_ROADMAP" 'offline classifier validates strict historical anchors'
require_text "$FINAL_ROADMAP" 'host test passes all'
require_text "$FINAL_ROADMAP" 'non-sample rejection'
require_text "$FINAL_ROADMAP" '26 of 27 items executed'
require_text "$FINAL_ROADMAP" 'D4-R3i is queue item 22'
require_text "$FINAL_ROADMAP" 'D4-R3j is queue item 23'
require_text "$FINAL_ROADMAP" '++g_raw_inflight'
require_text "$FINAL_ROADMAP" '--g_raw_inflight'
require_text "$FINAL_ROADMAP" 'The remaining F4.6 reassembly path is finite:'
require_text "$FINAL_ROADMAP" '`D4-R3k` exposed the inflight count'
require_text "$FINAL_ROADMAP" '`D4-R3l` restores read-only IABT frame decoding'
require_text "$FINAL_ROADMAP" 'shared route helper writes hit/reject statistics'
require_text "$FINAL_ROADMAP" 'docs/wxshadow-f4.6-d4-r3l-readonly-iabt-route-plan.md'
require_text "$FINAL_ROADMAP" '`D4-R3m` restores only the IABT raw-PTE transition'
require_text "$FINAL_ROADMAP" \
  '`D4-R3n` restored the full callback and passed its explicit Phase A'
require_text "$FINAL_ROADMAP" \
  '`D4-R3o` applies the reference lifetime corrections'
require_text "$FINAL_ROADMAP" 'No additional callback micro-slices may be inserted'
require_text "$FINAL_ROADMAP" 'compiler-only `barrier()`'
require_text "$FINAL_ROADMAP" '0a73bd18-ba4a-4bdf-87c6-a99f98d9d039'
require_text "$FINAL_ROADMAP" 'wxshadow-v2-f46-d4-r3i-lock-exposure-stable-20260724'
require_text "$FINAL_ROADMAP" 'D4-R3f-source-uxn-no-abort-stable'
require_text "$FINAL_ROADMAP" 'D4-R3g as queue item 20'
require_text "$FINAL_ROADMAP" 'docs/wxshadow-f4.6-d4-r3g-passthrough-wrapper-plan.md'
require_text "$FINAL_ROADMAP" 'D4-R3g-source-uxn-abort-passthrough-stable'
require_text "$FINAL_ROADMAP" 'D4-R3h is queue item 21'
require_text "$FINAL_ROADMAP" 'clean committed-source'
require_text "$FINAL_ROADMAP" 'docs/wxshadow-f4.6-d4-r3h-mm-reference-plan.md'
require_text "$FINAL_ROADMAP" 'postmortem-recovered B1-S3 panic row'
reject_text "$FINAL_ROADMAP" 'four clean-boot rows remain deferred'
require_text "$DEVELOPMENT_SEQUENCE" 'B1-S2 is'
require_text "$DEVELOPMENT_SEQUENCE" 'D4-R3e-L3-B1-external-snapshot-unstable'
require_text "$DEVELOPMENT_SEQUENCE" 'raw-hold idle stability'
require_text "$DEVELOPMENT_SEQUENCE" 'Activity/Logcat status transport'
require_text "$DEVELOPMENT_SEQUENCE" 'KPM status supercall'
require_text "$DEVELOPMENT_SEQUENCE" 'maps/seq_file reader behavior'
require_text scripts/test_raw_exit_hook_raw_hold_split_device.sh 'RAW_EXIT_HOOK_RAW_HOLD_SPLIT_TOKEN'
require_text scripts/test_raw_exit_hook_raw_hold_split_device.sh 'RAW_EXIT_HOOK_RAW_HOLD_SPLIT_PROC_MAPS'
require_text scripts/test_raw_exit_hook_raw_hold_split_device.sh 'raw raw-hold routing hold $TOKEN'
require_text scripts/test_raw_exit_hook_raw_hold_split_device.sh 'raw mode=raw-hold-routing-hold failures=0'
require_text scripts/test_raw_exit_hook_raw_hold_split_device.sh 'exit_mmap_armed=0'
require_text scripts/test_raw_exit_hook_raw_hold_split_device.sh 'hook_arm_rc=-1/-1'
require_text scripts/test_raw_exit_hook_raw_hold_split_device.sh 'hook_status_rc=-1/-1'
require_text scripts/test_raw_exit_hook_raw_hold_split_device.sh 'exit_hook_installed=not_queried/not_queried'
require_text scripts/test_raw_exit_hook_raw_hold_split_device.sh 'phase=d4_r3b_raw_hold_established result=pass exit_mmap_armed=0 raw_slots=2 page_records=2'
require_text scripts/test_raw_exit_hook_raw_hold_split_device.sh 'classify_status_after_raw_hold()'
require_text scripts/test_raw_exit_hook_raw_hold_split_device.sh 'phase=d4_r3c_status_after_raw_hold evidence=begin'
require_text scripts/test_raw_exit_hook_raw_hold_split_device.sh 'D4-R3c-status-logcat-timeout'
require_text scripts/test_raw_exit_hook_raw_hold_split_device.sh 'D4-R3c-status-empty-or-reset-state'
require_text scripts/test_raw_exit_hook_raw_hold_split_device.sh 'D4-R3c-status-malformed-output'
require_text scripts/test_raw_exit_hook_raw_hold_split_device.sh 'D4-R3c-status-healthy-boot-id-pending'
require_text scripts/test_raw_exit_hook_raw_hold_split_device.sh 'phase=d4_r3b_hold_quiet_status evidence=begin'
require_text scripts/test_raw_exit_hook_raw_hold_split_device.sh 'phase=d4_r3b_hold_quiet_no_boot_reader result=pass boot_id_reader=not_used'
require_text scripts/test_raw_exit_hook_raw_hold_split_device.sh 'phase=d4_r3b_boot_id_reader result=pass boot_stable=1'
require_text scripts/test_raw_exit_hook_raw_hold_split_device.sh 'raw_exit_hook_raw_hold_split=pass phases=5'
require_text scripts/test_raw_exit_hook_raw_hold_split_device.sh 'exit_hook_clear=not_used'
require_text scripts/test_raw_exit_hook_raw_hold_idle_device.sh 'RAW_EXIT_HOOK_RAW_HOLD_IDLE_TOKEN'
require_text scripts/test_raw_exit_hook_raw_hold_idle_device.sh 'RAW_EXIT_HOOK_RAW_HOLD_IDLE_SECONDS'
require_text scripts/test_raw_exit_hook_raw_hold_idle_device.sh 'raw raw-hold routing hold $TOKEN'
require_text scripts/test_raw_exit_hook_raw_hold_idle_device.sh 'phase=d4_r3d_raw_hold_established result=pass exit_mmap_armed=0 raw_slots=2 page_records=2'
require_text scripts/test_raw_exit_hook_raw_hold_idle_device.sh 'phase=d4_r3d_raw_hold_idle evidence=begin'
require_text scripts/test_raw_exit_hook_raw_hold_idle_device.sh 'classification=D4-R3d-raw-hold-idle-stable'
require_text scripts/test_raw_exit_hook_raw_hold_idle_device.sh 'classification=D4-R3d-raw-hold-self-unstable'
require_text scripts/test_raw_exit_hook_raw_hold_idle_device.sh 'post_hold_status=not_used boot_id_reader=not_used proc_maps=not_used cleanup=preserve'
require_text scripts/test_raw_exit_hook_raw_hold_idle_device.sh 'preserving module: active raw hold remains and cleanup reentry is forbidden'
require_text scripts/test_raw_hold_lifetime_matrix_device.sh 'RAW_HOLD_LIFETIME_MATRIX_IDLE_SECONDS'
require_text scripts/test_raw_hold_lifetime_matrix_device.sh 'raw raw-hold lifetime source single'
require_text scripts/test_raw_hold_lifetime_matrix_device.sh 'raw raw-hold lifetime shadow single'
require_text scripts/test_raw_hold_lifetime_matrix_device.sh 'raw raw-hold lifetime source double'
require_text scripts/test_raw_hold_lifetime_matrix_device.sh 'raw raw-hold lifetime shadow double'
require_text scripts/test_raw_hold_lifetime_matrix_device.sh 'post_hold_status=not_used boot_id_reader=not_used proc_maps=not_used cleanup=preserve'
require_text scripts/test_raw_hold_lifetime_matrix_device.sh 'D4-R3e-L1-single-source-uxn-unstable'
require_text scripts/test_raw_hold_lifetime_matrix_device.sh 'D4-R3e-L1-single-shadow-rx-unstable'
require_text scripts/test_raw_hold_lifetime_matrix_device.sh 'D4-R3e-L1-two-source-uxn-unstable'
require_text scripts/test_raw_hold_lifetime_matrix_device.sh 'D4-R3e-L1-two-shadow-rx-unstable'
require_text scripts/test_raw_hold_lifetime_matrix_device.sh 'classification=D4-R3e-L1-lifetime-matrix-stable'
require_text scripts/test_raw_hold_lifetime_matrix_device.sh 'preserving module: active raw hold remains and cleanup reentry is forbidden'
require_text lab-app/src/main/cpp/labprobe.c 'r0lab_raw_hold_lifetime'
require_text lab-app/src/main/cpp/labprobe.c 'raw raw-hold lifetime '
require_text lab-app/src/main/cpp/labprobe.c 'raw mode=raw-hold-lifetime failures=%d'
require_text lab-app/src/main/cpp/labprobe.c 'target_state=%s slots=%u raw_slots=%u page_records=%u'
require_text kpm/r0lab_raw.h 'struct r0lab_raw_live_pte_snapshot'
require_text kpm/r0lab_raw.h 'r0lab_raw_snapshot_live_pte'
require_function_text kpm/r0lab_raw_compat.c r0lab_raw_snapshot_live_pte 'mmap_read_lock(mm);'
require_function_text kpm/r0lab_raw_compat.c r0lab_raw_snapshot_live_pte 'r0lab_raw_walk_locked'
require_function_text kpm/r0lab_raw_compat.c r0lab_raw_snapshot_live_pte 'live_pte = READ_ONCE(*ptep);'
reject_function_text kpm/r0lab_raw_compat.c r0lab_raw_snapshot_live_pte 'r0lab_raw_replace_locked'
reject_function_text kpm/r0lab_raw_compat.c r0lab_raw_snapshot_live_pte 'flush_tlb'
reject_function_text kpm/r0lab_raw_compat.c r0lab_raw_snapshot_live_pte 'set_pte_at'
reject_function_text kpm/r0lab_raw_compat.c r0lab_raw_snapshot_live_pte 'memset'
require_text kpm/r0lab.c 'raw slot live pte '
require_text kpm/r0lab.c 'snapshot_stage=after_arm walk_rc=%d'
require_text kpm/r0lab.c 'page->record.backend == record_backend'
require_text kpm/r0lab.c 'page->record.state == record_state'
require_text kpm/r0lab.c 'record_backend=%s record_state=%s record_match=%u'
require_text lab-app/src/main/cpp/labprobe.c 'r0lab_raw_hold_live_pte'
require_text lab-app/src/main/cpp/labprobe.c 'raw raw-hold live-pte '
require_text lab-app/src/main/cpp/labprobe.c 'raw mode=raw-hold-live-pte failures=%d'
require_text scripts/test_raw_live_pte_snapshot_device.sh 'RAW_LIVE_PTE_SNAPSHOT_IDLE_SECONDS'
require_text scripts/test_raw_live_pte_snapshot_device.sh 'raw raw-hold live-pte $TOKEN'
require_text scripts/test_raw_live_pte_snapshot_device.sh 'D4-R3e-L2-live-pte-mismatch-before-idle'
require_text scripts/test_raw_live_pte_snapshot_device.sh 'D4-R3e-L2-live-pte-walk-failed'
require_text scripts/test_raw_live_pte_snapshot_device.sh 'D4-R3e-L2-live-pte-match-then-unstable'
require_text scripts/test_raw_live_pte_snapshot_device.sh 'D4-R3e-L2-live-pte-stable'
require_text scripts/test_raw_live_pte_snapshot_device.sh 'post_hold_status=not_used boot_id_reader=not_used proc_maps=not_used'
require_function_text scripts/test_raw_live_pte_snapshot_device.sh poll_adb_transport 'adb_device get-state'
require_function_text scripts/test_raw_live_pte_snapshot_device.sh poll_adb_transport 'preserve_active_hold 0 D4-R3e-L2-live-pte-stable'
reject_function_text scripts/test_raw_live_pte_snapshot_device.sh poll_adb_transport 'run_app_command'
reject_function_text scripts/test_raw_live_pte_snapshot_device.sh poll_adb_transport 'capture_pstore'
reject_function_text scripts/test_raw_live_pte_snapshot_device.sh poll_adb_transport 'wait-for-device'
reject_function_text scripts/test_raw_live_pte_snapshot_device.sh poll_adb_transport '/proc/'
reject_function_text scripts/test_raw_live_pte_snapshot_device.sh poll_adb_transport 'getprop'
reject_function_text scripts/test_raw_live_pte_snapshot_device.sh poll_adb_transport 'raw slot clear'
require_text scripts/test_raw_observer_perturbation_device.sh 'RAW_OBSERVER_CLEAN_BOOT_CONFIRMED'
require_text scripts/test_raw_observer_perturbation_device.sh 'RAW_OBSERVER_PAIRED_RUN'
require_text scripts/test_raw_observer_perturbation_device.sh 'git -C "$ROOT" status --porcelain --untracked-files=no'
require_text scripts/test_raw_observer_perturbation_device.sh "RAW_COMMAND='raw raw-hold lifetime source single'"
require_text scripts/test_raw_observer_perturbation_device.sh "RAW_COMMAND='raw raw-hold live-pte'"
require_text scripts/test_raw_observer_perturbation_device.sh 'D4-R3e-L3-B0-baseline-stable'
require_text scripts/test_raw_observer_perturbation_device.sh 'D4-R3e-L3-B0-baseline-unstable'
require_text scripts/test_raw_observer_perturbation_device.sh 'D4-R3e-L3-B1-external-snapshot-stable'
require_text scripts/test_raw_observer_perturbation_device.sh 'D4-R3e-L3-B1-external-snapshot-unstable'
require_text scripts/test_raw_observer_perturbation_device.sh 'D4-R3e-L3-B1-live-pte-walk-failed'
require_text scripts/test_raw_observer_perturbation_device.sh 'D4-R3e-L3-B1-live-pte-mismatch'
require_text scripts/test_raw_observer_perturbation_device.sh 'D4-R3e-L3-setup-blocked'
require_text scripts/test_raw_observer_perturbation_device.sh 'post_hold_status=not_used boot_id_reader=not_used proc_maps=not_used'
require_text scripts/test_raw_observer_perturbation_device.sh 'preserving module: active raw hold remains and cleanup reentry is forbidden'
require_function_text scripts/test_raw_observer_perturbation_device.sh cleanup 'if [ "$HOLD_ACTIVE" -eq 1 ]; then'
require_function_text scripts/test_raw_observer_perturbation_device.sh poll_adb_transport 'adb_device get-state'
require_function_text scripts/test_raw_observer_perturbation_device.sh poll_adb_transport 'preserve_active_hold 1 "$UNSTABLE_CLASSIFICATION"'
require_function_text scripts/test_raw_observer_perturbation_device.sh poll_adb_transport 'preserve_active_hold 0 "$STABLE_CLASSIFICATION"'
reject_function_text scripts/test_raw_observer_perturbation_device.sh poll_adb_transport 'run_app_command'
reject_function_text scripts/test_raw_observer_perturbation_device.sh poll_adb_transport 'supercmd'
reject_function_text scripts/test_raw_observer_perturbation_device.sh poll_adb_transport 'capture_pstore'
reject_function_text scripts/test_raw_observer_perturbation_device.sh poll_adb_transport 'wait-for-device'
reject_function_text scripts/test_raw_observer_perturbation_device.sh poll_adb_transport '/proc/'
reject_function_text scripts/test_raw_observer_perturbation_device.sh poll_adb_transport 'getprop'
reject_function_text scripts/test_raw_observer_perturbation_device.sh poll_adb_transport 'raw slot clear'
reject_function_text scripts/test_raw_observer_perturbation_device.sh poll_adb_transport 'module unload'
require_line_before scripts/test_raw_observer_perturbation_device.sh \
  'case "$CLEAN_BOOT_CONFIRMED" in' 'ensure_clean_source'
require_line_before scripts/test_raw_observer_perturbation_device.sh \
  'case "$PAIRED_RUN" in' 'ensure_clean_source'
require_line_before scripts/test_raw_observer_perturbation_device.sh \
  'ensure_clean_source' 'EXISTING=$(supercmd module list 2>&1) ||'
require_text kpm/r0lab.c 'bool abort_hook_suppressed;'
require_text kpm/r0lab.c 'raw slot arm no-abort '
require_function_text kpm/r0lab.c r0lab_raw_arm_worker \
  'if (!page->abort_hook_suppressed)'
require_function_text kpm/r0lab.c r0lab_raw_arm_worker \
  'r0lab_raw_abort_hook_acquire(page)'
require_function_text kpm/r0lab.c r0lab_raw_arm_worker \
  'r0lab_raw_arm_source_uxn(&page->raw)'
require_function_text kpm/r0lab.c r0lab_raw_slot_arm \
  'if (suppress_abort_hook || passthrough_abort_hook ||'
require_function_text kpm/r0lab.c r0lab_raw_slot_ready \
  'abort_hook_installed=%u abort_hook_suppressed=%u abort_hook_passthrough=%u'
require_text lab-app/src/main/cpp/labprobe.c 'r0lab_raw_hold_no_abort'
require_text lab-app/src/main/cpp/labprobe.c 'raw raw-hold no-abort '
require_text lab-app/src/main/cpp/labprobe.c 'raw mode=raw-hold-no-abort failures=%d'
require_text lab-app/src/main/cpp/labprobe.c \
  '"raw slot arm no-abort 0x%llx %u 0x%llx"'
require_text scripts/test_raw_abort_hook_exposure_device.sh \
  'RAW_ABORT_EXPOSURE_CLEAN_BOOT_CONFIRMED'
require_text scripts/test_raw_abort_hook_exposure_device.sh 'IDLE_SECONDS=15'
require_text scripts/test_raw_abort_hook_exposure_device.sh \
  'git -C "$ROOT" status --porcelain --untracked-files=no'
require_text scripts/test_raw_abort_hook_exposure_device.sh \
  'raw raw-hold no-abort $TOKEN'
require_text scripts/test_raw_abort_hook_exposure_device.sh \
  'abort_hook_installed=0'
require_text scripts/test_raw_abort_hook_exposure_device.sh \
  'abort_hook_suppressed=1'
require_text scripts/test_raw_abort_hook_exposure_device.sh \
  'D4-R3f-source-uxn-no-abort-stable'
require_text scripts/test_raw_abort_hook_exposure_device.sh \
  'D4-R3f-source-uxn-no-abort-unstable'
require_function_text scripts/test_raw_abort_hook_exposure_device.sh cleanup \
  'if [ "$HOLD_ACTIVE" -eq 1 ]; then'
require_function_text scripts/test_raw_abort_hook_exposure_device.sh \
  poll_adb_transport 'adb_device get-state'
require_function_text scripts/test_raw_abort_hook_exposure_device.sh \
  poll_adb_transport \
  'preserve_active_hold 1 D4-R3f-source-uxn-no-abort-unstable'
require_function_text scripts/test_raw_abort_hook_exposure_device.sh \
  poll_adb_transport \
  'preserve_active_hold 0 D4-R3f-source-uxn-no-abort-stable'
reject_function_text scripts/test_raw_abort_hook_exposure_device.sh \
  poll_adb_transport 'run_app_command'
reject_function_text scripts/test_raw_abort_hook_exposure_device.sh \
  poll_adb_transport 'supercmd'
reject_function_text scripts/test_raw_abort_hook_exposure_device.sh \
  poll_adb_transport 'capture_pstore'
reject_function_text scripts/test_raw_abort_hook_exposure_device.sh \
  poll_adb_transport 'wait-for-device'
reject_function_text scripts/test_raw_abort_hook_exposure_device.sh \
  poll_adb_transport '/proc/'
reject_function_text scripts/test_raw_abort_hook_exposure_device.sh \
  poll_adb_transport 'adb_device shell getprop'
reject_function_text scripts/test_raw_abort_hook_exposure_device.sh \
  poll_adb_transport 'raw slot clear'
reject_function_text scripts/test_raw_abort_hook_exposure_device.sh \
  poll_adb_transport 'module unload'
require_line_before scripts/test_raw_abort_hook_exposure_device.sh \
  'case "$CLEAN_BOOT_CONFIRMED" in' 'ensure_clean_source'
require_line_before scripts/test_raw_abort_hook_exposure_device.sh \
  'ensure_clean_source' 'EXISTING=$(supercmd module list 2>&1) ||'
require_text kpm/r0lab.c 'bool abort_hook_passthrough;'
require_text kpm/r0lab.c 'raw slot arm abort-passthrough '
require_function_text kpm/r0lab.c r0lab_raw_before_abort_passthrough \
  '(void)args;'
require_function_text kpm/r0lab.c r0lab_raw_before_abort_passthrough \
  '(void)udata;'
reject_function_text kpm/r0lab.c r0lab_raw_before_abort_passthrough \
  'args->'
reject_function_text kpm/r0lab.c r0lab_raw_before_abort_passthrough \
  'r0lab_lock'
reject_function_text kpm/r0lab.c r0lab_raw_before_abort_passthrough \
  'g_get_task_mm'
reject_function_text kpm/r0lab.c r0lab_raw_before_abort_passthrough \
  'g_raw_inflight'
reject_function_text kpm/r0lab.c r0lab_raw_before_abort_passthrough \
  'r0lab_record'
reject_function_text kpm/r0lab.c r0lab_raw_before_abort_passthrough \
  'skip_origin'
require_function_text kpm/r0lab.c r0lab_raw_abort_hook_acquire \
  'callback = r0lab_raw_abort_hook_callback('
require_function_text kpm/r0lab.c r0lab_raw_abort_hook_acquire \
  'result = hook_wrap3(g_do_mem_abort, callback, NULL, NULL);'
require_function_text kpm/r0lab.c r0lab_raw_abort_hook_release \
  'callback = r0lab_raw_abort_hook_callback('
require_function_text kpm/r0lab.c r0lab_raw_slot_arm \
  'active->abort_hook_passthrough'
require_function_text kpm/r0lab.c r0lab_raw_slot_ready \
  'abort_hook_passthrough = page->abort_hook_passthrough;'
require_line_before kpm/r0lab.c \
  '    if (!strncmp(args, "raw slot arm abort-passthrough ", 31)) {' \
  '    if (!strncmp(args, "raw slot arm ", 13)) {'
require_text lab-app/src/main/cpp/labprobe.c \
  'r0lab_raw_hold_abort_passthrough'
require_text lab-app/src/main/cpp/labprobe.c \
  'raw raw-hold abort-passthrough '
require_text lab-app/src/main/cpp/labprobe.c \
  'raw mode=raw-hold-abort-passthrough failures=%d'
require_text lab-app/src/main/cpp/labprobe.c \
  '"raw slot arm abort-passthrough 0x%llx %u 0x%llx"'
require_text lab-app/src/main/cpp/labprobe.c \
  'abort_hook_passthrough[index] != 1'
require_line_before lab-app/src/main/cpp/labprobe.c \
  '    if (!strncmp(args, "raw raw-hold no-abort ", 22)) {' \
  '    if (!strncmp(args, "raw raw-hold abort-passthrough ", 31)) {'
require_text scripts/test_raw_abort_wrapper_passthrough_device.sh \
  'RAW_ABORT_PASSTHROUGH_CLEAN_BOOT_CONFIRMED'
require_text scripts/test_raw_abort_wrapper_passthrough_device.sh \
  'IDLE_SECONDS=15'
require_text scripts/test_raw_abort_wrapper_passthrough_device.sh \
  'git -C "$ROOT" status --porcelain --untracked-files=no'
require_text scripts/test_raw_abort_wrapper_passthrough_device.sh \
  'raw raw-hold abort-passthrough $TOKEN'
require_text scripts/test_raw_abort_wrapper_passthrough_device.sh \
  'abort_hook_installed=1'
require_text scripts/test_raw_abort_wrapper_passthrough_device.sh \
  'abort_hook_suppressed=0'
require_text scripts/test_raw_abort_wrapper_passthrough_device.sh \
  'abort_hook_passthrough=1'
require_text scripts/test_raw_abort_wrapper_passthrough_device.sh \
  'D4-R3g-source-uxn-abort-passthrough-stable'
require_text scripts/test_raw_abort_wrapper_passthrough_device.sh \
  'D4-R3g-source-uxn-abort-passthrough-unstable'
require_function_text scripts/test_raw_abort_wrapper_passthrough_device.sh \
  cleanup 'if [ "$HOLD_ACTIVE" -eq 1 ]; then'
require_function_text scripts/test_raw_abort_wrapper_passthrough_device.sh \
  poll_adb_transport 'adb_device get-state'
require_function_text scripts/test_raw_abort_wrapper_passthrough_device.sh \
  poll_adb_transport \
  'preserve_active_hold 1 D4-R3g-source-uxn-abort-passthrough-unstable'
require_function_text scripts/test_raw_abort_wrapper_passthrough_device.sh \
  poll_adb_transport \
  'preserve_active_hold 0 D4-R3g-source-uxn-abort-passthrough-stable'
reject_function_text scripts/test_raw_abort_wrapper_passthrough_device.sh \
  poll_adb_transport 'run_app_command'
reject_function_text scripts/test_raw_abort_wrapper_passthrough_device.sh \
  poll_adb_transport 'supercmd'
reject_function_text scripts/test_raw_abort_wrapper_passthrough_device.sh \
  poll_adb_transport 'capture_pstore'
reject_function_text scripts/test_raw_abort_wrapper_passthrough_device.sh \
  poll_adb_transport 'wait-for-device'
reject_function_text scripts/test_raw_abort_wrapper_passthrough_device.sh \
  poll_adb_transport '/proc/'
reject_function_text scripts/test_raw_abort_wrapper_passthrough_device.sh \
  poll_adb_transport 'adb_device shell getprop'
reject_function_text scripts/test_raw_abort_wrapper_passthrough_device.sh \
  poll_adb_transport 'raw slot clear'
reject_function_text scripts/test_raw_abort_wrapper_passthrough_device.sh \
  poll_adb_transport 'module unload'
require_line_before scripts/test_raw_abort_wrapper_passthrough_device.sh \
  'case "$CLEAN_BOOT_CONFIRMED" in' 'ensure_clean_source'
require_line_before scripts/test_raw_abort_wrapper_passthrough_device.sh \
  'ensure_clean_source' 'EXISTING=$(supercmd module list 2>&1) ||'
require_function_sha256 kpm/r0lab.c r0lab_raw_before_abort \
  dfa2fef78a90a38c427a82041164acbfa3fab5b00b594976edd09d965f0fea61
require_function_sha256 kpm/r0lab.c r0lab_raw_arm_worker \
  874f8f12422ff744204799582b402d2541eab4e5447662f0a1eb4c377ad6e9a2
require_file_sha256 kpm/r0lab_raw_compat.c \
  698a9b36674408d4cf1cfb5bf4b0b69eaf1f879f27851dd0633f97e634e9bdb7
require_file_sha256 kpm/r0lab_raw.h \
  b8340cd245e26cdd2595231e7574e9910377eba99743670e318deeba1714d479
require_text kpm/r0lab.c 'bool abort_hook_mmget;'
require_text kpm/r0lab.c 'raw slot arm abort-mmget '
require_function_text kpm/r0lab.c \
  r0lab_raw_before_abort_mmget_passthrough '(void)args;'
require_function_text kpm/r0lab.c \
  r0lab_raw_before_abort_mmget_passthrough '(void)udata;'
require_function_text kpm/r0lab.c \
  r0lab_raw_before_abort_mmget_passthrough \
  'current_mm = g_get_task_mm(current);'
require_function_text kpm/r0lab.c \
  r0lab_raw_before_abort_mmget_passthrough 'g_mmput(current_mm);'
require_function_count kpm/r0lab.c \
  r0lab_raw_before_abort_mmget_passthrough 'g_get_task_mm(' 1
require_function_count kpm/r0lab.c \
  r0lab_raw_before_abort_mmget_passthrough 'g_mmput(' 1
reject_function_text kpm/r0lab.c \
  r0lab_raw_before_abort_mmget_passthrough 'args->'
reject_function_text kpm/r0lab.c \
  r0lab_raw_before_abort_mmget_passthrough 'udata->'
reject_function_text kpm/r0lab.c \
  r0lab_raw_before_abort_mmget_passthrough 'g_session'
reject_function_text kpm/r0lab.c \
  r0lab_raw_before_abort_mmget_passthrough 'r0lab_current_tgid'
reject_function_text kpm/r0lab.c \
  r0lab_raw_before_abort_mmget_passthrough 'r0lab_lock'
reject_function_text kpm/r0lab.c \
  r0lab_raw_before_abort_mmget_passthrough 'g_raw_inflight'
reject_function_text kpm/r0lab.c \
  r0lab_raw_before_abort_mmget_passthrough 'r0lab_record'
reject_function_text kpm/r0lab.c \
  r0lab_raw_before_abort_mmget_passthrough 'route'
reject_function_text kpm/r0lab.c \
  r0lab_raw_before_abort_mmget_passthrough 'skip_origin'
reject_function_text kpm/r0lab.c \
  r0lab_raw_before_abort_mmget_passthrough 'pte'
reject_function_text kpm/r0lab.c \
  r0lab_raw_before_abort_mmget_passthrough 'tlb'
reject_function_text kpm/r0lab.c \
  r0lab_raw_before_abort_mmget_passthrough 'cache'
require_function_text kpm/r0lab.c r0lab_raw_abort_hook_callback \
  'if (mmget)'
require_function_text kpm/r0lab.c r0lab_raw_abort_hook_callback \
  'return r0lab_raw_before_abort_mmget_passthrough;'
require_function_text kpm/r0lab.c r0lab_raw_abort_hook_callback \
  'if (passthrough)'
require_function_text kpm/r0lab.c r0lab_raw_abort_hook_callback \
  'return r0lab_raw_before_abort_passthrough;'
require_function_text kpm/r0lab.c r0lab_raw_abort_hook_acquire \
  'page->abort_hook_passthrough, page->abort_hook_mmget,'
require_function_text kpm/r0lab.c r0lab_raw_abort_hook_acquire \
  'page->hook_installed = true;'
require_function_text kpm/r0lab.c r0lab_raw_abort_hook_acquire \
  'r0lab_hook_detach(g_do_mem_abort, callback, NULL);'
require_function_text kpm/r0lab.c r0lab_raw_abort_hook_release \
  'mmget = page->abort_hook_mmget;'
require_function_text kpm/r0lab.c r0lab_raw_abort_hook_release \
  'passthrough = page->abort_hook_passthrough;'
require_function_text kpm/r0lab.c r0lab_raw_abort_hook_release \
  'page->hook_installed = false;'
require_function_text kpm/r0lab.c r0lab_raw_abort_hook_release \
  'r0lab_hook_detach(g_do_mem_abort, callback, NULL);'
require_function_text kpm/r0lab.c r0lab_raw_slot_arm \
  '(mmget_abort_hook ? 1U : 0U)'
require_function_text kpm/r0lab.c r0lab_raw_slot_arm \
  'active->abort_hook_mmget'
require_function_text kpm/r0lab.c r0lab_raw_slot_arm \
  'slot->abort_hook_mmget = mmget_abort_hook;'
require_function_text kpm/r0lab.c r0lab_raw_slot_ready \
  'abort_hook_mmget = page->abort_hook_mmget;'
require_function_text kpm/r0lab.c r0lab_raw_slot_ready \
  'abort_hook_passthrough=%u abort_hook_mmget=%u abort_hook_lock=%u'
require_line_before kpm/r0lab.c \
  '    if (!strncmp(args, "raw slot arm abort-mmget ", 25)) {' \
  '    if (!strncmp(args, "raw slot arm ", 13)) {'
require_text lab-app/src/main/cpp/labprobe.c 'r0lab_raw_hold_abort_mmget'
require_text lab-app/src/main/cpp/labprobe.c 'raw raw-hold abort-mmget '
require_text lab-app/src/main/cpp/labprobe.c \
  'raw mode=raw-hold-abort-mmget failures=%d'
require_text lab-app/src/main/cpp/labprobe.c \
  '"raw slot arm abort-mmget 0x%llx %u 0x%llx"'
require_function_text lab-app/src/main/cpp/labprobe.c \
  r0lab_raw_hold_lifetime_common \
  'error=abort-mmget hold requires source single'
require_function_text lab-app/src/main/cpp/labprobe.c \
  r0lab_raw_hold_lifetime_common \
  '(mmget_abort_hook ? 1U : 0U)'
require_function_text lab-app/src/main/cpp/labprobe.c \
  r0lab_raw_hold_lifetime_common \
  'abort_hook_mmget[index] != 1'
require_function_text lab-app/src/main/cpp/labprobe.c \
  r0lab_raw_hold_lifetime_common \
  'mmget_abort_hook ? 16 :'
require_line_before lab-app/src/main/cpp/labprobe.c \
  '    if (!strncmp(args, "raw raw-hold abort-mmget ", 25)) {' \
  '    if (!strncmp(args, "raw exit hook hold ", 19)) {'
require_text scripts/test_raw_abort_mmget_passthrough_device.sh \
  'RAW_ABORT_MMGET_CLEAN_BOOT_CONFIRMED'
require_text scripts/test_raw_abort_mmget_passthrough_device.sh \
  'IDLE_SECONDS=15'
require_text scripts/test_raw_abort_mmget_passthrough_device.sh \
  'git -C "$ROOT" status --porcelain --untracked-files=no'
require_text scripts/test_raw_abort_mmget_passthrough_device.sh \
  'raw raw-hold abort-mmget $TOKEN'
require_text scripts/test_raw_abort_mmget_passthrough_device.sh \
  'abort_hook_installed=1'
require_text scripts/test_raw_abort_mmget_passthrough_device.sh \
  'abort_hook_suppressed=0'
require_text scripts/test_raw_abort_mmget_passthrough_device.sh \
  'abort_hook_passthrough=0'
require_text scripts/test_raw_abort_mmget_passthrough_device.sh \
  'abort_hook_mmget=1'
require_text scripts/test_raw_abort_mmget_passthrough_device.sh \
  'D4-R3h-source-uxn-abort-mmget-stable'
require_text scripts/test_raw_abort_mmget_passthrough_device.sh \
  'D4-R3h-source-uxn-abort-mmget-unstable'
require_function_text scripts/test_raw_abort_mmget_passthrough_device.sh \
  cleanup 'if [ "$HOLD_ACTIVE" -eq 1 ]; then'
require_function_text scripts/test_raw_abort_mmget_passthrough_device.sh \
  poll_adb_transport 'adb_device get-state'
require_function_text scripts/test_raw_abort_mmget_passthrough_device.sh \
  poll_adb_transport \
  'preserve_active_hold 1 D4-R3h-source-uxn-abort-mmget-unstable'
require_function_text scripts/test_raw_abort_mmget_passthrough_device.sh \
  poll_adb_transport \
  'preserve_active_hold 0 D4-R3h-source-uxn-abort-mmget-stable'
reject_function_text scripts/test_raw_abort_mmget_passthrough_device.sh \
  poll_adb_transport 'run_app_command'
reject_function_text scripts/test_raw_abort_mmget_passthrough_device.sh \
  poll_adb_transport 'supercmd'
reject_function_text scripts/test_raw_abort_mmget_passthrough_device.sh \
  poll_adb_transport 'capture_pstore'
reject_function_text scripts/test_raw_abort_mmget_passthrough_device.sh \
  poll_adb_transport 'wait-for-device'
reject_function_text scripts/test_raw_abort_mmget_passthrough_device.sh \
  poll_adb_transport '/proc/'
reject_function_text scripts/test_raw_abort_mmget_passthrough_device.sh \
  poll_adb_transport 'adb_device shell getprop'
reject_function_text scripts/test_raw_abort_mmget_passthrough_device.sh \
  poll_adb_transport 'raw slot clear'
reject_function_text scripts/test_raw_abort_mmget_passthrough_device.sh \
  poll_adb_transport 'module unload'
require_line_before scripts/test_raw_abort_mmget_passthrough_device.sh \
  'case "$CLEAN_BOOT_CONFIRMED" in' 'ensure_clean_source'
require_line_before scripts/test_raw_abort_mmget_passthrough_device.sh \
  'ensure_clean_source' 'EXISTING=$(supercmd module list 2>&1) ||'
require_text scripts/classify_raw_observer_perturbation_evidence.sh \
  'RAW_OBSERVER_HISTORICAL_BASELINE_LOG'
require_text scripts/classify_raw_observer_perturbation_evidence.sh \
  'RAW_OBSERVER_HISTORICAL_EXTERNAL_LOG'
require_text scripts/classify_raw_observer_perturbation_evidence.sh \
  'raw-hold-lifetime-matrix-20260724-071931.log'
require_text scripts/classify_raw_observer_perturbation_evidence.sh \
  'raw-live-pte-snapshot-20260724-080322.log'
require_text scripts/classify_raw_observer_perturbation_evidence.sh \
  '[ "$#" -eq 4 ]'
require_text scripts/classify_raw_observer_perturbation_evidence.sh \
  '<B0-S2.log> <B1-S2.log> <B0-S3.log> <B1-S3.log>'
require_text scripts/classify_raw_observer_perturbation_evidence.sh \
  'D4-R3e-L1-single-source-uxn-unstable'
require_text scripts/classify_raw_observer_perturbation_evidence.sh \
  'D4-R3e-L2-live-pte-stable'
require_text scripts/classify_raw_observer_perturbation_evidence.sh \
  'require_text_before "$path" "$stable" "$blocked"'
require_text scripts/classify_raw_observer_perturbation_evidence.sh \
  'validate_new_log "$1" 1 2 baseline 0 d4_r3e_l3_b0_s2'
require_text scripts/classify_raw_observer_perturbation_evidence.sh \
  'validate_new_log "$2" 2 2 external 1 d4_r3e_l3_b1_s2'
require_text scripts/classify_raw_observer_perturbation_evidence.sh \
  'validate_new_log "$3" 3 3 baseline 0 d4_r3e_l3_b0_s3'
require_text scripts/classify_raw_observer_perturbation_evidence.sh \
  'validate_new_log "$4" 4 3 external 1 d4_r3e_l3_b1_s3'
require_text scripts/classify_raw_observer_perturbation_evidence.sh \
  'D4-R3e-L3-setup-blocked'
require_text scripts/classify_raw_observer_perturbation_evidence.sh \
  'D4-R3e-L3-B1-live-pte-walk-failed'
require_text scripts/classify_raw_observer_perturbation_evidence.sh \
  'D4-R3e-L3-B1-live-pte-mismatch'
require_text scripts/classify_raw_observer_perturbation_evidence.sh \
  'terminal evidence metadata mismatch'
require_text scripts/classify_raw_observer_perturbation_evidence.sh \
  'result=classified paired_run=$expected_run sample_index=$expected_sample variant=$expected_variant live_walk=$expected_live_walk active_hold=1 cleanup=not_run'
require_text scripts/classify_raw_observer_perturbation_evidence.sh \
  'majority_vote=not_used'
require_line_before scripts/classify_raw_observer_perturbation_evidence.sh \
  '  classification=D4-R3e-L3-observer-correlated' \
  '  classification=D4-R3e-L3-environment-drift'
require_line_before scripts/classify_raw_observer_perturbation_evidence.sh \
  '  classification=D4-R3e-L3-environment-drift' \
  '  classification=D4-R3e-L3-observer-not-correlated'
require_line_before scripts/classify_raw_observer_perturbation_evidence.sh \
  '  classification=D4-R3e-L3-observer-not-correlated' \
  '  classification=D4-R3e-L3-repeat-nondeterministic'
reject_text scripts/classify_raw_observer_perturbation_evidence.sh 'adb'
reject_text scripts/classify_raw_observer_perturbation_evidence.sh 'supercmd'
reject_text scripts/classify_raw_observer_perturbation_evidence.sh 'module load'
reject_text scripts/classify_raw_observer_perturbation_evidence.sh '/proc/'
require_text scripts/test_raw_observer_aggregate_host.sh \
  'run_valid_case correlated D4-R3e-L3-observer-correlated'
require_text scripts/test_raw_observer_aggregate_host.sh \
  'run_valid_case environment-drift D4-R3e-L3-environment-drift'
require_text scripts/test_raw_observer_aggregate_host.sh \
  'run_valid_case observer-not-correlated D4-R3e-L3-observer-not-correlated'
require_text scripts/test_raw_observer_aggregate_host.sh \
  'run_valid_case nondeterministic D4-R3e-L3-repeat-nondeterministic'
require_text scripts/test_raw_observer_aggregate_host.sh \
  "run_rejected_case malformed-order 'paired_run=1'"
require_text scripts/test_raw_observer_aggregate_host.sh \
  "run_rejected_case duplicate-terminal 'expected one evidence row'"
require_text scripts/test_raw_observer_aggregate_host.sh \
  "run_rejected_case non-sample 'rejected evidence is present'"
require_text scripts/test_raw_observer_aggregate_host.sh \
  'run_rejected_case terminal-metadata-mismatch'
require_text scripts/test_raw_observer_aggregate_host.sh \
  'real_historical=anchors-valid'
require_text scripts/test_raw_observer_aggregate_host.sh \
  'raw_observer_aggregate_host=pass valid_cases=4 rejected_cases=4 real_historical=%s result=pass'
reject_text scripts/test_raw_observer_aggregate_host.sh 'adb'
reject_text scripts/test_raw_observer_aggregate_host.sh 'supercmd'
reject_text scripts/test_raw_observer_aggregate_host.sh 'module load'
reject_text scripts/test_raw_observer_aggregate_host.sh '/proc/'
require_text scripts/test_raw_exit_hook_device.sh 'command_timeout command=%s wait_ms=10000'
require_text scripts/test_raw_exit_hook_device.sh '*"command=$command"*'
require_text scripts/test_raw_exit_hook_device.sh 'LC_ALL=C grep -F -- "$needle"'
require_text scripts/test_raw_exit_hook_device.sh 'LC_ALL=C grep -c'
require_text scripts/test_raw_exit_hook_routing_device.sh 'raw exit hook routing hold $TOKEN'
require_text scripts/test_raw_exit_hook_routing_device.sh 'raw mode=exit-hook-routing-hold failures=0'
require_text scripts/test_raw_exit_hook_routing_device.sh "require_contains \"\$STATUS_HELD\" 'raw_slots=2'"
require_text scripts/test_raw_exit_hook_routing_device.sh "require_contains \"\$STATUS_AFTER\" 'page_records=0'"
require_text scripts/test_raw_exit_hook_routing_device.sh "grep -c 'op=34 result=0'"
require_text scripts/test_raw_exit_hook_routing_device.sh "grep -c 'op=22 result=0'"
require_text scripts/test_raw_exit_hook_routing_device.sh 'raw_exit_hook_routing=pass route=exit_mmap page_record_routed=1'
require_text scripts/test_raw_exit_hook_routing_diagnostics_device.sh 'raw_exit_hook_routing_diagnostics=pass'
require_text scripts/test_raw_exit_hook_routing_diagnostics_device.sh 'force_stop_and_require_stable_boot'
require_text scripts/test_raw_exit_hook_routing_diagnostics_device.sh 'boot_id()'
require_text scripts/test_raw_exit_hook_routing_diagnostics_device.sh 'raw exit hook routing hold $TOKEN_HOLD'
require_text scripts/test_raw_exit_hook_routing_diagnostics_device.sh 'raw slot exit hook clear $TOKEN_HOLD 0 $GEN0'
require_text scripts/test_raw_exit_hook_routing_diagnostics_device.sh 'raw slot clear $TOKEN_HOLD 0'
require_text scripts/test_raw_exit_hook_routing_diagnostics_device.sh 'exit_mmap_owner_cleanup=not_exercised'
require_text scripts/test_raw_exit_hook_routing_diagnostics_device.sh 'target_exit_event=not_required_without_page_records'
require_text scripts/test_raw_exit_hook_routing_diagnostics_device.sh 'RAW_EXIT_HOOK_DIAG_ALLOW_D4_R1_RERUN'
require_text scripts/test_raw_exit_hook_routing_diagnostics_device.sh 'D4-R1-explicit-clear-cleanup-reentry-panic'
require_text scripts/test_raw_exit_hook_cleanup_isolation_device.sh 'RAW_EXIT_HOOK_CLEANUP_ISOLATION_TOKEN'
require_text scripts/test_raw_exit_hook_cleanup_isolation_device.sh 'raw exit hook routing hold $TOKEN'
require_text scripts/test_raw_exit_hook_cleanup_isolation_device.sh 'raw slot exit hook clear $TOKEN 0 $GEN0'
require_text scripts/test_raw_exit_hook_cleanup_isolation_device.sh 'raw slot clear $TOKEN 0'
require_text scripts/test_raw_exit_hook_cleanup_isolation_device.sh 'phase=d4_r2_slot0_clear_non_reentrant result=pass'
require_text scripts/test_raw_exit_hook_cleanup_isolation_device.sh 'raw_exit_hook_cleanup_isolation=pass phases=4'
require_text scripts/test_raw_exit_hook_cleanup_isolation_device.sh 'cleanup reentry is forbidden'
require_text scripts/test_raw_exit_hook_cleanup_isolation_device.sh 'require_boot_id()'
require_text scripts/test_raw_exit_hook_cleanup_isolation_device.sh 'wait_for_boot_completed()'
require_text scripts/test_raw_exit_hook_cleanup_isolation_device.sh 'BOOT_BEFORE_EXIT_CLEAR=$(require_boot_id d4_r2_exit_hook_clear_only)'
require_text scripts/test_raw_exit_hook_preclear_hold_split_device.sh 'RAW_EXIT_HOOK_PRECLEAR_SPLIT_TOKEN'
require_text scripts/test_raw_exit_hook_preclear_hold_split_device.sh 'RAW_EXIT_HOOK_PRECLEAR_SPLIT_PROC_MAPS'
require_text scripts/test_raw_exit_hook_preclear_hold_split_device.sh 'extract_boot_uuid()'
require_text scripts/test_raw_exit_hook_preclear_hold_split_device.sh 'raw exit hook routing hold $TOKEN'
require_text scripts/test_raw_exit_hook_preclear_hold_split_device.sh 'phase=d4_r3a_hold_quiet_no_boot_reader result=pass boot_id_reader=not_used'
require_text scripts/test_raw_exit_hook_preclear_hold_split_device.sh 'phase=d4_r3a_boot_id_reader result=pass boot_stable=1'
require_text scripts/test_raw_exit_hook_preclear_hold_split_device.sh 'raw_exit_hook_preclear_hold_split=pass phases=5'
require_text scripts/test_raw_exit_hook_preclear_hold_split_device.sh 'boot_after_uuid=%s'
require_text scripts/test_raw_exit_hook_preclear_hold_split_device.sh 'active hold state remains and cleanup reentry is forbidden'
require_text kpm/r0lab.c 'enum r0lab_raw_hook_kind'
require_text kpm/r0lab.c 'enum r0lab_raw_hook_route_flags'
require_text kpm/r0lab.c 'R0LAB_RAW_HOOK_ROUTE_MUTATING'
require_text kpm/r0lab.c 'R0LAB_RAW_HOOK_ROUTE_REQUIRE_SHADOW_RX'
require_text kpm/r0lab.c 'struct r0lab_raw_hook_route_stats'
require_text kpm/r0lab.c 'struct r0lab_raw_hook_page_token'
require_text kpm/r0lab.c 'hook_route_stats'
require_text kpm/r0lab.c 'hook_route_miss_stats'
require_text kpm/r0lab.c 'r0lab_raw_page_find_by_mm_addr_locked'
require_text kpm/r0lab.c 'r0lab_raw_page_find_by_fault_locked'
require_text kpm/r0lab.c 'r0lab_raw_page_find_for_hook_locked'
require_text kpm/r0lab.c 'r0lab_raw_hook_page_token_acquire_locked'
require_text kpm/r0lab.c 'r0lab_raw_hook_page_token_release_locked'
require_text kpm/r0lab.c 'r0lab_raw_fault_hook_users_locked'
require_text kpm/r0lab.c 'R0LAB_RAW_HOOK_FAULT, vma_mm, address'
require_text kpm/r0lab.c 'raw_hook_route_status slot=%u generation=%llu hook=all'
require_text kpm/r0lab.c 'route_helper_ready=1 page_record_routed=0'
require_text kpm/r0lab.c 'table_route_outside_page=%u'
require_text kpm/r0lab.c 'migration=callbacks_slot0_compat'
require_text kpm/r0lab.c 'raw hook route status '
require_text "$FINAL_ROADMAP" 'Current status: implemented and gate-passed on Pixel 7'
require_text "$VERIFICATION" 'current 29-phase ordered acceptance run passed'
require_text "$FINAL_ROADMAP" 'Current status: F4.1 abort routing, F4.3 GUP routing, F4.4 fork routing, and'
require_text "$FINAL_ROADMAP" 'diagnostic/observe-only on this Pixel 7'
require_text "$FINAL_ROADMAP" 'handle_mm_fault_positive_blocked'
require_text "$FINAL_ROADMAP" 'F4.6-D4 first-smoke evidence is captured'
require_text "$FINAL_ROADMAP" 'D4-R1'
require_text "$FINAL_ROADMAP" 'D4-R2 explicit-clear cleanup'
require_text "$FINAL_ROADMAP" 'D4-R1 diagnostic decoupling'
require_text "$FINAL_ROADMAP" 'D4-R1-explicit-clear-cleanup-reentry-panic'
require_text "$FINAL_ROADMAP" 'D4-R2 explicit-clear cleanup isolation'
require_text "$FINAL_ROADMAP" 'scripts/test_raw_exit_hook_cleanup_isolation_device.sh'
require_text "$FINAL_ROADMAP" 'D4-R2-exit-hook-clear-only-panic'
require_text "$FINAL_ROADMAP" 'D4-R3-pre-clear-hold-state-init-sigsegv'
require_text "$FINAL_ROADMAP" 'docs/wxshadow-f4.6-d4-r3a-diagnostic-split-plan.md'
require_text "$FINAL_ROADMAP" 'scripts/test_raw_exit_hook_preclear_hold_split_device.sh'
require_text "$FINAL_ROADMAP" 'D4-R3a-boot-id-reader-unstable'
require_text "$FINAL_ROADMAP" 'D4-R3b-post-hold-status-reader-unstable'
require_text "$FINAL_ROADMAP" 'docs/wxshadow-f4.6-d4-r3b-raw-hold-split-plan.md'
require_text "$FINAL_ROADMAP" 'docs/wxshadow-f4.6-d4-r3c-status-reader-split-plan.md'
require_text "$FINAL_ROADMAP" 'docs/wxshadow-f4.6-d4-r3d-status-transport-split-plan.md'
require_text "$FINAL_ROADMAP" 'docs/wxshadow-f4.6-d4-r3e-raw-hold-lifetime-plan.md'
require_text "$FINAL_ROADMAP" 'D4-R3c-status-logcat-timeout-kernel-panic'
require_text "$FINAL_ROADMAP" 'D4-R3d-raw-hold-self-unstable'
require_text "$FINAL_ROADMAP" 'scripts/test_raw_exit_hook_raw_hold_idle_device.sh'
require_text "$FINAL_ROADMAP" 'raw-exit-hook-raw-hold-idle-20260724-065137.log'
require_text "$FINAL_ROADMAP" 'raw raw-hold routing hold <token>'
require_text "$FINAL_ROADMAP" 'scripts/test_raw_exit_hook_raw_hold_split_device.sh'
require_text "$FINAL_ROADMAP" 'F4.6-D4-R3b'
require_text "$FINAL_ROADMAP" 'delayed `exit_mmap` wrapper'
require_text "$FINAL_ROADMAP" 'quiet hold, shell/getprop, optional Lab'
require_text "$FINAL_ROADMAP" 'D4-R3f kept the same `SOURCE_UXN` PTE path'
require_text "$FINAL_ROADMAP" 'same reader ladder without arming `exit_mmap`'
require_text "$FINAL_ROADMAP" 'D4-R3b-raw-hold-anchor-too-strong'
require_text "$FINAL_ROADMAP" 'exit_hook_installed=not_queried/not_queried'
require_text "$FINAL_ROADMAP" 'F4.5 syscall gate outcome'
require_text "$FINAL_ROADMAP" 'build/evidence/raw-syscall-hook-routing-20260724-033233.log'
require_text "$FINAL_ROADMAP" 'The active source checkpoint is F4.2a preflight, not callback migration.'
require_text "$FINAL_ROADMAP" 'The checkpoint follows the D0-D4 ladder'
require_text "$FINAL_ROADMAP" 'file-backed RX plus shadow-PTE access-flag clear'
require_text "$FINAL_ROADMAP" 'wxshadow-v2-f4-fork-routing-plan-20260724'
require_text "$FINAL_ROADMAP" 'update the plan or queue before changing source'
require_text "$VERIFICATION" 'F4 currently has the route-helper substrate and the first abort-routing slice.'
require_text "$VERIFICATION" 'docs/wxshadow-f4-hook-routing-by-page-record-plan.md'
require_text "$VERIFICATION" 'raw hook route status <token> <slot>'
require_text docs/kpm-compatibility-matrix.md 'Two-slot page-local patch records'
require_text "$VERIFICATION" 'raw_page_table_slots'
require_text docs/kpm-compatibility-matrix.md 'Raw page-table skeleton'
require_text "$CONTRACT" 'page_records'
require_text "$RAW_COMPAT" 'lab_two_pfn_pass'
require_text "$RAW_COMPAT" 'kpm_locked_target_mm_writer=proven'
require_text "$VERIFICATION" 'scripts/test_v1_device.sh'
require_text "$VERIFICATION" 'scripts/test_raw_device.sh'
require_text "$VERIFICATION" 'scripts/test_raw_page_table_device.sh'
require_text "$VERIFICATION" 'scripts/test_raw_read_cycle_device.sh'
require_text "$VERIFICATION" 'scripts/test_raw_syscall_read_cycle_device.sh'
require_text "$VERIFICATION" 'scripts/test_raw_prctl_patch_records_device.sh'
require_text "$VERIFICATION" 'scripts/test_raw_abort_probe_device.sh'
require_text "$VERIFICATION" 'scripts/test_raw_abort_read_cycle_device.sh'
require_text "$VERIFICATION" 'scripts/test_raw_abort_write_probe_device.sh'
require_text "$VERIFICATION" 'scripts/test_raw_abort_write_release_device.sh'
require_text "$VERIFICATION" 'scripts/test_raw_hook_routing_device.sh'
require_text "$VERIFICATION" 'scripts/test_raw_fault_hook_routing_device.sh'
require_text "$VERIFICATION" 'scripts/test_raw_fault_hook_positive_preflight_device.sh'
require_text "$VERIFICATION" 'scripts/test_raw_gup_hook_routing_device.sh'
require_text "$VERIFICATION" 'scripts/test_raw_fork_hook_routing_device.sh'
require_text "$VERIFICATION" 'docs/wxshadow-f4.3-gup-hook-routing-plan.md'
require_text "$VERIFICATION" 'docs/wxshadow-f4.4-fork-hook-routing-plan.md'
require_text "$VERIFICATION" 'two `op=31` begin events and two `op=32` finish'
require_text "$VERIFICATION" 'scripts/test_s4_raw_reg_device.sh'
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
require_text "$S4_PLAN" 'arbitrary register/value mutation API'
require_text "$S4_PLAN" 'S4_BRK_OBSERVED'
require_text docs/kpm-research-plan.md 'M0-M5 plus raw two-PFN are complete for this pinned device and Lab App.'
require_text docs/kpm-compatibility-matrix.md 'Trusting `.kpm.exit` to block FolkPatch unload'
require_text docs/kpm-compatibility-matrix.md 'reference only, not installed-binary provenance'
require_text docs/kpm-research-plan.md '[the FolkPatch source reference](folkpatch-runtime-reference.md)'
require_text "$REFERENCE_REVIEW" '141 annotated'
require_text "$REFERENCE_REVIEW" 'fixed two-slot Lab page table'
require_text "$REFERENCE_REVIEW" '1024 page-local patch records per armed slot'
require_text "$REFERENCE_REVIEW" \
  'Completed for the fixed two-slot Lab boundary.'
require_text "$REFERENCE_REVIEW" 'Runtime structure-offset scanning and raw TLB fallbacks'
require_text "$REFERENCE_REVIEW" 'scripts/verify_wxshadow_reference_source.sh'
require_text "$REFERENCE_REVIEW" 'scripts/verify_wxshadow_reference_inventory.sh'
require_text scripts/verify_wxshadow_reference_source.sh 'EXPECTED_LINES=7124'
require_text scripts/verify_wxshadow_reference_source.sh 'EXPECTED_FUNCTION_BLOCKS=141'
require_text scripts/verify_wxshadow_reference_source.sh '2b2fb7ade7e572fd5aea8a79f39bd9c743b1ee1dc57552449209612e69903191'
require_text "$REFERENCE_COVERAGE" 'annotated function count: 141'
require_text "$REFERENCE_COVERAGE" '`fork-exit-routing`'
require_text "$REFERENCE_COVERAGE" 'No F5,'
require_text "$REFERENCE_INVENTORY" 'shadow_page_switch_mapping|pte-transaction'
require_text "$REFERENCE_INVENTORY" 'exit_mmap_before_hook|fork-exit-routing'
require_text "$REFERENCE_INVENTORY" 'scan_mm_struct_offsets|layout-scanning'
require_text scripts/verify_wxshadow_reference_inventory.sh 'EXPECTED_FUNCTIONS=141'
require_text scripts/verify_wxshadow_reference_inventory.sh 'EXPECTED_FAMILIES=14'
require_text scripts/test_wxshadow_reference_inventory_host.sh 'duplicate-function'
require_text scripts/test_wxshadow_reference_inventory_host.sh 'missing-function'
require_text scripts/test_wxshadow_reference_inventory_host.sh 'unknown-family'
require_text scripts/test_wxshadow_reference_inventory_host.sh 'missing-matrix-family'
require_text "$REPLICA_PLAN" 'two fixed anonymous'
require_text "$REPLICA_PLAN" \
  "R3o's source-tagged two-slot natural owner-exit path"
require_text "$REPLICA_PLAN" 'exact 141-function review coverage'
require_text "$FINAL_ROADMAP" '26 of 27 items executed'
require_text "$FINAL_ROADMAP" 'progress, not release completion'
require_text "$FINAL_ROADMAP" 'Reference Completeness Gate'
require_text "$FINAL_ROADMAP" 'all 141 annotated reference functions'
require_text "$FINAL_ROADMAP" 'two fixed Lab-owned raw shadow pages'
require_text "$FINAL_ROADMAP" \
  'F4.7 integration is now complete'
require_text "$FINAL_ROADMAP" \
  'F5 must now choose the final hidden-read model'
require_text "$FINAL_ROADMAP" \
  'F5 selects the controlled translation-DABT read-cycle route as the final Lab'
require_text "$FINAL_ROADMAP" \
  'Raw-XOM, permission-fault hidden read, and `PTE_USER` clearing remain blocked'
require_text "$FINAL_ROADMAP" \
  'scripts/probe_raw_xom_device.sh'
require_text "$FINAL_ROADMAP" \
  'scripts/probe_shadow_page_capabilities_device.sh'
require_text "$FINAL_ROADMAP" \
  'scripts/test_raw_abort_probe_device.sh'
require_text "$FINAL_ROADMAP" \
  'scripts/test_raw_abort_read_cycle_device.sh'
require_text "$F5_DECISION_PLAN" 'wxshadow F5 Hidden-Read Decision Plan'
require_text "$F5_DECISION_PLAN" \
  'Status: F5 decision gate selected and device-verified'
require_text "$F5_DECISION_PLAN" \
  'F5 selects the controlled translation-DABT read-cycle route as the final Lab'
require_text "$F5_DECISION_PLAN" \
  'build/evidence/raw-xom-device-20260724-204923.log'
require_text "$F5_DECISION_PLAN" \
  'build/evidence/shadow-page-capabilities-20260724-204927.log'
require_text "$F5_DECISION_PLAN" \
  'build/evidence/raw-abort-probe-20260724-204942.log'
require_text "$F5_DECISION_PLAN" \
  'build/evidence/raw-abort-read-cycle-20260724-205015.log'
require_text "$F5_DECISION_PLAN" \
  'de505ef582843c7de9597678930394351b9e2df0032709208b24d6d7486e449a'
require_text "$F5_DECISION_PLAN" \
  'The final artifact must not say:'
require_text "$F5_DECISION_PLAN" \
  'arbitrary reads are hidden'
require_text "$F5_DECISION_PLAN" \
  '`handle_mm_fault` positive read routing is implemented'
require_text "$F5_DECISION_PLAN" \
  'raw-XOM or `PTE_USER` clearing is supported'
require_text "$F5_DECISION_PLAN" \
  '`/proc`, ptrace, VMA walks, or arbitrary kernel/user readers are concealed'
require_text "$F5_DECISION_PLAN" \
  'ANDROID_SERIAL=32250DLH2000Z3 scripts/test_raw_abort_probe_device.sh'
require_text "$F5_DECISION_PLAN" \
  'ANDROID_SERIAL=32250DLH2000Z3 scripts/test_raw_abort_read_cycle_device.sh'
require_text "$F5_DECISION_PLAN" \
  'scripts/probe_raw_xom_device.sh'
require_text "$DEVELOPMENT_SEQUENCE" '| 30 | F5-D0 | Controlled hidden-read decision gate |'
require_text "$DEVELOPMENT_SEQUENCE" \
  'Complete/device-verified'
require_text "$DEVELOPMENT_SEQUENCE" \
  'F5-D0 selected controlled translation-DABT read-cycle as the Lab hidden-read'
require_text "$FINAL_ROADMAP" \
  'docs/wxshadow-f6-brk-step-descriptor-abi-plan.md'
require_text "$FINAL_ROADMAP" \
  'F6-D1 adds page-owned descriptor scaffold'
require_text "$FINAL_ROADMAP" \
  'F6-D3 two-slot descriptor routing is implemented and device-verified'
require_text "$FINAL_ROADMAP" \
  'build/evidence/s4-descriptor-routing-20260724-235331.log'
require_text "$FINAL_ROADMAP" \
  'hook1=99 hook0=99'
require_text "$F6_BRK_STEP_PLAN" \
  'wxshadow F6 BRK/Step Descriptor ABI Plan'
require_text "$F6_BRK_STEP_PLAN" \
  'Status: F6-D3 two-slot descriptor routing is implemented and device-verified.'
require_text "$F6_BRK_STEP_PLAN" \
  'docs/wxshadow-f6-d3-two-slot-descriptor-routing-plan.md'
require_text "$F6_D3_PLAN" \
  'wxshadow F6-D3 Two-Slot Descriptor Routing Plan'
require_text "$F6_D3_PLAN" \
  'Status: F6-D3-L/D implemented and device-verified.'
require_text "$F6_D3_PLAN" \
  'wxshadow-v2-f6-d2-slot0-descriptor-compat-20260724'
require_text "$F6_D3_PLAN" \
  'KPM command:     s4 descriptor routing arm <token> <page0> <page1>'
require_text "$F6_D3_PLAN" \
  'r0lab_s4_descriptor_find_brk_locked()'
require_text "$F6_D3_PLAN" \
  'r0lab_s4_descriptor_find_step_locked()'
require_text "$F6_D3_PLAN" \
  'PTE begin/finish must operate on the selected page, not on the slot-0 alias'
require_text "$F6_D3_PLAN" \
  'scripts/test_s4_descriptor_routing_device.sh'
require_text "$F6_D3_PLAN" \
  'page 1 can execute through its descriptor first and return `99`'
require_text "$F6_D3_PLAN" \
  'page 0 can execute through its descriptor second and return `99`'
require_text "$F6_D3_PLAN" \
  'descriptor step owner crossed slots'
require_tag_target wxshadow-v2-f6-d2-slot0-descriptor-compat-20260724 \
  e086c917db1f0707953b58d0ef5f5650bd82a830
require_text "$F6_BRK_STEP_PLAN" \
  'shadow_page_begin_stepping'
require_text "$F6_BRK_STEP_PLAN" \
  'shadow_do_set_reg'
require_text "$F6_BRK_STEP_PLAN" \
  'allowed register index: 1'
require_text "$F6_BRK_STEP_PLAN" \
  'allowed value: 73'
require_text "$F6_BRK_STEP_PLAN" \
  'No arbitrary register/value API is allowed in F6'
require_text "$F6_BRK_STEP_PLAN" \
  'F6-D3 | Two-slot descriptor routing'
require_text "$F6_BRK_STEP_PLAN" \
  'scripts/test_s4_descriptor_routing_device.sh'
require_text "$F6_BRK_STEP_PLAN" \
  'scripts/test_s4_descriptor_negative_device.sh'
require_text "$F6_BRK_STEP_PLAN" \
  'docs/wxshadow-f6-d4-negative-descriptor-controls-plan.md'
require_text "$F6_D4_PLAN" \
  'wxshadow F6-D4 Negative Descriptor Controls Plan'
require_text "$F6_D4_PLAN" \
  'Status: F6-D4-L/D implemented and device-verified.'
require_text "$F6_D4_PLAN" \
  'wxshadow-v2-f6-d3-two-slot-descriptor-routing-20260724'
require_text "$F6_D4_PLAN" \
  'KPM command:     s4 descriptor negative probe <token>'
require_text "$F6_D4_PLAN" \
  'The real descriptor BRK and step matchers must reject descriptors whose'
require_text "$F6_D4_PLAN" \
  'baseline_brk_matches=2 baseline_step_matches=2'
require_text "$F6_D4_PLAN" \
  'reject_checks=11 state_intact=1'
require_text "$F6_D4_PLAN" \
  'wrong_brk_slot_rejected=1'
require_text "$F6_D4_PLAN" \
  'bad_register_index_rejected=1'
require_text "$F6_D4_PLAN" \
  'wrong_step_tid_rejected=1'
require_text "$F6_D4_PLAN" \
  'build/evidence/s4-descriptor-negative-20260725-002527.log'
require_text "$F6_D4_PLAN" \
  's4_descriptor_negative=pass warn_after=3 final_modules=empty result=pass'
require_text "$F6_D4_PLAN" \
  'build/evidence/s4-descriptor-routing-20260725-001907.log'
require_text "$F6_BRK_STEP_PLAN" \
  'F6-D4 is now device-verified'
require_text "$F6_BRK_STEP_PLAN" \
  'build/evidence/s4-descriptor-negative-20260725-002527.log'
require_text "$FINAL_ROADMAP" \
  'F6-D4 evidence:'
require_text "$FINAL_ROADMAP" \
  'build/evidence/s4-descriptor-negative-20260725-002527.log'
require_text "$FINAL_ROADMAP" \
  'BRK/step/PTE side effects'
require_text "$F6_BRK_STEP_PLAN" \
  'F6-D5 is now device-verified with the targeted S4 refresh'
require_text "$F6_BRK_STEP_PLAN" \
  'build/evidence/s4-abi-20260725-003130.log'
require_text "$F6_BRK_STEP_PLAN" \
  'build/evidence/s4-descriptor-negative-20260725-003341.log'
require_text "$F6_BRK_STEP_PLAN" \
  'build/evidence/v1-device-20260724T163408Z/manifest.log'
require_text "$FINAL_ROADMAP" \
  'F6-D5 evidence:'
require_text "$FINAL_ROADMAP" \
  'build/evidence/s4-descriptor-routing-20260725-003319.log'
require_text "$FINAL_ROADMAP" \
  'build/evidence/v1-device-20260724T163408Z/manifest.log'
require_text "$DEVELOPMENT_SEQUENCE" '| 31 | F6-D0 | BRK/step descriptor ABI plan gate |'
require_text "$DEVELOPMENT_SEQUENCE" '| 32 | F6-D1 | BRK/step descriptor scaffold |'
require_text "$DEVELOPMENT_SEQUENCE" '| 33 | F6-D2 | Slot-0 S4 descriptor compatibility migration |'
require_text "$DEVELOPMENT_SEQUENCE" '| 34 | F6-D3 | Two-slot S4 descriptor routing |'
require_text "$DEVELOPMENT_SEQUENCE" '| 35 | F6-D4 | Negative descriptor controls |'
require_text "$DEVELOPMENT_SEQUENCE" '| 36 | F6-D5 | Targeted S4 and full-runner refresh |'
require_text "$DEVELOPMENT_SEQUENCE" \
  '29 `status=pass evidence=` phase rows'
require_text "$DEVELOPMENT_SEQUENCE" \
  'arbitrary register/value mutation remains rejected'
require_text "$REFERENCE_COVERAGE" \
  'F6-D2 is device-verified for routing the existing slot-0 raw-step/raw-reg callback admission'
require_text "$REFERENCE_COVERAGE" \
  'F6-D3 is device-verified for routing two Lab raw slots through independent descriptors'
require_text "$REFERENCE_COVERAGE" \
  'F6-D4 is device-verified for rejecting bad offsets'
require_text "$REFERENCE_COVERAGE" \
  'F6-D5 refreshed S4 ABI'
require_text "$KPM_COMPAT_MATRIX" \
  'F6-D5 full runner preserves `warn_count=3`'
require_text "$KPM_COMPAT_MATRIX" \
  'build/evidence/v1-device-20260724T163408Z/manifest.log'
require_text "$F7_STRESS_PLAN" \
  'wxshadow F7 Final Lifecycle And Stress Pass Plan'
require_text "$F7_STRESS_PLAN" \
  'Status: F7-D0 plan/contract gate.'
require_text "$F7_STRESS_PLAN" \
  'wxshadow-v2-f6-d5-full-refresh-20260725'
require_text "$F7_STRESS_PLAN" \
  'scripts/test_m5_faults_device.sh'
require_text "$F7_STRESS_PLAN" \
  'scripts/test_m5_lifecycle_device.sh'
require_text "$F7_STRESS_PLAN" \
  'scripts/test_v1_device.sh'
require_text "$F7_STRESS_PLAN" \
  'scripts/test_f7_lifecycle_stress_device.sh'
require_text "$F7_STRESS_PLAN" \
  'historical `scripts/test_raw_r3o_lifetime_device.sh` remains a source-tagged'
require_text "$F7_STRESS_PLAN" \
  'No F7 source patch may merge until the failure class is named'
require_text "$FINAL_ROADMAP" \
  'docs/wxshadow-f7-final-lifecycle-stress-plan.md'
require_text "$FINAL_ROADMAP" \
  'F7-D1 adds a current-HEAD lifecycle stress runner'
require_text "$DEVELOPMENT_SEQUENCE" '| 37 | F7-D0 | Final lifecycle stress plan gate |'
require_text "$DEVELOPMENT_SEQUENCE" \
  'not historical source-tagged R3o scripts'
require_text "$VERIFICATION" \
  'F6-D0 is the plan gate that turns the singleton S4 proof into a descriptor'
require_text "$VERIFICATION" \
  'F6-D1 is now device-verified as a descriptor'
require_text "$VERIFICATION" \
  's4_descriptor_active=1 s4_descriptor_state=armed_shadow'
require_text "$VERIFICATION" \
  'F6-D2 is the slot-0 compatibility migration'
require_text "$F6_BRK_STEP_PLAN" \
  'build/evidence/m5-lifecycle-20260724-210942.log'
require_text "$F6_BRK_STEP_PLAN" \
  'build/evidence/s4-abi-20260724-231424.log'
require_text "$F6_BRK_STEP_PLAN" \
  'build/evidence/s4-raw-step-20260724-231442.log'
require_text "$F6_BRK_STEP_PLAN" \
  'build/evidence/s4-raw-reg-20260724-231505.log'
require_text "$F6_BRK_STEP_PLAN" \
  'build/evidence/m5-lifecycle-20260724-231528.log'
require_text "$F6_BRK_STEP_PLAN" \
  'normal_value=42 hook_value=73 restored_value=42'
require_text "$F6_BRK_STEP_PLAN" \
  'reg_write_events=1 register_apply=brk_before_single_step'
require_text "$DEVELOPMENT_SEQUENCE" \
  'Complete/device-verified: the existing slot-0 raw-step/raw-reg BRK and single-step callback admission now routes through page-owned descriptor checks'
require_text "$VERIFICATION" \
  'F6-D2 is now device-verified with:'
require_text "$VERIFICATION" \
  'F6-D3 is now device-verified with:'
require_text "$VERIFICATION" \
  'F6-D4 is now device-verified with:'
require_text "$VERIFICATION" \
  'F6-D5 is now device-verified with a fresh targeted S4 refresh'
require_text "$VERIFICATION" \
  'build/evidence/s4-descriptor-routing-20260724-235331.log'
require_text "$VERIFICATION" \
  'build/evidence/s4-descriptor-negative-20260725-002527.log'
require_text "$VERIFICATION" \
  'build/evidence/s4-descriptor-negative-20260725-003341.log'
require_text "$VERIFICATION" \
  'build/evidence/v1-device-20260724T163408Z/manifest.log'
require_text "$VERIFICATION" \
  '29 status=pass evidence= phase rows'
require_text "$VERIFICATION" \
  'reject_checks=11 state_intact=1'
require_text "$VERIFICATION" \
  's4_descriptor_routing=pass warn_after=3 final_modules=empty result=pass'
require_text "$VERIFICATION" \
  'normal0=42 normal1=42 hook1=99 hook0=99 restored0=42 restored1=42'
require_text "$VERIFICATION" \
  'build/evidence/s4-raw-step-20260724-231442.log'
require_text "$VERIFICATION" \
  'build/evidence/s4-raw-reg-20260724-231505.log'
require_text "$VERIFICATION" \
  'This proves the slot-0 descriptor consumption path and owner-exit descriptor'
require_text "$DEVELOPMENT_SEQUENCE" \
  'BRK/single-step callbacks still do not route through descriptors'
require_text "$VERIFICATION" \
  'F5-D0 verification is a decision gate, not a source gate'
require_text "$VERIFICATION" \
  'does not prove arbitrary hidden reads'
require_text "$VERIFICATION" \
  'raw-abort-read-cycle-20260724-205015.log'
require_text "$RAW_PLAN" \
  'F5-D0 selects this controlled translation-DABT read-cycle as the final Lab'
require_text "$RAW_PLAN" \
  'not raw-XOM, not permission-fault'
require_text "$RAW_PLAN" \
  'raw-xom-device-20260724-204923.log'
require_text "$SHADOW_PLAN" \
  'controlled translation-DABT read-cycle smoke'
require_text "$SHADOW_PLAN" \
  'raw-XOM/permission-fault hidden read blocked'
require_text "$SHADOW_PLAN" \
  'controlled_dabt_read_cycle=separate_smoke scripts/test_raw_abort_read_cycle_device.sh'
require_text "$KPM_COMPAT_MATRIX" \
  'F5 selected hidden-read model'
require_text "$KPM_COMPAT_MATRIX" \
  'Raw-XOM, permission-fault hidden read, `PTE_USER` clearing'
require_text "$F6_BRK_STEP_PLAN" \
  'F6-D1 may edit only:'
require_text "$F6_BRK_STEP_PLAN" \
  'F6-D2 may edit only:'
require_text "$F6_BRK_STEP_PLAN" \
  'F6-D2 must keep compatibility scoped to slot 0'
require_text "$F6_BRK_STEP_PLAN" \
  'Plain `s4 brk` and `s4 step` remain singleton-driven'
require_text kpm/r0lab.c 'enum r0lab_s4_descriptor_state'
require_text kpm/r0lab.c 'enum r0lab_s4_descriptor_mode'
require_text kpm/r0lab.c 'struct r0lab_s4_descriptor'
require_text kpm/r0lab.c 'struct r0lab_s4_descriptor s4_descriptor;'
require_text kpm/r0lab.c 'r0lab_s4_descriptor_prepare_locked(&g_raw_page, raw_mm,'
require_text kpm/r0lab.c 'r0lab_s4_descriptor_brk_matches_locked'
require_text kpm/r0lab.c 'r0lab_s4_descriptor_step_matches_locked'
require_text kpm/r0lab.c 'uint32_t pte_begin_events;'
require_text kpm/r0lab.c 'uint32_t pte_finish_events;'
require_text kpm/r0lab.c 'r0lab_s4_descriptor_find_brk_locked'
require_text kpm/r0lab.c 'r0lab_s4_descriptor_find_step_locked'
require_text kpm/r0lab.c 'r0lab_s4_descriptor_has_armed_locked'
require_text kpm/r0lab.c 'r0lab_s4_descriptor_clear_step_tid_locked'
require_text kpm/r0lab.c 'r0lab_s4_restore_descriptor_pages'
require_text kpm/r0lab.c 'r0lab_s4_descriptor_routing_arm'
require_text kpm/r0lab.c 'r0lab_s4_descriptor_routing_observed'
require_text kpm/r0lab.c 'r0lab_s4_descriptor_routing_clear'
require_text kpm/r0lab.c 'r0lab_s4_descriptor_negative_probe'
require_text kpm/r0lab.c 'descriptor->slot_id != page->slot_id'
require_text kpm/r0lab.c 's4_descriptor_negative_observed'
require_text kpm/r0lab.c 'bad_register_index_rejected'
require_text kpm/r0lab.c 'wrong_step_tid_rejected'
require_text kpm/r0lab.c 's4 descriptor routing arm '
require_text kpm/r0lab.c 's4 descriptor routing observed '
require_text kpm/r0lab.c 's4 descriptor routing clear '
require_text kpm/r0lab.c 's4 descriptor negative probe '
require_function_text kpm/r0lab.c r0lab_s4_brk_before \
  'raw_page = r0lab_s4_descriptor_find_brk_locked(regs->pc, esr)'
require_function_text kpm/r0lab.c r0lab_s4_brk_before \
  'r0lab_raw_begin_stepping(&raw_page->raw)'
require_function_text kpm/r0lab.c r0lab_s4_brk_before \
  '++descriptor->pte_begin_events'
require_function_text kpm/r0lab.c r0lab_s4_brk_before \
  'R0LAB_S4_DESCRIPTOR_BRK_MATCHED_ORIGINAL_STEP'
require_function_text kpm/r0lab.c r0lab_s4_brk_before \
  'regs->regs[descriptor->register_index] = reg_value'
require_function_text kpm/r0lab.c r0lab_s4_step_before \
  'raw_page = r0lab_s4_descriptor_find_step_locked(regs->pc)'
require_function_text kpm/r0lab.c r0lab_s4_step_before \
  'r0lab_raw_finish_stepping(&raw_page->raw)'
require_function_text kpm/r0lab.c r0lab_s4_step_before \
  '++descriptor->pte_finish_events'
require_function_text kpm/r0lab.c r0lab_s4_step_before \
  'r0lab_s4_descriptor_has_armed_locked()'
require_function_text kpm/r0lab.c r0lab_s4_step_before \
  'R0LAB_S4_DESCRIPTOR_STEP_MATCHED_SHADOW'
require_function_text kpm/r0lab.c r0lab_s4_step_before \
  'descriptor->generation == raw_generation'
require_text kpm/r0lab.c 's4_descriptor_slots=%u s4_descriptor_active=%u'
require_text kpm/r0lab.c 'r0lab_s4_descriptor_state_name(s4_descriptor_state)'
require_text kpm/r0lab.c 'r0lab_s4_descriptor_mode_name(s4_descriptor_mode)'
require_text lab-app/src/main/cpp/labprobe.c 'r0lab_s4_descriptor_routing_run'
require_text lab-app/src/main/cpp/labprobe.c 'r0lab_s4_descriptor_negative_run'
require_text lab-app/src/main/cpp/labprobe.c \
  '"s4 descriptor routing arm 0x%llx 0x%llx 0x%llx"'
require_text lab-app/src/main/cpp/labprobe.c \
  '"s4 descriptor negative probe 0x%llx"'
require_text lab-app/src/main/cpp/labprobe.c \
  '"s4 descriptor routing observed 0x%llx"'
require_text lab-app/src/main/cpp/labprobe.c \
  '"s4 descriptor routing clear 0x%llx"'
require_text lab-app/src/main/cpp/labprobe.c 'hook1 = ((int (*)(void))page1)()'
require_text lab-app/src/main/cpp/labprobe.c 'hook0 = ((int (*)(void))page0)()'
require_text scripts/test_s4_descriptor_routing_device.sh \
  's4 descriptor routing $TOKEN'
require_text scripts/test_s4_descriptor_routing_device.sh \
  'slot0_pte_begin_events=1'
require_text scripts/test_s4_descriptor_routing_device.sh \
  'slot1_pte_finish_events=1'
require_text scripts/test_s4_descriptor_negative_device.sh \
  's4 descriptor negative $TOKEN'
require_text scripts/test_s4_descriptor_negative_device.sh \
  'reject_checks=11'
require_text scripts/test_s4_descriptor_negative_device.sh \
  'wrong_brk_slot_rejected=1'
require_text scripts/test_s4_descriptor_negative_device.sh \
  'bad_register_value_rejected=1'
require_text scripts/probe_shadow_page_capabilities_device.sh \
  'ordinary_xom_read_path=blocked reason=user_xom_read_fault_absent'
require_text scripts/probe_shadow_page_capabilities_device.sh \
  'raw_xom_permission_read_path=blocked reason=disabled_after_kernel_panic_requires_separate_preflight'
require_text scripts/probe_shadow_page_capabilities_device.sh \
  'controlled_dabt_read_cycle=separate_smoke scripts/test_raw_abort_read_cycle_device.sh'
require_text scripts/probe_shadow_page_capabilities_device.sh \
  'result=blocked reason=hardware_xom_route_unavailable'
require_text "$DEVELOPMENT_SEQUENCE" '| 29 | F4.7 | F4 integration gate |'
require_text "$DEVELOPMENT_SEQUENCE" \
  'fixed the exit-protected patch-record admission mismatch'
require_text "$DEVELOPMENT_SEQUENCE" 'read-only audit selected D4-R3g'
require_text "$DEVELOPMENT_SEQUENCE" '| 17 | D4-R3e-L3-A-D |'
require_text "$DEVELOPMENT_SEQUENCE" 'Four clean-boot device rows'
require_text "$DEVELOPMENT_SEQUENCE" 'accounts for all 141'
require_text "$FOLKPATCH_REFERENCE" 'https://github.com/LyraVoid/FolkPatch.git'
require_text "$FOLKPATCH_REFERENCE" '5da126b92af481bd226eb161183ddacdfadb3987'
require_text "$FOLKPATCH_REFERENCE" 'FolkPatch reports `50ac6,d01`'
require_text "$FOLKPATCH_REFERENCE" 'The source checkout is useful for control-path review, but its current'
require_text "$FOLKPATCH_REFERENCE" '`0x1020` load, `0x1021` unload, `0x1022` control'
require_text "$FOLKPATCH_REFERENCE" '/data/adb/fp/kpms/kpm_autoload_config.json'
require_text "$FOLKPATCH_REFERENCE" '`references/KernelPatch` remains the pinned compile/header'
require_text "$FOLKPATCH_REFERENCE" '/system/bin/truncate su module ...'

SCRIPTS='
scripts/build_kpm.sh
scripts/build_lab_app.sh
scripts/verify_wxshadow_reference_source.sh
scripts/verify_wxshadow_reference_inventory.sh
scripts/test_wxshadow_reference_inventory_host.sh
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
scripts/test_s4_raw_reg_device.sh
scripts/test_raw_device.sh
scripts/test_raw_page_table_device.sh
scripts/test_raw_page_table_patch_records_device.sh
scripts/test_raw_read_cycle_device.sh
scripts/test_raw_syscall_read_cycle_device.sh
scripts/test_raw_syscall_hook_routing_device.sh
scripts/test_raw_prctl_read_cycle_device.sh
scripts/test_raw_prctl_patch_records_device.sh
scripts/test_raw_prctl_hook_routing_device.sh
scripts/test_raw_gup_hide_device.sh
scripts/test_raw_gup_hook_device.sh
scripts/test_raw_gup_hook_routing_device.sh
scripts/test_raw_fork_hook_device.sh
scripts/test_raw_fork_hook_routing_device.sh
scripts/test_raw_fault_hook_device.sh
scripts/test_raw_fault_hook_routing_device.sh
scripts/test_raw_fault_hook_positive_preflight_device.sh
scripts/test_raw_fault_data_probe_device.sh
scripts/test_raw_hook_routing_device.sh
scripts/test_raw_abort_probe_device.sh
scripts/test_raw_abort_write_probe_device.sh
scripts/test_raw_abort_write_release_device.sh
scripts/test_raw_exit_hook_device.sh
scripts/test_raw_exit_hook_preclear_hold_split_device.sh
scripts/test_raw_abort_mmget_passthrough_device.sh
scripts/verify_d4_r3j_plan_packet.sh
scripts/verify_d4_r3k_plan_packet.sh
scripts/verify_d4_r3l_plan_packet.sh
scripts/verify_d4_r3m_plan_packet.sh
scripts/test_raw_abort_visible_inflight_device.sh
scripts/verify_d4_r3k_disassembly.sh
scripts/test_raw_abort_iabt_route_device.sh
scripts/verify_d4_r3l_disassembly.sh
scripts/test_raw_abort_iabt_transition_device.sh
scripts/verify_d4_r3m_disassembly.sh
scripts/test_raw_full_abort_lifecycle_device.sh
scripts/verify_d4_r3n_disassembly.sh
scripts/verify_d4_r3o_reference_lifetime.sh
scripts/test_raw_r3o_lifetime_device.sh
scripts/test_v1_device.sh
'

SCRIPT_COUNT=0
for script in $SCRIPTS; do
  require_file "$script"
  [ -x "$ROOT/$script" ] || fail "required script is not executable: $script"
  sh -n "$ROOT/$script" || fail "shell syntax is invalid: $script"
  SCRIPT_COUNT=$((SCRIPT_COUNT + 1))
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
require_text kpm/r0lab.c '#define R0LAB_RAW_PAGE_SLOT_CAPACITY 2U'
require_text kpm/r0lab.c 'struct r0lab_raw_page_table'
require_text kpm/r0lab.c 'r0lab_raw_page_slot_reset_locked'
require_text kpm/r0lab.c 'r0lab_raw_selected_page_locked'
require_text kpm/r0lab.c 'r0lab_raw_page_table_active_count_locked'
require_text kpm/r0lab.c 'r0lab_raw_slot_arm'
require_text kpm/r0lab.c 'raw slot arm '
require_text kpm/r0lab.c 'raw_slot_ready slot=%u'
require_text kpm/r0lab.c 'raw_slot_observed slot=%u'
require_text kpm/r0lab.c 'raw_slot_inspect slot=%u'
require_text kpm/r0lab.c 'raw_slot_patch_check_ok slot=%u'
require_text kpm/r0lab.c 'r0lab_raw_abort_hook_users_locked'
require_text kpm/r0lab.c 'raw_page_table_slots=%u raw_page_table_active=%u raw_selected_slot=%u'
require_text kpm/r0lab.c 'raw_slot_backend=%s raw_slot_state=%s raw_slot_source=%llx raw_slot_source_pfn=%llx raw_slot_shadow_pfn=%llx'
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
require_function_text kpm/r0lab.c r0lab_m3_clear_worker \
  'result = page->raw.state == R0LAB_RAW_RESTORED ?'
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
require_text kpm/r0lab.c 'R0LAB_EVENT_RAW_SYSCALL_READ_CYCLE_BEGIN'
require_text kpm/r0lab.c 'R0LAB_EVENT_RAW_PRCTL_TRIGGER'
require_text kpm/r0lab.c 'R0LAB_EVENT_RAW_PRCTL_PATCH'
require_text kpm/r0lab.c 'R0LAB_EVENT_RAW_PRCTL_RELEASE'
require_text kpm/r0lab.c 'R0LAB_EVENT_RAW_ABORT_PROBE_HIT'
require_text kpm/r0lab.c 'R0LAB_EVENT_RAW_ABORT_WRITE_RELEASE'
require_text kpm/r0lab.c 'R0LAB_EVENT_S4_REG_WRITE'
require_text kpm/r0lab.c 'R0LAB_ABORT_PROBE_SOURCE_WRITE_PERMISSION'
require_text kpm/r0lab.c 'raw_read_cycle_begin result=%d state=%lu active_kind=%s read_cycle_active=%lu read_cycle_begin_events=%lu read_cycle_finish_events=%lu read_cycle=uxn_original_exec_resume trigger=supercall pte_switch=1 data_fault=absent exec_resume=pending'
require_text kpm/r0lab.c 'raw_read_cycle_status state=%lu active_kind=%s read_cycle_active=%lu read_cycle_begin_events=%lu read_cycle_finish_events=%lu read_cycle=uxn_original_exec_resume trigger=supercall pte_switch=%u data_fault=absent exec_resume=%s'
require_text kpm/r0lab.c 'raw_syscall_read_cycle_hook_ready symbol=getpid installed=1 target_mm_scoped=1 trigger=syscall_getpid read_cycle=uxn_original_exec_resume observe_only=0 pte_switch=1 data_fault=absent exec_resume=pending'
require_text kpm/r0lab.c 'raw_syscall_hook_status symbol=%s installed=%u hit_events=%u read_cycle_events=%u failures=%u target_mm_scoped=1 trigger=syscall_getpid read_cycle=uxn_original_exec_resume observe_only=0 pte_switch=1 data_fault=absent exec_resume=%s'
require_text kpm/r0lab.c 'r0lab_raw_syscall_hook_users_locked'
require_text kpm/r0lab.c 'raw_slot_syscall_read_cycle_hook_ready slot=%u generation=%llu symbol=getpid installed=1 selected=1 target_mm_scoped=1 page_record_routed=1'
require_text kpm/r0lab.c 'raw_slot_syscall_read_cycle_hook_selected slot=%u generation=%llu symbol=getpid selected=1 target_mm_scoped=1 page_record_routed=1'
require_text kpm/r0lab.c 'raw_slot_syscall_hook_status slot=%u generation=%llu symbol=%s installed=%u selected=%u hit_events=%u read_cycle_events=%u failures=%u target_mm_scoped=1 page_record_routed=1'
require_text kpm/r0lab.c 'raw_slot_syscall_hook_cleared slot=%u generation=%llu symbol=%s installed=0 hit_events=%u read_cycle_events=%u read_cycle_finish_events=%lu failures=%u page_record_routed=1'
require_text kpm/r0lab.c 'r0lab_raw_selected_page_locked()'
require_text kpm/r0lab.c '#define R0LAB_PRCTL_MAGIC 0x52304c42U'
require_text kpm/r0lab.c '#define R0LAB_PRCTL_OP_READ_CYCLE 1U'
require_text kpm/r0lab.c '#define R0LAB_PRCTL_OP_PATCH_WORD 2U'
require_text kpm/r0lab.c '#define R0LAB_PRCTL_OP_RELEASE_PATCH 3U'
require_text kpm/r0lab.c '#define R0LAB_PRCTL_OP_PATCH_RANGE 4U'
require_text kpm/r0lab.c '#define R0LAB_PRCTL_OP_RELEASE_RANGE 5U'
require_text kpm/r0lab.c '#define R0LAB_PATCH_RECORD_CAPACITY 1024U'
require_text kpm/r0lab.c 'g_sys_prctl = r0lab_lookup_first("__arm64_sys_prctl.cfi_jt"'
require_text kpm/r0lab.c 'r0lab_lookup_first("copy_from_user_nofault.cfi_jt",'
require_text kpm/r0lab.c 'g_sync_icache_aliases = (r0lab_sync_icache_aliases_fn_t)'
require_text kpm/r0lab.c 'r0lab_lookup_first("sync_icache_aliases.cfi_jt",'
require_text kpm/r0lab.c '(uint32_t)syscall_regs->regs[0] != R0LAB_PRCTL_MAGIC'
require_text kpm/r0lab.c 'session_valid = current_uid() == g_session.lab_uid && g_session.active &&'
require_text kpm/r0lab.c 'token == g_session.token'
require_text kpm/r0lab.c 'operation == R0LAB_PRCTL_OP_PATCH_WORD'
require_text kpm/r0lab.c 'operation == R0LAB_PRCTL_OP_RELEASE_PATCH'
require_text kpm/r0lab.c 'operation == R0LAB_PRCTL_OP_PATCH_RANGE'
require_text kpm/r0lab.c 'operation == R0LAB_PRCTL_OP_RELEASE_RANGE'
require_text kpm/r0lab.c 'uint16_t patch_rebuild_order[R0LAB_PATCH_RECORD_CAPACITY];'
require_function_text kpm/r0lab.c r0lab_raw_rebuild_patch_range \
  'order = page->patch_rebuild_order;'
require_function_text kpm/r0lab.c r0lab_raw_rebuild_patch_range \
  'g_sync_icache_aliases((unsigned long)page->raw.shadow_kaddr + offset,'
require_function_text kpm/r0lab.c r0lab_raw_rebuild_patch_range \
  'page->patch_records[order[index]]'
require_function_text kpm/r0lab.c r0lab_raw_upsert_patch_locked \
  'page->patch_records[*record_index].version = page->patch_version;'
require_function_text kpm/r0lab.c r0lab_raw_release_patch_locked \
  'record->data = NULL;'
require_function_text kpm/r0lab.c r0lab_raw_detach_one_patch_buffer_locked \
  'record->data = NULL;'
require_function_text kpm/r0lab.c r0lab_raw_drain_patch_buffers_page \
  'buffer = r0lab_raw_detach_one_patch_buffer_locked(page);'
require_text kpm/r0lab.c 'raw slot patch byte '
require_text kpm/r0lab.c 'raw slot patch word '
require_text kpm/r0lab.c 'raw slot patch release '
require_text kpm/r0lab.c 'raw slot patch status '
require_function_text kpm/r0lab.c r0lab_raw_slot_patch_admit_locked \
  'page->transitioning = true;'
require_function_text kpm/r0lab.c r0lab_raw_slot_patch_apply \
  'r0lab_raw_upsert_patch_locked('
require_function_text kpm/r0lab.c r0lab_raw_slot_patch_apply \
  'r0lab_raw_rebuild_patch_range(page, range_offset,'
require_function_text kpm/r0lab.c r0lab_raw_slot_patch_release \
  'r0lab_raw_release_patch_locked('
require_text kpm/r0lab.c 'raw_slot_patch_status slot=%u page=%llx generation=%llu state=%lu patch_record_slots=%u patch_active_count=%u patch_dirty_bytes=%u patch_version=%llu patch_capacity=%u patch_scope=page_slot_ranges'
require_text kpm/r0lab.c 'raw_slot_patch_ok slot=%u generation=%llu offset=%llu length=%u address=%llx patch_record_slots=%u patch_active_count=%u patch_dirty_bytes=%u patch_version=%llu patch_capacity=%u patch_scope=page_slot_ranges'
require_text kpm/r0lab.c 'raw_slot_patch_release_ok slot=%u generation=%llu offset=%llu address=%llx patch_record_slots=%u patch_active_count=%u patch_dirty_bytes=%u patch_version=%llu patch_capacity=%u patch_scope=page_slot_ranges'
require_function_text kpm/r0lab.c r0lab_raw_prctl_before \
  '++g_raw_inflight;'
require_function_text kpm/r0lab.c r0lab_raw_prctl_before \
  'goto out;'
require_function_text kpm/r0lab.c r0lab_raw_prctl_before \
  '--g_raw_inflight;'
require_text kpm/r0lab.c 'raw_prctl_hook_ready symbol=prctl installed=1 abi=prctl_magic'
require_text kpm/r0lab.c 'operations=%u,%u,%u,%u,%u read_cycle_op=%u patch_word_op=%u release_patch_op=%u patch_range_op=%u release_range_op=%u'
require_text kpm/r0lab.c 'patch_scope=single_page_ranges patch_capacity=%u overlap=version_last_write_wins'
require_text kpm/r0lab.c 'passthrough=nonmagic'
require_text kpm/r0lab.c 'raw_va_prot_none'
require_text kpm/r0lab.c 'raw_va_rx_write'
require_text kpm/r0lab.c 'raw_abort_probe_ready symbol=do_mem_abort installed=1 armed=1 target_mm_scoped=1 source=%s observe_only=1 pte_switch=0 data_fault=sync_el0_dabt read_cycle=absent'
require_text kpm/r0lab.c 'raw_slot_abort_probe_ready slot=%u generation=%llu symbol=do_mem_abort installed=1 armed=1 target_mm_scoped=1 page_record_routed=1 source=%s observe_only=1 pte_switch=0 data_fault=sync_el0_dabt read_cycle=absent'
require_text kpm/r0lab.c 'raw_abort_probe_status symbol=%s installed=%u armed=%u read_events=%u write_events=%u exec_events=%u hit_events=%u failures=%u last_far=%llx last_esr=%x last_ec=%u last_fsc_type=%x last_wnr=%u permission_fault=%u translation_fault=%u target_mm_scoped=1 source=%s observe_only=1 pte_switch=0 data_fault=sync_el0_dabt read_cycle=absent'
require_text kpm/r0lab.c 'raw_slot_abort_probe_status slot=%u generation=%llu symbol=%s installed=%u armed=%u read_events=%u write_events=%u exec_events=%u hit_events=%u failures=%u last_far=%llx last_esr=%x last_ec=%u last_fsc_type=%x last_wnr=%u permission_fault=%u translation_fault=%u target_mm_scoped=1 page_record_routed=1 source=%s observe_only=1 pte_switch=0 data_fault=sync_el0_dabt read_cycle=absent'
require_text kpm/r0lab.c 'R0LAB_EVENT_RAW_ABORT_READ_CYCLE_BEGIN'
require_function_text kpm/r0lab.c r0lab_raw_before_abort \
  'r0lab_raw_hook_page_token_acquire_full_abort_locked'
require_function_text kpm/r0lab.c r0lab_raw_before_abort \
  'r0lab_raw_begin_fault_read_cycle(&fault_page->raw);'
require_function_text kpm/r0lab.c r0lab_raw_before_abort \
  'r0lab_raw_restore_original(&fault_page->raw);'
require_function_text kpm/r0lab_raw_compat.c r0lab_raw_begin_fault_read_cycle \
  'pte_pfn(current_pte) != page->shadow_pfn'
require_function_text kpm/r0lab_raw_compat.c r0lab_raw_begin_fault_read_cycle \
  'page->state = R0LAB_RAW_ORIGINAL_READ;'
require_text kpm/r0lab.c 'raw_abort_read_cycle_ready symbol=do_mem_abort installed=1 armed=1 target_mm_scoped=1 source=raw_va_prot_none action=begin_read_cycle observe_only=0 pte_switch=1 data_fault=sync_el0_dabt read_cycle=uxn_original_exec_resume skip_origin=1 exec_resume=pending'
require_text kpm/r0lab.c 'raw_slot_abort_read_cycle_ready slot=%u generation=%llu symbol=do_mem_abort installed=1 armed=1 target_mm_scoped=1 page_record_routed=1 source=raw_va_prot_none action=begin_read_cycle observe_only=0 pte_switch=1 data_fault=sync_el0_dabt read_cycle=uxn_original_exec_resume skip_origin=1 exec_resume=pending'
require_text kpm/r0lab.c 'raw_abort_read_cycle_status symbol=%s installed=%u armed=%u read_events=%u failures=%u last_far=%llx last_esr=%x last_ec=%u last_fsc_type=%x last_wnr=%u permission_fault=%u translation_fault=%u begin_result=%d target_mm_scoped=1 source=raw_va_prot_none action=begin_read_cycle observe_only=0 pte_switch=%u data_fault=sync_el0_dabt read_cycle=uxn_original_exec_resume read_cycle_active=%lu read_cycle_begin_events=%lu read_cycle_finish_events=%lu skip_origin=1 exec_resume=%s'
require_text kpm/r0lab.c 'raw_slot_abort_read_cycle_status slot=%u generation=%llu symbol=%s installed=%u armed=%u read_events=%u failures=%u last_far=%llx last_esr=%x last_ec=%u last_fsc_type=%x last_wnr=%u permission_fault=%u translation_fault=%u begin_result=%d target_mm_scoped=1 page_record_routed=1 source=raw_va_prot_none action=begin_read_cycle observe_only=0 pte_switch=%u data_fault=sync_el0_dabt read_cycle=uxn_original_exec_resume read_cycle_active=%lu read_cycle_begin_events=%lu read_cycle_finish_events=%lu skip_origin=1 exec_resume=%s'
require_text kpm/r0lab.c 'raw abort read cycle arm '
require_text kpm/r0lab.c 'raw abort read cycle status '
require_text kpm/r0lab.c 'raw abort read cycle clear '
require_text kpm/r0lab.c 'raw slot abort read cycle arm '
require_text kpm/r0lab.c 'raw slot abort read cycle status '
require_text kpm/r0lab.c 'raw slot abort read cycle clear '
require_text kpm/r0lab.c 'raw abort write probe arm '
require_text kpm/r0lab.c 'raw abort write probe status '
require_text kpm/r0lab.c 'raw abort write probe clear '
require_text kpm/r0lab.c 'raw slot abort write probe arm '
require_text kpm/r0lab.c 'raw slot abort write probe status '
require_text kpm/r0lab.c 'raw slot abort write probe clear '
require_text kpm/r0lab.c 'raw_abort_write_release_ready symbol=do_mem_abort installed=1 armed=1 target_mm_scoped=1 source=raw_va_rx_write action=restore_original logical_release=1 observe_only=0 pte_switch=1 data_fault=sync_el0_dabt read_cycle=absent skip_origin=0'
require_text kpm/r0lab.c 'raw_slot_abort_write_release_ready slot=%u generation=%llu symbol=do_mem_abort installed=1 armed=1 target_mm_scoped=1 page_record_routed=1 source=raw_va_rx_write action=restore_original logical_release=1 observe_only=0 pte_switch=1 data_fault=sync_el0_dabt read_cycle=absent skip_origin=0'
require_text kpm/r0lab.c 'raw_abort_write_release_status symbol=%s installed=%u armed=%u release_events=%u failures=%u last_far=%llx last_esr=%x last_ec=%u last_fsc_type=%x last_wnr=%u permission_fault=%u translation_fault=%u restore_result=%d target_mm_scoped=1 source=raw_va_rx_write action=restore_original logical_release=1 observe_only=0 pte_switch=%u data_fault=sync_el0_dabt read_cycle=absent skip_origin=0'
require_text kpm/r0lab.c 'raw_slot_abort_write_release_status slot=%u generation=%llu symbol=%s installed=%u armed=%u release_events=%u failures=%u last_far=%llx last_esr=%x last_ec=%u last_fsc_type=%x last_wnr=%u permission_fault=%u translation_fault=%u restore_result=%d target_mm_scoped=1 page_record_routed=1 source=raw_va_rx_write action=restore_original logical_release=1 observe_only=0 pte_switch=%u data_fault=sync_el0_dabt read_cycle=absent skip_origin=0'
require_text kpm/r0lab.c 'raw abort write release arm '
require_text kpm/r0lab.c 'raw abort write release status '
require_text kpm/r0lab.c 'raw abort write release clear '
require_text kpm/r0lab.c 'raw slot abort write release arm '
require_text kpm/r0lab.c 'raw slot abort write release status '
require_text kpm/r0lab.c 'raw slot abort write release clear '
require_text kpm/r0lab.c 'R0LAB_EVENT_RAW_GUP_HOOK_BEGIN'
require_text kpm/r0lab.c 'R0LAB_EVENT_RAW_GUP_HOOK_FINISH'
require_text kpm/r0lab.c 'g_follow_page_pte = r0lab_lookup_first("follow_page_pte.cfi_jt"'
require_text kpm/r0lab.c 'hook_wrap5(hook_target, r0lab_raw_gup_pte_before'
require_text kpm/r0lab.c 'hook_wrap4(hook_target, r0lab_raw_gup_mask_before'
require_text kpm/r0lab.c 'r0lab_raw_gup_hook_users_locked'
require_text kpm/r0lab.c 'R0LAB_RAW_HOOK_GUP, vma_mm, address'
require_text kpm/r0lab.c 'local->data2 = slot_id;'
require_text kpm/r0lab.c 'raw slot gup hook arm '
require_text kpm/r0lab.c 'raw slot gup hook status '
require_text kpm/r0lab.c 'raw slot gup hook clear '
require_text kpm/r0lab.c 'raw_gup_hook_ready symbol=%s follow_page_pte=%s follow_page_mask=%s installed=1 mode=%s target_mm_scoped=1 external_reader=1'
require_text kpm/r0lab.c 'raw_gup_hook_status symbol=%s follow_page_pte=%s follow_page_mask=%s installed=%u hook_begin_events=%u hook_finish_events=%u hook_failures=%u primitive_begin_events=%lu primitive_finish_events=%lu target_mm_scoped=1 external_reader=1'
require_text kpm/r0lab.c 'raw_slot_gup_hook_ready slot=%u generation=%llu symbol=%s follow_page_pte=%s follow_page_mask=%s installed=1 mode=%s target_mm_scoped=1 page_record_routed=1 external_reader=1'
require_text kpm/r0lab.c 'raw_slot_gup_hook_status slot=%u generation=%llu symbol=%s follow_page_pte=%s follow_page_mask=%s installed=%u hook_begin_events=%u hook_finish_events=%u hook_failures=%u primitive_begin_events=%lu primitive_finish_events=%lu target_mm_scoped=1 page_record_routed=1 external_reader=1'
require_text kpm/r0lab.c 'raw_slot_gup_hook_cleared slot=%u generation=%llu follow_page_pte=%s follow_page_mask=%s installed=0 hook_begin_events=%u hook_finish_events=%u hook_failures=%u primitive_begin_events=%lu primitive_finish_events=%lu page_record_routed=1'
require_text kpm/r0lab.c 'R0LAB_EVENT_RAW_FORK_HOOK_BEGIN'
require_text kpm/r0lab.c 'R0LAB_EVENT_RAW_FORK_HOOK_FINISH'
require_text kpm/r0lab.c 'g_dup_mmap = r0lab_lookup_first("dup_mmap.cfi_jt", "dup_mmap")'
require_text kpm/r0lab.c 'hook_wrap2(g_dup_mmap, r0lab_raw_fork_before'
require_text kpm/r0lab.c 'r0lab_raw_fork_page_eligible_locked'
require_text kpm/r0lab.c 'r0lab_raw_fork_hook_users_locked'
require_text kpm/r0lab.c 'planned_mask |= r0lab_raw_fork_slot_mask'
require_text kpm/r0lab.c 'r0lab_raw_begin_fork_hide(&page->raw, oldmm)'
require_text kpm/r0lab.c 'r0lab_raw_finish_fork_hide(&page->raw, oldmm)'
require_text kpm/r0lab.c 'raw slot fork hook arm '
require_text kpm/r0lab.c 'raw slot fork hook status '
require_text kpm/r0lab.c 'raw slot fork hook clear '
require_text kpm/r0lab.c 'raw_fork_hook_ready symbol=dup_mmap installed=1 parent_pause=1 child_original_inherit=1 target_mm_scoped=1'
require_text kpm/r0lab.c 'raw_slot_fork_hook_ready slot=%u generation=%llu symbol=dup_mmap installed=1 parent_pause=1 child_original_inherit=1 target_mm_scoped=1 page_record_routed=1'
require_text kpm/r0lab.c 'raw_fork_hook_status symbol=%s installed=%u hook_begin_events=%u hook_finish_events=%u hook_failures=%u primitive_begin_events=%lu primitive_finish_events=%lu parent_pause=1 child_original_inherit=1 target_mm_scoped=1'
require_text kpm/r0lab.c 'raw_slot_fork_hook_status slot=%u generation=%llu symbol=%s installed=%u hook_begin_events=%u hook_finish_events=%u hook_failures=%u primitive_begin_events=%lu primitive_finish_events=%lu parent_pause=1 child_original_inherit=1 target_mm_scoped=1 page_record_routed=1'
require_text kpm/r0lab.c 'R0LAB_EVENT_RAW_FAULT_HOOK_HIT'
require_text kpm/r0lab.c 'g_handle_mm_fault = r0lab_lookup_first("handle_mm_fault.cfi_jt"'
require_text kpm/r0lab.c 'hook_wrap4(g_handle_mm_fault, r0lab_raw_fault_before'
require_text kpm/r0lab.c 'raw_fault_hook_ready symbol=handle_mm_fault installed=1 target_mm_scoped=1 observe_only=1 pte_switch=0'
require_text kpm/r0lab.c 'raw_fault_hook_status symbol=%s installed=%u read_events=%u write_events=%u exec_events=%u hit_events=%u failures=%u target_mm_scoped=1 observe_only=1 pte_switch=0'
require_text kpm/r0lab.c 'raw_slot_fault_hook_ready slot=%u generation=%llu symbol=handle_mm_fault installed=1 target_mm_scoped=1 page_record_routed=1 observe_only=1 pte_switch=0'
require_text kpm/r0lab.c 'raw_slot_fault_hook_status slot=%u generation=%llu symbol=%s installed=%u read_events=%u write_events=%u exec_events=%u hit_events=%u failures=%u target_mm_scoped=1 page_record_routed=1 observe_only=1 pte_switch=0'
require_text kpm/r0lab_raw.h 'int r0lab_raw_clear_shadow_access_flag(struct r0lab_raw_page *page);'
require_text kpm/r0lab_raw_compat.c 'int r0lab_raw_clear_shadow_access_flag(struct r0lab_raw_page *page)'
require_text kpm/r0lab_raw_compat.c 'shadow_old = pte_mkold(current_pte);'
require_text kpm/r0lab.c 'raw slot fault af clear '
require_text kpm/r0lab.c 'raw_slot_fault_af_cleared slot=%u generation=%llu page=%llx result=0 pte_af=cleared state=%lu target_mm_scoped=1 page_record_routed=1 pte_switch=0 source=file_backed_rx'
require_text kpm/r0lab.c 'raw slot fault hook arm '
require_text kpm/r0lab.c 'raw slot fault hook status '
require_text kpm/r0lab.c 'raw slot fault hook clear '
require_text kpm/r0lab.c 'raw_fault_probe_ready symbol=handle_mm_fault installed=1 armed=1 page=%llx reader_tgid=0 target_mm_scoped=1 observe_only=1 pte_switch=0 source=normal_anon_remote_gup'
require_text kpm/r0lab.c 'raw_fault_probe_reader_ready reader_tgid=%d armed=1 page=%llx target_mm_scoped=1 remote_only=1 observe_only=1 pte_switch=0 source=normal_anon_remote_gup'
require_text kpm/r0lab.c 'raw_fault_probe_status symbol=%s installed=%u armed=%u page=%llx reader_tgid=%d read_events=%u write_events=%u exec_events=%u hit_events=%u failures=%u target_mm_scoped=1 remote_only=1 observe_only=1 pte_switch=0 source=normal_anon_remote_gup'
require_text kpm/r0lab.c 'fault_hook=observe_only'
require_text kpm/r0lab.c 'hook_wrap3(g_s4_brk_handler, r0lab_s4_brk_before, NULL'
require_text kpm/r0lab.c 'hook_wrap3(g_s4_single_step_handler,'
require_text kpm/r0lab.c 'r0lab_hook_detach(g_s4_brk_handler, r0lab_s4_brk_before, NULL)'
require_text kpm/r0lab.c 'r0lab_hook_detach(g_s4_single_step_handler, r0lab_s4_step_before,'
require_text kpm/r0lab.c 'Step-mode only: consume BRK and run the fixed Lab instruction.'
require_text kpm/r0lab.c 'args->skip_origin = 1'
require_text kpm/r0lab.c 's4_raw_reg_ready target=%llx state=%s mode=raw_reg'
require_text kpm/r0lab.c 'regs->regs[descriptor->register_index] = reg_value'
require_text kpm/r0lab.c 'register_apply=brk_before_single_step'
require_text kpm/r0lab.c 's4 raw-reg arm '
require_text lab-app/src/main/cpp/labprobe.c 'ready=\"%s\" observed=\"%s\"'
require_text lab-app/src/main/cpp/labprobe.c 'R0LAB_S4_CODE_BRK_7'
require_text lab-app/src/main/cpp/labprobe.c 's4 mode=brk-only failures=%d'
require_text lab-app/src/main/cpp/labprobe.c 's4 mode=step-only failures=%d'
require_text lab-app/src/main/cpp/labprobe.c 's4 mode=raw-step failures=%d'
require_text lab-app/src/main/cpp/labprobe.c 's4 mode=raw-reg failures=%d'
require_text lab-app/src/main/cpp/labprobe.c 'strncmp(args, "s4 raw-reg ", 11)'
require_text lab-app/src/main/cpp/labprobe.c 'raw mode=gup-hide failures=%d'
require_text lab-app/src/main/cpp/labprobe.c 'raw mode=read-cycle failures=%d trigger=supercall data_fault=absent pte_switch=1 exec_resume=1'
require_text lab-app/src/main/cpp/labprobe.c 'raw mode=syscall-read-cycle failures=%d trigger=syscall_getpid read_cycle=uxn_original_exec_resume data_fault=absent pte_switch=1 exec_resume=1'
require_text lab-app/src/main/cpp/labprobe.c 'r0lab_raw_syscall_read_cycle_routing_run'
require_text lab-app/src/main/cpp/labprobe.c 'raw mode=syscall-read-cycle-routing failures=%d trigger=syscall_getpid read_cycle=uxn_original_exec_resume route=selected_slot page_record_routed=%d'
require_text lab-app/src/main/cpp/labprobe.c 'strncmp(args, "raw syscall read cycle routing run ", 35)'
require_text lab-app/src/main/cpp/labprobe.c 'r0lab_raw_prctl_hook_routing_run'
require_text lab-app/src/main/cpp/labprobe.c 'raw mode=prctl-routing failures=%d trigger=prctl_magic route=selected_slot,address page_record_routed=%d'
require_text lab-app/src/main/cpp/labprobe.c 'strncmp(args, "raw prctl hook routing run ", 27)'
require_text lab-app/src/main/cpp/labprobe.c 'raw mode=gup-hook failures=%d reader=external'
require_text lab-app/src/main/cpp/labprobe.c 'r0lab_raw_gup_hook_routing_run'
require_text lab-app/src/main/cpp/labprobe.c 'raw mode=gup-hook-routing failures=%d reader=external target_mm_scoped=1 page_record_routed=%d status_source=cross_inspect_clear'
require_text lab-app/src/main/cpp/labprobe.c 'strncmp(args, "raw gup hook routing run ", 25)'
require_text lab-app/src/main/cpp/labprobe.c 'raw mode=fork-hook failures=%d parent_pause=1 child_original_inherit=1'
require_text lab-app/src/main/cpp/labprobe.c 'raw mode=fault-hook failures=%d observe_only=1 target_mm_scoped=1 pte_switch=0'
require_text lab-app/src/main/cpp/labprobe.c 'raw mode=fault-hook-routing fault_route=handle_mm_fault failures=%d route_blocked=prot_none_badaccess_before_handle_mm_fault positive_route=0 page_record_routed=0 target_mm_scoped=1 observe_only=1 pte_switch=0 data_fault_source=raw_va_prot_none'
require_text lab-app/src/main/cpp/labprobe.c 'r0lab_raw_make_file_backed_code_page'
require_text lab-app/src/main/cpp/labprobe.c 'raw mode=fault-hook-positive-preflight fault_route=handle_mm_fault failures=%d source=file_backed_rx pte_af_clear=1 positive_route=%d page_record_routed=%d'
require_text lab-app/src/main/cpp/labprobe.c 'strncmp(args, "raw fault hook positive preflight run ", 38)'
require_text lab-app/src/main/cpp/labprobe.c 'raw mode=fault-data-probe failures=%d observe_only=1 target_mm_scoped=1 remote_only=1 pte_switch=0 data_fault_source=normal_anon_remote_gup'
require_text lab-app/src/main/cpp/labprobe.c 'raw mode=abort-probe failures=%d observe_only=1 target_mm_scoped=1 pte_switch=0 data_fault_source=raw_va_prot_none data_fault=sync_el0_dabt read_cycle=absent'
require_text lab-app/src/main/cpp/labprobe.c 'raw mode=abort-read-cycle failures=%d trigger=do_mem_abort target_mm_scoped=1 pte_switch=1 data_fault_source=raw_va_prot_none data_fault=sync_el0_dabt read_cycle=uxn_original_exec_resume skip_origin=1 observe_only=0'
require_text lab-app/src/main/cpp/labprobe.c 'raw mode=hook-routing abort_route=read-cycle failures=%d page_record_routed=1 target_mm_scoped=1'
require_text lab-app/src/main/cpp/labprobe.c 'raw mode=abort-write-probe failures=%d observe_only=1 target_mm_scoped=1 pte_switch=0 data_fault_source=raw_va_rx_write data_fault=sync_el0_dabt read_cycle=absent'
require_text lab-app/src/main/cpp/labprobe.c 'raw mode=abort-write-release failures=%d target_mm_scoped=1 source=raw_va_rx_write action=restore_original logical_release=1 observe_only=0 pte_switch=1 data_fault=sync_el0_dabt read_cycle=absent skip_origin=0'
require_text lab-app/src/main/cpp/labprobe.c 'g_r0lab_raw_signal_jump_on_fault = 1'
require_text lab-app/src/main/cpp/labprobe.c 'siglongjmp(g_r0lab_raw_signal_jump, 1)'
require_text lab-app/src/main/cpp/labprobe.c 'strncmp(args, "raw abort read cycle run ", 25)'
require_text lab-app/src/main/cpp/labprobe.c 'strncmp(args, "raw hook routing run ", 21)'
require_text lab-app/src/main/cpp/labprobe.c 'strncmp(args, "raw fault hook routing run ", 27)'
require_text lab-app/src/main/cpp/labprobe.c 'strncmp(args, "raw abort write probe run ", 26)'
require_text lab-app/src/main/cpp/labprobe.c 'strncmp(args, "raw abort write release run ", 28)'
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
require_text scripts/test_s4_raw_reg_device.sh 's4 mode=raw-reg failures=0'
require_text scripts/test_s4_raw_reg_device.sh 'hook_value=73'
require_text scripts/test_s4_raw_reg_device.sh 'restored_value=42'
require_text scripts/test_s4_raw_reg_device.sh 'reg_write_events=1'
require_text scripts/test_s4_raw_reg_device.sh 'register_apply=brk_before_single_step'
require_text scripts/test_s4_raw_reg_device.sh 'op=41 result=0'
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
require_text scripts/test_raw_syscall_read_cycle_device.sh 'raw mode=syscall-read-cycle failures=0'
require_text scripts/test_raw_syscall_read_cycle_device.sh 'trigger=syscall_getpid'
require_text scripts/test_raw_syscall_read_cycle_device.sh 'read_cycle=uxn_original_exec_resume'
require_text scripts/test_raw_syscall_read_cycle_device.sh 'original_read_word=52800540'
require_text scripts/test_raw_syscall_read_cycle_device.sh 'shadow_after_resume=52800c60'
require_text scripts/test_raw_syscall_read_cycle_device.sh 'read_cycle_events=1'
require_text scripts/test_raw_syscall_read_cycle_device.sh 'op=35 result=0'
require_text scripts/test_raw_syscall_read_cycle_device.sh 'op=38 result=0'
require_text scripts/test_raw_syscall_read_cycle_device.sh 'op=37 result=0'
require_text scripts/test_raw_syscall_hook_routing_device.sh 'raw mode=syscall-read-cycle-routing failures=0'
require_text scripts/test_raw_syscall_hook_routing_device.sh 'route=selected_slot'
require_text scripts/test_raw_syscall_hook_routing_device.sh 'page_record_routed=1'
require_text scripts/test_raw_syscall_hook_routing_device.sh 'stale_generation_rc=-11'
require_text scripts/test_raw_syscall_hook_routing_device.sh 'route_slot0_events=1 route_slot1_events=1'
require_text scripts/test_raw_syscall_hook_routing_device.sh 'status_slot0=1/1/1'
require_text scripts/test_raw_syscall_hook_routing_device.sh 'status_slot1=1/1/1'
require_text scripts/test_raw_syscall_hook_routing_device.sh 'clear_hit=1/1 clear_finish=1/1 clear_failures=0/0'
require_text scripts/test_raw_syscall_hook_routing_device.sh 'raw_syscall_hook_routing=pass'
require_text scripts/test_raw_prctl_read_cycle_device.sh 'raw mode=prctl-dispatch failures=0'
require_text scripts/test_raw_prctl_read_cycle_device.sh 'trigger=prctl_magic'
require_text scripts/test_raw_prctl_read_cycle_device.sh 'option=52304c42'
require_text scripts/test_raw_prctl_read_cycle_device.sh 'operations=1,2,3,4,5'
require_text scripts/test_raw_prctl_read_cycle_device.sh 'dispatch=patch_word,release_patch,patch_range,release_range,read_cycle'
require_text scripts/test_raw_prctl_read_cycle_device.sh 'reject_rc=-1'
require_text scripts/test_raw_prctl_read_cycle_device.sh 'reject_errno=1'
require_text scripts/test_raw_prctl_read_cycle_device.sh 'shadow_after_reject=52800c60'
require_text scripts/test_raw_prctl_read_cycle_device.sh 'patch_value=77'
require_text scripts/test_raw_prctl_read_cycle_device.sh 'patch_word=528009a0'
require_text scripts/test_raw_prctl_read_cycle_device.sh 'release_value=99'
require_text scripts/test_raw_prctl_read_cycle_device.sh 'released_word=52800c60'
require_text scripts/test_raw_prctl_read_cycle_device.sh 'patch_events=1'
require_text scripts/test_raw_prctl_read_cycle_device.sh 'release_events=1'
require_text scripts/test_raw_prctl_read_cycle_device.sh 'reject_events=1'
require_text scripts/test_raw_prctl_read_cycle_device.sh 'passthrough=nonmagic'
require_text scripts/test_raw_prctl_read_cycle_device.sh 'passthrough_stress_failures=0'
require_text scripts/test_raw_prctl_read_cycle_device.sh 'passthrough_thread_rc=0'
require_text scripts/test_raw_prctl_read_cycle_device.sh 'passthrough_join_rc=0'
require_text scripts/test_raw_prctl_read_cycle_device.sh 'STRESS_ITERATIONS'
require_text scripts/test_raw_prctl_read_cycle_device.sh 'op=42 result=-1'
require_text scripts/test_raw_prctl_read_cycle_device.sh 'op=42 result=0.* x0=2 '
require_text scripts/test_raw_prctl_read_cycle_device.sh 'op=42 result=0.* x0=3 '
require_text scripts/test_raw_prctl_read_cycle_device.sh 'op=42 result=0.* x0=1 '
require_text scripts/test_raw_prctl_read_cycle_device.sh 'op=43 result=0'
require_text scripts/test_raw_prctl_read_cycle_device.sh 'op=44 result=0'
require_text scripts/test_raw_prctl_read_cycle_device.sh 'op=38 result=0'
require_text scripts/test_raw_prctl_read_cycle_device.sh 'op=37 result=0'
require_text scripts/test_raw_prctl_patch_records_device.sh 'raw mode=prctl-patch-records failures=0'
require_text scripts/test_raw_prctl_patch_records_device.sh 'patch_capacity=1024'
require_text scripts/test_raw_prctl_patch_records_device.sh 'overlap_after_b=pass'
require_text scripts/test_raw_prctl_patch_records_device.sh 'release_b_rebuild=pass'
require_text scripts/test_raw_prctl_patch_records_device.sh 'shrink_rebuild=pass'
require_text scripts/test_raw_prctl_patch_records_device.sh 'capacity_fill_records=1023'
require_text scripts/test_raw_prctl_patch_records_device.sh 'capacity_overflow_errno=28'
require_text scripts/test_raw_prctl_patch_records_device.sh 'capacity_boundary=pass'
require_text scripts/test_raw_prctl_patch_records_device.sh 'active_progress=1,2,1,1,0,1,1024'
require_text scripts/test_raw_prctl_patch_records_device.sh 'dirty_progress=8,12,8,4,0,4,1027'
require_text scripts/test_raw_prctl_patch_records_device.sh 'op=45 result=-28'
require_text scripts/test_raw_prctl_hook_routing_device.sh 'raw prctl hook routing run'
require_text scripts/test_raw_prctl_hook_routing_device.sh 'raw mode=prctl-routing failures=0'
require_text scripts/test_raw_prctl_hook_routing_device.sh 'route=selected_slot,address'
require_text scripts/test_raw_prctl_hook_routing_device.sh 'page_record_routed=1'
require_text scripts/test_raw_prctl_hook_routing_device.sh 'stale_generation_rc=-11'
require_text scripts/test_raw_prctl_hook_routing_device.sh 'read_status=1/1'
require_text scripts/test_raw_prctl_hook_routing_device.sh 'patch_isolation=1'
require_text scripts/test_raw_prctl_hook_routing_device.sh 'release_isolation=1'
require_text scripts/test_raw_prctl_hook_routing_device.sh 'capacity_fill_records=1023'
require_text scripts/test_raw_prctl_hook_routing_device.sh 'capacity_boundary=pass'
require_text scripts/test_raw_prctl_hook_routing_device.sh 'raw_prctl_hook_routing=pass'
require_text lab-app/src/main/cpp/labprobe.c '#include <stdatomic.h>'
require_function_text lab-app/src/main/cpp/labprobe.c \
  r0lab_prctl_passthrough_stress_thread \
  'atomic_load_explicit(&stress->stop, memory_order_relaxed)'
require_function_text lab-app/src/main/cpp/labprobe.c \
  r0lab_prctl_passthrough_stress_thread \
  'atomic_fetch_add_explicit(&stress->iterations, 1,'
require_function_text lab-app/src/main/cpp/labprobe.c \
  r0lab_raw_prctl_read_cycle_run \
  'atomic_store_explicit(&passthrough_stress.stop, true,'
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
require_text scripts/test_raw_gup_hook_routing_device.sh 'raw gup hook routing run'
require_text scripts/test_raw_gup_hook_routing_device.sh 'raw mode=gup-hook-routing failures=0'
require_text scripts/test_raw_gup_hook_routing_device.sh 'page_record_routed=1'
require_text scripts/test_raw_gup_hook_routing_device.sh 'status_source=cross_inspect_clear'
require_text scripts/test_raw_gup_hook_routing_device.sh 'primitive_begin_events=1'
require_text scripts/test_raw_gup_hook_routing_device.sh 'primitive_finish_events=1'
require_text scripts/test_raw_gup_hook_routing_device.sh 'op=29 result=0'
require_text scripts/test_raw_gup_hook_routing_device.sh 'op=30 result=0'
require_text scripts/test_raw_gup_hook_routing_device.sh 'expected two raw gup hook begin events'
require_text scripts/test_raw_gup_hook_routing_device.sh 'raw_gup_hook_routing=pass'
require_text scripts/test_raw_fork_hook_device.sh 'raw mode=fork-hook failures=0'
require_text scripts/test_raw_fork_hook_device.sh 'parent_pause=1'
require_text scripts/test_raw_fork_hook_device.sh 'child_original_inherit=1'
require_text scripts/test_raw_fork_hook_device.sh 'child_word=52800540'
require_text scripts/test_raw_fork_hook_device.sh 'child_value=42'
require_text scripts/test_raw_fork_hook_device.sh 'fork_hide_primitive=proven'
require_text scripts/test_raw_fork_hook_device.sh 'op=31 result=0'
require_text scripts/test_raw_fork_hook_device.sh 'op=32 result=0'
require_text lab-app/src/main/cpp/labprobe.c 'slot_hook_status_rc'
require_text lab-app/src/main/cpp/labprobe.c 'raw slot fork hook status 0x%llx 0'
require_text lab-app/src/main/cpp/labprobe.c 'r0lab_control_raw_retry_once'
require_text lab-app/src/main/cpp/labprobe.c 'status_first_rc=%ld/%ld'
require_text scripts/test_raw_fork_hook_routing_device.sh 'raw fork hook routing run'
require_text scripts/test_raw_fork_hook_routing_device.sh 'raw mode=fork-hook-routing failures=0'
require_text scripts/test_raw_fork_hook_routing_device.sh 'page_record_routed=1'
require_text scripts/test_raw_fork_hook_routing_device.sh 'status_source=cross_clear'
require_text scripts/test_raw_fork_hook_routing_device.sh 'child_word=52800540/52800540'
require_text scripts/test_raw_fork_hook_routing_device.sh 'after_fork_shadow=52800c60/52800c60'
require_text scripts/test_raw_fork_hook_routing_device.sh 'raw_slot_fork_hook_ready slot=0'
require_text scripts/test_raw_fork_hook_routing_device.sh 'raw_slot_fork_hook_ready slot=1'
require_text lab-app/src/main/cpp/labprobe.c 'status_source=cross_clear'
require_text lab-app/src/main/cpp/labprobe.c 'raw_slot_fork_hook_cleared slot=0'
require_text lab-app/src/main/cpp/labprobe.c 'raw_slot_fork_hook_cleared slot=1'
require_text scripts/test_raw_fork_hook_routing_device.sh 'expected two raw fork hook begin events'
require_text scripts/test_raw_fork_hook_routing_device.sh 'expected two raw fork hook finish events'
require_text scripts/test_raw_fork_hook_routing_device.sh 'raw_fork_hook_routing=pass'
require_text scripts/test_raw_fault_hook_device.sh 'raw mode=fault-hook failures=0'
require_text scripts/test_raw_fault_hook_device.sh 'observe_only=1'
require_text scripts/test_raw_fault_hook_device.sh 'target_mm_scoped=1'
require_text scripts/test_raw_fault_hook_device.sh 'pte_switch=0'
require_text scripts/test_raw_fault_hook_device.sh 'op=33 result=0'
require_text scripts/test_raw_fault_hook_routing_device.sh 'raw mode=fault-hook-routing'
require_text scripts/test_raw_fault_hook_routing_device.sh 'fault_route=handle_mm_fault'
require_text scripts/test_raw_fault_hook_routing_device.sh 'route_blocked=prot_none_badaccess_before_handle_mm_fault'
require_text scripts/test_raw_fault_hook_routing_device.sh 'positive_route=0'
require_text scripts/test_raw_fault_hook_routing_device.sh 'page_record_routed=0'
require_text scripts/test_raw_fault_hook_routing_device.sh 'data_fault_source=raw_va_prot_none'
require_text scripts/test_raw_fault_hook_routing_device.sh 'handler_faults=2'
require_text scripts/test_raw_fault_hook_routing_device.sh 'expected zero raw fault hook hit events for blocked diagnostic'
require_text scripts/test_raw_fault_hook_positive_preflight_device.sh 'raw fault hook positive preflight run'
require_text scripts/test_raw_fault_hook_positive_preflight_device.sh 'source=file_backed_rx'
require_text scripts/test_raw_fault_hook_positive_preflight_device.sh 'pte_af_clear=1'
require_text scripts/test_raw_fault_hook_positive_preflight_device.sh 'positive_route=1'
require_text scripts/test_raw_fault_hook_positive_preflight_device.sh 'classification=handle_mm_fault_positive_blocked'
require_text scripts/test_raw_fault_hook_positive_preflight_device.sh 'expected zero raw fault hook hit events for blocked preflight'
require_text scripts/test_raw_fault_hook_positive_preflight_device.sh 'op=33 result=0'
require_text scripts/test_raw_fault_hook_positive_preflight_device.sh 'expected two raw fault hook hit events'
require_text scripts/test_raw_fault_hook_positive_preflight_device.sh 'raw_fault_hook_positive_preflight=pass'
require_text scripts/test_raw_fault_hook_positive_preflight_device.sh 'raw_fault_hook_positive_preflight=blocked'
require_text scripts/test_raw_fault_data_probe_device.sh 'raw mode=fault-data-probe failures=0'
require_text scripts/test_raw_fault_data_probe_device.sh 'data_fault_source=normal_anon_remote_gup'
require_text scripts/test_raw_fault_data_probe_device.sh 'raw_fault_probe_reader_ready'
require_text scripts/test_raw_fault_data_probe_device.sh 'target_mm_scoped=1'
require_text scripts/test_raw_fault_data_probe_device.sh 'remote_only=1'
require_text scripts/test_raw_fault_data_probe_device.sh 'fault_probe_read_events=1'
require_text scripts/test_raw_fault_data_probe_device.sh 'op=33 result=0'
require_text scripts/test_raw_abort_probe_device.sh 'raw mode=abort-probe failures=0'
require_text scripts/test_raw_abort_probe_device.sh 'data_fault_source=raw_va_prot_none'
require_text scripts/test_raw_abort_probe_device.sh 'data_fault=sync_el0_dabt'
require_text scripts/test_raw_abort_probe_device.sh 'handler_faults=1'
require_text scripts/test_raw_abort_probe_device.sh 'permission_fault=0'
require_text scripts/test_raw_abort_probe_device.sh 'translation_fault=1'
require_text scripts/test_raw_abort_probe_device.sh 'op=39 result=0'
require_text scripts/test_raw_abort_write_probe_device.sh 'raw mode=abort-write-probe failures=0'
require_text scripts/test_raw_abort_write_probe_device.sh 'data_fault_source=raw_va_rx_write'
require_text scripts/test_raw_abort_write_probe_device.sh 'source=raw_va_rx_write'
require_text scripts/test_raw_abort_write_probe_device.sh 'probe_write_fault_caught=1'
require_text scripts/test_raw_abort_write_probe_device.sh 'handler_faults=1'
require_text scripts/test_raw_abort_write_probe_device.sh 'last_fsc_type=c'
require_text scripts/test_raw_abort_write_probe_device.sh 'last_wnr=1'
require_text scripts/test_raw_abort_write_probe_device.sh 'permission_fault=1'
require_text scripts/test_raw_abort_write_probe_device.sh 'translation_fault=0'
require_text scripts/test_raw_abort_write_probe_device.sh 'op=39 result=0'
require_text scripts/test_raw_abort_write_probe_device.sh 'op=22 result=0'
require_text scripts/test_raw_abort_write_release_device.sh 'raw mode=abort-write-release failures=0'
require_text scripts/test_raw_abort_write_release_device.sh 'action=restore_original'
require_text scripts/test_raw_abort_write_release_device.sh 'logical_release=1'
require_text scripts/test_raw_abort_write_release_device.sh 'observe_only=0'
require_text scripts/test_raw_abort_write_release_device.sh 'pte_switch=1'
require_text scripts/test_raw_abort_write_release_device.sh 'skip_origin=0'
require_text scripts/test_raw_abort_write_release_device.sh 'post_release_exec_value=42'
require_text scripts/test_raw_abort_write_release_device.sh 'active_kind=original'
require_text scripts/test_raw_abort_write_release_device.sh 'release_events=1'
require_text scripts/test_raw_abort_write_release_device.sh 'restore_result=0'
require_text scripts/test_raw_abort_write_release_device.sh 'op=40 result=0'
require_text scripts/test_raw_abort_write_release_device.sh 'op=22 result=0'
require_text scripts/test_raw_hook_routing_device.sh 'raw mode=hook-routing'
require_text scripts/test_raw_hook_routing_device.sh 'abort_route=read-cycle'
require_text scripts/test_raw_hook_routing_device.sh 'page_record_routed=1'
require_text scripts/test_raw_hook_routing_device.sh 'raw_slot_abort_read_cycle_ready slot=0'
require_text scripts/test_raw_hook_routing_device.sh 'raw_slot_abort_read_cycle_ready slot=1'
require_text scripts/test_raw_hook_routing_device.sh 'op=47 result=0'
require_text scripts/test_raw_hook_routing_device.sh 'raw_hook_routing=pass abort_route=read-cycle'
require_text kpm/r0lab.c 'hook_unwrap_remove(func, before, after, 0);'
require_text references/KernelPatch/kernel/include/hook.h '#define HOOK_CHAIN_NUM 0x400'
require_text references/KernelPatch/kernel/include/hook.h '#define FP_HOOK_CHAIN_NUM 0x400'
if grep -F 'hook_unwrap(' kpm/r0lab.c >/dev/null; then
  printf '%s\n' 'unexpected unsafe hook_unwrap() call in kpm/r0lab.c' >&2
  exit 1
fi
require_text scripts/test_raw_exit_hook_device.sh 'raw mode=exit-hook-hold failures=0'
require_text scripts/test_raw_exit_hook_device.sh 'symbol=exit_mmap'
require_text scripts/test_raw_exit_hook_device.sh 'exit_hook_installed=1'
require_text scripts/test_raw_exit_hook_device.sh 'op=34 result=0'
require_text scripts/test_raw_exit_hook_device.sh 'op=22 result=0'
require_text scripts/test_v1_device.sh 'run_phase s4_step scripts/test_s4_step_device.sh'
require_text scripts/test_v1_device.sh 'run_phase s4_raw_step scripts/test_s4_raw_step_device.sh'
require_text scripts/test_v1_device.sh 'run_phase s4_raw_reg scripts/test_s4_raw_reg_device.sh'
require_text scripts/test_v1_device.sh 'run_phase raw_page_table scripts/test_raw_page_table_device.sh'
require_text scripts/test_v1_device.sh 'run_phase raw_read_cycle scripts/test_raw_read_cycle_device.sh'
require_text scripts/test_v1_device.sh 'run_phase raw_syscall_read_cycle scripts/test_raw_syscall_read_cycle_device.sh'
require_text scripts/test_v1_device.sh 'run_phase raw_prctl_read_cycle scripts/test_raw_prctl_read_cycle_device.sh'
require_text scripts/test_v1_device.sh 'run_phase raw_prctl_patch_records scripts/test_raw_prctl_patch_records_device.sh'
require_text scripts/test_v1_device.sh 'adb_device shell am force-stop "$LAB_PACKAGE"'
require_text scripts/test_v1_device.sh 'run_phase raw_gup_hide scripts/test_raw_gup_hide_device.sh'
require_text scripts/test_v1_device.sh 'run_phase raw_gup_hook scripts/test_raw_gup_hook_device.sh'
require_text scripts/test_v1_device.sh 'run_phase raw_fork_hook scripts/test_raw_fork_hook_device.sh'
require_text scripts/test_v1_device.sh 'run_phase raw_fault_hook scripts/test_raw_fault_hook_device.sh'
require_text scripts/test_v1_device.sh 'run_phase raw_fault_data_probe scripts/test_raw_fault_data_probe_device.sh'
require_text scripts/test_v1_device.sh 'run_phase raw_abort_probe scripts/test_raw_abort_probe_device.sh'
require_text scripts/test_v1_device.sh 'run_phase raw_abort_write_probe scripts/test_raw_abort_write_probe_device.sh'
require_text scripts/test_v1_device.sh 'run_phase raw_abort_write_release scripts/test_raw_abort_write_release_device.sh'
require_text scripts/test_m5_lifecycle_device.sh 'm5_target_exit_phases=m3,m4,raw,s4'
require_text scripts/test_m5_lifecycle_device.sh "run_phase raw \"\$RAW_TOKEN\" 'm5 raw-hold' 'raw-hold' 'raw_slots' 22"
require_text scripts/test_m5_lifecycle_device.sh "run_phase s4 \"\$S4_TOKEN\" 'm5 s4-hold' 's4-hold' 's4_slots' 26"
require_text scripts/test_m5_lifecycle_device.sh "run_dual_phase s4_raw_step \"\$S4_RAW_STEP_TOKEN\" 'm5 s4-raw-step-hold'"
require_text scripts/test_m3_device.sh 'op=14 result=0'
require_text scripts/test_m4_device.sh 'record_backend=visible_clone record_state=source_uxn'
require_text scripts/test_m4_device.sh 'source_perms=r-xp/r-xp/r-xp clone_perms=r-xp/r-xp/r-xp'
require_text lab-app/src/main/cpp/labprobe.c 'm5 mode=s4-hold failures=%d'
require_text lab-app/src/main/cpp/labprobe.c 'm5 mode=s4-raw-step-hold failures=%d'
require_text lab-app/src/main/cpp/labprobe.c 'raw mode=page-table failures=%d'
require_text lab-app/src/main/cpp/labprobe.c 'strncmp(args, "raw page table run ", 19)'
require_text lab-app/src/main/cpp/labprobe.c 'raw mode=exit-hook-hold failures=%d'
require_text lab-app/src/main/cpp/labprobe.c 'strncmp(args, "raw exit hook hold ", 19)'
require_text lab-app/src/main/cpp/labprobe.c 'strncmp(args, "m5 s4-raw-step-hold ", 20)'
require_text kpm/r0lab.c 'r0lab_s4_monitor_worker'
require_text kpm/r0lab.c 'r0lab_session_has_slots_locked'
require_text kpm/r0lab.c 'r0lab_session_monitor_loop'
require_text kpm/r0lab.c 'r0lab_raw_exit_mmap_before'
require_text kpm/r0lab.c 'page->clearing && !page->target_exiting'
require_text kpm/r0lab.c 'exit_hook=exit_mmap_observe'

require_text scripts/test_raw_abort_read_cycle_device.sh 'raw mode=abort-read-cycle failures=0'
require_text scripts/test_raw_abort_read_cycle_device.sh 'op=47 result=0'
require_text scripts/test_v1_device.sh 'run_phase raw_abort_read_cycle scripts/test_raw_abort_read_cycle_device.sh'
require_text scripts/test_raw_page_table_device.sh 'raw mode=page-table failures=0'
require_text scripts/test_raw_page_table_device.sh 'slot0_activations=1 slot1_activations=1'
require_text scripts/test_raw_page_table_device.sh 'patch_cross_page_rc=-22'
require_text scripts/test_raw_page_table_device.sh 'raw_page_table_activations=2'
require_text scripts/test_raw_page_table_patch_records_device.sh 'raw mode=page-table-patch-records failures=0'
require_text scripts/test_raw_page_table_patch_records_device.sh 'slot0_patch_value=77 slot1_patch_value=88'
require_text scripts/test_raw_page_table_patch_records_device.sh 'after_release0_slot0=99 after_release0_slot1=88'
require_text scripts/test_raw_page_table_patch_records_device.sh 'slot0_patch_record_slots=1024 slot0_patch_active_count=1024'
require_text scripts/test_raw_page_table_patch_records_device.sh 'slot0_capacity_overflow_errno=28'
require_text scripts/test_raw_page_table_patch_records_device.sh 'slot1_patch_record_slots=1 slot1_patch_active_count=1'
require_text scripts/test_raw_page_table_patch_records_device.sh 'slot1_after_slot0_capacity_value=88'
require_text scripts/test_raw_page_table_patch_records_device.sh 'final_original_slot0=42 final_original_slot1=42'
require_text scripts/test_raw_page_table_patch_records_device.sh 'raw_page_table_patch_records=pass'
require_text scripts/test_v1_device.sh 'run_phase raw_page_table_patch_records scripts/test_raw_page_table_patch_records_device.sh'
require_text lab-app/src/main/cpp/labprobe.c 'raw mode=page-table-patch-records failures=%d'
require_text lab-app/src/main/cpp/labprobe.c 'strncmp(args, "raw page table patch records run ", 33)'
reject_function_text kpm/r0lab.c r0lab_raw_page_has_aux_state_locked 'exit_hook_installed'

"$ROOT/scripts/verify_d4_r3o_reference_lifetime.sh" >/dev/null

printf '%s\n' "v1_contract=pass scripts=$SCRIPT_COUNT raw_pte_kpm=lab_two_pfn raw_page_table=two_slot_lab_harness raw_page_table_patch_records=page_local_records f3_plan=page_local_patch_records_locked f4_plan=hook_routing_by_page_record_locked f4_helper=route_token_status_ready f4_abort_route=two_slot_read_cycle f4_fault_route=prot_none_blocked_diagnostic f4_fault_positive_plan=file_backed_rx_pte_af_preflight_locked f4_fault_positive_preflight=classified_blocked f4_gup_route=two_slot_external_reader f4_fork_route=two_slot_dup_mmap_parent_page_list f4_syscall_route=selected_slot_generation f4_exit_plan=owner_exit_page_record_routing_locked f4_r3o=source_tagged_stage1_6_pass s4_brk=brk_only s4_step=raw_pte_step s4_reg=fixed_x1_before_step raw_gup_hide=primitive raw_read_cycle=uxn_original_exec_resume raw_syscall_read_cycle=hook_triggered raw_prctl_dispatch=read_patch_release_range_records raw_prctl_patch_records=versioned_overlap_rebuild raw_gup_hook=target_mm_external_reader raw_fork_hook=dup_mmap_parent_pause raw_fault_hook=handle_mm_fault_observe_only raw_fault_data_probe=normal_anon_remote_gup raw_abort_probe=sync_el0_translation_dabt_observe raw_abort_read_cycle=sync_el0_translation_dabt_read_cycle raw_abort_write_probe=sync_el0_permission_dabt_observe raw_abort_write_release=write_fault_restore_original raw_exit_hook=exit_mmap_observe result=pass"
