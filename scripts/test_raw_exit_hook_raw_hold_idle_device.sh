#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SERIAL=${ANDROID_SERIAL:-}
MODULE=r0lab-m1
REMOTE=/data/local/tmp/r0lab-m1.kpm
PACKAGE=dev.r0hook.lab
ACTIVITY=dev.r0hook.lab/.MainActivity
TOKEN=${RAW_EXIT_HOOK_RAW_HOLD_IDLE_TOKEN:-0x729261}
IDLE_SECONDS=${RAW_EXIT_HOOK_RAW_HOLD_IDLE_SECONDS:-15}
EVIDENCE_DIR="$ROOT/build/evidence"
EVIDENCE="$EVIDENCE_DIR/raw-exit-hook-raw-hold-idle-$(date +%Y%m%d-%H%M%S).log"
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
  printf '%s\n' "raw exit-hook raw-hold idle failure: $*" >&2
  exit 1
}

require_contains() {
  haystack=$1
  needle=$2
  printf '%s\n' "$haystack" | LC_ALL=C grep -F -- "$needle" >/dev/null ||
    fail "expected output not found: $needle"
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

preserve_active_hold() {
  status=$1
  reason=$2
  trap - EXIT INT TERM
  printf 'phase=d4_r3d_raw_hold_idle_preserve active_hold=1 cleanup=not_run reason=%s status=%s\n' \
    "$reason" "$status" >> "$EVIDENCE"
  printf '%s\n' \
    "raw exit-hook raw-hold idle preserving module: active raw hold remains and cleanup reentry is forbidden" >&2
  exit "$status"
}

cleanup() {
  status=$?
  trap - EXIT INT TERM
  if [ "$HOLD_ACTIVE" -eq 1 ]; then
    printf 'phase=d4_r3d_raw_hold_idle_preserve active_hold=1 cleanup=not_run reason=trap status=%s\n' \
      "$status" >> "$EVIDENCE"
    printf '%s\n' \
      "raw exit-hook raw-hold idle preserving module: active raw hold remains and cleanup reentry is forbidden" >&2
    exit "$status"
  fi
  if [ "$status" -ne 0 ] && [ "$SESSION_OPEN" -eq 1 ]; then
    run_app_command "close $TOKEN" >/dev/null 2>&1 || true
    SESSION_OPEN=0
  fi
  if [ "$status" -ne 0 ] && [ "$MODULE_LOADED" -eq 1 ]; then
    if prepare_worker_shutdown >/dev/null 2>&1; then
      supercmd module unload "$MODULE" >/dev/null 2>&1 || true
    else
      printf '%s\n' \
        "raw exit-hook raw-hold idle cleanup could not stop workers; preserving module" >&2
    fi
  fi
  exit "$status"
}

poll_adb_transport() {
  elapsed=0
  printf 'phase=d4_r3d_raw_hold_idle evidence=begin idle_seconds=%s post_hold_status=not_used boot_id_reader=not_used proc_maps=not_used cleanup=preserve\n' \
    "$IDLE_SECONDS" >> "$EVIDENCE"
  while [ "$elapsed" -lt "$IDLE_SECONDS" ]; do
    sleep 1
    elapsed=$((elapsed + 1))
    set +e
    state=$(adb_device get-state 2>&1)
    rc=$?
    set -e
    state=$(printf '%s' "$state" | LC_ALL=C tr -d '\r')
    printf 'phase=d4_r3d_raw_hold_idle_poll second=%s adb_state_rc=%s adb_state="%s"\n' \
      "$elapsed" "$rc" "$state" >> "$EVIDENCE"
    if [ "$rc" -ne 0 ] || [ "$state" != device ]; then
      printf 'phase=d4_r3d_raw_hold_idle result=fail classification=D4-R3d-raw-hold-self-unstable second=%s adb_state_rc=%s adb_state="%s"\n' \
        "$elapsed" "$rc" "$state" >> "$EVIDENCE"
      capture_pstore "$(basename "$EVIDENCE" .log)-d4_r3d_raw_hold_idle"
      fail "D4-R3d-raw-hold-self-unstable: adb transport changed during idle hold"
    fi
  done
  printf 'phase=d4_r3d_raw_hold_idle result=pass classification=D4-R3d-raw-hold-idle-stable idle_seconds=%s post_hold_status=not_used boot_id_reader=not_used proc_maps=not_used cleanup=preserve\n' \
    "$IDLE_SECONDS" | tee -a "$EVIDENCE"
  preserve_active_hold 0 D4-R3d-raw-hold-idle-stable
}

trap cleanup EXIT INT TERM

case "$IDLE_SECONDS" in
  ''|*[!0-9]*) fail "RAW_EXIT_HOOK_RAW_HOLD_IDLE_SECONDS must be a non-negative integer" ;;
esac

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
printf 'serial=%s lab_uid=%s token=%s idle_seconds=%s warn_before=%s %s\n' \
  "${SERIAL:-default}" "$LAB_UID" "$TOKEN" "$IDLE_SECONDS" \
  "$WARN_BEFORE" "$(boot_props)" | tee "$EVIDENCE"

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
require_contains "$RAW_OUTPUT" 'exit_hook_installed=not_queried/not_queried'
require_contains "$RAW_OUTPUT" 'generation='
HOLD_ACTIVE=1
GENERATIONS=$(printf '%s\n' "$RAW_OUTPUT" |
  sed -n 's/.*generation=\([0-9][0-9]*\)\/\([0-9][0-9]*\).*/\1 \2/p' |
  sed -n '1p')
[ -n "$GENERATIONS" ] || fail "could not parse raw-hold generations"
set -- $GENERATIONS
GEN0=$1
GEN1=$2
printf 'phase=d4_r3d_raw_hold_established result=pass exit_mmap_armed=0 raw_slots=2 page_records=2 generation=%s/%s\n' \
  "$GEN0" "$GEN1" >> "$EVIDENCE"

poll_adb_transport
