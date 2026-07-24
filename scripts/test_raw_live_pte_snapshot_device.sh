#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SERIAL=${ANDROID_SERIAL:-}
MODULE=r0lab-m1
REMOTE=/data/local/tmp/r0lab-m1.kpm
PACKAGE=dev.r0hook.lab
ACTIVITY=dev.r0hook.lab/.MainActivity
IDLE_SECONDS=${RAW_LIVE_PTE_SNAPSHOT_IDLE_SECONDS:-15}
TOKEN=${RAW_LIVE_PTE_SNAPSHOT_TOKEN:-0x729281}
EVIDENCE_DIR="$ROOT/build/evidence"
EVIDENCE="$EVIDENCE_DIR/raw-live-pte-snapshot-$(date +%Y%m%d-%H%M%S).log"
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
  [ -z "$output" ] || printf '%s\n' "$output"
  printf 'command_timeout command=%s wait_ms=10000\n' "$command"
}

fail() {
  printf '%s\n' "raw live PTE snapshot failure: $*" >&2
  exit 1
}

require_contains() {
  haystack=$1
  needle=$2
  printf '%s\n' "$haystack" | LC_ALL=C grep -F -- "$needle" >/dev/null ||
    fail "expected output not found: $needle"
}

preserve_active_hold() {
  status=$1
  classification=$2
  reason=$3
  trap - EXIT INT TERM
  printf 'classification=%s result=classified active_hold=1 cleanup=not_run reason=%s post_hold_status=not_used boot_id_reader=not_used proc_maps=not_used\n' \
    "$classification" "$reason" | tee -a "$EVIDENCE"
  printf '%s\n' \
    "raw live PTE snapshot preserving module: active raw hold remains and cleanup reentry is forbidden" >&2
  exit "$status"
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
  if [ "$HOLD_ACTIVE" -eq 1 ]; then
    printf 'classification=D4-R3e-L2-setup-or-cleanup-blocked result=classified active_hold=1 cleanup=not_run reason=trap status=%s post_hold_status=not_used boot_id_reader=not_used proc_maps=not_used\n' \
      "$status" >> "$EVIDENCE"
    printf '%s\n' \
      "raw live PTE snapshot preserving module: active raw hold remains and cleanup reentry is forbidden" >&2
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
    esac
  fi
  exit "$status"
}

poll_adb_transport() {
  elapsed=0

  printf 'phase=d4_r3e_l2_idle evidence=begin idle_seconds=%s post_hold_status=not_used boot_id_reader=not_used proc_maps=not_used cleanup=preserve\n' \
    "$IDLE_SECONDS" >> "$EVIDENCE"
  while [ "$elapsed" -lt "$IDLE_SECONDS" ]; do
    sleep 1
    elapsed=$((elapsed + 1))
    set +e
    state=$(adb_device get-state 2>&1)
    rc=$?
    set -e
    state=$(printf '%s' "$state" | LC_ALL=C tr -d '\r')
    printf 'phase=d4_r3e_l2_idle_poll second=%s adb_state_rc=%s adb_state="%s"\n' \
      "$elapsed" "$rc" "$state" >> "$EVIDENCE"
    if [ "$rc" -ne 0 ] || [ "$state" != device ]; then
      printf 'phase=d4_r3e_l2_idle result=fail classification=D4-R3e-L2-live-pte-match-then-unstable second=%s adb_state_rc=%s adb_state="%s"\n' \
        "$elapsed" "$rc" "$state" >> "$EVIDENCE"
      preserve_active_hold 1 \
        D4-R3e-L2-live-pte-match-then-unstable \
        adb-transport-changed
    fi
  done
  printf 'phase=d4_r3e_l2_idle result=pass classification=D4-R3e-L2-live-pte-stable idle_seconds=%s post_hold_status=not_used boot_id_reader=not_used proc_maps=not_used cleanup=preserve\n' \
    "$IDLE_SECONDS" | tee -a "$EVIDENCE"
  preserve_active_hold 0 D4-R3e-L2-live-pte-stable \
    timing-perturbation-observed
}

trap cleanup EXIT INT TERM

case "$IDLE_SECONDS" in
  ''|*[!0-9]*)
    fail "RAW_LIVE_PTE_SNAPSHOT_IDLE_SECONDS must be a non-negative integer"
    ;;
esac

mkdir -p "$EVIDENCE_DIR"
"$ROOT/scripts/build_kpm.sh" >/dev/null
"$ROOT/scripts/build_lab_app.sh" >/dev/null

EXISTING=$(supercmd module list 2>&1) ||
  fail "module list failed: $EXISTING"
[ -z "$EXISTING" ] || fail "refusing to replace a resident KPM: $EXISTING"

adb_device install -r "$ROOT/build/lab-app/r0lab-debug.apk" >/dev/null
adb_device push "$ROOT/kpm/build/r0lab-m1.kpm" "$REMOTE" >/dev/null
LAB_UID=$(adb_device shell "cmd package list packages -U $PACKAGE" |
  LC_ALL=C tr -d '\r' | sed -n 's/.* uid:\([0-9][0-9]*\).*/\1/p')
[ -n "$LAB_UID" ] || fail "Lab App UID not found"
WARN_BEFORE=$(adb_device shell su -c cat /sys/kernel/warn_count |
  LC_ALL=C tr -d '\r')
printf 'serial=%s lab_uid=%s idle_seconds=%s token=%s warn_before=%s\n' \
  "${SERIAL:-default}" "$LAB_UID" "$IDLE_SECONDS" "$TOKEN" "$WARN_BEFORE" |
  tee "$EVIDENCE"

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

HOLD_ACTIVE=1
RAW_OUTPUT=$(run_app_command "raw raw-hold live-pte $TOKEN")
printf '%s\n' "$RAW_OUTPUT" >> "$EVIDENCE"
require_contains "$RAW_OUTPUT" 'raw mode=raw-hold-live-pte'
require_contains "$RAW_OUTPUT" 'exit_mmap_armed=0'
require_contains "$RAW_OUTPUT" 'target_state=source_uxn'
require_contains "$RAW_OUTPUT" 'slots=1'
require_contains "$RAW_OUTPUT" 'raw_slots=1'
require_contains "$RAW_OUTPUT" 'page_records=1'
require_contains "$RAW_OUTPUT" 'activations=0/0'
require_contains "$RAW_OUTPUT" 'states=2/0'
require_contains "$RAW_OUTPUT" 'snapshot_stage=after_arm'
require_contains "$RAW_OUTPUT" 'pte_lock='
require_contains "$RAW_OUTPUT" 'mmap_lock=read'

case "$RAW_OUTPUT" in
  *"walk_rc=0"*) ;;
  *)
    preserve_active_hold 1 D4-R3e-L2-live-pte-walk-failed \
      live-pte-walk-failed
    ;;
esac
case "$RAW_OUTPUT" in
  *"live_match=1"*"live_state=source_uxn"*"stored_state=source_uxn"*"record_backend=raw_two_pfn"*"record_state=source_uxn"*"record_match=1"*) ;;
  *)
    preserve_active_hold 1 D4-R3e-L2-live-pte-mismatch-before-idle \
      live-pte-or-record-mismatch
    ;;
esac

require_contains "$RAW_OUTPUT" 'failures=0'
require_contains "$RAW_OUTPUT" 'inspect=1/0'
require_contains "$RAW_OUTPUT" 'pte_lock=held'
require_contains "$RAW_OUTPUT" 'live_pte='
require_contains "$RAW_OUTPUT" 'expected_pte='
require_contains "$RAW_OUTPUT" 'live_pfn='
require_contains "$RAW_OUTPUT" 'expected_pfn='
printf 'phase=d4_r3e_l2_established result=pass target_state=source_uxn raw_slots=1 page_records=1 snapshot_stage=after_arm live_match=1 post_hold_status=not_used boot_id_reader=not_used proc_maps=not_used\n' \
  >> "$EVIDENCE"

poll_adb_transport
