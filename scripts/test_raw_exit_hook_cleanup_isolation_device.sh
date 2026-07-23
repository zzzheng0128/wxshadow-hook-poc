#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SERIAL=${ANDROID_SERIAL:-}
MODULE=r0lab-m1
REMOTE=/data/local/tmp/r0lab-m1.kpm
PACKAGE=dev.r0hook.lab
ACTIVITY=dev.r0hook.lab/.MainActivity
TOKEN=${RAW_EXIT_HOOK_CLEANUP_ISOLATION_TOKEN:-0x729249}
EVIDENCE_DIR="$ROOT/build/evidence"
EVIDENCE="$EVIDENCE_DIR/raw-exit-hook-cleanup-isolation-$(date +%Y%m%d-%H%M%S).log"
MODULE_LOADED=0
SESSION_OPEN=0
HOLD_ACTIVE=0
SLOT0_CLEARED=0
SLOT1_CLEARED=0

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

app_logs() {
  adb_device logcat -d -v brief -s R0Lab:I '*:S' | LC_ALL=C tr -d '\r'
}

run_app_command() {
  command=$1
  output=''
  attempt=0
  adb_device logcat -c >/dev/null 2>&1 || true
  adb_device shell "am start -W -n $ACTIVITY --es r0lab_command '$command'" \
    >/dev/null 2>&1 || true
  while [ "$attempt" -lt 40 ]; do
    sleep 0.25
    output=$(app_logs || true)
    case "$output" in
      *"command=$command"*)
        printf '%s\n' "$output"
        return 0
        ;;
    esac
    attempt=$((attempt + 1))
  done
  if [ -n "$output" ]; then
    printf '%s\n' "$output"
  fi
  printf 'command_timeout command=%s wait_ms=10000\n' "$command"
}

boot_id() {
  adb_device shell cat /proc/sys/kernel/random/boot_id 2>/dev/null |
    LC_ALL=C tr -d '\r'
}

require_boot_id() {
  phase=$1
  value=$(boot_id || true)
  [ -n "$value" ] || fail "empty boot id before $phase"
  printf '%s\n' "$value"
}

boot_props() {
  boot_completed=$(adb_device shell getprop sys.boot_completed | LC_ALL=C tr -d '\r')
  bootreason=$(adb_device shell getprop ro.boot.bootreason | LC_ALL=C tr -d '\r')
  printf 'boot_completed=%s bootreason=%s\n' "$boot_completed" "$bootreason"
}

wait_for_boot_completed() {
  attempt=0
  adb_device wait-for-device >/dev/null 2>&1 || true
  while [ "$attempt" -lt 120 ]; do
    completed=$(adb_device shell getprop sys.boot_completed 2>/dev/null |
      LC_ALL=C tr -d '\r' || true)
    [ "$completed" = 1 ] && return 0
    attempt=$((attempt + 1))
    sleep 1
  done
  return 1
}

capture_pstore() {
  stem=$1
  wait_for_boot_completed >/dev/null 2>&1 || adb_device wait-for-device >/dev/null 2>&1 || true
  adb_device shell su -c 'cat /sys/fs/pstore/console-ramoops-0' \
    > "$EVIDENCE_DIR/$stem.console-ramoops-0.txt" 2>/dev/null || true
  adb_device shell su -c 'cat /sys/fs/pstore/pmsg-ramoops-0' \
    > "$EVIDENCE_DIR/$stem.pmsg-ramoops-0.txt" 2>/dev/null || true
}

fail() {
  printf '%s\n' "raw exit-hook cleanup-isolation failure: $*" >&2
  exit 1
}

require_contains() {
  haystack=$1
  needle=$2
  printf '%s\n' "$haystack" | LC_ALL=C grep -F -- "$needle" >/dev/null ||
    fail "expected output not found: $needle"
}

require_boot_stable() {
  phase=$1
  before=$2
  [ -n "$before" ] || fail "empty boot id before $phase"
  adb_device wait-for-device >/dev/null 2>&1 || true
  after=$(boot_id || true)
  props=$(boot_props || true)
  printf 'phase=%s boot_before=%s boot_after=%s %s\n' \
    "$phase" "$before" "$after" "$props" >> "$EVIDENCE"
  if [ -z "$after" ] || [ "$before" != "$after" ]; then
    capture_pstore "$(basename "$EVIDENCE" .log)-$phase"
    fail "device rebooted during $phase: $before -> $after"
  fi
}

wait_for_slot_cleared() {
  token=$1
  slot=$2
  attempt=0
  while [ "$attempt" -lt 40 ]; do
    output=$(run_app_command "raw slot cleared $token $slot")
    case "$output" in
      *"raw_slot_cleared slot=$slot"*)
        printf '%s\n' "$output"
        return 0
        ;;
    esac
    attempt=$((attempt + 1))
    sleep 0.25
  done
  printf '%s\n' "$output"
  return 1
}

prepare_worker_shutdown() {
  WORKER_SHUTDOWN_OUTPUT=$(run_app_command 'workers shutdown') || return 1
  case "$WORKER_SHUTDOWN_OUTPUT" in
    *"workers_shutdown_requested"*) ;;
    *) return 1 ;;
  esac

  attempt=0
  while [ "$attempt" -lt 20 ]; do
    WORKER_STATUS_OUTPUT=$(run_app_command status) || return 1
    case "$WORKER_STATUS_OUTPUT" in
      *"workers_live=0"*"workers_shutdown=1"*) return 0 ;;
    esac
    attempt=$((attempt + 1))
    sleep 0.1
  done
  return 1
}

cleanup() {
  status=$?
  trap - EXIT INT TERM
  if [ "$status" -ne 0 ] && [ "$HOLD_ACTIVE" -eq 1 ]; then
    printf '%s\n' \
      "raw exit-hook cleanup-isolation preserving module: active hold state remains and cleanup reentry is forbidden" >&2
    exit "$status"
  fi
  if [ "$status" -ne 0 ] && [ "$SESSION_OPEN" -eq 1 ]; then
    run_app_command "close $TOKEN" >/dev/null 2>&1 || true
    SESSION_OPEN=0
  fi
  if [ "$MODULE_LOADED" -eq 1 ] && [ "$status" -ne 0 ]; then
    CLEANUP_STATUS=$(run_app_command status 2>&1 || true)
    case "$CLEANUP_STATUS" in
      *"active=0"*"raw_slots=0"*"page_records=0"*)
        if prepare_worker_shutdown >/dev/null 2>&1; then
          supercmd module unload "$MODULE" >/dev/null 2>&1 || true
        fi
        ;;
      *)
        printf '%s\n' \
          "raw exit-hook cleanup-isolation cleanup did not confirm empty state; preserving module" >&2
        ;;
    esac
  fi
  exit "$status"
}

trap cleanup EXIT INT TERM

mkdir -p "$EVIDENCE_DIR"
"$ROOT/scripts/build_kpm.sh" >/dev/null
"$ROOT/scripts/build_lab_app.sh" >/dev/null

EXISTING=$(supercmd module list 2>&1) || fail "module list failed: $EXISTING"
[ -z "$EXISTING" ] || fail "refusing to replace a resident KPM: $EXISTING"

adb_device install -r "$ROOT/build/lab-app/r0lab-debug.apk" >/dev/null
adb_device push "$ROOT/kpm/build/r0lab-m1.kpm" "$REMOTE" >/dev/null
LAB_UID=$(adb_device shell "cmd package list packages -U $PACKAGE" |
  LC_ALL=C tr -d '\r' | sed -n 's/.* uid:\([0-9][0-9]*\).*/\1/p')
[ -n "$LAB_UID" ] || fail "Lab App UID not found"
WARN_BEFORE=$(adb_device shell su -c cat /sys/kernel/warn_count | LC_ALL=C tr -d '\r')
BOOT_START=$(require_boot_id start)
printf 'serial=%s lab_uid=%s token=%s warn_before=%s boot_start=%s %s\n' \
  "${SERIAL:-default}" "$LAB_UID" "$TOKEN" "$WARN_BEFORE" "$BOOT_START" \
  "$(boot_props)" | tee "$EVIDENCE"

LOAD_OUTPUT=$(supercmd module load "$REMOTE" "lab_uid=$LAB_UID" 2>&1) ||
  fail "module load failed: $LOAD_OUTPUT"
case "$LOAD_OUTPUT" in
  *"supercmd error code"*) fail "module load rejected: $LOAD_OUTPUT" ;;
esac
MODULE_LOADED=1
printf 'module_load=%s\n' "$LOAD_OUTPUT" >> "$EVIDENCE"

STATUS_INITIAL=$(run_app_command status)
require_contains "$STATUS_INITIAL" 'version=5'
require_contains "$STATUS_INITIAL" 'active=0'
require_contains "$STATUS_INITIAL" 'raw_slots=0'
require_contains "$STATUS_INITIAL" 'page_records=0'
printf '%s\n' "$STATUS_INITIAL" >> "$EVIDENCE"

ARM_OUTPUT=$(run_app_command "arm $TOKEN")
require_contains "$ARM_OUTPUT" 'armed uid='
SESSION_OPEN=1
printf '%s\n' "$ARM_OUTPUT" >> "$EVIDENCE"

RAW_OUTPUT=$(run_app_command "raw exit hook routing hold $TOKEN")
printf '%s\n' "$RAW_OUTPUT" >> "$EVIDENCE"
require_contains "$RAW_OUTPUT" 'raw mode=exit-hook-routing-hold failures=0'
require_contains "$RAW_OUTPUT" 'target_mm_scoped=1'
require_contains "$RAW_OUTPUT" 'page_record_routed=1'
require_contains "$RAW_OUTPUT" 'cleanup=monitor'
require_contains "$RAW_OUTPUT" 'generation='
HOLD_ACTIVE=1
GENERATIONS=$(printf '%s\n' "$RAW_OUTPUT" |
  sed -n 's/.*generation=\([0-9][0-9]*\)\/\([0-9][0-9]*\).*/\1 \2/p' |
  sed -n '1p')
[ -n "$GENERATIONS" ] || fail "could not parse routing hold generations"
set -- $GENERATIONS
GEN0=$1
GEN1=$2

STATUS_HELD=$(run_app_command status)
require_contains "$STATUS_HELD" 'active=1'
require_contains "$STATUS_HELD" 'raw_slots=2'
require_contains "$STATUS_HELD" 'page_records=2'
printf '%s\n' "$STATUS_HELD" >> "$EVIDENCE"

BOOT_BEFORE_EXIT_CLEAR=$(require_boot_id d4_r2_exit_hook_clear_only)
HOOK0_CLEAR=$(run_app_command "raw slot exit hook clear $TOKEN 0 $GEN0")
HOOK1_CLEAR=$(run_app_command "raw slot exit hook clear $TOKEN 1 $GEN1")
require_contains "$HOOK0_CLEAR" 'raw_slot_exit_hook_cleared slot=0'
require_contains "$HOOK1_CLEAR" 'raw_slot_exit_hook_cleared slot=1'
printf '%s\n' "$HOOK0_CLEAR" >> "$EVIDENCE"
printf '%s\n' "$HOOK1_CLEAR" >> "$EVIDENCE"
HOOK0_STATUS_AFTER_CLEAR=$(run_app_command "raw slot exit hook status $TOKEN 0")
HOOK1_STATUS_AFTER_CLEAR=$(run_app_command "raw slot exit hook status $TOKEN 1")
require_contains "$HOOK0_STATUS_AFTER_CLEAR" 'installed=0'
require_contains "$HOOK1_STATUS_AFTER_CLEAR" 'installed=0'
printf '%s\n' "$HOOK0_STATUS_AFTER_CLEAR" >> "$EVIDENCE"
printf '%s\n' "$HOOK1_STATUS_AFTER_CLEAR" >> "$EVIDENCE"
STATUS_AFTER_EXIT_CLEAR=$(run_app_command status)
require_contains "$STATUS_AFTER_EXIT_CLEAR" 'active=1'
require_contains "$STATUS_AFTER_EXIT_CLEAR" 'raw_slots=2'
require_contains "$STATUS_AFTER_EXIT_CLEAR" 'page_records=2'
printf '%s\n' "$STATUS_AFTER_EXIT_CLEAR" >> "$EVIDENCE"
require_boot_stable d4_r2_exit_hook_clear_only "$BOOT_BEFORE_EXIT_CLEAR"
printf 'phase=d4_r2_exit_hook_clear_only result=pass boot_stable=1 raw_slots=2 page_records=2 installed=0/0 cleanup_reentry=0\n' >> "$EVIDENCE"

BOOT_BEFORE_SLOT0_CLEAR=$(require_boot_id d4_r2_slot0_clear_non_reentrant)
SLOT0_CLEAR=$(run_app_command "raw slot clear $TOKEN 0")
require_contains "$SLOT0_CLEAR" 'raw_slot_clear_pending slot=0'
printf '%s\n' "$SLOT0_CLEAR" >> "$EVIDENCE"
SLOT0_CLEARED_OUTPUT=$(wait_for_slot_cleared "$TOKEN" 0) ||
  fail "slot 0 did not report cleared"
SLOT0_CLEARED=1
printf '%s\n' "$SLOT0_CLEARED_OUTPUT" >> "$EVIDENCE"
STATUS_AFTER_SLOT0_CLEAR=$(run_app_command status)
require_contains "$STATUS_AFTER_SLOT0_CLEAR" 'active=1'
require_contains "$STATUS_AFTER_SLOT0_CLEAR" 'raw_slots=1'
require_contains "$STATUS_AFTER_SLOT0_CLEAR" 'page_records=1'
printf '%s\n' "$STATUS_AFTER_SLOT0_CLEAR" >> "$EVIDENCE"
require_boot_stable d4_r2_slot0_clear_non_reentrant "$BOOT_BEFORE_SLOT0_CLEAR"
printf 'phase=d4_r2_slot0_clear_non_reentrant result=pass boot_stable=1 raw_slot_cleared=0 cleanup_reentry=0 raw_slots=1 page_records=1\n' >> "$EVIDENCE"

BOOT_BEFORE_SLOT1_CLEAR=$(require_boot_id d4_r2_slot1_clear_non_reentrant)
SLOT1_CLEAR=$(run_app_command "raw slot clear $TOKEN 1")
require_contains "$SLOT1_CLEAR" 'raw_slot_clear_pending slot=1'
printf '%s\n' "$SLOT1_CLEAR" >> "$EVIDENCE"
SLOT1_CLEARED_OUTPUT=$(wait_for_slot_cleared "$TOKEN" 1) ||
  fail "slot 1 did not report cleared"
SLOT1_CLEARED=1
printf '%s\n' "$SLOT1_CLEARED_OUTPUT" >> "$EVIDENCE"
STATUS_AFTER_SLOT1_CLEAR=$(run_app_command status)
require_contains "$STATUS_AFTER_SLOT1_CLEAR" 'active=1'
require_contains "$STATUS_AFTER_SLOT1_CLEAR" 'raw_slots=0'
require_contains "$STATUS_AFTER_SLOT1_CLEAR" 'page_records=0'
printf '%s\n' "$STATUS_AFTER_SLOT1_CLEAR" >> "$EVIDENCE"
require_boot_stable d4_r2_slot1_clear_non_reentrant "$BOOT_BEFORE_SLOT1_CLEAR"
printf 'phase=d4_r2_slot1_clear_non_reentrant result=pass boot_stable=1 raw_slot_cleared=1 cleanup_reentry=0 raw_slots=0 page_records=0\n' >> "$EVIDENCE"
HOLD_ACTIVE=0

CLOSE_OUTPUT=$(run_app_command "close $TOKEN")
require_contains "$CLOSE_OUTPUT" 'closed final_summary_seq='
SESSION_OPEN=0
printf '%s\n' "$CLOSE_OUTPUT" >> "$EVIDENCE"
STATUS_AFTER_CLOSE=$(run_app_command status)
require_contains "$STATUS_AFTER_CLOSE" 'active=0'
require_contains "$STATUS_AFTER_CLOSE" 'raw_slots=0'
require_contains "$STATUS_AFTER_CLOSE" 'page_records=0'
printf '%s\n' "$STATUS_AFTER_CLOSE" >> "$EVIDENCE"

prepare_worker_shutdown || fail "workers did not stop before module unload"
printf '%s\n' "$WORKER_SHUTDOWN_OUTPUT" >> "$EVIDENCE"
printf '%s\n' "$WORKER_STATUS_OUTPUT" >> "$EVIDENCE"
UNLOAD_OUTPUT=$(supercmd module unload "$MODULE" 2>&1) ||
  fail "module unload failed: $UNLOAD_OUTPUT"
case "$UNLOAD_OUTPUT" in
  *"supercmd error code"*) fail "module unload rejected: $UNLOAD_OUTPUT" ;;
esac
MODULE_LOADED=0
FINAL_MODULES=$(supercmd module list 2>&1) || fail "final list failed: $FINAL_MODULES"
[ -z "$FINAL_MODULES" ] || fail "module remained loaded: $FINAL_MODULES"
WARN_AFTER=$(adb_device shell su -c cat /sys/kernel/warn_count | LC_ALL=C tr -d '\r')
[ "$WARN_BEFORE" = "$WARN_AFTER" ] ||
  fail "warn_count changed: $WARN_BEFORE -> $WARN_AFTER"
BOOT_END=$(require_boot_id final)
[ "$BOOT_START" = "$BOOT_END" ] ||
  fail "boot id changed across cleanup isolation: $BOOT_START -> $BOOT_END"
printf 'raw_exit_hook_cleanup_isolation=pass phases=4 slot0_cleared=%s slot1_cleared=%s warn_after=%s boot_end=%s final_modules=empty result=pass\n' \
  "$SLOT0_CLEARED" "$SLOT1_CLEARED" "$WARN_AFTER" "$BOOT_END" | tee -a "$EVIDENCE"
printf '%s\n' "$EVIDENCE"
