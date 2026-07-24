#!/bin/sh
set -eu

ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
SERIAL=${ANDROID_SERIAL:-}
EXPECTED_SERIAL=32250DLH2000Z3
SOURCE_TAG=${R3O_SOURCE_TAG:-wxshadow-v2-f46-d4-r3o-stage4-entry-candidate-20260724}
STAGE=${R3O_STAGE:-}
MODULE=r0lab-m1
REMOTE=/data/local/tmp/r0lab-r3o.kpm
PACKAGE=dev.r0hook.lab
ACTIVITY=dev.r0hook.lab/.MainActivity
TOKEN_2=${R3O_TOKEN_2:-0x7293a2}
TOKEN_3=${R3O_TOKEN_3:-0x7293a3}
TOKEN_4=${R3O_TOKEN_4:-0x7293a4}
TOKEN_5=${R3O_TOKEN_5:-0x7293a5}
STAGE4_HOLD_SECONDS=${R3O_STAGE4_HOLD_SECONDS:-60}
KEYSTORE=${R0LAB_DEBUG_KEYSTORE:-$ROOT/lab-app/debug.keystore}
EXPECTED_SIGNER_CERT_SHA=73f1e2d251423909f33bfc7573580bd096834b57f680d5edb6e68655b1f903dd
EVIDENCE_DIR="$ROOT/build/evidence"
STATE_FILE="$EVIDENCE_DIR/r3o-device-state.env"
EVIDENCE="$EVIDENCE_DIR/raw-r3o-stage-${STAGE:-invalid}-$(date +%Y%m%d-%H%M%S).log"
FAIL_REASON=unexpected-exit
R3O_LOG_BEFORE_COUNT=0
FAILURE_ADB_STATE=
FAILURE_RECOVERY_WAIT=0

fail() {
  FAIL_REASON=$1
  printf '%s\n' "raw R3o device failure: $*" >&2
  exit 1
}

require_contains() {
  haystack=$1
  needle=$2
  printf '%s\n' "$haystack" | LC_ALL=C grep -F -- "$needle" >/dev/null ||
    fail "expected output not found: $needle"
}

count_contains() {
  haystack=$1
  needle=$2
  expected=$3
  actual=$(printf '%s\n' "$haystack" |
    LC_ALL=C grep -c -F -- "$needle" || true)
  [ "$actual" -eq "$expected" ] ||
    fail "unexpected count for '$needle': expected=$expected actual=$actual"
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
  if command -v shasum >/dev/null 2>&1; then
    keytool -exportcert -keystore "$KEYSTORE" -storepass android \
      -alias androiddebugkey | shasum -a 256 | awk '{ print $1 }'
  elif command -v sha256sum >/dev/null 2>&1; then
    keytool -exportcert -keystore "$KEYSTORE" -storepass android \
      -alias androiddebugkey | sha256sum | awk '{ print $1 }'
  else
    fail "neither shasum nor sha256sum is available"
  fi
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
  adb_device logcat -d -v brief -s R0Lab:I '*:S' |
    LC_ALL=C tr -d '\r'
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

read_pid() {
  process=$1
  adb_device shell pidof "$process" |
    LC_ALL=C tr -d '\r'
}

read_crash_buffer() {
  adb_device logcat -b crash -d -v threadtime -t 200 |
    LC_ALL=C tr -d '\r'
}

module_list() {
  supercmd module list 2>&1 | LC_ALL=C tr -d '\r'
}

r3o_dmesg() {
  adb_device shell su -c dmesg 2>/dev/null |
    LC_ALL=C tr -d '\r' |
    LC_ALL=C grep -F 'r0lab-r3o:' || true
}

start_log_window() {
  logs=$(r3o_dmesg)
  R3O_LOG_BEFORE_COUNT=$(printf '%s\n' "$logs" |
    LC_ALL=C grep -c -F 'r0lab-r3o:' || true)
}

finish_log_window() {
  logs=$(r3o_dmesg)
  start=$((R3O_LOG_BEFORE_COUNT + 1))
  R3O_STAGE_LOGS=$(printf '%s\n' "$logs" | sed -n "${start},\$p")
  printf '%s\n' "$R3O_STAGE_LOGS" >> "$EVIDENCE"
}

require_empty_status() {
  status=$1
  require_contains "$status" ' active=0 '
  require_contains "$status" ' raw_slots=0 '
  require_contains "$status" ' raw_inflight=0 '
  require_contains "$status" ' page_records=0 '
  require_contains "$status" ' raw_page_table_active=0 '
}

require_resident_callbacks_status() {
  status=$1
  require_contains "$status" 'raw_abort_resident=1'
  require_contains "$status" 'raw_exit_resident=1'
}

require_exit_only_callbacks_status() {
  status=$1
  require_contains "$status" 'raw_abort_resident=0'
  require_contains "$status" 'raw_exit_resident=1'
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
  if LOAD_OUTPUT=$(supercmd module load "$REMOTE" "lab_uid=$LAB_UID" 2>&1); then
    load_rc=0
  else
    load_rc=$?
  fi
  printf 'module_load=%s\n' "$LOAD_OUTPUT" >> "$EVIDENCE"
  [ "$load_rc" -eq 0 ] || return 1
  case "$LOAD_OUTPUT" in
    *"supercmd error code"*) return 1 ;;
  esac
}

unload_module() {
  prepare_worker_shutdown || return 1
  printf '%s\n%s\n' "$WORKER_SHUTDOWN_OUTPUT" "$WORKER_STATUS_OUTPUT" \
    >> "$EVIDENCE"
  UNLOAD_OUTPUT=$(supercmd module unload "$MODULE" 2>&1) || return 1
  case "$UNLOAD_OUTPUT" in
    *"supercmd error code"*) return 1 ;;
  esac
  modules=$(module_list) || return 1
  [ -z "$modules" ] || return 1
  printf 'module_unload=%s module_list=empty\n' "$UNLOAD_OUTPUT" \
    >> "$EVIDENCE"
}

poll_transport() {
  seconds=$1
  elapsed=0
  while [ "$elapsed" -lt "$seconds" ]; do
    sleep 1
    elapsed=$((elapsed + 1))
    state=$(adb_device get-state 2>&1 || true)
    state=$(printf '%s' "$state" | LC_ALL=C tr -d '\r')
    printf 'idle_poll=%s/%s adb_state=%s\n' \
      "$elapsed" "$seconds" "$state" >> "$EVIDENCE"
    [ "$state" = device ] ||
      fail "ADB transport changed during idle poll $elapsed/$seconds"
  done
}

poll_runtime_continuity() {
  seconds=$1
  expected_system_server_pid=$2
  expected_lab_pid=$3
  elapsed=0

  while [ "$elapsed" -lt "$seconds" ]; do
    sleep 1
    elapsed=$((elapsed + 1))
    state=$(adb_device get-state 2>&1 || true)
    state=$(printf '%s' "$state" | LC_ALL=C tr -d '\r')
    system_server_pid=$(read_pid system_server 2>/dev/null || true)
    lab_pid=$(read_pid "$PACKAGE" 2>/dev/null || true)
    printf 'runtime_poll=%s/%s adb_state=%s system_server_pid=%s lab_pid=%s\n' \
      "$elapsed" "$seconds" "$state" "$system_server_pid" "$lab_pid" \
      >> "$EVIDENCE"
    [ "$state" = device ] ||
      fail "ADB transport changed during runtime poll $elapsed/$seconds"
    [ "$system_server_pid" = "$expected_system_server_pid" ] ||
      fail "system_server PID changed during runtime poll $elapsed/$seconds: expected=$expected_system_server_pid actual=$system_server_pid"
    [ "$lab_pid" = "$expected_lab_pid" ] ||
      fail "Lab PID changed during runtime poll $elapsed/$seconds: expected=$expected_lab_pid actual=$lab_pid"
  done

  CRASH_BUFFER=$(read_crash_buffer 2>&1 || true)
  printf 'crash_buffer_begin\n%s\ncrash_buffer_end\n' "$CRASH_BUFFER" \
    >> "$EVIDENCE"
  [ -z "$CRASH_BUFFER" ] ||
    fail "crash buffer became non-empty during active raw hold"
}

write_state() {
  next_stage=$1
  tmp="$STATE_FILE.tmp"
  printf '%s\n' \
    "R3O_BOOT_ID=$R3O_BOOT_ID" \
    "R3O_WARN_BASE=$R3O_WARN_BASE" \
    "R3O_SOURCE_COMMIT=$SOURCE_COMMIT" \
    "R3O_SOURCE_TAG=$SOURCE_TAG" \
    "R3O_KPM_SHA=$KPM_SHA" \
    "R3O_LABPROBE_SHA=$LABPROBE_SHA" \
    "R3O_CLASSES_SHA=$CLASSES_SHA" \
    "R3O_SIGNER_SHA=$SIGNER_SHA" \
    "R3O_LAB_UID=$LAB_UID" \
    "R3O_NEXT_STAGE=$next_stage" > "$tmp"
  mv "$tmp" "$STATE_FILE"
}

load_state() {
  [ -f "$STATE_FILE" ] ||
    fail "R3o continuity state is missing: run stage 1 first"
  # shellcheck disable=SC1090
  . "$STATE_FILE"
  [ "$R3O_NEXT_STAGE" = "$STAGE" ] ||
    fail "unexpected stage order: state expects $R3O_NEXT_STAGE, requested $STAGE"
  [ "$R3O_SOURCE_COMMIT" = "$SOURCE_COMMIT" ] ||
    fail "source commit changed after stage 1"
  [ "$R3O_SOURCE_TAG" = "$SOURCE_TAG" ] ||
    fail "source tag changed after stage 1"
  [ "$R3O_KPM_SHA" = "$KPM_SHA" ] ||
    fail "KPM artifact changed after stage 1"
  [ "$R3O_LABPROBE_SHA" = "$LABPROBE_SHA" ] ||
    fail "Lab native artifact changed after stage 1"
  [ "$R3O_CLASSES_SHA" = "$CLASSES_SHA" ] ||
    fail "Lab classes artifact changed after stage 1"
  [ "$R3O_SIGNER_SHA" = "$SIGNER_SHA" ] ||
    fail "Lab signer changed after stage 1"
  LAB_UID=$R3O_LAB_UID
}

verify_boot_and_warning() {
  boot_now=$(read_boot_id) || fail "boot ID read failed"
  warn_now=$(read_warn_count) || fail "warn_count read failed"
  [ "$boot_now" = "$R3O_BOOT_ID" ] ||
    fail "boot ID changed: expected=$R3O_BOOT_ID actual=$boot_now"
  [ "$warn_now" = "$R3O_WARN_BASE" ] ||
    fail "warn_count changed: expected=$R3O_WARN_BASE actual=$warn_now"
  printf 'boot_id=%s warn_count=%s continuity=pass\n' \
    "$boot_now" "$warn_now" >> "$EVIDENCE"
}

wait_for_failure_device() {
  FAILURE_RECOVERY_WAIT=0
  while [ "$FAILURE_RECOVERY_WAIT" -le 180 ]; do
    FAILURE_ADB_STATE=$(adb_device get-state 2>&1 || true)
    FAILURE_ADB_STATE=$(printf '%s' "$FAILURE_ADB_STATE" |
      LC_ALL=C tr -d '\r')
    [ "$FAILURE_ADB_STATE" = device ] && return 0
    [ "$FAILURE_RECOVERY_WAIT" -eq 180 ] && return 1
    sleep 1
    FAILURE_RECOVERY_WAIT=$((FAILURE_RECOVERY_WAIT + 1))
  done
  return 1
}

capture_failure() {
  status=$?
  trap - EXIT INT TERM
  printf 'classification=D4-R3o-stage-%s-failed result=fail reason=%s status=%s\n' \
    "$STAGE" "$FAIL_REASON" "$status" >> "$EVIDENCE"
  wait_for_failure_device || true
  printf 'failure_adb_state=%s recovery_wait_seconds=%s\n' \
    "$FAILURE_ADB_STATE" "$FAILURE_RECOVERY_WAIT" >> "$EVIDENCE"
  if [ "$FAILURE_ADB_STATE" = device ]; then
    printf 'failure_boot_id=%s\n' "$(read_boot_id 2>&1 || true)" \
      >> "$EVIDENCE"
    printf 'failure_warn_count=%s\n' "$(read_warn_count 2>&1 || true)" \
      >> "$EVIDENCE"
    printf 'failure_module_list_begin\n%s\nfailure_module_list_end\n' \
      "$(module_list 2>&1 || true)" >> "$EVIDENCE"
    printf 'failure_r3o_dmesg_begin\n%s\nfailure_r3o_dmesg_end\n' \
      "$(r3o_dmesg)" >> "$EVIDENCE"
    printf 'failure_crash_buffer_begin\n%s\nfailure_crash_buffer_end\n' \
      "$(read_crash_buffer 2>&1 || true)" >> "$EVIDENCE"
    printf 'failure_pstore_list_begin\n%s\nfailure_pstore_list_end\n' \
      "$(adb_device shell su -c ls -l /sys/fs/pstore 2>&1 || true)" \
      >> "$EVIDENCE"
    printf 'failure_pstore_console_begin\n%s\nfailure_pstore_console_end\n' \
      "$(adb_device shell su -c cat /sys/fs/pstore/console-ramoops-0 \
          2>&1 || true)" >> "$EVIDENCE"
  fi
  printf '%s\n' "$EVIDENCE" >&2
  exit "$status"
}

case "$STAGE" in
  1 | 2 | 3 | 4 | 5 | 6) ;;
  *) fail "R3O_STAGE=1..6 is required" ;;
esac
case "$STAGE4_HOLD_SECONDS" in
  '' | *[!0-9]*)
    fail "R3O_STAGE4_HOLD_SECONDS must be an integer of at least 60"
    ;;
esac
[ "$STAGE4_HOLD_SECONDS" -ge 60 ] ||
  fail "R3O_STAGE4_HOLD_SECONDS must be an integer of at least 60"
[ -n "$SERIAL" ] ||
  fail "ANDROID_SERIAL=$EXPECTED_SERIAL is required"
[ "$SERIAL" = "$EXPECTED_SERIAL" ] ||
  fail "refusing non-target serial: expected=$EXPECTED_SERIAL actual=$SERIAL"
[ -f "$KEYSTORE" ] || fail "fixed Lab keystore is missing"

mkdir -p "$EVIDENCE_DIR"
trap capture_failure EXIT INT TERM

dirty=$(git -C "$ROOT" status --porcelain --untracked-files=no)
[ -z "$dirty" ] || fail "R3o requires a clean tracked source tree: $dirty"
SOURCE_COMMIT=$(git -C "$ROOT" rev-parse --verify HEAD)
TAG_COMMIT=$(git -C "$ROOT" rev-parse --verify "refs/tags/${SOURCE_TAG}^{commit}")
[ "$SOURCE_COMMIT" = "$TAG_COMMIT" ] ||
  fail "R3o requires source-tagged HEAD"

"$ROOT/scripts/verify_d4_r3o_reference_lifetime.sh" >/dev/null
R0LAB_DEBUG_KEYSTORE="$KEYSTORE" "$ROOT/scripts/build_lab_app.sh" >/dev/null
KPM_SHA=$(sha256_file "$ROOT/kpm/build/r0lab-m1.kpm")
LABPROBE_SHA=$(sha256_apk_entry "$ROOT/build/lab-app/r0lab-debug.apk" \
  lib/arm64-v8a/liblabprobe.so)
CLASSES_SHA=$(sha256_apk_entry "$ROOT/build/lab-app/r0lab-debug.apk" classes.dex)
SIGNER_SHA=$(sha256_signer_cert)
[ "$SIGNER_SHA" = "$EXPECTED_SIGNER_CERT_SHA" ] ||
  fail "unexpected Lab signer: $SIGNER_SHA"

if [ "$STAGE" = 1 ]; then
  [ ! -f "$STATE_FILE" ] ||
    fail "R3o continuity state already exists: $STATE_FILE"
  adb_device install -r "$ROOT/build/lab-app/r0lab-debug.apk" >/dev/null
  adb_device push "$ROOT/kpm/build/r0lab-m1.kpm" "$REMOTE" >/dev/null
  LAB_UID=$(adb_device shell "cmd package list packages -U $PACKAGE" |
    LC_ALL=C tr -d '\r' |
    sed -n 's/.* uid:\([0-9][0-9]*\).*/\1/p')
  [ -n "$LAB_UID" ] || fail "Lab UID not found"
  sleep 3
  R3O_BOOT_ID=$(read_boot_id) || fail "initial boot ID read failed"
  R3O_WARN_BASE=$(read_warn_count) || fail "initial warn_count read failed"
  modules=$(module_list) || fail "initial module list failed"
  [ -z "$modules" ] || fail "stage 1 requires an empty module list: $modules"
else
  load_state
fi

printf 'stage=%s serial=%s source_commit=%s source_tag=%s boot_id=%s warn_base=%s lab_uid=%s kpm_sha256=%s labprobe_sha256=%s classes_sha256=%s signer_sha256=%s\n' \
  "$STAGE" "$SERIAL" "$SOURCE_COMMIT" "$SOURCE_TAG" "$R3O_BOOT_ID" \
  "$R3O_WARN_BASE" "$LAB_UID" "$KPM_SHA" "$LABPROBE_SHA" \
  "$CLASSES_SHA" "$SIGNER_SHA" | tee "$EVIDENCE"

start_log_window

case "$STAGE" in
  1)
    load_module || fail "stage 1 module load failed"
    STATUS=$(run_app_command status) ||
      fail "stage 1 status command timed out"
    require_empty_status "$STATUS"
    require_contains "$STATUS" 'raw_abort_resident=0'
    require_contains "$STATUS" 'raw_exit_resident=0'
    printf '%s\n' "$STATUS" >> "$EVIDENCE"
    unload_module || fail "stage 1 unload failed"
    verify_boot_and_warning
    write_state 2
    ;;
  2)
    modules=$(module_list) || fail "stage 2 module list failed"
    [ -z "$modules" ] || fail "stage 2 expected no loaded module: $modules"
    load_module || fail "stage 2 module load failed"
    STATUS=$(run_app_command status) ||
      fail "stage 2 initial status timed out"
    require_empty_status "$STATUS"
    ARM=$(run_app_command "arm $TOKEN_2") ||
      fail "stage 2 session arm timed out"
    HOLD=$(run_app_command "raw raw-hold no-abort $TOKEN_2") ||
      fail "stage 2 source hold timed out"
    for field in \
      'raw mode=raw-hold-no-abort failures=0' \
      'exit_hook_installed=1' \
      'target_state=source_uxn' \
      'slots=1' \
      'raw_slots=1' \
      'page_records=1' \
      'normal=42' \
      'shadow=-1' \
      'activations=0' \
      'states=2/0' \
      'abort_hook_installed=0' \
      'abort_hook_suppressed=1' \
      'handler_faults=0'
    do
      require_contains "$HOLD" "$field"
    done
    CLEAR=$(run_app_command "raw raw-hold clear $TOKEN_2") ||
      fail "stage 2 explicit clear timed out"
    for field in \
      'raw mode=raw-hold-clear failures=0' \
      'mode=11' \
      'slots=1' \
      'restored=42/-1' \
      'restore_faults=0' \
      'session_closed=1' \
      'hold_released=1'
    do
      require_contains "$CLEAR" "$field"
    done
    STATUS=$(run_app_command status) ||
      fail "stage 2 final status timed out"
    require_empty_status "$STATUS"
    require_exit_only_callbacks_status "$STATUS"
    printf '%s\n%s\n%s\n%s\n' "$ARM" "$HOLD" "$CLEAR" "$STATUS" \
      >> "$EVIDENCE"
    finish_log_window
    for field in \
      'slot_owned slot=0' \
      'shadow_ready slot=0' \
      'exit_protection_ready slot=0' \
      'source_uxn_installed slot=0' \
      'arm_complete slot=0' \
      'clear_begin slot=0' \
      'clear_end slot=0' \
      'ref=mm_count op=mmdrop'
    do
      require_contains "$R3O_STAGE_LOGS" "$field"
    done
    verify_boot_and_warning
    write_state 3
    ;;
  3)
    modules=$(module_list) || fail "stage 3 module list failed"
    require_contains "$modules" "$MODULE"
    STATUS=$(run_app_command status) ||
      fail "stage 3 initial status timed out"
    require_empty_status "$STATUS"
    require_exit_only_callbacks_status "$STATUS"
    ARM=$(run_app_command "arm $TOKEN_3") ||
      fail "stage 3 session arm timed out"
    HOLD=$(run_app_command "raw raw-hold abort-iabt-transition $TOKEN_3") ||
      fail "stage 3 IABT hold timed out"
    for field in \
      'raw mode=raw-hold-abort-iabt-transition failures=0' \
      'exit_hook_installed=1' \
      'target_state=shadow_rx' \
      'slots=1' \
      'normal=42' \
      'shadow=99' \
      'activations=1' \
      'state=3' \
      'active_kind=shadow_rx' \
      'record_state=shadow_active' \
      'abort_hook_iabt_transition=1' \
      'handler_faults=0' \
      'signal_fault_caught=0' \
      'restore_abi=skip_origin_ret0'
    do
      require_contains "$HOLD" "$field"
    done
    printf '%s\n%s\n' "$ARM" "$HOLD" >> "$EVIDENCE"
    poll_transport 15
    CLEAR=$(run_app_command "raw raw-hold clear $TOKEN_3") ||
      fail "stage 3 explicit clear timed out"
    require_contains "$CLEAR" 'raw mode=raw-hold-clear failures=0'
    require_contains "$CLEAR" 'mode=20'
    require_contains "$CLEAR" 'restored=42/-1'
    require_contains "$CLEAR" 'session_closed=1'
    STATUS=$(run_app_command status) ||
      fail "stage 3 final status timed out"
    require_empty_status "$STATUS"
    require_exit_only_callbacks_status "$STATUS"
    printf '%s\n%s\n' "$CLEAR" "$STATUS" >> "$EVIDENCE"
    finish_log_window
    require_contains "$R3O_STAGE_LOGS" 'exit_protection_ready slot=0'
    require_contains "$R3O_STAGE_LOGS" 'source_uxn_installed slot=0'
    require_contains "$R3O_STAGE_LOGS" 'clear_end slot=0'
    verify_boot_and_warning
    write_state 4
    ;;
  4)
    modules=$(module_list) || fail "stage 4 module list failed"
    require_contains "$modules" "$MODULE"
    STATUS=$(run_app_command status) ||
      fail "stage 4 initial status timed out"
    require_empty_status "$STATUS"
    require_exit_only_callbacks_status "$STATUS"
    ARM=$(run_app_command "arm $TOKEN_4") ||
      fail "stage 4 session arm timed out"
    STAGE4_SYSTEM_SERVER_PID=$(read_pid system_server) ||
      fail "stage 4 system_server PID read failed"
    STAGE4_LAB_PID=$(read_pid "$PACKAGE") ||
      fail "stage 4 Lab PID read failed"
    [ -n "$STAGE4_SYSTEM_SERVER_PID" ] ||
      fail "stage 4 system_server PID is empty"
    [ -n "$STAGE4_LAB_PID" ] ||
      fail "stage 4 Lab PID is empty"
    printf 'stage4_pid_baseline system_server_pid=%s lab_pid=%s hold_seconds=%s\n' \
      "$STAGE4_SYSTEM_SERVER_PID" "$STAGE4_LAB_PID" \
      "$STAGE4_HOLD_SECONDS" >> "$EVIDENCE"
    HOLD=$(run_app_command "raw raw-hold lifetime shadow double $TOKEN_4") ||
      fail "stage 4 two-page hold timed out"
    for field in \
      'raw mode=raw-hold-lifetime failures=0' \
      'exit_hook_installed=1/1' \
      'target_state=shadow_rx' \
      'slots=2' \
      'raw_slots=2' \
      'page_records=2' \
      'normal=42/42' \
      'shadow=99/99' \
      'activations=1/1' \
      'states=3/3' \
      'inspect=1/1' \
      'handler_faults=0'
    do
      require_contains "$HOLD" "$field"
    done
    printf '%s\n%s\n' "$ARM" "$HOLD" >> "$EVIDENCE"
    poll_runtime_continuity "$STAGE4_HOLD_SECONDS" \
      "$STAGE4_SYSTEM_SERVER_PID" "$STAGE4_LAB_PID"
    CLEAR=$(run_app_command "raw raw-hold clear $TOKEN_4") ||
      fail "stage 4 explicit clear timed out"
    require_contains "$CLEAR" 'raw mode=raw-hold-clear failures=0'
    require_contains "$CLEAR" 'mode=14'
    require_contains "$CLEAR" 'slots=2'
    require_contains "$CLEAR" 'restored=42/42'
    require_contains "$CLEAR" 'session_closed=1'
    STATUS=$(run_app_command status) ||
      fail "stage 4 final status timed out"
    require_empty_status "$STATUS"
    require_resident_callbacks_status "$STATUS"
    printf '%s\n%s\n' "$CLEAR" "$STATUS" >> "$EVIDENCE"
    finish_log_window
    count_contains "$R3O_STAGE_LOGS" 'slot_owned slot=' 2
    count_contains "$R3O_STAGE_LOGS" 'source_uxn_installed slot=' 2
    count_contains "$R3O_STAGE_LOGS" 'clear_end slot=' 2
    count_contains "$R3O_STAGE_LOGS" 'ref=mm_count op=mmdrop' 2
    verify_boot_and_warning
    write_state 5
    ;;
  5)
    modules=$(module_list) || fail "stage 5 module list failed"
    require_contains "$modules" "$MODULE"
    STATUS=$(run_app_command status) ||
      fail "stage 5 initial status timed out"
    require_empty_status "$STATUS"
    require_resident_callbacks_status "$STATUS"
    ARM=$(run_app_command "arm $TOKEN_5") ||
      fail "stage 5 session arm timed out"
    HOLD=$(run_app_command "raw exit hook routing hold $TOKEN_5") ||
      fail "stage 5 owner-exit hold timed out"
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
      require_contains "$HOLD" "$field"
    done
    printf '%s\n%s\n' "$ARM" "$HOLD" >> "$EVIDENCE"
    poll_transport 15
    adb_device shell am force-stop "$PACKAGE" >/dev/null
    printf 'force_stop=done package=%s\n' "$PACKAGE" >> "$EVIDENCE"

    attempt=0
    STATUS=
    while [ "$attempt" -lt 80 ]; do
      STATUS=$(run_app_command status 2>&1 || true)
      printf '%s\n' "$STATUS" >> "$EVIDENCE"
      if printf '%s\n' "$STATUS" |
          LC_ALL=C grep -F ' active=0 ' >/dev/null &&
         printf '%s\n' "$STATUS" |
          LC_ALL=C grep -F ' raw_slots=0 ' >/dev/null; then
        break
      fi
      attempt=$((attempt + 1))
      sleep 0.1
    done
    require_empty_status "$STATUS"
    require_resident_callbacks_status "$STATUS"
    EVENTS=$(run_app_command "events $TOKEN_5") ||
      fail "stage 5 events command timed out"
    printf '%s\n' "$EVENTS" >> "$EVIDENCE"
    count_contains "$EVENTS" 'op=34 result=0' 2
    count_contains "$EVENTS" 'op=22 result=0' 2
    count_contains "$EVENTS" 'op=5 result=0' 1
    count_contains "$EVENTS" 'op=7 result=0' 1
    finish_log_window
    count_contains "$R3O_STAGE_LOGS" 'exit_restore_begin slot=' 2
    count_contains "$R3O_STAGE_LOGS" 'exit_restore_end slot=' 2
    count_contains "$R3O_STAGE_LOGS" 'ref=mm_count op=mmdrop' 2
    verify_boot_and_warning
    write_state 6
    ;;
  6)
    modules=$(module_list) || fail "stage 6 module list failed"
    require_contains "$modules" "$MODULE"
    STATUS=$(run_app_command status) ||
      fail "stage 6 pre-unload status timed out"
    require_empty_status "$STATUS"
    require_resident_callbacks_status "$STATUS"
    unload_module || fail "stage 6 first unload failed"
    verify_boot_and_warning
    load_module || fail "stage 6 reload failed"
    STATUS=$(run_app_command status) ||
      fail "stage 6 reloaded status timed out"
    require_empty_status "$STATUS"
    require_contains "$STATUS" 'raw_abort_resident=0'
    require_contains "$STATUS" 'raw_exit_resident=0'
    printf '%s\n' "$STATUS" >> "$EVIDENCE"
    unload_module || fail "stage 6 second unload failed"
    verify_boot_and_warning
    write_state complete
    ;;
esac

if [ "$STAGE" = 1 ]; then
  finish_log_window
fi
FAIL_REASON=none
trap - EXIT INT TERM
printf 'classification=D4-R3o-stage-%s-stable result=pass next_stage=%s boot_id=%s warn_count=%s evidence=%s\n' \
  "$STAGE" "$(sed -n 's/^R3O_NEXT_STAGE=//p' "$STATE_FILE")" \
  "$R3O_BOOT_ID" "$R3O_WARN_BASE" "$EVIDENCE" | tee -a "$EVIDENCE"
printf '%s\n' "$EVIDENCE"
