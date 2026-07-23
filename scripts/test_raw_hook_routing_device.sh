#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SERIAL=${ANDROID_SERIAL:-}
MODULE=r0lab-m1
REMOTE=/data/local/tmp/r0lab-m1.kpm
PACKAGE=dev.r0hook.lab
ACTIVITY=dev.r0hook.lab/.MainActivity
TOKEN=${RAW_TOKEN:-0x729021}
EVIDENCE_DIR="$ROOT/build/evidence"
EVIDENCE="$EVIDENCE_DIR/raw-hook-routing-$(date +%Y%m%d-%H%M%S).log"
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
  printf '%s\n' "raw hook-routing failure: $*" >&2
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
    run_app_command "raw slot abort read cycle clear $TOKEN 0" >/dev/null 2>&1 || true
    run_app_command "raw slot abort read cycle clear $TOKEN 1" >/dev/null 2>&1 || true
    run_app_command "raw slot clear $TOKEN 0" >/dev/null 2>&1 || true
    run_app_command "raw slot clear $TOKEN 1" >/dev/null 2>&1 || true
    CLEANUP_STATUS=$(run_app_command status 2>&1 || true)
    case "$CLEANUP_STATUS" in
      *"raw_slots=0"*) ;;
      *)
        printf '%s\n' "raw hook-routing cleanup did not confirm empty raw slots; preserving module" >&2
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
      printf '%s\n' "raw hook-routing cleanup could not stop workers; preserving module" >&2
    fi
  elif [ "$MODULE_LOADED" -eq 1 ]; then
    printf '%s\n' "raw hook-routing cleanup incomplete; module intentionally left resident" >&2
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
require_contains "$STATUS_INITIAL" 'raw_page_table_slots=2'
printf '%s\n' "$STATUS_INITIAL" >> "$EVIDENCE"

ARM_OUTPUT=$(run_app_command "arm $TOKEN")
require_contains "$ARM_OUTPUT" 'armed uid='
SESSION_OPEN=1
printf '%s\n' "$ARM_OUTPUT" >> "$EVIDENCE"

RAW_OUTPUT=$(run_app_command "raw hook routing run $TOKEN")
printf '%s\n' "$RAW_OUTPUT" >> "$EVIDENCE"
require_contains "$RAW_OUTPUT" 'raw mode=hook-routing'
require_contains "$RAW_OUTPUT" 'abort_route=read-cycle'
require_contains "$RAW_OUTPUT" 'failures=0'
require_contains "$RAW_OUTPUT" 'page_record_routed=1'
require_contains "$RAW_OUTPUT" 'target_mm_scoped=1'
require_contains "$RAW_OUTPUT" 'normal=42/42 shadow=99/99'
require_contains "$RAW_OUTPUT" 'resume=99/99 final=42/42'
require_contains "$RAW_OUTPUT" 'read_completed=1/1'
require_contains "$RAW_OUTPUT" 'route_slot0_events=1 route_slot1_events=1'
require_contains "$RAW_OUTPUT" 'cross_slot0_after_slot1=1 cross_slot1_after_slot0=0'
require_contains "$RAW_OUTPUT" 'raw_slot_abort_read_cycle_ready slot=0'
require_contains "$RAW_OUTPUT" 'raw_slot_abort_read_cycle_ready slot=1'
require_contains "$RAW_OUTPUT" 'status1_after_slot0="raw_slot_abort_read_cycle_status slot=1'
require_contains "$RAW_OUTPUT" 'read_events=0'
require_contains "$RAW_OUTPUT" 'status0_after_slot1="raw_slot_abort_read_cycle_status slot=0'
require_contains "$RAW_OUTPUT" 'read_events=1'
require_contains "$RAW_OUTPUT" 'handler_faults=0'

EVENT_OUTPUT=$(run_app_command "events $TOKEN")
BEGIN_COUNT=$(printf '%s\n' "$EVENT_OUTPUT" | grep -c 'op=47 result=0' || true)
FINISH_COUNT=$(printf '%s\n' "$EVENT_OUTPUT" | grep -c 'op=37 result=0' || true)
[ "$BEGIN_COUNT" -eq 2 ] ||
  fail "expected two raw abort read-cycle begin events, got $BEGIN_COUNT"
[ "$FINISH_COUNT" -eq 2 ] ||
  fail "expected two raw read-cycle finish events, got $FINISH_COUNT"
printf '%s\n' "$EVENT_OUTPUT" >> "$EVIDENCE"

STATUS_ACTIVE=$(run_app_command status)
require_contains "$STATUS_ACTIVE" 'active=1'
require_contains "$STATUS_ACTIVE" 'raw_slots=0'
require_contains "$STATUS_ACTIVE" 'raw_page_table_active=0'
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
printf 'raw_hook_routing=pass abort_route=read-cycle warn_after=%s final_modules=empty result=pass\n' \
  "$WARN_AFTER" | tee -a "$EVIDENCE"
printf '%s\n' "$EVIDENCE"
