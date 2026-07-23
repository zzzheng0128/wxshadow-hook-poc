#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SERIAL=${ANDROID_SERIAL:-}
MODULE=r0lab-m1
REMOTE=/data/local/tmp/r0lab-m1.kpm
PACKAGE=dev.r0hook.lab
ACTIVITY=dev.r0hook.lab/.MainActivity
TOKEN=${RAW_EXIT_HOOK_ROUTING_TOKEN:-0x729236}
EVIDENCE_DIR="$ROOT/build/evidence"
EVIDENCE="$EVIDENCE_DIR/raw-exit-hook-routing-$(date +%Y%m%d-%H%M%S).log"
MODULE_LOADED=0
SESSION_OPEN=0
APP_FORCED=0

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
  printf '%s\n' "raw exit-hook-routing failure: $*" >&2
  exit 1
}

require_contains() {
  haystack=$1
  needle=$2
  printf '%s\n' "$haystack" | LC_ALL=C grep -F -- "$needle" >/dev/null ||
    fail "expected output not found: $needle"
}

cleanup() {
  status=$?
  cleanup_ok=1
  trap - EXIT INT TERM
  if [ "$status" -eq 0 ]; then
    exit 0
  fi
  if [ "$SESSION_OPEN" -eq 1 ]; then
    run_app_command "raw slot clear $TOKEN 0" >/dev/null 2>&1 || true
    run_app_command "raw slot clear $TOKEN 1" >/dev/null 2>&1 || true
    CLEANUP_STATUS=$(run_app_command status 2>&1 || true)
    case "$CLEANUP_STATUS" in
      *"raw_slots=0"*) ;;
      *)
        printf '%s\n' "raw exit-hook-routing cleanup did not confirm empty raw slots; preserving module" >&2
        cleanup_ok=0
        ;;
    esac
    if [ "$cleanup_ok" -eq 1 ]; then
      run_app_command "close $TOKEN" >/dev/null 2>&1 || cleanup_ok=0
    fi
  elif [ "$APP_FORCED" -eq 1 ]; then
    CLEANUP_STATUS=$(run_app_command status 2>&1 || true)
    case "$CLEANUP_STATUS" in
      *"active=0"*"raw_slots=0"*"page_records=0"*) ;;
      *)
        printf '%s\n' "raw exit-hook-routing target-exit cleanup incomplete; preserving module" >&2
        cleanup_ok=0
        ;;
    esac
  fi
  if [ "$MODULE_LOADED" -eq 1 ] && [ "$cleanup_ok" -eq 1 ]; then
    if prepare_worker_shutdown >/dev/null 2>&1; then
      supercmd module unload "$MODULE" >/dev/null 2>&1 || true
    else
      printf '%s\n' "raw exit-hook-routing cleanup could not stop workers; preserving module" >&2
    fi
  elif [ "$MODULE_LOADED" -eq 1 ]; then
    printf '%s\n' "raw exit-hook-routing cleanup incomplete; module intentionally left resident" >&2
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

RAW_OUTPUT=$(run_app_command "raw exit hook routing hold $TOKEN")
printf '%s\n' "$RAW_OUTPUT" >> "$EVIDENCE"
require_contains "$RAW_OUTPUT" 'raw mode=exit-hook-routing-hold failures=0'
require_contains "$RAW_OUTPUT" 'target_mm_scoped=1'
require_contains "$RAW_OUTPUT" 'page_record_routed=1'
require_contains "$RAW_OUTPUT" 'cleanup=monitor'
require_contains "$RAW_OUTPUT" 'normal=42/42'
require_contains "$RAW_OUTPUT" 'shadow=99/99'
require_contains "$RAW_OUTPUT" 'activations=1/1'
require_contains "$RAW_OUTPUT" 'states=3/3'
require_contains "$RAW_OUTPUT" 'hook_ready=1/1'
require_contains "$RAW_OUTPUT" 'hook_status=1/1'
require_contains "$RAW_OUTPUT" 'inspect=1/1'
require_contains "$RAW_OUTPUT" 'handler_faults=0'

STATUS_HELD=$(run_app_command status)
require_contains "$STATUS_HELD" 'active=1'
require_contains "$STATUS_HELD" 'raw_slots=2'
require_contains "$STATUS_HELD" 'raw_page_table_active=2'
printf '%s\n' "$STATUS_HELD" >> "$EVIDENCE"

adb_device shell am force-stop "$PACKAGE" >/dev/null
APP_FORCED=1
SESSION_OPEN=0
printf 'force_stop=done package=%s\n' "$PACKAGE" >> "$EVIDENCE"

attempt=0
STATUS_AFTER=''
while [ "$attempt" -lt 50 ]; do
  STATUS_AFTER=$(run_app_command status)
  printf '%s\n' "$STATUS_AFTER" >> "$EVIDENCE"
  case "$STATUS_AFTER" in
    *"active=0"*"raw_slots=0"*"page_records=0"*) break ;;
  esac
  attempt=$((attempt + 1))
  sleep 0.1
done
require_contains "$STATUS_AFTER" 'active=0'
require_contains "$STATUS_AFTER" 'raw_slots=0'
require_contains "$STATUS_AFTER" 'page_records=0'

EVENT_OUTPUT=$(run_app_command "events $TOKEN")
printf '%s\n' "$EVENT_OUTPUT" >> "$EVIDENCE"
EXIT_HOOK_COUNT=$(printf '%s\n' "$EVENT_OUTPUT" | LC_ALL=C grep -c 'op=34 result=0' || true)
TARGET_EXIT_COUNT=$(printf '%s\n' "$EVENT_OUTPUT" | LC_ALL=C grep -c 'op=18 result=0' || true)
RAW_CLEAR_COUNT=$(printf '%s\n' "$EVENT_OUTPUT" | LC_ALL=C grep -c 'op=22 result=0' || true)
CLOSE_COUNT=$(printf '%s\n' "$EVENT_OUTPUT" | LC_ALL=C grep -c 'op=5 result=0' || true)
SUMMARY_COUNT=$(printf '%s\n' "$EVENT_OUTPUT" | LC_ALL=C grep -c 'op=7 result=0' || true)
[ "$EXIT_HOOK_COUNT" -ge 2 ] || fail "expected at least two exit_mmap hook events, got $EXIT_HOOK_COUNT"
[ "$TARGET_EXIT_COUNT" -ge 1 ] || fail "expected target-exit event, got $TARGET_EXIT_COUNT"
[ "$RAW_CLEAR_COUNT" -ge 2 ] || fail "expected two raw-clear events, got $RAW_CLEAR_COUNT"
[ "$CLOSE_COUNT" -ge 1 ] || fail "expected close event, got $CLOSE_COUNT"
[ "$SUMMARY_COUNT" -ge 1 ] || fail "expected summary event, got $SUMMARY_COUNT"

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
printf 'raw_exit_hook_routing=pass route=exit_mmap page_record_routed=1 exit_events=%s raw_clear_events=%s warn_after=%s final_modules=empty result=pass\n' \
  "$EXIT_HOOK_COUNT" "$RAW_CLEAR_COUNT" "$WARN_AFTER" | tee -a "$EVIDENCE"
printf '%s\n' "$EVIDENCE"
