#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TMP_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/wxshadow-inventory-test.XXXXXX")
FIXTURE="$TMP_ROOT/fixture"

fail() {
  printf '%s\n' "wxshadow reference inventory host test failure: $*" >&2
  exit 1
}

cleanup() {
  rm -rf "$TMP_ROOT"
}
trap cleanup EXIT HUP INT TERM

reset_fixture() {
  rm -rf "$FIXTURE"
  mkdir -p "$FIXTURE/scripts" "$FIXTURE/docs"
  cp "$ROOT/scripts/verify_wxshadow_reference_source.sh" "$FIXTURE/scripts/"
  cp "$ROOT/scripts/verify_wxshadow_reference_inventory.sh" "$FIXTURE/scripts/"
  cp "$ROOT/docs/wxshadow-reference-function-inventory.txt" "$FIXTURE/docs/"
  cp "$ROOT/docs/wxshadow-reference-function-coverage.md" "$FIXTURE/docs/"
}

expect_rejected() {
  case_name=$1
  if sh "$FIXTURE/scripts/verify_wxshadow_reference_inventory.sh" \
    > "$TMP_ROOT/$case_name.log" 2>&1; then
    fail "$case_name was accepted"
  fi
  printf 'case=%s result=pass rejected=1\n' "$case_name"
}

reset_fixture
sh "$FIXTURE/scripts/verify_wxshadow_reference_inventory.sh" \
  > "$TMP_ROOT/valid.log"
printf '%s\n' 'case=valid result=pass'

reset_fixture
tail -n 1 "$ROOT/docs/wxshadow-reference-function-inventory.txt" \
  >> "$FIXTURE/docs/wxshadow-reference-function-inventory.txt"
expect_rejected duplicate-function

reset_fixture
sed '$d' "$ROOT/docs/wxshadow-reference-function-inventory.txt" \
  > "$FIXTURE/docs/inventory.next"
mv "$FIXTURE/docs/inventory.next" \
  "$FIXTURE/docs/wxshadow-reference-function-inventory.txt"
expect_rejected missing-function

reset_fixture
sed 's/safe_read_kernel_u64|layout-scanning/safe_read_kernel_u64|unknown-family/' \
  "$ROOT/docs/wxshadow-reference-function-inventory.txt" \
  > "$FIXTURE/docs/inventory.next"
mv "$FIXTURE/docs/inventory.next" \
  "$FIXTURE/docs/wxshadow-reference-function-inventory.txt"
expect_rejected unknown-family

reset_fixture
sed '/^| `import-stubs` |/d' \
  "$ROOT/docs/wxshadow-reference-function-coverage.md" \
  > "$FIXTURE/docs/coverage.next"
mv "$FIXTURE/docs/coverage.next" \
  "$FIXTURE/docs/wxshadow-reference-function-coverage.md"
expect_rejected missing-matrix-family

printf '%s\n' \
  'wxshadow_reference_inventory_host=pass valid_cases=1 rejected_cases=4 result=pass'
