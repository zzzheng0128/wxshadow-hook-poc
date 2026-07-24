#!/bin/sh
set -eu

REFERENCE=${WXSHADOW_REFERENCE_SOURCE:-/Users/ivory/Project/article/r0hook/docs/wxshadow_final.c}
EXPECTED_SHA256=2b2fb7ade7e572fd5aea8a79f39bd9c743b1ee1dc57552449209612e69903191
EXPECTED_LINES=7124
EXPECTED_FUNCTION_BLOCKS=141

fail() {
  printf '%s\n' "wxshadow reference verification failure: $*" >&2
  exit 1
}

[ -f "$REFERENCE" ] || fail "reference file is missing: $REFERENCE"

if command -v shasum >/dev/null 2>&1; then
  actual_sha256=$(shasum -a 256 "$REFERENCE" | awk '{ print $1 }')
elif command -v sha256sum >/dev/null 2>&1; then
  actual_sha256=$(sha256sum "$REFERENCE" | awk '{ print $1 }')
else
  fail "neither shasum nor sha256sum is available"
fi

actual_lines=$(wc -l < "$REFERENCE" | tr -d '[:space:]')
actual_function_blocks=$(
  grep -Ec '^// [A-Za-z_][A-Za-z0-9_]* +@ 0x' "$REFERENCE"
)

[ "$actual_sha256" = "$EXPECTED_SHA256" ] ||
  fail "SHA-256 mismatch: expected=$EXPECTED_SHA256 actual=$actual_sha256"
[ "$actual_lines" = "$EXPECTED_LINES" ] ||
  fail "line-count mismatch: expected=$EXPECTED_LINES actual=$actual_lines"
[ "$actual_function_blocks" = "$EXPECTED_FUNCTION_BLOCKS" ] ||
  fail "function-block mismatch: expected=$EXPECTED_FUNCTION_BLOCKS actual=$actual_function_blocks"

printf 'wxshadow_reference_source=pass path=%s sha256=%s lines=%s function_blocks=%s result=pass\n' \
  "$REFERENCE" "$actual_sha256" "$actual_lines" "$actual_function_blocks"
