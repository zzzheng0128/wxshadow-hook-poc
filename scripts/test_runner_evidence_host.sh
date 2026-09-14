#!/bin/sh
set -eu

# Synthetic shell processes and a fake adb_device function only.
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
WORK=$(mktemp -d "${TMPDIR:-/tmp}/r0lab-runner-evidence.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
CASES=0

fail() {
  printf 'runner evidence host test failure: %s\n' "$*" >&2
  exit 1
}

cat > "$WORK/fixture.sh" <<'FIXTURE'
#!/bin/sh
set -eu
ROOT=$1
. "$ROOT/scripts/lib/evidence_run.sh"
evidence_run_init "$2" fixture same-second
printf '%s\n' "$MANIFEST" > "$3"
EVIDENCE_RUN_PHASE=fixture
case "$4" in
  complete) evidence_run_complete 0 ;;
  unfinished) : ;;
  earlier_pass) printf 'phase=child result=pass\n' >> "$MANIFEST" ;;
  explicit_zero) exit 0 ;;
  explicit_error) exit 7 ;;
  errexit) false ;;
  completed_error) evidence_run_complete 0; exit 9 ;;
  HUP|INT|TERM) kill -"$4" "$$" ;;
  completed_HUP|completed_INT|completed_TERM)
    evidence_run_complete 0
    kill -"${4#completed_}" "$$"
    ;;
  *) exit 99 ;;
esac
FIXTURE

run_exit_case() {
  action=$1
  expected_status=$2
  expected_verdict=$3
  expected_reason=$4
  actual_status=0
  sh "$WORK/fixture.sh" "$ROOT" "$WORK/evidence space" "$WORK/$action.path" "$action" \
    > "$WORK/$action.stdout" 2> "$WORK/$action.stderr" || actual_status=$?
  [ "$actual_status" -eq "$expected_status" ] ||
    fail "$action: expected exit $expected_status, got $actual_status"
  manifest=$(cat "$WORK/$action.path")
  actual_verdict=pass
  "$ROOT/scripts/verify_evidence_result.sh" "$manifest" > "$WORK/verdict" 2>&1 ||
    actual_verdict=reject
  [ "$actual_verdict" = "$expected_verdict" ] ||
    fail "$action: expected verdict $expected_verdict, got $actual_verdict"
  if [ -n "$expected_reason" ]; then
    tail -n 1 "$manifest" | grep -F "reason=$expected_reason " >/dev/null ||
      fail "$action: missing terminal failure reason"
    tail -n 1 "$manifest" | grep -F 'phase=fixture ' >/dev/null ||
      fail "$action: missing interrupted phase"
  fi
  CASES=$((CASES + 1))
}

run_exit_case complete 0 pass ''
run_exit_case unfinished 1 reject runner_incomplete
run_exit_case earlier_pass 1 reject runner_incomplete
run_exit_case explicit_zero 1 reject runner_incomplete
run_exit_case explicit_error 7 reject runner_incomplete
run_exit_case errexit 1 reject runner_incomplete
run_exit_case completed_error 9 reject runner_incomplete
run_exit_case HUP 129 reject signal_HUP
run_exit_case INT 130 reject signal_INT
run_exit_case TERM 143 reject signal_TERM
run_exit_case completed_HUP 129 reject signal_HUP
run_exit_case completed_INT 130 reject signal_INT
run_exit_case completed_TERM 143 reject signal_TERM

original_manifest=$(cat "$WORK/complete.path")
cp "$original_manifest" "$WORK/original-manifest"
sh "$WORK/fixture.sh" "$ROOT" "$WORK/evidence space" "$WORK/parallel1.path" complete &
first_pid=$!
sh "$WORK/fixture.sh" "$ROOT" "$WORK/evidence space" "$WORK/parallel2.path" complete &
second_pid=$!
wait "$first_pid" || fail 'first concurrent fixture failed'
wait "$second_pid" || fail 'second concurrent fixture failed'
first_manifest=$(cat "$WORK/parallel1.path")
second_manifest=$(cat "$WORK/parallel2.path")
[ "$first_manifest" != "$second_manifest" ] || fail 'concurrent manifests collided'
[ "$first_manifest" != "$original_manifest" ] || fail 'previous manifest reused'
[ "$second_manifest" != "$original_manifest" ] || fail 'previous manifest reused'
cmp "$original_manifest" "$WORK/original-manifest" || fail 'previous evidence changed'
"$ROOT/scripts/verify_evidence_result.sh" "$first_manifest"
"$ROOT/scripts/verify_evidence_result.sh" "$second_manifest"
CASES=$((CASES + 1))

cat > "$WORK/snapshot.sh" <<'SNAPSHOT'
#!/bin/sh
set -eu
ROOT=$1
FAKE_MODE=$2
FAKE_COUNTER=$3
. "$ROOT/scripts/lib/device_snapshot.sh"
printf '0\n' > "$FAKE_COUNTER"
adb_device() {
  fake_index=$(cat "$FAKE_COUNTER")
  fake_index=$((fake_index + 1))
  printf '%s\n' "$fake_index" > "$FAKE_COUNTER"
  case "$FAKE_MODE:$*" in
    ready_ok:get-state) printf 'device\n'; return 0 ;;
    ready_error:get-state) printf 'device\n'; return 17 ;;
    ready_extra:get-state) printf 'device\nunexpected\n'; return 0 ;;
    ready_offline:get-state) printf 'offline\n'; return 0 ;;
  esac
  case "$fake_index:$*" in
    '1:shell cat /proc/sys/kernel/random/boot_id'|'3:shell cat /proc/sys/kernel/random/boot_id')
      fake_value=01234567-89ab-cdef-0123-456789abcdef
      if [ "$FAKE_MODE" = reboot ] && [ "$fake_index" -eq 3 ]; then
        fake_value=ffffffff-89ab-cdef-0123-456789abcdef
      fi
      ;;
    '2:shell su -c cat /sys/kernel/warn_count') fake_value=0 ;;
    *) printf 'unexpected fake invocation\n' >&2; return 99 ;;
  esac
  case "$FAKE_MODE" in
    crlf) printf '%s\r\n' "$fake_value" ;;
    extra) printf '%s\nunexpected\n' "$fake_value" ;;
    empty) : ;;
    *) printf '%s\n' "$fake_value" ;;
  esac
  case "$FAKE_MODE:$fake_index" in
    fail_first:1|fail_warn:2|fail_last:3) return 17 ;;
  esac
  return 0
}
case "$FAKE_MODE" in
  ready_*) r0lab_device_ready ;;
  *)
    r0lab_device_snapshot
    [ "$R0LAB_SNAPSHOT_BOOT" = 01234567-89ab-cdef-0123-456789abcdef ]
    [ "$R0LAB_SNAPSHOT_WARN" = 0 ]
    ;;
esac
SNAPSHOT

for mode in stable crlf reboot extra empty fail_first fail_warn fail_last \
  ready_ok ready_error ready_extra ready_offline; do
  expected=reject
  case "$mode" in stable|crlf|ready_ok) expected=pass ;; esac
  actual=pass
  sh "$WORK/snapshot.sh" "$ROOT" "$mode" "$WORK/counter" \
    > "$WORK/snapshot.stdout" 2> "$WORK/snapshot.stderr" || actual=reject
  [ "$actual" = "$expected" ] || fail "snapshot $mode: expected $expected, got $actual"
  CASES=$((CASES + 1))
done

printf 'runner_evidence_host cases=%s device_access=none result=pass\n' "$CASES"
