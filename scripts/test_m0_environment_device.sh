#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SERIAL=${ANDROID_SERIAL:-}
ACTIVITY=dev.r0hook.lab/.MainActivity
EVIDENCE_DIR="$ROOT/build/evidence"
EVIDENCE="$EVIDENCE_DIR/m0-environment-$(date +%Y%m%d-%H%M%S).log"

adb_device() {
  if [ -n "$SERIAL" ]; then
    adb -s "$SERIAL" "$@"
  else
    adb "$@"
  fi
}

fail() {
  printf '%s\n' "M0 environment failure: $*" >&2
  exit 1
}

require_contains() {
  haystack=$1
  needle=$2
  printf '%s\n' "$haystack" | grep -F -- "$needle" >/dev/null ||
    fail "expected output not found: $needle"
}

require_nonempty_field() {
  output=$1
  field=$2
  value=$(printf '%s\n' "$output" | sed -n "s/^$field=//p" | sed -n '1p')
  [ -n "$value" ] || fail "expected non-empty field: $field"
}

run_app_command() {
  command=$1
  adb_device logcat -c >/dev/null
  adb_device shell "am start -W -n $ACTIVITY --es r0lab_command '$command'" >/dev/null
  sleep 1
  adb_device logcat -d -v brief -s R0Lab:I '*:S' | tr -d '\r'
}

mkdir -p "$EVIDENCE_DIR"
"$ROOT/scripts/build_lab_app.sh" >/dev/null
adb_device install -r "$ROOT/build/lab-app/r0lab-debug.apk" >/dev/null

CAPABILITY_EVIDENCE=$("$ROOT/scripts/probe_m3_m4_capabilities_device.sh") ||
  fail "capability probe failed"
require_nonempty_field "$CAPABILITY_EVIDENCE" arm64_features
require_nonempty_field "$CAPABILITY_EVIDENCE" arm64_cpu_parts
require_nonempty_field "$CAPABILITY_EVIDENCE" folkpatch_runtime_version
DESCRIBE_OUTPUT=$(run_app_command describe)
require_contains "$DESCRIBE_OUTPUT" 'pid='
require_contains "$DESCRIBE_OUTPUT" 'tid='
require_contains "$DESCRIBE_OUTPUT" 'uid='
require_contains "$DESCRIBE_OUTPUT" 'marker='
require_contains "$DESCRIBE_OUTPUT" 'return_site='
require_contains "$DESCRIBE_OUTPUT" 'marker_value=62'
require_contains "$DESCRIBE_OUTPUT" 'library='

{
  printf 'capability_evidence=%s\n' "$CAPABILITY_EVIDENCE"
  printf '%s\n' "$DESCRIBE_OUTPUT"
  printf 'result=pass\n'
} | tee "$EVIDENCE"
printf '%s\n' "$EVIDENCE"
