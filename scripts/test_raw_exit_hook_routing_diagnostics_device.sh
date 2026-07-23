#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SERIAL=${ANDROID_SERIAL:-}
MODULE=r0lab-m1
REMOTE=/data/local/tmp/r0lab-m1.kpm
PACKAGE=dev.r0hook.lab
ACTIVITY=dev.r0hook.lab/.MainActivity
TOKEN_NO_HOOK=${RAW_EXIT_HOOK_DIAG_NO_HOOK_TOKEN:-0x729247}
TOKEN_HOLD=${RAW_EXIT_HOOK_DIAG_HOLD_TOKEN:-0x729248}
ALLOW_RERUN=${RAW_EXIT_HOOK_DIAG_ALLOW_D4_R1_RERUN:-0}
EVIDENCE_DIR="$ROOT/build/evidence"
EVIDENCE="$EVIDENCE_DIR/raw-exit-hook-routing-diagnostics-$(date +%Y%m%d-%H%M%S).log"
MODULE_LOADED=0
CLEANUP_TOKEN=''
HOLD_CLEANUP=0

if [ "$ALLOW_RERUN" != "1" ]; then
  printf '%s\n' \
    "raw exit-hook-routing diagnostics locked after D4-R1-explicit-clear-cleanup-reentry-panic; plan and run D4-R2 isolation instead" >&2
  printf '%s\n' \
    "set RAW_EXIT_HOOK_DIAG_ALLOW_D4_R1_RERUN=1 only for evidence-preserving archaeology, not normal development" >&2
  exit 2
fi

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
  adb_device logcat -c >/dev/null
  adb_device shell "am start -W -n $ACTIVITY --es r0lab_command '$command'" >/dev/null
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

boot_props() {
  boot_completed=$(adb_device shell getprop sys.boot_completed | LC_ALL=C tr -d '\r')
  bootreason=$(adb_device shell getprop ro.boot.bootreason | LC_ALL=C tr -d '\r')
  printf 'boot_completed=%s bootreason=%s\n' "$boot_completed" "$bootreason"
}

fail() {
  printf '%s\n' "raw exit-hook-routing diagnostics failure: $*" >&2
  exit 1
}

require_contains() {
  haystack=$1
  needle=$2
  printf '%s\n' "$haystack" | LC_ALL=C grep -F -- "$needle" >/dev/null ||
    fail "expected output not found: $needle"
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

parse_exit_hook_generation() {
  slot=$1
  status_output=$2
  printf '%s\n' "$status_output" |
    sed -n "s/.*raw_slot_exit_hook_status slot=$slot generation=\\([0-9][0-9]*\\).*/\\1/p" |
    sed -n '1p'
}

diagnostic_clear_hold_state() {
  token=$1
  for slot in 0 1; do
    status_output=$(run_app_command "raw slot exit hook status $token $slot" 2>/dev/null || true)
    generation=$(parse_exit_hook_generation "$slot" "$status_output")
    if [ -n "$generation" ]; then
      run_app_command "raw slot exit hook clear $token $slot $generation" >/dev/null 2>&1 || true
    fi
  done
  for slot in 0 1; do
    run_app_command "raw slot clear $token $slot" >/dev/null 2>&1 || true
    wait_for_slot_cleared "$token" "$slot" >/dev/null 2>&1 || true
  done
}

force_stop_and_require_stable_boot() {
  phase=$1
  before=$(boot_id)
  adb_device shell am force-stop "$PACKAGE" >/dev/null
  sleep 2
  adb_device wait-for-device >/dev/null
  after=$(boot_id)
  props=$(boot_props)
  printf 'phase=%s force_stop=done boot_before=%s boot_after=%s %s\n' \
    "$phase" "$before" "$after" "$props" >> "$EVIDENCE"
  [ "$before" = "$after" ] ||
    fail "device rebooted during $phase force-stop: $before -> $after"
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
  if [ "$MODULE_LOADED" -eq 1 ]; then
    if [ -n "$CLEANUP_TOKEN" ]; then
      if [ "$HOLD_CLEANUP" -eq 1 ]; then
        diagnostic_clear_hold_state "$CLEANUP_TOKEN"
      fi
      run_app_command "close $CLEANUP_TOKEN" >/dev/null 2>&1 || true
      CLEANUP_TOKEN=''
      HOLD_CLEANUP=0
    fi
    adb_device shell am force-stop "$PACKAGE" >/dev/null 2>&1 || true
    CLEANUP_STATUS=$(run_app_command status 2>&1 || true)
    case "$CLEANUP_STATUS" in
      *"active=0"*"raw_slots=0"*"page_records=0"*)
        if prepare_worker_shutdown >/dev/null 2>&1; then
          supercmd module unload "$MODULE" >/dev/null 2>&1 || true
        else
          printf '%s\n' \
            "raw exit-hook-routing diagnostics could not stop workers; preserving module" >&2
        fi
        ;;
      *)
        printf '%s\n' \
          "raw exit-hook-routing diagnostics cleanup did not confirm empty state; preserving module" >&2
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
LAB_UID=$(adb_device shell "cmd package list packages -U $PACKAGE" |
  LC_ALL=C tr -d '\r' | sed -n 's/.* uid:\([0-9][0-9]*\).*/\1/p')
[ -n "$LAB_UID" ] || fail "Lab App UID not found"
WARN_BEFORE=$(adb_device shell su -c cat /sys/kernel/warn_count | LC_ALL=C tr -d '\r')
BOOT_START=$(boot_id)
printf 'serial=%s lab_uid=%s token_no_hook=%s token_hold=%s warn_before=%s boot_start=%s %s\n' \
  "${SERIAL:-default}" "$LAB_UID" "$TOKEN_NO_HOOK" "$TOKEN_HOLD" \
  "$WARN_BEFORE" "$BOOT_START" "$(boot_props)" | tee "$EVIDENCE"

DESCRIBE_OUTPUT=$(run_app_command describe)
require_contains "$DESCRIBE_OUTPUT" 'marker_value=62'
printf '%s\n' "$DESCRIBE_OUTPUT" >> "$EVIDENCE"
force_stop_and_require_stable_boot no_kpm_force_stop
printf 'phase=no_kpm_force_stop result=pass boot_stable=1\n' >> "$EVIDENCE"

adb_device push "$ROOT/kpm/build/r0lab-m1.kpm" "$REMOTE" >/dev/null
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
force_stop_and_require_stable_boot kpm_loaded_no_session_force_stop
STATUS_AFTER_NO_SESSION=$(run_app_command status)
require_contains "$STATUS_AFTER_NO_SESSION" 'active=0'
require_contains "$STATUS_AFTER_NO_SESSION" 'raw_slots=0'
require_contains "$STATUS_AFTER_NO_SESSION" 'page_records=0'
printf '%s\n' "$STATUS_AFTER_NO_SESSION" >> "$EVIDENCE"
printf 'phase=kpm_loaded_no_session_force_stop result=pass boot_stable=1\n' >> "$EVIDENCE"

ARM_NO_HOOK_OUTPUT=$(run_app_command "arm $TOKEN_NO_HOOK")
require_contains "$ARM_NO_HOOK_OUTPUT" 'armed uid='
CLEANUP_TOKEN=$TOKEN_NO_HOOK
printf '%s\n' "$ARM_NO_HOOK_OUTPUT" >> "$EVIDENCE"
force_stop_and_require_stable_boot kpm_session_no_exit_hook_force_stop
CLEANUP_TOKEN=''
STATUS_AFTER_NO_HOOK=$(run_app_command status)
require_contains "$STATUS_AFTER_NO_HOOK" 'active=0'
require_contains "$STATUS_AFTER_NO_HOOK" 'raw_slots=0'
require_contains "$STATUS_AFTER_NO_HOOK" 'page_records=0'
printf '%s\n' "$STATUS_AFTER_NO_HOOK" >> "$EVIDENCE"
EVENTS_NO_HOOK=$(run_app_command "events $TOKEN_NO_HOOK")
printf '%s\n' "$EVENTS_NO_HOOK" >> "$EVIDENCE"
require_contains "$EVENTS_NO_HOOK" 'op=5 result=0'
require_contains "$EVENTS_NO_HOOK" 'op=7 result=0'
printf 'phase=kpm_session_no_exit_hook_force_stop result=pass boot_stable=1 cleanup=monitor target_exit_event=not_required_without_page_records\n' >> "$EVIDENCE"

ARM_HOLD_OUTPUT=$(run_app_command "arm $TOKEN_HOLD")
require_contains "$ARM_HOLD_OUTPUT" 'armed uid='
CLEANUP_TOKEN=$TOKEN_HOLD
printf '%s\n' "$ARM_HOLD_OUTPUT" >> "$EVIDENCE"
RAW_OUTPUT=$(run_app_command "raw exit hook routing hold $TOKEN_HOLD")
require_contains "$RAW_OUTPUT" 'raw mode=exit-hook-routing-hold failures=0'
require_contains "$RAW_OUTPUT" 'page_record_routed=1'
require_contains "$RAW_OUTPUT" 'generation='
HOLD_CLEANUP=1
printf '%s\n' "$RAW_OUTPUT" >> "$EVIDENCE"
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

HOOK0_CLEAR=$(run_app_command "raw slot exit hook clear $TOKEN_HOLD 0 $GEN0")
HOOK1_CLEAR=$(run_app_command "raw slot exit hook clear $TOKEN_HOLD 1 $GEN1")
require_contains "$HOOK0_CLEAR" 'raw_slot_exit_hook_cleared slot=0'
require_contains "$HOOK1_CLEAR" 'raw_slot_exit_hook_cleared slot=1'
printf '%s\n' "$HOOK0_CLEAR" >> "$EVIDENCE"
printf '%s\n' "$HOOK1_CLEAR" >> "$EVIDENCE"
SLOT0_CLEAR=$(run_app_command "raw slot clear $TOKEN_HOLD 0")
require_contains "$SLOT0_CLEAR" 'raw_slot_clear_pending slot=0'
printf '%s\n' "$SLOT0_CLEAR" >> "$EVIDENCE"
SLOT0_CLEARED=$(wait_for_slot_cleared "$TOKEN_HOLD" 0) ||
  fail "slot 0 did not report cleared"
SLOT1_CLEAR=$(run_app_command "raw slot clear $TOKEN_HOLD 1")
require_contains "$SLOT1_CLEAR" 'raw_slot_clear_pending slot=1'
printf '%s\n' "$SLOT1_CLEAR" >> "$EVIDENCE"
SLOT1_CLEARED=$(wait_for_slot_cleared "$TOKEN_HOLD" 1) ||
  fail "slot 1 did not report cleared"
printf '%s\n' "$SLOT0_CLEARED" >> "$EVIDENCE"
printf '%s\n' "$SLOT1_CLEARED" >> "$EVIDENCE"
STATUS_AFTER_EXPLICIT_CLEAR=$(run_app_command status)
require_contains "$STATUS_AFTER_EXPLICIT_CLEAR" 'active=1'
require_contains "$STATUS_AFTER_EXPLICIT_CLEAR" 'raw_slots=0'
require_contains "$STATUS_AFTER_EXPLICIT_CLEAR" 'page_records=0'
printf '%s\n' "$STATUS_AFTER_EXPLICIT_CLEAR" >> "$EVIDENCE"
CLOSE_HOLD=$(run_app_command "close $TOKEN_HOLD")
require_contains "$CLOSE_HOLD" 'closed final_summary_seq='
CLEANUP_TOKEN=''
HOLD_CLEANUP=0
printf '%s\n' "$CLOSE_HOLD" >> "$EVIDENCE"
force_stop_and_require_stable_boot d3_hold_explicit_clear_then_force_stop
printf 'phase=d3_hold_explicit_clear_then_force_stop result=pass boot_stable=1 explicit_clear=1 exit_mmap_owner_cleanup=not_exercised\n' >> "$EVIDENCE"

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
BOOT_END=$(boot_id)
[ "$BOOT_START" = "$BOOT_END" ] ||
  fail "boot id changed across diagnostics: $BOOT_START -> $BOOT_END"
printf 'raw_exit_hook_routing_diagnostics=pass phases=4 warn_after=%s boot_end=%s final_modules=empty result=pass\n' \
  "$WARN_AFTER" "$BOOT_END" | tee -a "$EVIDENCE"
printf '%s\n' "$EVIDENCE"
