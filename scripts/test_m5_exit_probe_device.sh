#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SERIAL=${ANDROID_SERIAL:-}
MODULE=r0lab-m1
REMOTE=/data/local/tmp/r0lab-m1.kpm
PACKAGE=dev.r0hook.lab
ACTIVITY=dev.r0hook.lab/.MainActivity
EVIDENCE_DIR="$ROOT/build/evidence"
EVIDENCE="$EVIDENCE_DIR/m5-exit-probe-$(date +%Y%m%d-%H%M%S).log"

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
  printf '%s\n' "M5 exit-probe failure: $*" >&2
  exit 1
}

require_contains() {
  haystack=$1
  needle=$2
  printf '%s\n' "$haystack" | grep -F -- "$needle" >/dev/null ||
    fail "expected output not found: $needle"
}

run_app_command() {
  command=$1

  adb_device logcat -c >/dev/null
  adb_device shell "am start -W -n $ACTIVITY --es r0lab_command '$command'" >/dev/null
  sleep 1
  adb_device logcat -d -v brief -s R0Lab:I '*:S' | tr -d '\r'
}

mkdir -p "$EVIDENCE_DIR"
"$ROOT/scripts/build_kpm.sh" >/dev/null
"$ROOT/scripts/build_lab_app.sh" >/dev/null

EXISTING=$(supercmd module list 2>&1) || fail "module list failed: $EXISTING"
[ -z "$EXISTING" ] || fail "refusing to probe with a resident KPM: $EXISTING"

adb_device install -r "$ROOT/build/lab-app/r0lab-debug.apk" >/dev/null
adb_device push "$ROOT/kpm/build/r0lab-m1.kpm" "$REMOTE" >/dev/null
LAB_UID=$(adb_device shell "cmd package list packages -U $PACKAGE" |
  tr -d '\r' | sed -n 's/.* uid:\([0-9][0-9]*\).*/\1/p')
[ -n "$LAB_UID" ] || fail "Lab App UID not found"
WARN_BEFORE=$(adb_device shell su -c cat /sys/kernel/warn_count | tr -d '\r')
printf 'serial=%s lab_uid=%s warn_before=%s\n' \
  "${SERIAL:-default}" "$LAB_UID" "$WARN_BEFORE" | tee "$EVIDENCE"

LOAD_OUTPUT=$(supercmd module load "$REMOTE" "lab_uid=$LAB_UID,exit_probe=1" 2>&1) ||
  fail "probe module load failed: $LOAD_OUTPUT"
case "$LOAD_OUTPUT" in
  *"supercmd error code"*) fail "probe module load rejected: $LOAD_OUTPUT" ;;
esac
printf 'module_load=%s\n' "$LOAD_OUTPUT" | tee -a "$EVIDENCE"

set +e
REJECT_OUTPUT=$(supercmd module unload "$MODULE" 2>&1)
REJECT_STATUS=$?
set -e
printf 'rejection_status=%s rejection_output=%s\n' \
  "$REJECT_STATUS" "$REJECT_OUTPUT" | tee -a "$EVIDENCE"

LIST_AFTER_REJECT=$(supercmd module list 2>&1) || fail "list after rejection failed: $LIST_AFTER_REJECT"
printf 'list_after_rejection=%s\n' "$LIST_AFTER_REJECT" | tee -a "$EVIDENCE"
if [ -z "$LIST_AFTER_REJECT" ]; then
  WARN_AFTER=$(adb_device shell su -c cat /sys/kernel/warn_count | tr -d '\r')
  [ "$WARN_BEFORE" = "$WARN_AFTER" ] ||
    fail "warn_count changed: $WARN_BEFORE -> $WARN_AFTER"
  printf 'runtime_behavior=exit_return_ignored\n' | tee -a "$EVIDENCE"
  printf 'warn_after=%s final_modules=empty result=pass-runtime_exit_ignored\n' \
    "$WARN_AFTER" | tee -a "$EVIDENCE"
  printf '%s\n' "$EVIDENCE"
  exit 0
fi
printf '%s\n' "$LIST_AFTER_REJECT" | grep -qx "$MODULE" ||
  fail "unexpected module list after exit rejection: $LIST_AFTER_REJECT"

ALLOW_OUTPUT=$(run_app_command 'unload allow')
require_contains "$ALLOW_OUTPUT" 'unload_allowed'
printf '%s\n' "$ALLOW_OUTPUT" >> "$EVIDENCE"

UNLOAD_OUTPUT=$(supercmd module unload "$MODULE" 2>&1) ||
  fail "permitted unload failed: $UNLOAD_OUTPUT"
case "$UNLOAD_OUTPUT" in
  *"supercmd error code"*) fail "permitted unload rejected: $UNLOAD_OUTPUT" ;;
esac
printf 'permitted_unload=%s\n' "$UNLOAD_OUTPUT" | tee -a "$EVIDENCE"
FINAL_MODULES=$(supercmd module list 2>&1) || fail "final list failed: $FINAL_MODULES"
[ -z "$FINAL_MODULES" ] || fail "module remained loaded: $FINAL_MODULES"
WARN_AFTER=$(adb_device shell su -c cat /sys/kernel/warn_count | tr -d '\r')
[ "$WARN_BEFORE" = "$WARN_AFTER" ] ||
  fail "warn_count changed: $WARN_BEFORE -> $WARN_AFTER"
printf 'warn_after=%s final_modules=empty result=pass\n' "$WARN_AFTER" | tee -a "$EVIDENCE"
printf '%s\n' "$EVIDENCE"
