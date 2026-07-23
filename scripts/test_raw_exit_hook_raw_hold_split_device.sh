#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SERIAL=${ANDROID_SERIAL:-}
MODULE=r0lab-m1
REMOTE=/data/local/tmp/r0lab-m1.kpm
PACKAGE=dev.r0hook.lab
ACTIVITY=dev.r0hook.lab/.MainActivity
TOKEN=${RAW_EXIT_HOOK_RAW_HOLD_SPLIT_TOKEN:-0x729251}
PROBE_PROC_MAPS=${RAW_EXIT_HOOK_RAW_HOLD_SPLIT_PROC_MAPS:-0}
EVIDENCE_DIR="$ROOT/build/evidence"
EVIDENCE="$EVIDENCE_DIR/raw-exit-hook-raw-hold-split-$(date +%Y%m%d-%H%M%S).log"
MODULE_LOADED=0
SESSION_OPEN=0
HOLD_ACTIVE=0

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

extract_boot_uuid() {
  printf '%s\n' "$1" |
    LC_ALL=C grep -Eo '[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}' |
    sed -n '1p'
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
  raw=$(adb_device shell cat /proc/sys/kernel/random/boot_id 2>/dev/null |
    LC_ALL=C tr -d '\r' || true)
  extract_boot_uuid "$raw"
}

boot_props() {
  boot_completed=$(adb_device shell getprop sys.boot_completed 2>/dev/null |
    LC_ALL=C tr -d '\r' || true)
  bootreason=$(adb_device shell getprop ro.boot.bootreason 2>/dev/null |
    LC_ALL=C tr -d '\r' || true)
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
  printf '%s\n' "raw exit-hook raw-hold split failure: $*" >&2
  exit 1
}

require_contains() {
  haystack=$1
  needle=$2
  printf '%s\n' "$haystack" | LC_ALL=C grep -F -- "$needle" >/dev/null ||
    fail "expected output not found: $needle"
}

require_boot_stable_after_reader() {
  phase=$1
  before=$2
  set +e
  raw_output=$(adb_device shell cat /proc/sys/kernel/random/boot_id 2>&1)
  rc=$?
  set -e
  output=$(printf '%s' "$raw_output" | LC_ALL=C tr -d '\r')
  parsed=$(extract_boot_uuid "$output")
  props=$(boot_props || true)
  printf 'phase=%s boot_id_reader_rc=%s boot_before=%s boot_after_uuid=%s output="%s" %s\n' \
    "$phase" "$rc" "$before" "$parsed" "$output" "$props" >> "$EVIDENCE"
  if [ "$rc" -ne 0 ] || [ -z "$parsed" ] || [ "$before" != "$parsed" ]; then
    capture_pstore "$(basename "$EVIDENCE" .log)-$phase"
    fail "boot-id reader unstable during $phase: rc=$rc before=$before after=$parsed"
  fi
}

probe_adb_shell_true() {
  set +e
  output=$(adb_device shell true 2>&1)
  rc=$?
  set -e
  output=$(printf '%s' "$output" | LC_ALL=C tr -d '\r')
  printf 'phase=d4_r3b_shell_true shell_rc=%s output="%s"\n' \
    "$rc" "$output" >> "$EVIDENCE"
  [ "$rc" -eq 0 ] || fail "adb shell true failed while hold active: rc=$rc"
}

probe_getprop() {
  set +e
  output=$(adb_device shell getprop sys.boot_completed 2>&1)
  rc=$?
  set -e
  output=$(printf '%s' "$output" | LC_ALL=C tr -d '\r')
  printf 'phase=d4_r3b_getprop shell_rc=%s sys.boot_completed="%s"\n' \
    "$rc" "$output" >> "$EVIDENCE"
  [ "$rc" -eq 0 ] || fail "getprop failed while hold active: rc=$rc"
}

probe_lab_proc_maps() {
  [ "$PROBE_PROC_MAPS" = 1 ] || return 0
  set +e
  pid=$(adb_device shell pidof "$PACKAGE" 2>/dev/null | LC_ALL=C tr -d '\r')
  rc_pid=$?
  set -e
  printf 'phase=d4_r3b_proc_maps_pid pidof_rc=%s lab_pid="%s"\n' \
    "$rc_pid" "$pid" >> "$EVIDENCE"
  [ "$rc_pid" -eq 0 ] && [ -n "$pid" ] ||
    fail "could not resolve Lab App pid while hold active"

  set +e
  output=$(adb_device shell "cat /proc/$pid/maps >/dev/null" 2>&1)
  rc=$?
  set -e
  output=$(printf '%s' "$output" | LC_ALL=C tr -d '\r')
  printf 'phase=d4_r3b_proc_maps shell_rc=%s output="%s"\n' \
    "$rc" "$output" >> "$EVIDENCE"
  [ "$rc" -eq 0 ] || fail "Lab /proc/<pid>/maps read failed while hold active: rc=$rc"
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
      "raw exit-hook raw-hold split preserving module: active raw hold remains and cleanup reentry is forbidden" >&2
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
          "raw exit-hook raw-hold split cleanup did not confirm empty state; preserving module" >&2
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
BOOT_START=$(boot_id || true)
[ -n "$BOOT_START" ] || fail "empty boot id before d4_r3b setup"
printf 'serial=%s lab_uid=%s token=%s proc_maps=%s warn_before=%s boot_start=%s %s\n' \
  "${SERIAL:-default}" "$LAB_UID" "$TOKEN" "$PROBE_PROC_MAPS" \
  "$WARN_BEFORE" "$BOOT_START" "$(boot_props)" | tee "$EVIDENCE"

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

RAW_OUTPUT=$(run_app_command "raw raw-hold routing hold $TOKEN")
printf '%s\n' "$RAW_OUTPUT" >> "$EVIDENCE"
require_contains "$RAW_OUTPUT" 'raw mode=raw-hold-routing-hold failures=0'
require_contains "$RAW_OUTPUT" 'exit_mmap_armed=0'
require_contains "$RAW_OUTPUT" 'raw_slots=2'
require_contains "$RAW_OUTPUT" 'page_records=2'
require_contains "$RAW_OUTPUT" 'normal=42/42'
require_contains "$RAW_OUTPUT" 'shadow=99/99'
require_contains "$RAW_OUTPUT" 'activations=1/1'
require_contains "$RAW_OUTPUT" 'states=3/3'
require_contains "$RAW_OUTPUT" 'inspect=1/1'
require_contains "$RAW_OUTPUT" 'hook_arm_rc=-1/-1'
require_contains "$RAW_OUTPUT" 'hook_status_rc=-1/-1'
require_contains "$RAW_OUTPUT" 'exit_hook_installed=0/0'
require_contains "$RAW_OUTPUT" 'generation='
HOLD_ACTIVE=1
GENERATIONS=$(printf '%s\n' "$RAW_OUTPUT" |
  sed -n 's/.*generation=\([0-9][0-9]*\)\/\([0-9][0-9]*\).*/\1 \2/p' |
  sed -n '1p')
[ -n "$GENERATIONS" ] || fail "could not parse raw-hold generations"
set -- $GENERATIONS
GEN0=$1
GEN1=$2
printf 'phase=d4_r3b_raw_hold_established result=pass exit_mmap_armed=0 raw_slots=2 page_records=2 generation=%s/%s\n' \
  "$GEN0" "$GEN1" >> "$EVIDENCE"

STATUS_HELD=$(run_app_command status)
require_contains "$STATUS_HELD" 'active=1'
require_contains "$STATUS_HELD" 'raw_slots=2'
require_contains "$STATUS_HELD" 'page_records=2'
printf '%s\n' "$STATUS_HELD" >> "$EVIDENCE"

sleep 2
STATUS_HELD_QUIET=$(run_app_command status)
require_contains "$STATUS_HELD_QUIET" 'active=1'
require_contains "$STATUS_HELD_QUIET" 'raw_slots=2'
require_contains "$STATUS_HELD_QUIET" 'page_records=2'
printf '%s\n' "$STATUS_HELD_QUIET" >> "$EVIDENCE"
printf 'phase=d4_r3b_hold_quiet_no_boot_reader result=pass boot_id_reader=not_used\n' >> "$EVIDENCE"

probe_adb_shell_true
probe_getprop
probe_lab_proc_maps
require_boot_stable_after_reader d4_r3b_boot_id_reader "$BOOT_START"
printf 'phase=d4_r3b_boot_id_reader result=pass boot_stable=1\n' >> "$EVIDENCE"

SLOT0_CLEAR=$(run_app_command "raw slot clear $TOKEN 0")
require_contains "$SLOT0_CLEAR" 'raw_slot_clear_pending slot=0'
printf '%s\n' "$SLOT0_CLEAR" >> "$EVIDENCE"
SLOT0_CLEARED_OUTPUT=$(wait_for_slot_cleared "$TOKEN" 0) ||
  fail "slot 0 did not report cleared"
printf '%s\n' "$SLOT0_CLEARED_OUTPUT" >> "$EVIDENCE"

SLOT1_CLEAR=$(run_app_command "raw slot clear $TOKEN 1")
require_contains "$SLOT1_CLEAR" 'raw_slot_clear_pending slot=1'
printf '%s\n' "$SLOT1_CLEAR" >> "$EVIDENCE"
SLOT1_CLEARED_OUTPUT=$(wait_for_slot_cleared "$TOKEN" 1) ||
  fail "slot 1 did not report cleared"
printf '%s\n' "$SLOT1_CLEARED_OUTPUT" >> "$EVIDENCE"
HOLD_ACTIVE=0

STATUS_AFTER_CLEAR=$(run_app_command status)
require_contains "$STATUS_AFTER_CLEAR" 'active=1'
require_contains "$STATUS_AFTER_CLEAR" 'raw_slots=0'
require_contains "$STATUS_AFTER_CLEAR" 'page_records=0'
printf '%s\n' "$STATUS_AFTER_CLEAR" >> "$EVIDENCE"
printf 'phase=d4_r3b_explicit_cleanup result=pass raw_slots=0 page_records=0 exit_hook_clear=not_used\n' >> "$EVIDENCE"

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
BOOT_END=$(boot_id || true)
[ "$BOOT_START" = "$BOOT_END" ] ||
  fail "boot id changed across raw-hold split: $BOOT_START -> $BOOT_END"
printf 'raw_exit_hook_raw_hold_split=pass phases=5 boot_reader=pass proc_maps=%s warn_after=%s boot_end=%s final_modules=empty result=pass\n' \
  "$PROBE_PROC_MAPS" "$WARN_AFTER" "$BOOT_END" | tee -a "$EVIDENCE"
