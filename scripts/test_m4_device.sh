#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SERIAL=${ANDROID_SERIAL:-}
MODULE=r0lab-m1
REMOTE=/data/local/tmp/r0lab-m1.kpm
PACKAGE=dev.r0hook.lab
ACTIVITY=dev.r0hook.lab/.MainActivity
TOKEN=${M4_TOKEN:-0x4d4001}
RUN_TOKEN=$(printf '0x%x' $((TOKEN + 1)))
EVIDENCE_DIR="$ROOT/build/evidence"
EVIDENCE="$EVIDENCE_DIR/m4-device-$(date +%Y%m%d-%H%M%S).log"
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
  printf '%s\n' "M4 failure: $*" >&2
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
    run_app_command "m4 clear $CURRENT_TOKEN" >/dev/null 2>&1 || true
    CLEANUP_STATUS=$(run_app_command status 2>&1 || true)
    case "$CLEANUP_STATUS" in
      *"m4_slots=0"*) ;;
      *)
        printf '%s\n' "M4 cleanup did not confirm an empty page slot; preserving module" >&2
        cleanup_ok=0
        ;;
    esac
    if [ "$cleanup_ok" -eq 1 ]; then
      run_app_command "close $CURRENT_TOKEN" >/dev/null 2>&1 || cleanup_ok=0
    fi
  fi
  if [ "$MODULE_LOADED" -eq 1 ] && [ "$cleanup_ok" -eq 1 ]; then
    if prepare_worker_shutdown >/dev/null 2>&1; then
      supercmd module unload "$MODULE" >/dev/null 2>&1 || true
    else
      printf '%s\n' "M4 cleanup could not stop workers; preserving module" >&2
    fi
  elif [ "$MODULE_LOADED" -eq 1 ]; then
    printf '%s\n' "M4 cleanup incomplete; module intentionally left resident" >&2
  fi
  exit "$status"
}

trap cleanup EXIT INT TERM

run_phase() {
  phase=$1
  token=$2
  command=$3
  expected_redirects=$4

  CURRENT_TOKEN=$token
  ARM_OUTPUT=$(run_app_command "arm $token")
  require_contains "$ARM_OUTPUT" 'armed uid='
  SESSION_OPEN=1
  printf '%s\n' "phase=$phase token=$token" >> "$EVIDENCE"
  printf '%s\n' "$ARM_OUTPUT" >> "$EVIDENCE"

  M4_OUTPUT=$(run_app_command "$command $token")
  printf '%s\n' "$M4_OUTPUT" >> "$EVIDENCE"
  EVENT_OUTPUT=$(run_app_command "events $token")
  printf '%s\n' "$EVENT_OUTPUT" >> "$EVIDENCE"
  require_contains "$M4_OUTPUT" "m4 mode=$phase expected_redirects=$expected_redirects observed_redirects=$expected_redirects failures=0"
  require_contains "$M4_OUTPUT" 'source_normal=42 clone_normal=99'
  require_contains "$M4_OUTPUT" 'source_after=42 clone_after=99'
  require_contains "$M4_OUTPUT" 'source_perms=r-xp/r-xp/r-xp clone_perms=r-xp/r-xp/r-xp'
  require_contains "$M4_OUTPUT" 'source_transition=pte_uxn pte_switch=1'
  require_contains "$M4_OUTPUT" 'ready="m4_ready'
  require_contains "$M4_OUTPUT" 'observed="m4_observed'
  RECORD_EVIDENCE_COUNT=$(printf '%s\n' "$M4_OUTPUT" |
    grep -o 'record_backend=visible_clone record_state=source_uxn' |
    wc -l | tr -d ' ')
  [ "$RECORD_EVIDENCE_COUNT" -ge 2 ] ||
    fail "expected M4 ready/observed record evidence, got $RECORD_EVIDENCE_COUNT"
  if [ "$expected_redirects" -eq 1 ]; then
    require_contains "$M4_OUTPUT" 'redirected_value=99'
  fi
  REDIRECT_COUNT=$(printf '%s\n' "$EVENT_OUTPUT" | grep -c 'op=16 result=0' || true)
  [ "$REDIRECT_COUNT" -eq "$expected_redirects" ] ||
    fail "expected $expected_redirects M4 redirect events, got $REDIRECT_COUNT"
  printf '%s\n' "$EVENT_OUTPUT" >> "$EVIDENCE"

  STATUS_ACTIVE=$(run_app_command status)
  require_contains "$STATUS_ACTIVE" 'active=1'
  require_contains "$STATUS_ACTIVE" 'm3_slots=0'
  require_contains "$STATUS_ACTIVE" 'm4_slots=0'
  printf '%s\n' "$STATUS_ACTIVE" >> "$EVIDENCE"

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
MODULE_LOADED=1
printf 'module_load=%s\n' "$LOAD_OUTPUT" | tee -a "$EVIDENCE"

STATUS_INITIAL=$(run_app_command status)
require_contains "$STATUS_INITIAL" 'version=5'
require_contains "$STATUS_INITIAL" 'm3_slots=0'
require_contains "$STATUS_INITIAL" 'm4_slots=0'
printf '%s\n' "$STATUS_INITIAL" >> "$EVIDENCE"

run_phase preflight "$TOKEN" 'm4 preflight' 0
run_phase redirect "$RUN_TOKEN" 'm4 run' 1

STATUS_FINAL=$(run_app_command status)
require_contains "$STATUS_FINAL" 'active=0'
require_contains "$STATUS_FINAL" 'm3_slots=0'
require_contains "$STATUS_FINAL" 'm4_slots=0'
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
printf 'redirects_preflight=0 redirects_run=1 warn_after=%s final_modules=empty result=pass\n' \
  "$WARN_AFTER" | tee -a "$EVIDENCE"
printf '%s\n' "$EVIDENCE"
