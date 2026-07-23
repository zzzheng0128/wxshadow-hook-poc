#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SERIAL=${ANDROID_SERIAL:-}
MODULE=r0lab-m1
REMOTE=/data/local/tmp/r0lab-m1.kpm
PACKAGE=dev.r0hook.lab
ACTIVITY=dev.r0hook.lab/.MainActivity
TOKEN=${M5_TOKEN:-0x6a1a10}
M4_TOKEN=$(printf '0x%x' $((TOKEN + 1)))
RAW_TOKEN=$(printf '0x%x' $((TOKEN + 2)))
S4_TOKEN=$(printf '0x%x' $((TOKEN + 3)))
S4_RAW_STEP_TOKEN=$(printf '0x%x' $((TOKEN + 4)))
EVIDENCE_DIR="$ROOT/build/evidence"
EVIDENCE="$EVIDENCE_DIR/m5-lifecycle-$(date +%Y%m%d-%H%M%S).log"
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
  printf '%s\n' "M5 lifecycle failure: $*" >&2
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
  trap - EXIT INT TERM
  if [ "$MODULE_LOADED" -eq 1 ]; then
    adb_device shell am force-stop "$PACKAGE" >/dev/null 2>&1 || true
    CLEANUP_STATUS=$(run_app_command status 2>&1 || true)
    case "$CLEANUP_STATUS" in
      *"active=0"*m3_slots=0*m4_slots=0*raw_slots=0*s4_slots=0*)
        if prepare_worker_shutdown >/dev/null 2>&1; then
          supercmd module unload "$MODULE" >/dev/null 2>&1 || true
        else
          printf '%s\n' "M5 cleanup could not stop workers; preserving module" >&2
        fi
        ;;
      *)
        printf '%s\n' "M5 cleanup did not confirm empty page slots; preserving module" >&2
        ;;
    esac
  fi
  exit "$status"
}

trap cleanup EXIT INT TERM

wait_for_clean_status() {
  phase=$1
  attempt=0
  while [ "$attempt" -lt 50 ]; do
    STATUS_OUTPUT=$(run_app_command status)
    case "$STATUS_OUTPUT" in
      *"active=0"*m3_slots=0*m4_slots=0*raw_slots=0*s4_slots=0*)
        printf '%s\n' "$STATUS_OUTPUT"
        return 0
        ;;
    esac
    attempt=$((attempt + 1))
    sleep 0.1
  done
  fail "$phase did not clean active session/page slots"
}

run_phase() {
  phase=$1
  token=$2
  hold_command=$3
  expected_mode=$4
  expected_slot=$5
  expected_clear_op=$6

  printf '%s\n' "phase=$phase token=$token" >> "$EVIDENCE"
  ARM_OUTPUT=$(run_app_command "arm $token")
  require_contains "$ARM_OUTPUT" 'armed uid='
  printf '%s\n' "$ARM_OUTPUT" >> "$EVIDENCE"

  HOLD_OUTPUT=$(run_app_command "$hold_command $token")
  require_contains "$HOLD_OUTPUT" "m5 mode=$expected_mode failures=0"
  require_contains "$HOLD_OUTPUT" 'arm_rc='
  require_contains "$HOLD_OUTPUT" 'ready_rc='
  case "$HOLD_OUTPUT" in
    *"arm_rc=-"*|*"ready_rc=-"*) fail "$phase hold returned a negative arm/ready rc" ;;
  esac
  printf '%s\n' "$HOLD_OUTPUT" >> "$EVIDENCE"

  STATUS_ARMED=$(run_app_command status)
  require_contains "$STATUS_ARMED" 'active=1'
  require_contains "$STATUS_ARMED" "$expected_slot=1"
  printf '%s\n' "$STATUS_ARMED" >> "$EVIDENCE"

  adb_device shell am force-stop "$PACKAGE" >/dev/null
  sleep 1
  STATUS_CLEAN=$(wait_for_clean_status "$phase")
  require_contains "$STATUS_CLEAN" 'active=0'
  require_contains "$STATUS_CLEAN" 'm3_slots=0'
  require_contains "$STATUS_CLEAN" 'm4_slots=0'
  require_contains "$STATUS_CLEAN" 'raw_slots=0'
  require_contains "$STATUS_CLEAN" 's4_slots=0'
  printf '%s\n' "$STATUS_CLEAN" >> "$EVIDENCE"

  EVENT_OUTPUT=$(run_app_command "events $token")
  require_contains "$EVENT_OUTPUT" 'op=18 result=0'
  require_contains "$EVENT_OUTPUT" "op=$expected_clear_op result=0"
  require_contains "$EVENT_OUTPUT" 'op=5 result=0'
  require_contains "$EVENT_OUTPUT" 'op=7 result=0'
  printf '%s\n' "$EVENT_OUTPUT" >> "$EVIDENCE"
}

run_dual_phase() {
  phase=$1
  token=$2
  hold_command=$3
  expected_mode=$4
  expected_slot_a=$5
  expected_slot_b=$6
  expected_clear_op=$7

  printf '%s\n' "phase=$phase token=$token" >> "$EVIDENCE"
  ARM_OUTPUT=$(run_app_command "arm $token")
  require_contains "$ARM_OUTPUT" 'armed uid='
  printf '%s\n' "$ARM_OUTPUT" >> "$EVIDENCE"

  HOLD_OUTPUT=$(run_app_command "$hold_command $token")
  require_contains "$HOLD_OUTPUT" "m5 mode=$expected_mode failures=0"
  require_contains "$HOLD_OUTPUT" 'arm_rc='
  require_contains "$HOLD_OUTPUT" 'ready_rc='
  case "$HOLD_OUTPUT" in
    *"arm_rc=-"*|*"ready_rc=-"*) fail "$phase hold returned a negative arm/ready rc" ;;
  esac
  printf '%s\n' "$HOLD_OUTPUT" >> "$EVIDENCE"

  STATUS_ARMED=$(run_app_command status)
  require_contains "$STATUS_ARMED" 'active=1'
  require_contains "$STATUS_ARMED" "$expected_slot_a=1"
  require_contains "$STATUS_ARMED" "$expected_slot_b=1"
  printf '%s\n' "$STATUS_ARMED" >> "$EVIDENCE"

  adb_device shell am force-stop "$PACKAGE" >/dev/null
  sleep 1
  STATUS_CLEAN=$(wait_for_clean_status "$phase")
  require_contains "$STATUS_CLEAN" 'active=0'
  require_contains "$STATUS_CLEAN" 'm3_slots=0'
  require_contains "$STATUS_CLEAN" 'm4_slots=0'
  require_contains "$STATUS_CLEAN" 'raw_slots=0'
  require_contains "$STATUS_CLEAN" 's4_slots=0'
  printf '%s\n' "$STATUS_CLEAN" >> "$EVIDENCE"

  EVENT_OUTPUT=$(run_app_command "events $token")
  require_contains "$EVENT_OUTPUT" 'op=18 result=0'
  require_contains "$EVENT_OUTPUT" "op=$expected_clear_op result=0"
  require_contains "$EVENT_OUTPUT" 'op=5 result=0'
  require_contains "$EVENT_OUTPUT" 'op=7 result=0'
  printf '%s\n' "$EVENT_OUTPUT" >> "$EVIDENCE"
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
require_contains "$STATUS_INITIAL" 'active=0'
require_contains "$STATUS_INITIAL" 'm3_slots=0'
require_contains "$STATUS_INITIAL" 'm4_slots=0'
require_contains "$STATUS_INITIAL" 's4_slots=0'
printf '%s\n' "$STATUS_INITIAL" >> "$EVIDENCE"

run_phase m3 "$TOKEN" 'm5 m3-hold' 'm3-hold' 'm3_slots' 14
run_phase m4 "$M4_TOKEN" 'm5 m4-hold' 'm4-hold' 'm4_slots' 17
run_phase raw "$RAW_TOKEN" 'm5 raw-hold' 'raw-hold' 'raw_slots' 22
run_phase s4 "$S4_TOKEN" 'm5 s4-hold' 's4-hold' 's4_slots' 26
run_dual_phase s4_raw_step "$S4_RAW_STEP_TOKEN" 'm5 s4-raw-step-hold' \
  's4-raw-step-hold' 'raw_slots' 's4_slots' 26

STATUS_FINAL=$(run_app_command status)
require_contains "$STATUS_FINAL" 'active=0'
require_contains "$STATUS_FINAL" 'm3_slots=0'
require_contains "$STATUS_FINAL" 'm4_slots=0'
require_contains "$STATUS_FINAL" 'raw_slots=0'
require_contains "$STATUS_FINAL" 's4_slots=0'
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
printf 'm5_target_exit_phases=m3,m4,raw,s4,s4_raw_step warn_after=%s final_modules=empty result=pass\n' \
  "$WARN_AFTER" | tee -a "$EVIDENCE"
printf '%s\n' "$EVIDENCE"
