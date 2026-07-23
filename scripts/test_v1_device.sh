#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SERIAL=${ANDROID_SERIAL:-}
M0_LOOPS=${M0_LOOPS:-100}
EVIDENCE_DIR="$ROOT/build/evidence"
RUN_ID=$(date -u +%Y%m%dT%H%M%SZ)
RUN_DIR="$EVIDENCE_DIR/v1-device-$RUN_ID"
MANIFEST="$RUN_DIR/manifest.log"

adb_device() {
  if [ -n "$SERIAL" ]; then
    adb -s "$SERIAL" "$@"
  else
    adb "$@"
  fi
}

supercmd() {
  command="/system/bin/truncate su"
  for argument do
    quoted=$(printf '%s' "$argument" | sed "s/'/'\\\\''/g")
    command="$command '$quoted'"
  done
  adb_device shell su -c "$command"
}

fail() {
  printf '%s\n' "v1 device verification failure: $*" >&2
  printf 'result=fail reason=%s\n' "$*" >> "$MANIFEST"
  exit 1
}

read_warn_count() {
  adb_device shell su -c cat /sys/kernel/warn_count | tr -d '\r'
}

assert_clean_device() {
  phase=$1
  modules=$(supercmd module list 2>&1) || fail "$phase: module list failed: $modules"
  [ -z "$modules" ] || fail "$phase: resident KPM remains: $modules"
  warn_now=$(read_warn_count) || fail "$phase: could not read warn_count"
  [ "$warn_now" = "$WARN_BEFORE" ] ||
    fail "$phase: warn_count changed: $WARN_BEFORE -> $warn_now"
  printf 'phase=%s modules=empty warn_count=%s\n' "$phase" "$warn_now" >> "$MANIFEST"
}

run_phase() {
  phase=$1
  script=$2
  phase_log="$RUN_DIR/$phase.log"

  printf 'phase=%s script=%s status=running\n' "$phase" "$script" >> "$MANIFEST"
  if [ "$script" = 'scripts/test_m0_device.sh' ]; then
    if [ -n "$SERIAL" ]; then
      if ! ANDROID_SERIAL="$SERIAL" M0_LOOPS="$M0_LOOPS" "$ROOT/$script" >"$phase_log" 2>&1; then
        sed -n '1,240p' "$phase_log" >&2
        fail "$phase: script failed"
      fi
    elif ! M0_LOOPS="$M0_LOOPS" "$ROOT/$script" >"$phase_log" 2>&1; then
      sed -n '1,240p' "$phase_log" >&2
      fail "$phase: script failed"
    fi
  elif [ -n "$SERIAL" ]; then
    if ! ANDROID_SERIAL="$SERIAL" "$ROOT/$script" >"$phase_log" 2>&1; then
      sed -n '1,240p' "$phase_log" >&2
      fail "$phase: script failed"
    fi
  elif ! "$ROOT/$script" >"$phase_log" 2>&1; then
    sed -n '1,240p' "$phase_log" >&2
    fail "$phase: script failed"
  fi

  phase_evidence=$(tail -n 1 "$phase_log" | tr -d '\r')
  case "$phase_evidence" in
    "$ROOT"/build/evidence/*.log) ;;
    *) fail "$phase: final output is not an evidence path: $phase_evidence" ;;
  esac
  [ -f "$phase_evidence" ] || fail "$phase: evidence file does not exist: $phase_evidence"
  grep -F -- 'result=pass' "$phase_evidence" >/dev/null ||
    fail "$phase: evidence does not report result=pass"
  printf 'phase=%s script=%s status=pass evidence=%s raw_log=%s\n' \
    "$phase" "$script" "$phase_evidence" "$phase_log" >> "$MANIFEST"
  assert_clean_device "$phase"
}

mkdir -p "$RUN_DIR"
"$ROOT/scripts/verify_v1_contract.sh" >"$RUN_DIR/static-contract.log" 2>&1 || {
  sed -n '1,240p' "$RUN_DIR/static-contract.log" >&2
  printf 'result=fail reason=static_contract\n' > "$MANIFEST"
  exit 1
}

adb_device get-state 2>/dev/null | grep -qx device || {
  printf 'result=fail reason=device_unavailable\n' > "$MANIFEST"
  exit 1
}

modules_before=$(supercmd module list 2>&1) || {
  printf 'result=fail reason=module_list_unavailable\n' > "$MANIFEST"
  exit 1
}
[ -z "$modules_before" ] || {
  printf 'result=fail reason=resident_module_before_run modules=%s\n' "$modules_before" > "$MANIFEST"
  exit 1
}
WARN_BEFORE=$(read_warn_count) || {
  printf 'result=fail reason=warn_count_unavailable\n' > "$MANIFEST"
  exit 1
}

{
  printf 'run_id=%s\n' "$RUN_ID"
  printf 'serial=%s\n' "${SERIAL:-default}"
  printf 'm0_loops=%s\n' "$M0_LOOPS"
  printf 'warn_before=%s\n' "$WARN_BEFORE"
  printf 'static_contract=%s\n' "$RUN_DIR/static-contract.log"
} > "$MANIFEST"

run_phase m0_environment scripts/test_m0_environment_device.sh
run_phase m0_lifecycle scripts/test_m0_device.sh
run_phase m1_isolation scripts/test_m1_isolation_device.sh
run_phase m2_hwbp scripts/test_m2_device.sh
run_phase m3_uxn scripts/test_m3_device.sh
run_phase m4_clone scripts/test_m4_device.sh
run_phase s4_brk scripts/test_s4_brk_device.sh
run_phase s4_step scripts/test_s4_step_device.sh
run_phase s4_raw_step scripts/test_s4_raw_step_device.sh
run_phase m5_exit_probe scripts/test_m5_exit_probe_device.sh
run_phase m5_lifecycle scripts/test_m5_lifecycle_device.sh
run_phase m5_faults scripts/test_m5_faults_device.sh
run_phase raw_two_pfn scripts/test_raw_device.sh
run_phase raw_gup_hide scripts/test_raw_gup_hide_device.sh
run_phase raw_gup_hook scripts/test_raw_gup_hook_device.sh
run_phase raw_fork_hook scripts/test_raw_fork_hook_device.sh
run_phase raw_fault_hook scripts/test_raw_fault_hook_device.sh
run_phase raw_fault_data_probe scripts/test_raw_fault_data_probe_device.sh
run_phase raw_abort_probe scripts/test_raw_abort_probe_device.sh
run_phase raw_abort_write_probe scripts/test_raw_abort_write_probe_device.sh

WARN_AFTER=$(read_warn_count) || fail 'final: could not read warn_count'
assert_clean_device final
printf 'warn_after=%s result=pass\n' "$WARN_AFTER" >> "$MANIFEST"
printf '%s\n' "$MANIFEST"
