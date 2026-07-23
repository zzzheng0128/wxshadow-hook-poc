#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SERIAL=${ANDROID_SERIAL:-}
MODULE=r0lab-m1
REMOTE=/data/local/tmp/r0lab-m1.kpm
REMOTE_PROBE=/data/local/tmp/r0lab-nonlab-control-probe
PACKAGE=dev.r0hook.lab
ACTIVITY=dev.r0hook.lab/.MainActivity
TOKEN=${M1_TOKEN:-0x1a1100}
EVIDENCE_DIR="$ROOT/build/evidence"
EVIDENCE="$EVIDENCE_DIR/m1-isolation-$(date +%Y%m%d-%H%M%S).log"
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
  printf '%s\n' "M1 isolation failure: $*" >&2
  exit 1
}

require_contains() {
  haystack=$1
  needle=$2
  printf '%s\n' "$haystack" | grep -F -- "$needle" >/dev/null ||
    fail "expected output not found: $needle"
}

run_nonlab_probe() {
  adb_device shell "$REMOTE_PROBE" 2>&1 | tr -d '\r'
}

cleanup() {
  status=$?
  trap - EXIT INT TERM
  if [ "$MODULE_LOADED" -eq 1 ]; then
    run_app_command "close $TOKEN" >/dev/null 2>&1 || true
    CLEANUP_STATUS=$(run_app_command status 2>&1 || true)
    case "$CLEANUP_STATUS" in
      *"active=0"*"hwbp_slots=0"*"m3_slots=0"*"m4_slots=0"*)
        if prepare_worker_shutdown >/dev/null 2>&1; then
          supercmd module unload "$MODULE" >/dev/null 2>&1 || true
        else
          printf '%s\n' "M1 cleanup could not stop workers; preserving module" >&2
        fi
        ;;
      *) printf '%s\n' "M1 cleanup did not confirm empty state; preserving module" >&2 ;;
    esac
  fi
  exit "$status"
}

trap cleanup EXIT INT TERM

mkdir -p "$EVIDENCE_DIR" "$ROOT/build/tools"
"$ROOT/scripts/build_kpm.sh" >/dev/null
"$ROOT/scripts/build_lab_app.sh" >/dev/null
"${ANDROID_NDK_HOME:-/Users/ivory/Library/Android/sdk/ndk/29.0.14206865}/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android23-clang" \
  -O2 -Wall -Wextra -Werror -o "$ROOT/build/tools/r0lab-nonlab-control-probe" \
  "$ROOT/tools/r0lab_nonlab_control_probe.c"

EXISTING=$(supercmd module list 2>&1) || fail "module list failed: $EXISTING"
[ -z "$EXISTING" ] || fail "refusing to replace a resident KPM: $EXISTING"
adb_device install -r "$ROOT/build/lab-app/r0lab-debug.apk" >/dev/null
adb_device push "$ROOT/kpm/build/r0lab-m1.kpm" "$REMOTE" >/dev/null
adb_device push "$ROOT/build/tools/r0lab-nonlab-control-probe" "$REMOTE_PROBE" >/dev/null
adb_device shell chmod 755 "$REMOTE_PROBE" >/dev/null
LAB_UID=$(adb_device shell "cmd package list packages -U $PACKAGE" |
  tr -d '\r' | sed -n 's/.* uid:\([0-9][0-9]*\).*/\1/p')
[ -n "$LAB_UID" ] || fail "Lab App UID not found"
WARN_BEFORE=$(adb_device shell su -c cat /sys/kernel/warn_count | tr -d '\r')

LOAD_OUTPUT=$(supercmd module load "$REMOTE" "lab_uid=$LAB_UID" 2>&1) ||
  fail "module load failed: $LOAD_OUTPUT"
case "$LOAD_OUTPUT" in
  *"supercmd error code"*) fail "module load rejected: $LOAD_OUTPUT" ;;
esac
MODULE_LOADED=1

PROBE_IDLE=$(run_nonlab_probe)
require_contains "$PROBE_IDLE" 'uid=2000 rc=-1 errno=1'
ARM_OUTPUT=$(run_app_command "arm $TOKEN")
require_contains "$ARM_OUTPUT" 'armed uid='
PROBE_ACTIVE=$(run_nonlab_probe)
require_contains "$PROBE_ACTIVE" 'uid=2000 rc=-1 errno=1'
CLOSE_OUTPUT=$(run_app_command "close $TOKEN")
require_contains "$CLOSE_OUTPUT" 'closed final_summary_seq='
EVENT_OUTPUT=$(run_app_command "events $TOKEN")
require_contains "$EVENT_OUTPUT" 'op=5 result=0'
require_contains "$EVENT_OUTPUT" 'op=7 result=0'
case "$EVENT_OUTPUT" in
  *"cpu=-1"*) fail "event CPU capture remained unavailable" ;;
esac
PROBE_CLOSED=$(run_nonlab_probe)
require_contains "$PROBE_CLOSED" 'uid=2000 rc=-1 errno=1'
STATUS_OUTPUT=$(run_app_command status)
require_contains "$STATUS_OUTPUT" 'active=0'
require_contains "$STATUS_OUTPUT" 'hwbp_slots=0'
require_contains "$STATUS_OUTPUT" 'm3_slots=0'
require_contains "$STATUS_OUTPUT" 'm4_slots=0'

prepare_worker_shutdown || fail "workers did not stop before module unload"
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

{
  printf 'lab_uid=%s token=%s warn_before=%s\n' "$LAB_UID" "$TOKEN" "$WARN_BEFORE"
  printf 'probe_idle=%s\n' "$PROBE_IDLE"
  printf '%s\n' "$ARM_OUTPUT"
  printf 'probe_active=%s\n' "$PROBE_ACTIVE"
  printf '%s\n' "$CLOSE_OUTPUT"
  printf '%s\n' "$EVENT_OUTPUT"
  printf 'probe_closed=%s\n' "$PROBE_CLOSED"
  printf '%s\n' "$STATUS_OUTPUT"
  printf 'warn_after=%s final_modules=empty result=pass\n' "$WARN_AFTER"
} | tee "$EVIDENCE"
printf '%s\n' "$EVIDENCE"
