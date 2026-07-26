#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TMP_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/wxshadow-inventory-test.XXXXXX")
FIXTURE="$TMP_ROOT/fixture"
REFERENCE_FIXTURE="$FIXTURE/reference.c"
INVENTORY_FIXTURE="$FIXTURE/inventory.txt"
COVERAGE_FIXTURE="$FIXTURE/coverage.md"

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
  mkdir -p "$FIXTURE"
  cat > "$REFERENCE_FIXTURE" <<'EOF'
// safe_read_kernel_u64 @ 0x1000
// hook_wrap3 @ 0x1010
// r0lab_raw_slot_arm @ 0x1020
EOF
  cat > "$INVENTORY_FIXTURE" <<'EOF'
function|family
safe_read_kernel_u64|layout-scanning
hook_wrap3|control-lifecycle
r0lab_raw_slot_arm|state-transition
EOF
  cat > "$COVERAGE_FIXTURE" <<'EOF'
| Family | Status | Notes |
| --- | --- | --- |
| `control-lifecycle` | Adapted | fixture |
| `layout-scanning` | Adapted | fixture |
| `state-transition` | Adapted | fixture |
EOF
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

run_verifier() {
  reference_sha256=$(sha256_file "$REFERENCE_FIXTURE")
  reference_lines=$(wc -l < "$REFERENCE_FIXTURE" | tr -d '[:space:]')
  WXSHADOW_REFERENCE_SOURCE="$REFERENCE_FIXTURE" \
  WXSHADOW_REFERENCE_INVENTORY="$INVENTORY_FIXTURE" \
  WXSHADOW_REFERENCE_COVERAGE="$COVERAGE_FIXTURE" \
  WXSHADOW_REFERENCE_EXPECTED_SHA256="$reference_sha256" \
  WXSHADOW_REFERENCE_EXPECTED_LINES="$reference_lines" \
  WXSHADOW_REFERENCE_EXPECTED_FUNCTION_BLOCKS=3 \
  WXSHADOW_REFERENCE_EXPECTED_FUNCTIONS=3 \
  WXSHADOW_REFERENCE_EXPECTED_FAMILIES=3 \
    "$ROOT/scripts/verify_wxshadow_reference_inventory.sh"
}

expect_rejected() {
  case_name=$1
  if run_verifier \
    > "$TMP_ROOT/$case_name.log" 2>&1; then
    fail "$case_name was accepted"
  fi
  printf 'case=%s result=pass rejected=1\n' "$case_name"
}

reset_fixture
run_verifier > "$TMP_ROOT/valid.log"
printf '%s\n' 'case=valid result=pass'

reset_fixture
tail -n 1 "$INVENTORY_FIXTURE" >> "$INVENTORY_FIXTURE"
expect_rejected duplicate-function

reset_fixture
sed '$d' "$INVENTORY_FIXTURE" > "$FIXTURE/inventory.next"
mv "$FIXTURE/inventory.next" "$INVENTORY_FIXTURE"
expect_rejected missing-function

reset_fixture
sed 's/safe_read_kernel_u64|layout-scanning/safe_read_kernel_u64|unknown-family/' \
  "$INVENTORY_FIXTURE" > "$FIXTURE/inventory.next"
mv "$FIXTURE/inventory.next" "$INVENTORY_FIXTURE"
expect_rejected unknown-family

reset_fixture
sed '/^| `state-transition` |/d' "$COVERAGE_FIXTURE" \
  > "$FIXTURE/coverage.next"
mv "$FIXTURE/coverage.next" "$COVERAGE_FIXTURE"
expect_rejected missing-matrix-family

printf '%s\n' \
  'wxshadow_reference_inventory_host=pass valid_cases=1 rejected_cases=4 result=pass'
