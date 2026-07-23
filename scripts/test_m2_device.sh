#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SERIAL=${ANDROID_SERIAL:-}
MODULE=r0lab-m1
REMOTE=/data/local/tmp/r0lab-m1.kpm
PACKAGE=dev.r0hook.lab
ACTIVITY=dev.r0hook.lab/.MainActivity
TOKEN=${M2_TOKEN:-0x6a19f4}
SINGLE_TOKEN=$(printf '0x%x' $((TOKEN + 1)))
MULTI_TOKEN=$(printf '0x%x' $((TOKEN + 2)))
EVIDENCE_DIR="$ROOT/build/evidence"
EVIDENCE="$EVIDENCE_DIR/m2-device-$(date +%Y%m%d-%H%M%S).log"
MODULE_LOADED=0
SESSION_OPEN=0
CURRENT_TOKEN=

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
  printf '%s\n' "M2 failure: $*" >&2
  exit 1
}

require_contains() {
  haystack=$1
  needle=$2
  printf '%s\n' "$haystack" | grep -F -- "$needle" >/dev/null ||
    fail "expected output not found: $needle"
}

status_field() {
  text=$1
  field=$2
  printf '%s\n' "$text" | sed -n "s/.*$field=\([0-9][0-9]*\).*/\1/p" | tail -n 1
}

cleanup() {
  status=$?
  trap - EXIT INT TERM
  if [ "$SESSION_OPEN" -eq 1 ]; then
    CLEANUP_STATUS=$(run_app_command status 2>&1 || true)
    case "$CLEANUP_STATUS" in
      *"hwbp_slots=0"*)
        run_app_command "close $CURRENT_TOKEN" >/dev/null 2>&1 || true
        ;;
      *)
        printf '%s\n' "M2 cleanup did not confirm empty HWBP slots; preserving module" >&2
        exit "$status"
        ;;
    esac
  fi
  if [ "$MODULE_LOADED" -eq 1 ]; then
    if prepare_worker_shutdown >/dev/null 2>&1; then
      supercmd module unload "$MODULE" >/dev/null 2>&1 || true
    else
      printf '%s\n' "M2 cleanup could not stop workers; preserving module" >&2
    fi
  fi
  exit "$status"
}

trap cleanup EXIT INT TERM

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

run_phase() {
  phase=$1
  token=$2
  command=$3
  expected=$4
  expected_entries=${5:-}
  expected_returns=${6:-}

  CURRENT_TOKEN=$token
  ARM_OUTPUT=$(run_app_command "arm $token")
  require_contains "$ARM_OUTPUT" 'armed uid='
  SESSION_OPEN=1
  printf '%s\n' "phase=$phase token=$token" >> "$EVIDENCE"
  printf '%s\n' "$ARM_OUTPUT" >> "$EVIDENCE"

  STATUS_BEFORE=$(run_app_command status)
  ENTRY_BEFORE=$(status_field "$STATUS_BEFORE" hwbp_entry_events)
  RETURN_BEFORE=$(status_field "$STATUS_BEFORE" hwbp_return_events)
  [ -n "$ENTRY_BEFORE" ] || fail "missing hwbp_entry_events before $phase"
  [ -n "$RETURN_BEFORE" ] || fail "missing hwbp_return_events before $phase"
  printf '%s\n' "$STATUS_BEFORE" >> "$EVIDENCE"

  M2_OUTPUT=$(run_app_command "$command $token")
  require_contains "$M2_OUTPUT" "$expected"
  require_contains "$M2_OUTPUT" 'failures=0'
  printf '%s\n' "$M2_OUTPUT" >> "$EVIDENCE"

  STATUS_ACTIVE=$(run_app_command status)
  require_contains "$STATUS_ACTIVE" 'active=1'
  require_contains "$STATUS_ACTIVE" 'hwbp_slots=0'
  printf '%s\n' "$STATUS_ACTIVE" >> "$EVIDENCE"

  if [ -n "$expected_entries" ]; then
    ENTRY_AFTER=$(status_field "$STATUS_ACTIVE" hwbp_entry_events)
    RETURN_AFTER=$(status_field "$STATUS_ACTIVE" hwbp_return_events)
    [ -n "$ENTRY_AFTER" ] || fail "missing hwbp_entry_events after $phase"
    [ -n "$RETURN_AFTER" ] || fail "missing hwbp_return_events after $phase"
    ENTRY_COUNT=$((ENTRY_AFTER - ENTRY_BEFORE))
    RETURN_COUNT=$((RETURN_AFTER - RETURN_BEFORE))
    [ "$ENTRY_COUNT" -eq "$expected_entries" ] ||
      fail "expected $expected_entries $phase entry events, got $ENTRY_COUNT"
    [ "$RETURN_COUNT" -eq "$expected_returns" ] ||
      fail "expected $expected_returns $phase return events, got $RETURN_COUNT"
    EVENT_OUTPUT=$(run_app_command "events $token")
    printf '%s\n' "$EVENT_OUTPUT" >> "$EVIDENCE"
  fi
}

close_phase() {
  token=$1

  CLOSE_OUTPUT=$(run_app_command "close $token")
  require_contains "$CLOSE_OUTPUT" 'closed final_summary_seq='
  SESSION_OPEN=0
  printf '%s\n' "$CLOSE_OUTPUT" >> "$EVIDENCE"
}

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
printf 'module_load=%s\n' "$LOAD_OUTPUT" | tee -a "$EVIDENCE"
MODULE_LOADED=1

adb_device logcat -c
STATUS_INITIAL=$(run_app_command status)
require_contains "$STATUS_INITIAL" 'version=5'
require_contains "$STATUS_INITIAL" 'hwbp_slots=0'
printf '%s\n' "$STATUS_INITIAL" >> "$EVIDENCE"

ARM_OUTPUT=$(run_app_command "arm $TOKEN")
require_contains "$ARM_OUTPUT" 'armed uid='
CURRENT_TOKEN=$TOKEN
SESSION_OPEN=1
printf '%s\n' "phase=preflight token=$TOKEN" >> "$EVIDENCE"
printf '%s\n' "$ARM_OUTPUT" >> "$EVIDENCE"

M2_OUTPUT=$(run_app_command "m2 preflight $TOKEN")
require_contains "$M2_OUTPUT" 'm2 threads=1 expected_entry=0 expected_return=0 failures=0 mode=preflight'
printf '%s\n' "$M2_OUTPUT" >> "$EVIDENCE"

STATUS_ACTIVE=$(run_app_command status)
require_contains "$STATUS_ACTIVE" 'active=1'
require_contains "$STATUS_ACTIVE" 'hwbp_slots=0'
printf '%s\n' "$STATUS_ACTIVE" >> "$EVIDENCE"
close_phase "$TOKEN"

run_phase single "$SINGLE_TOKEN" 'm2 single' \
  'm2 threads=1 expected_entry=1 expected_return=1 failures=0 mode=single' 1 1
close_phase "$SINGLE_TOKEN"

run_phase multi "$MULTI_TOKEN" 'm2 run' \
  'm2 threads=3 expected_entry=3 expected_return=3 failures=0 mode=multi' 3 3
close_phase "$MULTI_TOKEN"

SUMMARY_OUTPUT=$(run_app_command "events $MULTI_TOKEN")
require_contains "$SUMMARY_OUTPUT" 'op=7 result=0'
printf '%s\n' "$SUMMARY_OUTPUT" >> "$EVIDENCE"

STATUS_FINAL=$(run_app_command status)
require_contains "$STATUS_FINAL" 'active=0'
require_contains "$STATUS_FINAL" 'hwbp_slots=0'
printf '%s\n' "$STATUS_FINAL" >> "$EVIDENCE"

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
printf 'entry_events=%s return_events=%s warn_after=%s final_modules=empty result=pass\n' \
  "$ENTRY_COUNT" "$RETURN_COUNT" "$WARN_AFTER" | tee -a "$EVIDENCE"
printf '%s\n' "$EVIDENCE"
