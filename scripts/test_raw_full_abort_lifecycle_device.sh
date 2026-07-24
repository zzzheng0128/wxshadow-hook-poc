#!/bin/sh
set -eu

ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
SERIAL=${ANDROID_SERIAL:-}
EXPECTED_SERIAL=32250DLH2000Z3
EXPECTED_BOOT_ID=85ea557e-0182-4cc8-8509-0586bdaaab04
SOURCE_TAG=wxshadow-v2-f46-d4-r3n-owner-exit-mmput-fix-retry1-source-20260724
MODULE=r0lab-m1
REMOTE=/data/local/tmp/r0lab-r3n.kpm
PACKAGE=dev.r0hook.lab
ACTIVITY=dev.r0hook.lab/.MainActivity
TOKEN_A=${RAW_FULL_ABORT_TOKEN_A:-0x72939b}
TOKEN_B=${RAW_FULL_ABORT_TOKEN_B:-0x72939c}
KEYSTORE=${R0LAB_DEBUG_KEYSTORE:-$ROOT/lab-app/debug.keystore}
EVIDENCE_DIR="$ROOT/build/evidence"
EVIDENCE="$EVIDENCE_DIR/raw-full-abort-lifecycle-$(date +%Y%m%d-%H%M%S).log"
EXPECTED_KPM_SHA=318c9a86e57d1301c34aa357fa9d8da7c6de11df7fdfdfa1dedd442dc582cc68
EXPECTED_LABPROBE_SHA=ce787040a378c18e6619f2e43d07d703da9929303176853bce42a6824b9109b4
EXPECTED_CLASSES_DEX_SHA=325e8a54bd306ef4da230de9919d0da46dec112f97efa9fbf42646dc7dd7ec79
EXPECTED_SIGNER_CERT_SHA=73f1e2d251423909f33bfc7573580bd096834b57f680d5edb6e68655b1f903dd
MODULE_LOADED=0
PHASE=setup
SOURCE_COMMIT=
WARN_BEFORE=
BOOT_BEFORE=

fail() {
  printf '%s\n' "raw full-abort lifecycle failure: $*" >&2
  exit 1
}

classify() {
  classification=$1
  reason=$2
  printf 'classification=%s result=fail phase=%s reason=%s\n' \
    "$classification" "$PHASE" "$reason" | tee -a "$EVIDENCE" >&2
  fail "$reason"
}

require_contains() {
  haystack=$1
  needle=$2
  printf '%s\n' "$haystack" | LC_ALL=C grep -F -- "$needle" >/dev/null ||
    return 1
}

sha256_file() {
  path=$1
  if command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$path" | awk '{ print $1 }'
  elif command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$path" | awk '{ print $1 }'
  else
    fail "neither shasum nor sha256sum is available"
  fi
}

sha256_apk_entry() {
  apk=$1
  entry=$2
  if command -v shasum >/dev/null 2>&1; then
    unzip -p "$apk" "$entry" | shasum -a 256 | awk '{ print $1 }'
  elif command -v sha256sum >/dev/null 2>&1; then
    unzip -p "$apk" "$entry" | sha256sum | awk '{ print $1 }'
  else
    fail "neither shasum nor sha256sum is available"
  fi
}

sha256_signer_cert() {
  keystore=$1
  if command -v shasum >/dev/null 2>&1; then
    keytool -exportcert -keystore "$keystore" -storepass android \
      -alias androiddebugkey | shasum -a 256 | awk '{ print $1 }'
  elif command -v sha256sum >/dev/null 2>&1; then
    keytool -exportcert -keystore "$keystore" -storepass android \
      -alias androiddebugkey | sha256sum | awk '{ print $1 }'
  else
    fail "neither shasum nor sha256sum is available"
  fi
}

require_sha256() {
  label=$1
  expected=$2
  actual=$3
  [ "$actual" = "$expected" ] ||
    fail "$label SHA-256 mismatch: expected=$expected actual=$actual"
}

adb_device() {
  adb -s "$SERIAL" "$@"
}

supercmd() {
  command="/system/bin/truncate su"
  for argument do
    quoted=$(printf '%s' "$argument" | sed "s/'/'\\\\\\\\''/g")
    command="$command '$quoted'"
  done
  adb_device shell su -c "$command"
}

app_logs() {
  adb_device logcat -d -v brief -s R0Lab:I '*:S' | LC_ALL=C tr -d '\r'
}

run_app_command() {
  command=$1
  output=
  attempt=0

  adb_device logcat -c >/dev/null 2>&1 || true
  adb_device shell "am start -W -n $ACTIVITY --es r0lab_command '$command'" \
    >/dev/null 2>&1 || true
  while [ "$attempt" -lt 60 ]; do
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
  printf 'command_timeout command=%s wait_ms=15000\n' "$command"
  return 1
}

read_boot_id() {
  adb_device shell cat /proc/sys/kernel/random/boot_id |
    LC_ALL=C tr -d '\r'
}

read_warn_count() {
  adb_device shell su -c cat /sys/kernel/warn_count |
    LC_ALL=C tr -d '\r'
}

module_list() {
  supercmd module list 2>&1 | LC_ALL=C tr -d '\r'
}

ensure_clean_source() {
  dirty=$(
    git -C "$ROOT" status --porcelain --untracked-files=no 2>/dev/null
  ) || fail "R3n requires a git clean source tree"
  [ -z "$dirty" ] ||
    fail "R3n refuses tracked source changes: $dirty"
}

require_tagged_head() {
  tagged=$(
    git -C "$ROOT" rev-parse --verify "refs/tags/${SOURCE_TAG}^{commit}" \
      2>/dev/null
  ) || fail "required source tag is missing: $SOURCE_TAG"
  SOURCE_COMMIT=$(
    git -C "$ROOT" rev-parse --verify HEAD 2>/dev/null
  ) || fail "current Git HEAD is unavailable"
  [ "$SOURCE_COMMIT" = "$tagged" ] ||
    fail "R3n requires source-tagged HEAD: tagged=$tagged head=$SOURCE_COMMIT"
}

require_empty_status() {
  empty_status=$1
  require_contains "$empty_status" ' active=0 ' &&
    require_contains "$empty_status" ' raw_slots=0 ' &&
    require_contains "$empty_status" ' raw_inflight=0 ' &&
    require_contains "$empty_status" ' page_records=0 ' &&
    require_contains "$empty_status" ' raw_page_table_active=0 '
}

prepare_worker_shutdown() {
  WORKER_SHUTDOWN_OUTPUT=$(run_app_command 'workers shutdown') || return 1
  require_contains "$WORKER_SHUTDOWN_OUTPUT" \
    'workers_shutdown_requested' || return 1

  attempt=0
  while [ "$attempt" -lt 30 ]; do
    WORKER_STATUS_OUTPUT=$(run_app_command status) || return 1
    case "$WORKER_STATUS_OUTPUT" in
      *"workers_live=0"*"workers_shutdown=1"*) return 0 ;;
    esac
    attempt=$((attempt + 1))
    sleep 0.1
  done
  return 1
}

load_module() {
  LOAD_OUTPUT=$(supercmd module load "$REMOTE" "lab_uid=$LAB_UID" 2>&1) ||
    return 1
  case "$LOAD_OUTPUT" in
    *"supercmd error code"*) return 1 ;;
  esac
  MODULE_LOADED=1
  printf 'phase=%s module_load=%s\n' "$PHASE" "$LOAD_OUTPUT" >> "$EVIDENCE"
  return 0
}

unload_module() {
  prepare_worker_shutdown || return 1
  printf '%s\n' "$WORKER_SHUTDOWN_OUTPUT" >> "$EVIDENCE"
  printf '%s\n' "$WORKER_STATUS_OUTPUT" >> "$EVIDENCE"
  UNLOAD_OUTPUT=$(supercmd module unload "$MODULE" 2>&1) || return 1
  case "$UNLOAD_OUTPUT" in
    *"supercmd error code"*) return 1 ;;
  esac
  MODULE_LOADED=0
  modules=$(module_list) || return 1
  [ -z "$modules" ] || return 1
  printf 'phase=%s module_unload=%s module_list=empty\n' \
    "$PHASE" "$UNLOAD_OUTPUT" >> "$EVIDENCE"
  return 0
}

cleanup() {
  cleanup_rc=$?
  trap - EXIT INT TERM

  adb_device shell am force-stop "$PACKAGE" >/dev/null 2>&1 || true
  if [ "$cleanup_rc" -ne 0 ] && [ "$MODULE_LOADED" -eq 1 ]; then
    cleanup_status=$(run_app_command status 2>&1 || true)
    if require_empty_status "$cleanup_status"; then
      if prepare_worker_shutdown >/dev/null 2>&1; then
        supercmd module unload "$MODULE" >/dev/null 2>&1 || true
        modules=$(module_list 2>&1 || true)
        if [ -z "$modules" ]; then
          MODULE_LOADED=0
          printf 'cleanup=bounded module_list=empty\n' >> "$EVIDENCE"
        fi
      fi
    fi
    if [ "$MODULE_LOADED" -eq 1 ]; then
      printf 'cleanup=preserve module_resident=1 reason=state-not-proven-empty\n' \
        >> "$EVIDENCE"
      printf '%s\n' \
        "R3n cleanup preserved the resident module because empty state was not proven" >&2
    fi
  fi
  adb_device shell rm -f "$REMOTE" >/dev/null 2>&1 || true
  exit "$cleanup_rc"
}

[ -n "$SERIAL" ] ||
  fail "ANDROID_SERIAL=$EXPECTED_SERIAL is required before device access"
[ "$SERIAL" = "$EXPECTED_SERIAL" ] ||
  fail "refusing non-target device serial: expected=$EXPECTED_SERIAL actual=$SERIAL"

ensure_clean_source
require_tagged_head
[ -f "$KEYSTORE" ] || fail "fixed Lab keystore is missing: $KEYSTORE"
mkdir -p "$EVIDENCE_DIR"

"$ROOT/scripts/build_kpm.sh" >/dev/null
"$ROOT/scripts/verify_d4_r3n_disassembly.sh" >/dev/null
R0LAB_DEBUG_KEYSTORE="$KEYSTORE" "$ROOT/scripts/build_lab_app.sh" >/dev/null

KPM_SHA=$(sha256_file "$ROOT/kpm/build/r0lab-m1.kpm")
LABPROBE_SHA=$(
  sha256_apk_entry "$ROOT/build/lab-app/r0lab-debug.apk" \
    lib/arm64-v8a/liblabprobe.so
)
CLASSES_DEX_SHA=$(
  sha256_apk_entry "$ROOT/build/lab-app/r0lab-debug.apk" classes.dex
)
SIGNER_CERT_SHA=$(sha256_signer_cert "$KEYSTORE")
require_sha256 kpm "$EXPECTED_KPM_SHA" "$KPM_SHA"
require_sha256 labprobe "$EXPECTED_LABPROBE_SHA" "$LABPROBE_SHA"
require_sha256 classes.dex "$EXPECTED_CLASSES_DEX_SHA" "$CLASSES_DEX_SHA"
require_sha256 signer-cert "$EXPECTED_SIGNER_CERT_SHA" "$SIGNER_CERT_SHA"

BOOT_BEFORE=$(read_boot_id) || fail "initial boot ID read failed"
[ "$BOOT_BEFORE" = "$EXPECTED_BOOT_ID" ] ||
  fail "unexpected boot ID: expected=$EXPECTED_BOOT_ID actual=$BOOT_BEFORE"
WARN_BEFORE=$(read_warn_count) || fail "initial warn_count read failed"
EXISTING=$(module_list) || fail "initial module list failed"
[ -z "$EXISTING" ] || fail "refusing resident KPM: $EXISTING"

trap cleanup EXIT INT TERM

adb_device install -r "$ROOT/build/lab-app/r0lab-debug.apk" >/dev/null
adb_device push "$ROOT/kpm/build/r0lab-m1.kpm" "$REMOTE" >/dev/null
LAB_UID=$(adb_device shell "cmd package list packages -U $PACKAGE" |
  LC_ALL=C tr -d '\r' |
  sed -n 's/.* uid:\([0-9][0-9]*\).*/\1/p')
[ -n "$LAB_UID" ] || classify D4-R3n-setup-blocked lab-uid-not-found

printf 'serial=%s source_commit=%s source_tag=%s boot_id=%s warn_before=%s lab_uid=%s token_a=%s token_b=%s kpm_sha256=%s labprobe_so_sha256=%s classes_dex_sha256=%s signer_cert_sha256=%s\n' \
  "$SERIAL" "$SOURCE_COMMIT" "$SOURCE_TAG" "$BOOT_BEFORE" "$WARN_BEFORE" \
  "$LAB_UID" "$TOKEN_A" "$TOKEN_B" "$KPM_SHA" "$LABPROBE_SHA" \
  "$CLASSES_DEX_SHA" "$SIGNER_CERT_SHA" | tee "$EVIDENCE"

PHASE=phase-a
load_module ||
  classify D4-R3n-explicit-cleanup-unstable phase-a-module-load-failed
STATUS_A0=$(run_app_command status) ||
  classify D4-R3n-explicit-cleanup-unstable phase-a-status-timeout
require_empty_status "$STATUS_A0" ||
  classify D4-R3n-explicit-cleanup-unstable phase-a-initial-state-not-empty
printf '%s\n' "$STATUS_A0" >> "$EVIDENCE"

ARM_A=$(run_app_command "arm $TOKEN_A") ||
  classify D4-R3n-abort-routing-unstable phase-a-arm-timeout
require_contains "$ARM_A" 'armed uid=' ||
  classify D4-R3n-abort-routing-unstable phase-a-session-arm-failed
printf '%s\n' "$ARM_A" >> "$EVIDENCE"

RUN_A=$(run_app_command "raw full abort lifecycle run $TOKEN_A") ||
  classify D4-R3n-abort-routing-unstable phase-a-command-timeout
printf '%s\n' "$RUN_A" >> "$EVIDENCE"
for field in \
  'raw mode=full-abort-lifecycle failures=0' \
  'raw_slots=0' \
  'page_records=0' \
  'slot0_initial=42' \
  'slot1_initial=42' \
  'slot0_shadow=99' \
  'slot1_shadow=99' \
  'slot0_read_cycle=original_read_to_shadow' \
  'slot0_read_value=42' \
  'slot0_resume=99' \
  'slot1_write_signal=1' \
  'slot1_write_release_result=0' \
  'abort_hook_full0=1' \
  'abort_hook_full1=1' \
  'handler_repairs=0' \
  'restored=42/42' \
  'session_closed=1'
do
  require_contains "$RUN_A" "$field" ||
    classify D4-R3n-abort-routing-unstable "phase-a-result-missing-$field"
done
STATUS_A1=$(run_app_command status) ||
  classify D4-R3n-explicit-cleanup-unstable phase-a-final-status-timeout
require_empty_status "$STATUS_A1" ||
  classify D4-R3n-explicit-cleanup-unstable phase-a-final-state-not-empty
printf '%s\n' "$STATUS_A1" >> "$EVIDENCE"
unload_module ||
  classify D4-R3n-explicit-cleanup-unstable phase-a-unload-failed

BOOT_A=$(read_boot_id) || classify D4-R3n-explicit-cleanup-unstable phase-a-boot-read-failed
[ "$BOOT_A" = "$BOOT_BEFORE" ] ||
  classify D4-R3n-explicit-cleanup-unstable phase-a-boot-changed
WARN_A=$(read_warn_count) ||
  classify D4-R3n-explicit-cleanup-unstable phase-a-warn-read-failed
[ "$WARN_A" = "$WARN_BEFORE" ] ||
  classify D4-R3n-explicit-cleanup-unstable phase-a-warn-count-changed
printf 'phase=phase-a result=pass module_list=empty boot_id=%s warn_after=%s\n' \
  "$BOOT_A" "$WARN_A" | tee -a "$EVIDENCE"

PHASE=phase-b
load_module ||
  classify D4-R3n-no-reboot-reload-unstable phase-b-reload-failed
STATUS_B0=$(run_app_command status) ||
  classify D4-R3n-no-reboot-reload-unstable phase-b-status-timeout
require_empty_status "$STATUS_B0" ||
  classify D4-R3n-no-reboot-reload-unstable phase-b-initial-state-not-empty
printf '%s\n' "$STATUS_B0" >> "$EVIDENCE"

ARM_B=$(run_app_command "arm $TOKEN_B") ||
  classify D4-R3n-owner-exit-cleanup-unstable phase-b-arm-timeout
require_contains "$ARM_B" 'armed uid=' ||
  classify D4-R3n-owner-exit-cleanup-unstable phase-b-session-arm-failed
printf '%s\n' "$ARM_B" >> "$EVIDENCE"

HOLD_B=$(run_app_command "raw exit hook routing hold $TOKEN_B") ||
  classify D4-R3n-owner-exit-cleanup-unstable phase-b-hold-timeout
printf '%s\n' "$HOLD_B" >> "$EVIDENCE"
for field in \
  'raw mode=exit-hook-routing-hold failures=0' \
  'page_record_routed=1' \
  'cleanup=monitor' \
  'normal=42/42' \
  'shadow=99/99' \
  'activations=1/1' \
  'states=3/3' \
  'abort_hook_full=1/1' \
  'hook_ready=1/1' \
  'hook_status=1/1' \
  'inspect=1/1' \
  'handler_faults=0'
do
  require_contains "$HOLD_B" "$field" ||
    classify D4-R3n-owner-exit-cleanup-unstable "phase-b-hold-missing-$field"
done

STATUS_HELD=$(run_app_command status) ||
  classify D4-R3n-owner-exit-cleanup-unstable phase-b-held-status-timeout
if ! {
  require_contains "$STATUS_HELD" ' active=1 ' &&
    require_contains "$STATUS_HELD" ' raw_slots=2 ' &&
    require_contains "$STATUS_HELD" ' raw_inflight=0 ' &&
    require_contains "$STATUS_HELD" ' page_records=2 ' &&
    require_contains "$STATUS_HELD" ' raw_page_table_active=2 '
}; then
  classify D4-R3n-owner-exit-cleanup-unstable phase-b-held-state-invalid
fi
printf '%s\n' "$STATUS_HELD" >> "$EVIDENCE"

adb_device shell am force-stop "$PACKAGE" >/dev/null
printf 'phase=phase-b force_stop=done package=%s\n' "$PACKAGE" >> "$EVIDENCE"

attempt=0
STATUS_B1=
while [ "$attempt" -lt 80 ]; do
  STATUS_B1=$(run_app_command status 2>&1 || true)
  printf '%s\n' "$STATUS_B1" >> "$EVIDENCE"
  if require_empty_status "$STATUS_B1"; then
    break
  fi
  attempt=$((attempt + 1))
  sleep 0.1
done
require_empty_status "$STATUS_B1" ||
  classify D4-R3n-owner-exit-cleanup-unstable phase-b-owner-exit-not-empty

EVENT_B=$(run_app_command "events $TOKEN_B") ||
  classify D4-R3n-owner-exit-cleanup-unstable phase-b-events-timeout
printf '%s\n' "$EVENT_B" >> "$EVIDENCE"
EXIT_HOOK_COUNT=$(printf '%s\n' "$EVENT_B" |
  LC_ALL=C grep -c 'op=34 result=0' || true)
RAW_CLEAR_COUNT=$(printf '%s\n' "$EVENT_B" |
  LC_ALL=C grep -c 'op=22 result=0' || true)
CLOSE_COUNT=$(printf '%s\n' "$EVENT_B" |
  LC_ALL=C grep -c 'op=5 result=0' || true)
SUMMARY_COUNT=$(printf '%s\n' "$EVENT_B" |
  LC_ALL=C grep -c 'op=7 result=0' || true)
[ "$EXIT_HOOK_COUNT" -eq 2 ] ||
  classify D4-R3n-owner-exit-cleanup-unstable phase-b-exit-hit-count
[ "$RAW_CLEAR_COUNT" -eq 2 ] ||
  classify D4-R3n-owner-exit-cleanup-unstable phase-b-raw-clear-count
[ "$CLOSE_COUNT" -eq 1 ] ||
  classify D4-R3n-owner-exit-cleanup-unstable phase-b-close-count
[ "$SUMMARY_COUNT" -eq 1 ] ||
  classify D4-R3n-owner-exit-cleanup-unstable phase-b-summary-count

unload_module ||
  classify D4-R3n-owner-exit-cleanup-unstable phase-b-unload-failed
BOOT_B=$(read_boot_id) ||
  classify D4-R3n-owner-exit-cleanup-unstable phase-b-boot-read-failed
[ "$BOOT_B" = "$BOOT_BEFORE" ] ||
  classify D4-R3n-owner-exit-cleanup-unstable phase-b-boot-changed
WARN_B=$(read_warn_count) ||
  classify D4-R3n-owner-exit-cleanup-unstable phase-b-warn-read-failed
[ "$WARN_B" = "$WARN_BEFORE" ] ||
  classify D4-R3n-owner-exit-cleanup-unstable phase-b-warn-count-changed

PHASE=complete
printf 'classification=D4-R3n-full-abort-lifecycle-stable result=pass phases=explicit,owner-exit unloads=2 reload_without_reboot=1 exit_hits=%s raw_clears=%s closes=%s summaries=%s boot_id=%s warn_after=%s module_list=empty\n' \
  "$EXIT_HOOK_COUNT" "$RAW_CLEAR_COUNT" "$CLOSE_COUNT" "$SUMMARY_COUNT" \
  "$BOOT_B" "$WARN_B" | tee -a "$EVIDENCE"
printf '%s\n' "$EVIDENCE"
