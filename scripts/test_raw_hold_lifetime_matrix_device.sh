#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SERIAL=${ANDROID_SERIAL:-}
MODULE=r0lab-m1
REMOTE=/data/local/tmp/r0lab-m1.kpm
PACKAGE=dev.r0hook.lab
ACTIVITY=dev.r0hook.lab/.MainActivity
IDLE_SECONDS=${RAW_HOLD_LIFETIME_MATRIX_IDLE_SECONDS:-15}
TOKEN_SOURCE_SINGLE=${RAW_HOLD_LIFETIME_SOURCE_SINGLE_TOKEN:-0x729271}
TOKEN_SHADOW_SINGLE=${RAW_HOLD_LIFETIME_SHADOW_SINGLE_TOKEN:-0x729272}
TOKEN_SOURCE_DOUBLE=${RAW_HOLD_LIFETIME_SOURCE_DOUBLE_TOKEN:-0x729273}
TOKEN_SHADOW_DOUBLE=${RAW_HOLD_LIFETIME_SHADOW_DOUBLE_TOKEN:-0x729274}
EVIDENCE_DIR="$ROOT/build/evidence"
EVIDENCE="$EVIDENCE_DIR/raw-hold-lifetime-matrix-$(date +%Y%m%d-%H%M%S).log"
MODULE_LOADED=0
SESSION_OPEN=0
HOLD_ACTIVE=0
ACTIVE_TOKEN=
ACTIVE_SLOTS=0
ACTIVE_PHASE=

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
  printf '%s\n' "raw hold lifetime matrix failure: $*" >&2
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
  printf 'phase=%s_preserve active_hold=1 cleanup=not_run reason=%s status=%s token=%s slots=%s\n' \
    "$ACTIVE_PHASE" "$reason" "$status" "$ACTIVE_TOKEN" "$ACTIVE_SLOTS" \
    >> "$EVIDENCE"
  printf '%s\n' \
    "raw hold lifetime matrix preserving module: active raw hold remains and cleanup reentry is forbidden" >&2
  exit "$status"
}

cleanup() {
  status=$?
  trap - EXIT INT TERM
  if [ "$HOLD_ACTIVE" -eq 1 ]; then
    printf 'phase=%s_preserve active_hold=1 cleanup=not_run reason=trap status=%s token=%s slots=%s\n' \
      "$ACTIVE_PHASE" "$status" "$ACTIVE_TOKEN" "$ACTIVE_SLOTS" \
      >> "$EVIDENCE"
    printf '%s\n' \
      "raw hold lifetime matrix preserving module: active raw hold remains and cleanup reentry is forbidden" >&2
    exit "$status"
  fi
  if [ "$status" -ne 0 ] && [ "$SESSION_OPEN" -eq 1 ]; then
    run_app_command "close $ACTIVE_TOKEN" >/dev/null 2>&1 || true
    SESSION_OPEN=0
  fi
  if [ "$MODULE_LOADED" -eq 1 ] && [ "$status" -ne 0 ]; then
    CLEANUP_STATUS=$(run_app_command status 2>&1 || true)
    case "$CLEANUP_STATUS" in
      *"active=0"*"raw_slots=0"*"page_records=0"*)
        if prepare_worker_shutdown >/dev/null 2>&1; then
          supercmd module unload "$MODULE" >/dev/null 2>&1 || true
        else
          printf '%s\n' \
            "raw hold lifetime matrix cleanup could not stop workers; preserving module" >&2
        fi
        ;;
      *)
        printf '%s\n' \
          "raw hold lifetime matrix cleanup did not confirm empty state; preserving module" >&2
        ;;
    esac
  fi
  exit "$status"
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

wait_for_clean_status() {
  phase=$1
  attempt=0
  while [ "$attempt" -lt 50 ]; do
    output=$(run_app_command status)
    case "$output" in
      *"active=0"*"raw_slots=0"*"page_records=0"*)
        printf '%s\n' "$output"
        return 0
        ;;
    esac
    attempt=$((attempt + 1))
    sleep 0.1
  done
  printf '%s\n' "$output"
  fail "$phase did not return to clean status"
}

poll_adb_transport() {
  phase=$1
  classification=$2
  elapsed=0

  printf 'phase=%s_idle evidence=begin idle_seconds=%s post_hold_status=not_used boot_id_reader=not_used proc_maps=not_used cleanup=preserve\n' \
    "$phase" "$IDLE_SECONDS" >> "$EVIDENCE"
  while [ "$elapsed" -lt "$IDLE_SECONDS" ]; do
    sleep 1
    elapsed=$((elapsed + 1))
    set +e
    state=$(adb_device get-state 2>&1)
    rc=$?
    set -e
    state=$(printf '%s' "$state" | LC_ALL=C tr -d '\r')
    printf 'phase=%s_idle_poll second=%s adb_state_rc=%s adb_state="%s"\n' \
      "$phase" "$elapsed" "$rc" "$state" >> "$EVIDENCE"
    if [ "$rc" -ne 0 ] || [ "$state" != device ]; then
      printf 'phase=%s_idle result=fail classification=%s second=%s adb_state_rc=%s adb_state="%s"\n' \
        "$phase" "$classification" "$elapsed" "$rc" "$state" >> "$EVIDENCE"
      capture_pstore "$(basename "$EVIDENCE" .log)-$phase"
      fail "$classification: adb transport changed during idle hold"
    fi
  done
  printf 'phase=%s_idle result=pass classification=%s-stable idle_seconds=%s post_hold_status=not_used boot_id_reader=not_used proc_maps=not_used cleanup=preserve\n' \
    "$phase" "$phase" "$IDLE_SECONDS" | tee -a "$EVIDENCE"
}

clear_active_slots() {
  phase=$1
  token=$2
  slots=$3
  slot=0

  while [ "$slot" -lt "$slots" ]; do
    CLEAR_OUTPUT=$(run_app_command "raw slot clear $token $slot")
    require_contains "$CLEAR_OUTPUT" "raw_slot_clear_pending slot=$slot"
    printf '%s\n' "$CLEAR_OUTPUT" >> "$EVIDENCE"
    CLEARED_OUTPUT=$(wait_for_slot_cleared "$token" "$slot") ||
      fail "$phase slot $slot did not report cleared"
    printf '%s\n' "$CLEARED_OUTPUT" >> "$EVIDENCE"
    slot=$((slot + 1))
  done
  HOLD_ACTIVE=0

  STATUS_AFTER_CLEAR=$(run_app_command status)
  require_contains "$STATUS_AFTER_CLEAR" 'active=1'
  require_contains "$STATUS_AFTER_CLEAR" 'raw_slots=0'
  require_contains "$STATUS_AFTER_CLEAR" 'page_records=0'
  printf '%s\n' "$STATUS_AFTER_CLEAR" >> "$EVIDENCE"
  printf 'phase=%s_explicit_cleanup result=pass raw_slots=0 page_records=0\n' \
    "$phase" >> "$EVIDENCE"

  CLOSE_OUTPUT=$(run_app_command "close $token")
  require_contains "$CLOSE_OUTPUT" 'closed final_summary_seq='
  SESSION_OPEN=0
  printf '%s\n' "$CLOSE_OUTPUT" >> "$EVIDENCE"

  adb_device shell am force-stop "$PACKAGE" >/dev/null 2>&1 || true
  sleep 1
  STATUS_CLEAN=$(wait_for_clean_status "$phase")
  require_contains "$STATUS_CLEAN" 'active=0'
  require_contains "$STATUS_CLEAN" 'raw_slots=0'
  require_contains "$STATUS_CLEAN" 'page_records=0'
  printf '%s\n' "$STATUS_CLEAN" >> "$EVIDENCE"
}

run_matrix_phase() {
  phase=$1
  token=$2
  command=$3
  target_state=$4
  slots=$5
  expected_normal=$6
  expected_shadow=$7
  expected_activations=$8
  expected_states=$9
  expected_inspect=${10}
  unstable_classification=${11}

  ACTIVE_PHASE=$phase
  ACTIVE_TOKEN=$token
  ACTIVE_SLOTS=$slots
  printf 'phase=%s token=%s command="%s"\n' "$phase" "$token" "$command" \
    >> "$EVIDENCE"

  STATUS_CLEAN=$(wait_for_clean_status "$phase-pre")
  require_contains "$STATUS_CLEAN" 'raw_slots=0'
  require_contains "$STATUS_CLEAN" 'page_records=0'
  printf '%s\n' "$STATUS_CLEAN" >> "$EVIDENCE"

  ARM_OUTPUT=$(run_app_command "arm $token")
  require_contains "$ARM_OUTPUT" 'armed uid='
  SESSION_OPEN=1
  printf '%s\n' "$ARM_OUTPUT" >> "$EVIDENCE"

  RAW_OUTPUT=$(run_app_command "$command $token")
  printf '%s\n' "$RAW_OUTPUT" >> "$EVIDENCE"
  require_contains "$RAW_OUTPUT" 'raw mode=raw-hold-lifetime failures=0'
  require_contains "$RAW_OUTPUT" 'exit_mmap_armed=0'
  require_contains "$RAW_OUTPUT" "target_state=$target_state"
  require_contains "$RAW_OUTPUT" "slots=$slots"
  require_contains "$RAW_OUTPUT" "raw_slots=$slots"
  require_contains "$RAW_OUTPUT" "page_records=$slots"
  require_contains "$RAW_OUTPUT" "normal=$expected_normal"
  require_contains "$RAW_OUTPUT" "shadow=$expected_shadow"
  require_contains "$RAW_OUTPUT" "activations=$expected_activations"
  require_contains "$RAW_OUTPUT" "states=$expected_states"
  require_contains "$RAW_OUTPUT" "inspect=$expected_inspect"
  require_contains "$RAW_OUTPUT" 'generation='
  HOLD_ACTIVE=1

  GENERATIONS=$(printf '%s\n' "$RAW_OUTPUT" |
    sed -n 's/.*generation=\([0-9][0-9]*\)\/\([0-9][0-9]*\).*/\1 \2/p' |
    sed -n '1p')
  [ -n "$GENERATIONS" ] || fail "could not parse $phase generations"
  set -- $GENERATIONS
  printf 'phase=%s_established result=pass exit_mmap_armed=0 target_state=%s raw_slots=%s page_records=%s generation=%s/%s\n' \
    "$phase" "$target_state" "$slots" "$slots" "$1" "$2" >> "$EVIDENCE"

  poll_adb_transport "$phase" "$unstable_classification"
  clear_active_slots "$phase" "$token" "$slots"
}

trap cleanup EXIT INT TERM

case "$IDLE_SECONDS" in
  ''|*[!0-9]*) fail "RAW_HOLD_LIFETIME_MATRIX_IDLE_SECONDS must be a non-negative integer" ;;
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
printf 'serial=%s lab_uid=%s idle_seconds=%s tokens=%s,%s,%s,%s warn_before=%s %s\n' \
  "${SERIAL:-default}" "$LAB_UID" "$IDLE_SECONDS" \
  "$TOKEN_SOURCE_SINGLE" "$TOKEN_SHADOW_SINGLE" "$TOKEN_SOURCE_DOUBLE" \
  "$TOKEN_SHADOW_DOUBLE" "$WARN_BEFORE" "$(boot_props)" | tee "$EVIDENCE"

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

run_matrix_phase d4_r3e_l1_source_single "$TOKEN_SOURCE_SINGLE" \
  'raw raw-hold lifetime source single' source_uxn 1 42/-1 -1/-1 0/0 2/0 1/0 \
  D4-R3e-L1-single-source-uxn-unstable
run_matrix_phase d4_r3e_l1_shadow_single "$TOKEN_SHADOW_SINGLE" \
  'raw raw-hold lifetime shadow single' shadow_rx 1 42/-1 99/-1 1/0 3/0 1/0 \
  D4-R3e-L1-single-shadow-rx-unstable
run_matrix_phase d4_r3e_l1_source_double "$TOKEN_SOURCE_DOUBLE" \
  'raw raw-hold lifetime source double' source_uxn 2 42/42 -1/-1 0/0 2/2 1/1 \
  D4-R3e-L1-two-source-uxn-unstable
run_matrix_phase d4_r3e_l1_shadow_double "$TOKEN_SHADOW_DOUBLE" \
  'raw raw-hold lifetime shadow double' shadow_rx 2 42/42 99/99 1/1 3/3 1/1 \
  D4-R3e-L1-two-shadow-rx-unstable

FINAL_STATUS=$(run_app_command status)
require_contains "$FINAL_STATUS" 'active=0'
require_contains "$FINAL_STATUS" 'raw_slots=0'
require_contains "$FINAL_STATUS" 'page_records=0'
printf '%s\n' "$FINAL_STATUS" >> "$EVIDENCE"

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
printf 'raw_hold_lifetime_matrix=pass classification=D4-R3e-L1-lifetime-matrix-stable warn_after=%s final_modules=empty result=pass\n' \
  "$WARN_AFTER" | tee -a "$EVIDENCE"
printf '%s\n' "$EVIDENCE"
