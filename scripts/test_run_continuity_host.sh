#!/bin/sh
set -eu

# Offline synthetic arguments only; never invokes a device or build runner.
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
VALIDATOR="$ROOT/scripts/verify_run_continuity.sh"
TMP_DIR=$(mktemp -d "${TMPDIR:-/tmp}/r0lab-run-continuity.XXXXXX")
CASES=0

cleanup() {
  rm -rf "$TMP_DIR"
}

fail() {
  printf 'run continuity host test failure: %s\n' "$*" >&2
  exit 1
}

check_case() {
  name=$1
  expected_status=$2
  shift 2
  if "$VALIDATOR" "$@" > "$TMP_DIR/output" 2> "$TMP_DIR/error"; then
    actual_status=0
  else
    actual_status=$?
  fi
  [ "$actual_status" = "$expected_status" ] ||
    fail "$name: expected status $expected_status, got $actual_status"
  [ ! -s "$TMP_DIR/output" ] || fail "$name: unexpected stdout"
  if [ "$actual_status" -eq 0 ]; then
    [ ! -s "$TMP_DIR/error" ] || fail "$name: successful validation must be silent"
  else
    [ -s "$TMP_DIR/error" ] || fail "$name: missing rejection diagnostic"
  fi
  CASES=$((CASES + 1))
  printf 'case=%s result=pass status=%s\n' "$name" "$actual_status"
}

trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

BOOT=01234567-89ab-cdef-0123-456789abcdef
UPPER_BOOT=01234567-89AB-CDEF-0123-456789ABCDEF
OTHER_BOOT=11234567-89ab-cdef-0123-456789abcdef
CR=$(printf '\r')
LF=$(printf '\nx')
LF=${LF%x}
TAB=$(printf '\t')

check_case unchanged_zero 0 "$BOOT" "$BOOT" 0 0
check_case unchanged_nonzero 0 "$BOOT" "$BOOT" 17 17
check_case case_insensitive_uuid 0 "$BOOT" "$UPPER_BOOT" 0 0
check_case case_insensitive_uuid_reversed 0 "$UPPER_BOOT" "$BOOT" 0 0
check_case reboot_warning_still_zero 1 "$BOOT" "$OTHER_BOOT" 0 0
check_case reboot_warning_unchanged 1 "$BOOT" "$OTHER_BOOT" 17 17
check_case warning_increased 1 "$BOOT" "$BOOT" 17 18
check_case warning_decreased 1 "$BOOT" "$BOOT" 17 16
check_case warning_reset 1 "$BOOT" "$BOOT" 17 0
check_case leading_zeroes 0 "$BOOT" "$BOOT" 00017 17
check_case leading_zeroes_reversed 0 "$BOOT" "$BOOT" 17 00017
check_case all_zeroes 0 "$BOOT" "$BOOT" 00000 0
check_case different_counts_same_length 1 "$BOOT" "$BOOT" 0017 0018
check_case above_float_precision 1 "$BOOT" "$BOOT" 9007199254740992 9007199254740993
check_case above_uint64_range 1 "$BOOT" "$BOOT" 18446744073709551616 18446744073709551617

LONG_COUNT=$(awk 'BEGIN { for (i = 0; i < 4096; i++) printf "9" }')
LONG_ZEROES=$(awk 'BEGIN { for (i = 0; i < 4096; i++) printf "0" }')
check_case long_count_unchanged 0 "$BOOT" "$BOOT" "$LONG_COUNT" "$LONG_COUNT"
check_case long_count_leading_zeroes 0 "$BOOT" "$BOOT" "${LONG_ZEROES}${LONG_COUNT}" "$LONG_COUNT"
check_case long_all_zeroes 0 "$BOOT" "$BOOT" "$LONG_ZEROES" 0
check_case long_count_increased 1 "$BOOT" "$BOOT" "${LONG_COUNT}8" "${LONG_COUNT}9"
check_case long_count_decreased 1 "$BOOT" "$BOOT" "${LONG_COUNT}9" "${LONG_COUNT}8"

INVALID_INDEX=0
for invalid in '' 'not-a-uuid' '0123456789abcdef0123456789abcdef' \
  'g1234567-89ab-cdef-0123-456789abcdef' \
  '0123456--89ab-cdef-0123-456789abcdef' \
  '01234567-89ab-cdef-0123-456789abcde' \
  '01234567-89ab-cdef-0123-456789abcdef0' \
  " $BOOT" "$BOOT " "$BOOT extra=value" "$BOOT$TAB" \
  "$BOOT$CR" "$BOOT$LF" "$BOOT$CR$LF" \
  "$BOOT$LF$BOOT" "01234567-89ab-cdef-0123-456789abcde$CR"; do
  INVALID_INDEX=$((INVALID_INDEX + 1))
  check_case "invalid_base_boot_$INVALID_INDEX" 1 "$invalid" "$BOOT" 0 0
  check_case "invalid_current_boot_$INVALID_INDEX" 1 "$BOOT" "$invalid" 0 0
done

INVALID_INDEX=0
for invalid in '' '-1' '+1' '1.0' '1e0' '0x10' 'NaN' 'inf' \
  ' 0' '0 ' '0 0' '0 extra=value' "0$TAB" "0$CR" "0$LF" \
  "0$CR$LF" "0${LF}0" "0${CR}0" '１２'; do
  INVALID_INDEX=$((INVALID_INDEX + 1))
  check_case "invalid_base_warn_$INVALID_INDEX" 1 "$BOOT" "$BOOT" "$invalid" 0
  check_case "invalid_current_warn_$INVALID_INDEX" 1 "$BOOT" "$BOOT" 0 "$invalid"
done

check_case no_arguments 2
check_case one_argument 2 "$BOOT"
check_case two_arguments 2 "$BOOT" "$BOOT"
check_case three_arguments 2 "$BOOT" "$BOOT" 0
check_case extra_argument 2 "$BOOT" "$BOOT" 0 0 extra=value
check_case extra_empty_argument 2 "$BOOT" "$BOOT" 0 0 ''

printf 'run_continuity_host=pass cases=%s device_access=none result=pass\n' "$CASES"
