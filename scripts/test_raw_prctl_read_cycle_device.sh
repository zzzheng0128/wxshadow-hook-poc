#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SERIAL=${ANDROID_SERIAL:-}
MODULE=r0lab-m1
REMOTE=/data/local/tmp/r0lab-m1.kpm
PACKAGE=dev.r0hook.lab
ACTIVITY=dev.r0hook.lab/.MainActivity
TOKEN=${RAW_PRCTL_READ_CYCLE_TOKEN:-0x729406}
EVIDENCE_DIR="$ROOT/build/evidence"
EVIDENCE="$EVIDENCE_DIR/raw-prctl-read-cycle-$(date +%Y%m%d-%H%M%S).log"
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
  printf '%s\n' "raw prctl-read-cycle failure: $*" >&2
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
        printf '%s\n' "raw prctl-read-cycle cleanup did not confirm an empty raw slot; preserving module" >&2
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
      printf '%s\n' "raw prctl-read-cycle cleanup could not stop workers; preserving module" >&2
    fi
  elif [ "$MODULE_LOADED" -eq 1 ]; then
    printf '%s\n' "raw prctl-read-cycle cleanup incomplete; module intentionally left resident" >&2
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

RAW_OUTPUT=$(run_app_command "raw prctl read cycle run $TOKEN")
printf '%s\n' "$RAW_OUTPUT" >> "$EVIDENCE"
require_contains "$RAW_OUTPUT" 'raw mode=prctl-dispatch failures=0'
require_contains "$RAW_OUTPUT" 'trigger=prctl_magic'
require_contains "$RAW_OUTPUT" 'option=52304c42'
require_contains "$RAW_OUTPUT" 'operations=1,2,3'
require_contains "$RAW_OUTPUT" 'dispatch=patch_word,release_patch,read_cycle'
require_contains "$RAW_OUTPUT" 'read_cycle=uxn_original_exec_resume'
require_contains "$RAW_OUTPUT" 'patch_scope=single_aligned_word'
require_contains "$RAW_OUTPUT" 'cache_sync=sync_icache_aliases'
require_contains "$RAW_OUTPUT" 'data_fault=absent'
require_contains "$RAW_OUTPUT" 'pte_switch=1'
require_contains "$RAW_OUTPUT" 'exec_resume=1'
require_contains "$RAW_OUTPUT" 'normal_value=42'
require_contains "$RAW_OUTPUT" 'shadow_value=99'
require_contains "$RAW_OUTPUT" 'reject_rc=-1'
require_contains "$RAW_OUTPUT" 'reject_errno=1'
require_contains "$RAW_OUTPUT" 'shadow_after_reject=52800c60'
require_contains "$RAW_OUTPUT" 'patch_rc=0'
require_contains "$RAW_OUTPUT" 'patch_value=77'
require_contains "$RAW_OUTPUT" 'patch_word=528009a0'
require_contains "$RAW_OUTPUT" 'release_rc=0'
require_contains "$RAW_OUTPUT" 'release_value=99'
require_contains "$RAW_OUTPUT" 'released_word=52800c60'
require_contains "$RAW_OUTPUT" 'original_read_word=52800540'
require_contains "$RAW_OUTPUT" 'resume_value=99'
require_contains "$RAW_OUTPUT" 'shadow_after_resume=52800c60'
require_contains "$RAW_OUTPUT" 'restored_value=42'
require_contains "$RAW_OUTPUT" 'trigger_rc=0'
require_contains "$RAW_OUTPUT" 'raw_prctl_hook_ready'
require_contains "$RAW_OUTPUT" 'symbol=prctl'
require_contains "$RAW_OUTPUT" 'abi=prctl_magic'
require_contains "$RAW_OUTPUT" 'read_cycle_op=1'
require_contains "$RAW_OUTPUT" 'patch_word_op=2'
require_contains "$RAW_OUTPUT" 'release_patch_op=3'
require_contains "$RAW_OUTPUT" 'lab_uid_scoped=1'
require_contains "$RAW_OUTPUT" 'target_mm_scoped=1'
require_contains "$RAW_OUTPUT" 'token_scoped=1'
require_contains "$RAW_OUTPUT" 'passthrough=nonmagic'
require_contains "$RAW_OUTPUT" 'passthrough_stress_failures=0'
require_contains "$RAW_OUTPUT" 'passthrough_thread_rc=0'
require_contains "$RAW_OUTPUT" 'passthrough_join_rc=0'
require_contains "$RAW_OUTPUT" 'installed=1'
require_contains "$RAW_OUTPUT" 'hit_events=3'
require_contains "$RAW_OUTPUT" 'read_cycle_events=1'
require_contains "$RAW_OUTPUT" 'patch_events=1'
require_contains "$RAW_OUTPUT" 'release_events=1'
require_contains "$RAW_OUTPUT" 'reject_events=1'
require_contains "$RAW_OUTPUT" 'patch_active=0'
require_contains "$RAW_OUTPUT" 'read_cycle_begin_events=1'
require_contains "$RAW_OUTPUT" 'read_cycle_finish_events=1'
require_contains "$RAW_OUTPUT" 'exec_resume=pending'
require_contains "$RAW_OUTPUT" 'exec_resume=proven'
require_contains "$RAW_OUTPUT" 'failures=0'
require_contains "$RAW_OUTPUT" 'handler_faults=0'
STRESS_ITERATIONS=$(printf '%s\n' "$RAW_OUTPUT" |
  sed -n 's/.*passthrough_stress_iterations=\([0-9][0-9]*\).*/\1/p' |
  sed -n '1p')
[ -n "$STRESS_ITERATIONS" ] ||
  fail 'missing passthrough stress iteration count'
[ "$STRESS_ITERATIONS" -ge 1000 ] ||
  fail "expected at least 1000 passthrough stress iterations, got $STRESS_ITERATIONS"

EVENT_OUTPUT=$(run_app_command "events $TOKEN")
ACTIVATE_COUNT=$(printf '%s\n' "$EVENT_OUTPUT" | grep -c 'op=21 result=0' || true)
PRCTL_PATCH_TRIGGER_COUNT=$(printf '%s\n' "$EVENT_OUTPUT" |
  grep -c 'op=42 result=0.* x0=2 ' || true)
PRCTL_RELEASE_TRIGGER_COUNT=$(printf '%s\n' "$EVENT_OUTPUT" |
  grep -c 'op=42 result=0.* x0=3 ' || true)
PRCTL_READ_TRIGGER_COUNT=$(printf '%s\n' "$EVENT_OUTPUT" |
  grep -c 'op=42 result=0.* x0=1 ' || true)
PRCTL_REJECT_COUNT=$(printf '%s\n' "$EVENT_OUTPUT" | grep -c 'op=42 result=-1' || true)
PRCTL_PATCH_COUNT=$(printf '%s\n' "$EVENT_OUTPUT" |
  grep -c 'op=43 result=0.* x0=52800c60 x30=1528009a0' || true)
PRCTL_RELEASE_COUNT=$(printf '%s\n' "$EVENT_OUTPUT" |
  grep -c 'op=44 result=0.* x0=528009a0 x30=152800c60' || true)
SYSCALL_READ_CYCLE_COUNT=$(printf '%s\n' "$EVENT_OUTPUT" | grep -c 'op=38 result=0' || true)
READ_CYCLE_FINISH_COUNT=$(printf '%s\n' "$EVENT_OUTPUT" | grep -c 'op=37 result=0' || true)
CLEAR_COUNT=$(printf '%s\n' "$EVENT_OUTPUT" | grep -c 'op=22 result=0' || true)
[ "$ACTIVATE_COUNT" -eq 1 ] || fail "expected one raw activation event, got $ACTIVATE_COUNT"
[ "$PRCTL_REJECT_COUNT" -eq 1 ] || fail "expected one rejected prctl trigger event, got $PRCTL_REJECT_COUNT"
[ "$PRCTL_PATCH_TRIGGER_COUNT" -eq 1 ] ||
  fail "expected one patch trigger event, got $PRCTL_PATCH_TRIGGER_COUNT"
[ "$PRCTL_RELEASE_TRIGGER_COUNT" -eq 1 ] ||
  fail "expected one release trigger event, got $PRCTL_RELEASE_TRIGGER_COUNT"
[ "$PRCTL_READ_TRIGGER_COUNT" -eq 1 ] ||
  fail "expected one read-cycle trigger event, got $PRCTL_READ_TRIGGER_COUNT"
[ "$PRCTL_PATCH_COUNT" -eq 1 ] ||
  fail "expected one patch completion event, got $PRCTL_PATCH_COUNT"
[ "$PRCTL_RELEASE_COUNT" -eq 1 ] ||
  fail "expected one release completion event, got $PRCTL_RELEASE_COUNT"
[ "$SYSCALL_READ_CYCLE_COUNT" -eq 1 ] || fail "expected one syscall read-cycle begin event, got $SYSCALL_READ_CYCLE_COUNT"
[ "$READ_CYCLE_FINISH_COUNT" -eq 1 ] || fail "expected one raw read-cycle finish event, got $READ_CYCLE_FINISH_COUNT"
[ "$CLEAR_COUNT" -eq 1 ] || fail "expected one raw clear event, got $CLEAR_COUNT"
PRCTL_REJECT_SEQ=$(printf '%s\n' "$EVENT_OUTPUT" |
  sed -n 's/.*seq=\([0-9][0-9]*\).*op=42 result=-1.*/\1/p')
PRCTL_PATCH_TRIGGER_SEQ=$(printf '%s\n' "$EVENT_OUTPUT" |
  sed -n 's/.*seq=\([0-9][0-9]*\).*op=42 result=0.* x0=2 .*/\1/p')
PRCTL_PATCH_SEQ=$(printf '%s\n' "$EVENT_OUTPUT" |
  sed -n 's/.*seq=\([0-9][0-9]*\).*op=43 result=0.*/\1/p')
PRCTL_RELEASE_TRIGGER_SEQ=$(printf '%s\n' "$EVENT_OUTPUT" |
  sed -n 's/.*seq=\([0-9][0-9]*\).*op=42 result=0.* x0=3 .*/\1/p')
PRCTL_RELEASE_SEQ=$(printf '%s\n' "$EVENT_OUTPUT" |
  sed -n 's/.*seq=\([0-9][0-9]*\).*op=44 result=0.*/\1/p')
PRCTL_READ_TRIGGER_SEQ=$(printf '%s\n' "$EVENT_OUTPUT" |
  sed -n 's/.*seq=\([0-9][0-9]*\).*op=42 result=0.* x0=1 .*/\1/p')
READ_CYCLE_BEGIN_SEQ=$(printf '%s\n' "$EVENT_OUTPUT" |
  sed -n 's/.*seq=\([0-9][0-9]*\).*op=38 result=0.*/\1/p')
READ_CYCLE_FINISH_SEQ=$(printf '%s\n' "$EVENT_OUTPUT" |
  sed -n 's/.*seq=\([0-9][0-9]*\).*op=37 result=0.*/\1/p')
[ "$PRCTL_REJECT_SEQ" -lt "$PRCTL_PATCH_TRIGGER_SEQ" ] ||
  fail "rejected prctl event did not precede the patch trigger"
[ "$PRCTL_PATCH_TRIGGER_SEQ" -lt "$PRCTL_PATCH_SEQ" ] ||
  fail "patch trigger did not precede patch completion"
[ "$PRCTL_PATCH_SEQ" -lt "$PRCTL_RELEASE_TRIGGER_SEQ" ] ||
  fail "patch completion did not precede release trigger"
[ "$PRCTL_RELEASE_TRIGGER_SEQ" -lt "$PRCTL_RELEASE_SEQ" ] ||
  fail "release trigger did not precede release completion"
[ "$PRCTL_RELEASE_SEQ" -lt "$PRCTL_READ_TRIGGER_SEQ" ] ||
  fail "release completion did not precede read-cycle trigger"
[ "$PRCTL_READ_TRIGGER_SEQ" -lt "$READ_CYCLE_BEGIN_SEQ" ] ||
  fail "read-cycle trigger did not precede read-cycle begin"
[ "$READ_CYCLE_BEGIN_SEQ" -lt "$READ_CYCLE_FINISH_SEQ" ] ||
  fail "read-cycle begin did not precede finish"
printf '%s\n' "$EVENT_OUTPUT" >> "$EVIDENCE"

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
printf 'raw_prctl_dispatch=pass warn_after=%s final_modules=empty result=pass\n' \
  "$WARN_AFTER" | tee -a "$EVIDENCE"
printf '%s\n' "$EVIDENCE"
