#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SERIAL=${ANDROID_SERIAL:-}
MODULE=r0lab-m1
REMOTE=/data/local/tmp/r0lab-m1.kpm
PACKAGE=dev.r0hook.lab
ACTIVITY=dev.r0hook.lab/.MainActivity
TOKEN=${RAW_PRCTL_PATCH_RECORDS_TOKEN:-0x729407}
EVIDENCE_DIR="$ROOT/build/evidence"
EVIDENCE="$EVIDENCE_DIR/raw-prctl-patch-records-$(date +%Y%m%d-%H%M%S).log"
MODULE_LOADED=0
SESSION_OPEN=0

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
  adb_device logcat -d -v brief -s R0Lab:I '*:S' | tr -d '\r'
}

run_app_command() {
  command=$1
  adb_device logcat -c >/dev/null
  adb_device shell "am start -W -n $ACTIVITY --es r0lab_command '$command'" >/dev/null
  sleep 1
  app_logs
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

fail() {
  printf '%s\n' "raw prctl patch-records failure: $*" >&2
  exit 1
}

require_contains() {
  haystack=$1
  needle=$2
  printf '%s\n' "$haystack" | grep -F -- "$needle" >/dev/null ||
    fail "expected output not found: $needle"
}

cleanup() {
  status=$?
  cleanup_ok=1
  trap - EXIT INT TERM
  if [ "$SESSION_OPEN" -eq 1 ]; then
    run_app_command "raw prctl hook clear $TOKEN" >/dev/null 2>&1 || true
    run_app_command "raw clear $TOKEN" >/dev/null 2>&1 || true
    CLEANUP_STATUS=$(run_app_command status 2>&1 || true)
    case "$CLEANUP_STATUS" in
      *"raw_slots=0"*) ;;
      *)
        printf '%s\n' "raw prctl patch-records cleanup did not confirm an empty raw slot; preserving module" >&2
        cleanup_ok=0
        ;;
    esac
    if [ "$cleanup_ok" -eq 1 ]; then
      run_app_command "close $TOKEN" >/dev/null 2>&1 || cleanup_ok=0
    fi
  fi
  if [ "$MODULE_LOADED" -eq 1 ] && [ "$cleanup_ok" -eq 1 ]; then
    if prepare_worker_shutdown >/dev/null 2>&1; then
      supercmd module unload "$MODULE" >/dev/null 2>&1 || true
    else
      printf '%s\n' "raw prctl patch-records cleanup could not stop workers; preserving module" >&2
    fi
  elif [ "$MODULE_LOADED" -eq 1 ]; then
    printf '%s\n' "raw prctl patch-records cleanup incomplete; module intentionally left resident" >&2
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
  tr -d '\r' | sed -n 's/.* uid:\([0-9][0-9]*\).*/\1/p')
[ -n "$LAB_UID" ] || fail "Lab App UID not found"
WARN_BEFORE=$(adb_device shell su -c cat /sys/kernel/warn_count | tr -d '\r')
printf 'serial=%s lab_uid=%s token=%s warn_before=%s\n' \
  "${SERIAL:-default}" "$LAB_UID" "$TOKEN" "$WARN_BEFORE" | tee "$EVIDENCE"

LOAD_OUTPUT=$(supercmd module load "$REMOTE" "lab_uid=$LAB_UID" 2>&1) ||
  fail "module load failed: $LOAD_OUTPUT"
case "$LOAD_OUTPUT" in
  *"supercmd error code"*) fail "module load rejected: $LOAD_OUTPUT" ;;
esac
MODULE_LOADED=1
printf 'module_load=%s\n' "$LOAD_OUTPUT" | tee -a "$EVIDENCE"

STATUS_INITIAL=$(run_app_command status)
require_contains "$STATUS_INITIAL" 'version=5'
require_contains "$STATUS_INITIAL" 'raw_slots=0'
printf '%s\n' "$STATUS_INITIAL" >> "$EVIDENCE"

ARM_OUTPUT=$(run_app_command "arm $TOKEN")
require_contains "$ARM_OUTPUT" 'armed uid='
SESSION_OPEN=1
printf '%s\n' "$ARM_OUTPUT" >> "$EVIDENCE"

RAW_OUTPUT=$(run_app_command "raw prctl patch records run $TOKEN")
printf '%s\n' "$RAW_OUTPUT" >> "$EVIDENCE"
require_contains "$RAW_OUTPUT" 'raw mode=prctl-patch-records failures=0'
require_contains "$RAW_OUTPUT" 'operations=4,5'
require_contains "$RAW_OUTPUT" 'patch_scope=single_page_ranges'
require_contains "$RAW_OUTPUT" 'patch_capacity=1024'
require_contains "$RAW_OUTPUT" 'overlap=version_last_write_wins'
require_contains "$RAW_OUTPUT" 'rebuild=original_seed_active_records'
require_contains "$RAW_OUTPUT" 'release=exact_start'
require_contains "$RAW_OUTPUT" 'dirty_tracking=byte_bitmap'
require_contains "$RAW_OUTPUT" 'user_copy=copy_from_user_nofault'
require_contains "$RAW_OUTPUT" 'cache_sync=sync_icache_aliases'
require_contains "$RAW_OUTPUT" 'apply_a_rc=0'
require_contains "$RAW_OUTPUT" 'apply_b_rc=0'
require_contains "$RAW_OUTPUT" 'release_b_rc=0'
require_contains "$RAW_OUTPUT" 'update_a_rc=0'
require_contains "$RAW_OUTPUT" 'invalid_rc=-1'
require_contains "$RAW_OUTPUT" 'invalid_errno=22'
require_contains "$RAW_OUTPUT" 'release_a_rc=0'
require_contains "$RAW_OUTPUT" 'cleanup_patch_rc=0'
require_contains "$RAW_OUTPUT" 'capacity_fill_records=1023'
require_contains "$RAW_OUTPUT" 'capacity_fill_failures=0'
require_contains "$RAW_OUTPUT" 'capacity_overflow_rc=-1'
require_contains "$RAW_OUTPUT" 'capacity_overflow_errno=28'
require_contains "$RAW_OUTPUT" 'capacity_boundary=pass'
require_contains "$RAW_OUTPUT" 'overlap_after_b=pass'
require_contains "$RAW_OUTPUT" 'release_b_rebuild=pass'
require_contains "$RAW_OUTPUT" 'shrink_rebuild=pass'
require_contains "$RAW_OUTPUT" 'invalid_preserved=pass'
require_contains "$RAW_OUTPUT" 'final_original=pass'
require_contains "$RAW_OUTPUT" 'cleanup_active_before_clear=pass'
require_contains "$RAW_OUTPUT" 'active_progress=1,2,1,1,0,1,1024'
require_contains "$RAW_OUTPUT" 'dirty_progress=8,12,8,4,0,4,1027'
require_contains "$RAW_OUTPUT" 'slots_progress=1,2,2,2,2,2,1024'
require_contains "$RAW_OUTPUT" 'normal_value=42'
require_contains "$RAW_OUTPUT" 'shadow_value=99'
require_contains "$RAW_OUTPUT" 'restored_value=42'
require_contains "$RAW_OUTPUT" 'patch_events=3'
require_contains "$RAW_OUTPUT" 'release_events=2'
require_contains "$RAW_OUTPUT" 'failures=1'
require_contains "$RAW_OUTPUT" 'status_capacity_rc='
require_contains "$RAW_OUTPUT" 'handler_faults=0'

EVENT_OUTPUT=$(run_app_command "events $TOKEN")
printf '%s\n' "$EVENT_OUTPUT" >> "$EVIDENCE"
PATCH_OVERFLOW_TRIGGER_COUNT=$(printf '%s\n' "$EVENT_OUTPUT" |
  grep -c 'op=42 result=-28.* x0=4 ' || true)
PATCH_RANGE_OVERFLOW_COUNT=$(printf '%s\n' "$EVENT_OUTPUT" |
  grep -c 'op=45 result=-28' || true)
[ "$PATCH_OVERFLOW_TRIGGER_COUNT" -ge 1 ] ||
  fail "expected an ENOSPC range-patch trigger in the 10-event snapshot, got $PATCH_OVERFLOW_TRIGGER_COUNT"
[ "$PATCH_RANGE_OVERFLOW_COUNT" -ge 1 ] ||
  fail "expected an ENOSPC range patch completion, got $PATCH_RANGE_OVERFLOW_COUNT"

STATUS_ACTIVE=$(run_app_command status)
require_contains "$STATUS_ACTIVE" 'active=1'
require_contains "$STATUS_ACTIVE" 'raw_slots=0'
printf '%s\n' "$STATUS_ACTIVE" >> "$EVIDENCE"

CLOSE_OUTPUT=$(run_app_command "close $TOKEN")
require_contains "$CLOSE_OUTPUT" 'closed final_summary_seq='
SESSION_OPEN=0
printf '%s\n' "$CLOSE_OUTPUT" >> "$EVIDENCE"

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
WARN_AFTER=$(adb_device shell su -c cat /sys/kernel/warn_count | tr -d '\r')
[ "$WARN_BEFORE" = "$WARN_AFTER" ] ||
  fail "warn_count changed: $WARN_BEFORE -> $WARN_AFTER"
printf 'raw_prctl_patch_records=pass warn_after=%s final_modules=empty result=pass\n' \
  "$WARN_AFTER" | tee -a "$EVIDENCE"
printf '%s\n' "$EVIDENCE"
