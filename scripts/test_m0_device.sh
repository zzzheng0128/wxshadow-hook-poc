#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SERIAL=${ANDROID_SERIAL:-}
LOOPS=${M0_LOOPS:-100}
MODULE=r0lab-m1
REMOTE=/data/local/tmp/r0lab-m1.kpm
PACKAGE=dev.r0hook.lab
ACTIVITY=dev.r0hook.lab/.MainActivity
EVIDENCE_DIR="$ROOT/build/evidence"
EVIDENCE="$EVIDENCE_DIR/m0-device-$(date +%Y%m%d-%H%M%S).log"
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

fail() {
  printf '%s\n' "M0 failure: $*" >&2
  exit 1
}

cleanup() {
  if [ "$MODULE_LOADED" -eq 1 ]; then
    if prepare_worker_shutdown >/dev/null 2>&1; then
      supercmd module unload "$MODULE" >/dev/null 2>&1 || true
    else
      printf '%s\n' "M0 cleanup could not stop workers; preserving module" >&2
    fi
  fi
}

trap cleanup EXIT

app_logs() {
  adb_device logcat -d -v brief -s R0Lab:I '*:S' | tr -d '\r'
}

run_app_command() {
  command=$1
  adb_device logcat -c >/dev/null
  adb_device shell "am start -W -n $ACTIVITY --es r0lab_command '$command'" >/dev/null
  sleep 0.1
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

BEFORE_WARN=$(adb_device shell su -c cat /sys/kernel/warn_count | tr -d '\r')
printf 'serial=%s loops=%s lab_uid=%s warn_before=%s\n' "${SERIAL:-default}" "$LOOPS" "$LAB_UID" "$BEFORE_WARN" | tee "$EVIDENCE"

for i in $(seq 1 "$LOOPS"); do
  LOAD_OUTPUT=$(supercmd module load "$REMOTE" "lab_uid=$LAB_UID" 2>&1) || fail "load command failed at iteration $i: $LOAD_OUTPUT"
  case "$LOAD_OUTPUT" in
    *"supercmd error code"*) fail "load rejected at iteration $i: $LOAD_OUTPUT" ;;
  esac
  MODULE_LOADED=1

  LIST_OUTPUT=$(supercmd module list 2>&1) || fail "list command failed at iteration $i: $LIST_OUTPUT"
  printf '%s\n' "$LIST_OUTPUT" | grep -qx "$MODULE" || fail "module absent after load at iteration $i"

  prepare_worker_shutdown || fail "workers did not stop before unload at iteration $i"
  UNLOAD_OUTPUT=$(supercmd module unload "$MODULE" 2>&1) || fail "unload command failed at iteration $i: $UNLOAD_OUTPUT"
  case "$UNLOAD_OUTPUT" in
    *"supercmd error code"*) fail "unload rejected at iteration $i: $UNLOAD_OUTPUT" ;;
  esac
  MODULE_LOADED=0

  if [ $((i % 10)) -eq 0 ] || [ "$i" -eq "$LOOPS" ]; then
    printf 'completed=%s\n' "$i" | tee -a "$EVIDENCE"
  fi
done

AFTER_WARN=$(adb_device shell su -c cat /sys/kernel/warn_count | tr -d '\r')
FINAL_LIST=$(supercmd module list 2>&1) || fail "final list failed: $FINAL_LIST"
[ -z "$FINAL_LIST" ] || fail "module remained loaded: $FINAL_LIST"
[ "$BEFORE_WARN" = "$AFTER_WARN" ] || fail "warn_count changed: $BEFORE_WARN -> $AFTER_WARN"

printf 'warn_after=%s final_modules=empty result=pass\n' "$AFTER_WARN" | tee -a "$EVIDENCE"
printf '%s\n' "$EVIDENCE"
