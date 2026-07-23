#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SERIAL=${ANDROID_SERIAL:-}
MODULE=r0lab-m1
REMOTE=/data/local/tmp/r0lab-m1.kpm
PACKAGE=dev.r0hook.lab
ACTIVITY=dev.r0hook.lab/.MainActivity
TOKEN=${M5_FAULT_TOKEN:-0x6a1b00}
INTERRUPT_TOKEN=$TOKEN
ENOMEM_TOKEN=$(printf '0x%x' $((TOKEN + 1)))
FORK_TOKEN=$(printf '0x%x' $((TOKEN + 2)))
EXEC_TOKEN=$(printf '0x%x' $((TOKEN + 3)))
EVIDENCE_DIR="$ROOT/build/evidence"
EVIDENCE="$EVIDENCE_DIR/m5-faults-$(date +%Y%m%d-%H%M%S).log"
MODULE_LOADED=0

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

start_exec_command() {
  command=$1
  adb_device logcat -c >/dev/null
  adb_device shell "am start -n $ACTIVITY --es r0lab_command '$command'" >/dev/null 2>&1 || true
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
  printf '%s\n' "M5 fault test failure: $*" >&2
  exit 1
}

require_contains() {
  haystack=$1
  needle=$2
  printf '%s\n' "$haystack" | grep -F -- "$needle" >/dev/null ||
    fail "expected output not found: $needle"
}

wait_for_clean_status() {
  phase=$1
  attempt=0
  while [ "$attempt" -lt 50 ]; do
    STATUS_OUTPUT=$(run_app_command status)
    case "$STATUS_OUTPUT" in
      *"active=0"*"hwbp_slots=0"*"m3_slots=0"*"m4_slots=0"*)
        printf '%s\n' "$STATUS_OUTPUT"
        return 0
        ;;
    esac
    attempt=$((attempt + 1))
    sleep 0.1
  done
  fail "$phase did not clean active session/page slots"
}

close_and_record() {
  phase=$1
  token=$2
  expected_event=$3

  STATUS_ACTIVE=$(run_app_command status)
  require_contains "$STATUS_ACTIVE" 'active=1'
  require_contains "$STATUS_ACTIVE" 'hwbp_slots=0'
  require_contains "$STATUS_ACTIVE" 'm3_slots=0'
  require_contains "$STATUS_ACTIVE" 'm4_slots=0'
  CLOSE_OUTPUT=$(run_app_command "close $token")
  require_contains "$CLOSE_OUTPUT" 'closed final_summary_seq='
  EVENT_OUTPUT=$(run_app_command "events $token")
  require_contains "$EVENT_OUTPUT" "$expected_event"
  require_contains "$EVENT_OUTPUT" 'op=5 result=0'
  require_contains "$EVENT_OUTPUT" 'op=7 result=0'
  printf 'phase=%s token=%s\n' "$phase" "$token" >> "$EVIDENCE"
  printf '%s\n' "$STATUS_ACTIVE" >> "$EVIDENCE"
  printf '%s\n' "$CLOSE_OUTPUT" >> "$EVIDENCE"
  printf '%s\n' "$EVENT_OUTPUT" >> "$EVIDENCE"
}

run_rollback_phase() {
  phase=$1
  token=$2
  command=$3
  expected_mode=$4
  expected_event=$5

  ARM_OUTPUT=$(run_app_command "arm $token")
  require_contains "$ARM_OUTPUT" 'armed uid='
  PHASE_OUTPUT=$(run_app_command "$command $token")
  printf 'phase=%s token=%s\n' "$phase" "$token" >> "$EVIDENCE"
  printf '%s\n' "$ARM_OUTPUT" >> "$EVIDENCE"
  printf '%s\n' "$PHASE_OUTPUT" >> "$EVIDENCE"
  require_contains "$PHASE_OUTPUT" "m5 mode=$expected_mode failures=0"
  close_and_record "$phase" "$token" "$expected_event"
}

cleanup() {
  status=$?
  trap - EXIT INT TERM
  if [ "$MODULE_LOADED" -eq 1 ]; then
    CLEANUP_STATUS=$(run_app_command status 2>&1 || true)
    case "$CLEANUP_STATUS" in
      *"active=0"*"hwbp_slots=0"*"m3_slots=0"*"m4_slots=0"*)
        if prepare_worker_shutdown >/dev/null 2>&1; then
          supercmd module unload "$MODULE" >/dev/null 2>&1 || true
        else
          printf '%s\n' "M5 cleanup could not stop workers; preserving module" >&2
        fi
        ;;
      *) printf '%s\n' "M5 cleanup did not confirm empty state; preserving module" >&2 ;;
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
  tr -d '\r' | sed -n 's/.* uid:\([0-9][0-9]*\).*/\1/p')
[ -n "$LAB_UID" ] || fail "Lab App UID not found"
WARN_BEFORE=$(adb_device shell su -c cat /sys/kernel/warn_count | tr -d '\r')
printf 'lab_uid=%s tokens=%s,%s,%s,%s warn_before=%s\n' \
  "$LAB_UID" "$INTERRUPT_TOKEN" "$ENOMEM_TOKEN" "$FORK_TOKEN" "$EXEC_TOKEN" \
  "$WARN_BEFORE" | tee "$EVIDENCE"

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
printf '%s\n' "$STATUS_INITIAL" >> "$EVIDENCE"

run_rollback_phase interrupt "$INTERRUPT_TOKEN" 'm5 m3-interrupt' 'm3-interrupt' 'op=14 result=0'
run_rollback_phase enomem "$ENOMEM_TOKEN" 'm5 m3-enomem' 'm3-enomem' 'op=2 result=-12'
run_rollback_phase fork_exec "$FORK_TOKEN" 'm5 m3-fork-exec' 'm3-fork-exec' 'op=14 result=0'

ARM_EXEC_OUTPUT=$(run_app_command "arm $EXEC_TOKEN")
require_contains "$ARM_EXEC_OUTPUT" 'armed uid='
printf 'phase=owner_exec token=%s\n' "$EXEC_TOKEN" >> "$EVIDENCE"
printf '%s\n' "$ARM_EXEC_OUTPUT" >> "$EVIDENCE"
EXEC_START_OUTPUT=$(start_exec_command "m5 m3-exec $EXEC_TOKEN")
printf '%s\n' "$EXEC_START_OUTPUT" >> "$EVIDENCE"
STATUS_EXEC=$(wait_for_clean_status owner_exec)
require_contains "$STATUS_EXEC" 'active=0'
EVENT_EXEC=$(run_app_command "events $EXEC_TOKEN")
require_contains "$EVENT_EXEC" 'op=12 result=0'
require_contains "$EVENT_EXEC" 'op=18 result=0'
require_contains "$EVENT_EXEC" 'op=14 result=0'
require_contains "$EVENT_EXEC" 'op=5 result=0'
require_contains "$EVENT_EXEC" 'op=7 result=0'
printf '%s\n' "$STATUS_EXEC" >> "$EVIDENCE"
printf '%s\n' "$EVENT_EXEC" >> "$EVIDENCE"

STATUS_FINAL=$(run_app_command status)
require_contains "$STATUS_FINAL" 'active=0'
require_contains "$STATUS_FINAL" 'hwbp_slots=0'
require_contains "$STATUS_FINAL" 'm3_slots=0'
require_contains "$STATUS_FINAL" 'm4_slots=0'
printf '%s\n' "$STATUS_FINAL" >> "$EVIDENCE"

prepare_worker_shutdown || fail "workers did not stop before module unload"
printf '%s\n' "$WORKER_SHUTDOWN_OUTPUT" >> "$EVIDENCE"
printf '%s\n' "$WORKER_STATUS_OUTPUT" >> "$EVIDENCE"
UNLOAD_OUTPUT=$(supercmd module unload "$MODULE" 2>&1) || fail "module unload failed: $UNLOAD_OUTPUT"
case "$UNLOAD_OUTPUT" in
  *"supercmd error code"*) fail "module unload rejected: $UNLOAD_OUTPUT" ;;
esac
MODULE_LOADED=0
FINAL_MODULES=$(supercmd module list 2>&1) || fail "final list failed: $FINAL_MODULES"
[ -z "$FINAL_MODULES" ] || fail "module remained loaded: $FINAL_MODULES"
WARN_AFTER=$(adb_device shell su -c cat /sys/kernel/warn_count | tr -d '\r')
[ "$WARN_BEFORE" = "$WARN_AFTER" ] ||
  fail "warn_count changed: $WARN_BEFORE -> $WARN_AFTER"
printf 'm5_fault_phases=interrupt,enomem,fork_exec,owner_exec warn_after=%s final_modules=empty result=pass\n' \
  "$WARN_AFTER" | tee -a "$EVIDENCE"
printf '%s\n' "$EVIDENCE"
