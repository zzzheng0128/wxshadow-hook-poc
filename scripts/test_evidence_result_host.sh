#!/bin/sh
set -eu

# Offline synthetic logs only; never invokes a device runner.
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
VALIDATOR="$ROOT/scripts/verify_evidence_result.sh"
TMP_DIR=$(mktemp -d "${TMPDIR:-/tmp}/r0lab-evidence-result.XXXXXX")
CASES=0

cleanup() {
  rm -rf "$TMP_DIR"
}

fail() {
  printf 'evidence result host test failure: %s\n' "$*" >&2
  exit 1
}

check_case() {
  name=$1
  expected=$2
  contents=$3
  printf '%b' "$contents" > "$TMP_DIR/$name.log"
  if "$VALIDATOR" "$TMP_DIR/$name.log" > "$TMP_DIR/output" 2>&1; then
    actual=pass
    [ ! -s "$TMP_DIR/output" ] || fail "$name: successful validation must be silent"
  else
    actual=reject
    [ -s "$TMP_DIR/output" ] || fail "$name: missing rejection diagnostic"
  fi
  [ "$actual" = "$expected" ] || fail "$name: expected $expected, got $actual"
  CASES=$((CASES + 1))
  printf 'case=%s result=pass verdict=%s\n' "$name" "$actual"
}

trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

check_case summary pass 'warn_after=0 final_modules=empty result=pass\n'
check_case crlf pass 'phase=setup result=pass\r\nstatus=pass result=pass\r\n\r\n'
check_case trailing_whitespace pass '  result=pass\tfinal_modules=empty\n \t\n\n'
check_case no_final_newline pass 'result=pass'
check_case expected_failure_then_pass pass 'phase=negative expected=fail result=fail\nwarn_after=0 result=pass\n'
check_case expected_summary_fields pass 'expected=fail expected_status=blocked expected_result=skip result=pass\n'
check_case metadata_substrings pass 'route_blocked=expected_denial reason=skip_origin_ret0 result=pass\n'

check_case early_pass_final_fail reject 'phase=setup result=pass\nresult=fail reason=cleanup\n'
check_case early_pass_incomplete reject 'phase=setup result=pass\ncleanup=running\n'
check_case empty reject ''
check_case blank reject '\r\n \t\n'
check_case missing_result reject 'status=pass\n'
check_case prefixed_result reject 'previous_result=pass\n'
check_case embedded_result reject 'message=result=pass\n'
check_case pass_suffix reject 'result=pass-runtime_exit_ignored\n'
check_case pass_underscore_suffix reject 'result=pass_suffix\n'
check_case duplicate_result reject 'result=pass result=pass\n'
check_case conflicting_result reject 'result=fail result=pass\n'
check_case conflicting_result_reversed reject 'result=pass result=fail\n'
check_case empty_result reject 'result=\n'
check_case status_nonpass reject 'status=running result=pass\n'
check_case empty_status reject 'status= result=pass\n'

for verdict in blocked skip classified fail; do
  check_case "terminal_$verdict" reject "phase=setup result=pass\nresult=$verdict\n"
  check_case "field_$verdict" reject "probe=$verdict result=pass\n"
  check_case "status_$verdict" reject "result=pass status=$verdict\n"
  check_case "bare_$verdict" reject "result=pass $verdict\n"
done

if "$VALIDATOR" "$TMP_DIR/missing.log" > "$TMP_DIR/output" 2>&1; then
  fail 'missing file unexpectedly passed'
fi
if "$VALIDATOR" "$TMP_DIR" > "$TMP_DIR/output" 2>&1; then
  fail 'directory unexpectedly passed'
fi
if "$VALIDATOR" > "$TMP_DIR/output" 2>&1; then
  fail 'missing argument unexpectedly passed'
fi
if "$VALIDATOR" "$TMP_DIR/summary.log" "$TMP_DIR/crlf.log" > "$TMP_DIR/output" 2>&1; then
  fail 'extra argument unexpectedly passed'
fi
CASES=$((CASES + 4))

printf 'evidence_result_host=pass cases=%s device_access=none result=pass\n' "$CASES"
