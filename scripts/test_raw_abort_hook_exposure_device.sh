#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SERIAL=${ANDROID_SERIAL:-}
MODULE=r0lab-m1
REMOTE=/data/local/tmp/r0lab-m1.kpm
PACKAGE=dev.r0hook.lab
ACTIVITY=dev.r0hook.lab/.MainActivity
IDLE_SECONDS=15
CLEAN_BOOT_CONFIRMED=${RAW_ABORT_EXPOSURE_CLEAN_BOOT_CONFIRMED:-}
TOKEN=${RAW_ABORT_EXPOSURE_TOKEN:-0x729295}
EVIDENCE_DIR="$ROOT/build/evidence"
EVIDENCE="$EVIDENCE_DIR/raw-abort-hook-exposure-$(date +%Y%m%d-%H%M%S).log"
MODULE_LOADED=0
SESSION_OPEN=0
HOLD_ACTIVE=0

fail() {
  printf '%s\n' "raw abort-hook exposure failure: $*" >&2
  exit 1
}

case "$CLEAN_BOOT_CONFIRMED" in
  1) ;;
  *)
    fail "RAW_ABORT_EXPOSURE_CLEAN_BOOT_CONFIRMED=1 is required before device access"
    ;;
esac

ensure_clean_source() {
  dirty=$(
    git -C "$ROOT" status --porcelain --untracked-files=no 2>/dev/null
  ) || fail "D4-R3f requires a git clean-source temporary tree"
  [ -z "$dirty" ] ||
    fail "D4-R3f refuses tracked source changes: $dirty"
}

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
  classification=$2
  reason=$3
  trap - EXIT INT TERM
  printf 'classification=%s result=classified active_hold=1 cleanup=not_run reason=%s post_hold_status=not_used boot_id_reader=not_used getprop=not_used proc_maps=not_used pstore=not_used\n' \
    "$classification" "$reason" | tee -a "$EVIDENCE"
  printf '%s\n' \
    "raw abort-hook exposure preserving module: active raw hold remains and cleanup reentry is forbidden" >&2
  exit "$status"
}

cleanup() {
  status=$?
  trap - EXIT INT TERM
  if [ "$HOLD_ACTIVE" -eq 1 ]; then
    printf 'classification=D4-R3f-setup-blocked result=classified active_hold=unknown cleanup=not_run reason=trap status=%s post_hold_status=not_used boot_id_reader=not_used getprop=not_used proc_maps=not_used pstore=not_used\n' \
      "$status" >> "$EVIDENCE"
    printf '%s\n' \
      "raw abort-hook exposure preserving module: raw hold may be active and cleanup reentry is forbidden" >&2
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

  printf 'phase=d4_r3f_no_abort_idle evidence=begin idle_seconds=%s post_hold_status=not_used boot_id_reader=not_used getprop=not_used proc_maps=not_used pstore=not_used cleanup=preserve\n' \
    "$IDLE_SECONDS" >> "$EVIDENCE"
  while [ "$elapsed" -lt "$IDLE_SECONDS" ]; do
    sleep 1
    elapsed=$((elapsed + 1))
    set +e
    state=$(adb_device get-state 2>&1)
    rc=$?
    set -e
    state=$(printf '%s' "$state" | LC_ALL=C tr -d '\r')
    printf 'phase=d4_r3f_no_abort_idle_poll second=%s adb_state_rc=%s adb_state="%s"\n' \
      "$elapsed" "$rc" "$state" >> "$EVIDENCE"
    if [ "$rc" -ne 0 ] || [ "$state" != device ]; then
      printf 'phase=d4_r3f_no_abort_idle result=fail classification=D4-R3f-source-uxn-no-abort-unstable second=%s adb_state_rc=%s adb_state="%s"\n' \
        "$elapsed" "$rc" "$state" >> "$EVIDENCE"
      preserve_active_hold 1 D4-R3f-source-uxn-no-abort-unstable \
        adb-transport-changed
    fi
  done
  printf 'phase=d4_r3f_no_abort_idle result=pass classification=D4-R3f-source-uxn-no-abort-stable idle_seconds=%s post_hold_status=not_used boot_id_reader=not_used getprop=not_used proc_maps=not_used pstore=not_used cleanup=preserve\n' \
    "$IDLE_SECONDS" | tee -a "$EVIDENCE"
  preserve_active_hold 0 D4-R3f-source-uxn-no-abort-stable \
    idle-window-complete
}

ensure_clean_source
mkdir -p "$EVIDENCE_DIR"
trap cleanup EXIT INT TERM

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
printf 'serial=%s lab_uid=%s idle_seconds=%s token=%s warn_before=%s source_tree=clean clean_boot_confirmed=1 diagnostic=global_abort_hook_suppressed\n' \
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
RAW_OUTPUT=$(run_app_command "raw raw-hold no-abort $TOKEN")
printf '%s\n' "$RAW_OUTPUT" >> "$EVIDENCE"
require_contains "$RAW_OUTPUT" 'raw mode=raw-hold-no-abort'
require_contains "$RAW_OUTPUT" 'failures=0'
require_contains "$RAW_OUTPUT" 'exit_mmap_armed=0'
require_contains "$RAW_OUTPUT" 'target_state=source_uxn'
require_contains "$RAW_OUTPUT" 'slots=1'
require_contains "$RAW_OUTPUT" 'raw_slots=1'
require_contains "$RAW_OUTPUT" 'page_records=1'
require_contains "$RAW_OUTPUT" 'normal=42/-1'
require_contains "$RAW_OUTPUT" 'shadow=-1/-1'
require_contains "$RAW_OUTPUT" 'activations=0/0'
require_contains "$RAW_OUTPUT" 'states=2/0'
require_contains "$RAW_OUTPUT" 'inspect=1/0'
require_contains "$RAW_OUTPUT" 'abort_hook_installed=0'
require_contains "$RAW_OUTPUT" 'abort_hook_suppressed=1'
require_contains "$RAW_OUTPUT" 'generation='
printf 'phase=d4_r3f_no_abort_established result=pass target_state=source_uxn raw_slots=1 page_records=1 abort_hook_installed=0 abort_hook_suppressed=1 post_hold_status=not_used boot_id_reader=not_used getprop=not_used proc_maps=not_used pstore=not_used\n' \
  >> "$EVIDENCE"

poll_adb_transport
