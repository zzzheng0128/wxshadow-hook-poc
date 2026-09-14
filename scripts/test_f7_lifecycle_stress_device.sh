#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SERIAL=${ANDROID_SERIAL:-}
EXPECTED_SERIAL=${F7_EXPECTED_SERIAL:-32250DLH2000Z3}
F7_REPEAT_COUNT=${F7_REPEAT_COUNT:-1}
M0_LOOPS=${M0_LOOPS:-100}
LAB_PACKAGE=dev.r0hook.lab
EVIDENCE_DIR="$ROOT/build/evidence"
RUN_ID=$(date -u +%Y%m%dT%H%M%SZ)
. "$ROOT/scripts/lib/evidence_run.sh"
. "$ROOT/scripts/lib/device_snapshot.sh"

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
  classification=$1
  shift
  reason=$*
  printf '%s\n' "F7 lifecycle stress failure: $classification: $reason" >&2
  printf 'result=fail classification=%s reason=%s\n' \
    "$classification" "$reason" >> "$MANIFEST"
  exit 1
}

assert_device_continuity() {
  r0lab_device_snapshot ||
    fail device_transport_or_reboot "$1: could not capture a consistent device snapshot"
  "$ROOT/scripts/verify_run_continuity.sh" \
    "$BOOT_BEFORE" "$R0LAB_SNAPSHOT_BOOT" "$WARN_BEFORE" "$R0LAB_SNAPSHOT_WARN" ||
    fail device_continuity_changed "$1: device rebooted or warn_count changed"
}

require_positive_integer() {
  name=$1
  value=$2
  case "$value" in
    ''|*[!0-9]*)
      fail static_contract_regression "$name must be a positive integer: $value"
      ;;
  esac
  [ "$value" -ge 1 ] ||
    fail static_contract_regression "$name must be at least 1: $value"
}

assert_expected_serial() {
  [ -n "$SERIAL" ] ||
    fail device_transport_or_reboot 'ANDROID_SERIAL is required for F7'
  if [ "$EXPECTED_SERIAL" != any ] && [ "$SERIAL" != "$EXPECTED_SERIAL" ]; then
    fail device_transport_or_reboot \
      "ANDROID_SERIAL=$SERIAL does not match F7_EXPECTED_SERIAL=$EXPECTED_SERIAL"
  fi
}

assert_clean_device() {
  phase=$1
  assert_device_continuity "$phase"
  modules=$(supercmd module list 2>&1) ||
    fail device_transport_or_reboot "$phase: module list failed: $modules"
  [ -z "$modules" ] ||
    fail module_residue_after_child "$phase: resident KPM remains: $modules"
  assert_device_continuity "$phase"
  printf 'phase=%s modules=empty boot_id=%s warn_count=%s\n' \
    "$phase" "$R0LAB_SNAPSHOT_BOOT" "$R0LAB_SNAPSHOT_WARN" >> "$MANIFEST"
}

run_child() {
  phase=$1
  script=$2
  classification=$3
  phase_log="$RUN_DIR/$phase.log"
  EVIDENCE_RUN_PHASE=$phase
  assert_device_continuity "$phase: before run"

  adb_device shell am force-stop "$LAB_PACKAGE" >/dev/null 2>&1 ||
    fail device_transport_or_reboot "$phase: could not reset Lab app process"
  printf 'phase=%s script=%s status=running\n' "$phase" "$script" >> "$MANIFEST"

  if [ "$script" = scripts/test_v1_device.sh ]; then
    if ! ANDROID_SERIAL="$SERIAL" M0_LOOPS="$M0_LOOPS" \
      "$ROOT/$script" >"$phase_log" 2>&1; then
      sed -n '1,240p' "$phase_log" >&2
      fail "$classification" "$phase: script failed"
    fi
  elif ! ANDROID_SERIAL="$SERIAL" "$ROOT/$script" >"$phase_log" 2>&1; then
    sed -n '1,240p' "$phase_log" >&2
    fail "$classification" "$phase: script failed"
  fi

  phase_evidence=$(tail -n 1 "$phase_log" | tr -d '\r')
  case "$phase_evidence" in
    "$ROOT"/build/evidence/*.log|"$ROOT"/build/evidence/*/manifest.log) ;;
    *)
      fail "$classification" \
        "$phase: final output is not an evidence path: $phase_evidence"
      ;;
  esac
  [ -f "$phase_evidence" ] ||
    fail "$classification" "$phase: evidence file does not exist: $phase_evidence"
  "$ROOT/scripts/verify_evidence_result.sh" "$phase_evidence" ||
    fail "$classification" "$phase: evidence does not end with an unambiguous result=pass"

  printf 'phase=%s script=%s status=pass evidence=%s raw_log=%s\n' \
    "$phase" "$script" "$phase_evidence" "$phase_log" >> "$MANIFEST"
  assert_clean_device "$phase"
}

evidence_run_init "$EVIDENCE_DIR" f7-lifecycle-stress "$RUN_ID"
require_positive_integer F7_REPEAT_COUNT "$F7_REPEAT_COUNT"
require_positive_integer M0_LOOPS "$M0_LOOPS"
assert_expected_serial

"$ROOT/scripts/verify_v1_contract.sh" >"$RUN_DIR/static-contract.log" 2>&1 || {
  sed -n '1,240p' "$RUN_DIR/static-contract.log" >&2
  printf 'result=fail classification=static_contract_regression reason=static_contract\n' \
    > "$MANIFEST"
  exit 1
}

r0lab_device_ready ||
  fail device_transport_or_reboot 'device is unavailable'

r0lab_device_snapshot ||
  fail device_transport_or_reboot 'could not capture initial device snapshot'
BOOT_BEFORE=$R0LAB_SNAPSHOT_BOOT
WARN_BEFORE=$R0LAB_SNAPSHOT_WARN

modules_before=$(supercmd module list 2>&1) ||
  fail device_transport_or_reboot "module list failed before run: $modules_before"
[ -z "$modules_before" ] ||
  fail resident_kpm_before_run "resident KPM before run: $modules_before"

assert_device_continuity setup

{
  printf 'run_id=%s\n' "$RUN_ID"
  printf 'serial=%s\n' "$SERIAL"
  printf 'expected_serial=%s\n' "$EXPECTED_SERIAL"
  printf 'repeat_count=%s\n' "$F7_REPEAT_COUNT"
  printf 'm0_loops=%s\n' "$M0_LOOPS"
  printf 'boot_before=%s\n' "$BOOT_BEFORE"
  printf 'warn_before=%s\n' "$WARN_BEFORE"
  printf 'static_contract=%s\n' "$RUN_DIR/static-contract.log"
  printf 'child_phases=m5_faults,m5_lifecycle,v1_device\n'
} >> "$MANIFEST"

iteration=1
while [ "$iteration" -le "$F7_REPEAT_COUNT" ]; do
  run_child "m5_faults_$iteration" \
    scripts/test_m5_faults_device.sh m5_fault_rollback_regression
  run_child "m5_lifecycle_$iteration" \
    scripts/test_m5_lifecycle_device.sh m5_owner_exit_regression
  iteration=$((iteration + 1))
done

run_child v1_device scripts/test_v1_device.sh full_runner_regression

EVIDENCE_RUN_PHASE=final
assert_clean_device final
evidence_run_complete "$R0LAB_SNAPSHOT_WARN"
printf '%s\n' "$MANIFEST"
