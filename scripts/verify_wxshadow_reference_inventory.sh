#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
REFERENCE=${WXSHADOW_REFERENCE_SOURCE:-/Users/ivory/Project/article/r0hook/docs/wxshadow_final.c}
INVENTORY="$ROOT/docs/wxshadow-reference-function-inventory.txt"
COVERAGE="$ROOT/docs/wxshadow-reference-function-coverage.md"
EXPECTED_FUNCTIONS=141
EXPECTED_FAMILIES=14

fail() {
  printf '%s\n' "wxshadow reference inventory verification failure: $*" >&2
  exit 1
}

"$ROOT/scripts/verify_wxshadow_reference_source.sh" >/dev/null
[ -f "$INVENTORY" ] || fail "inventory is missing: $INVENTORY"
[ -f "$COVERAGE" ] || fail "coverage matrix is missing: $COVERAGE"

TMP_DIR=$(mktemp -d "${TMPDIR:-/tmp}/wxshadow-inventory.XXXXXX")
trap 'rm -rf "$TMP_DIR"' EXIT HUP INT TERM

grep -E '^// [A-Za-z_][A-Za-z0-9_]* +@ 0x' "$REFERENCE" |
  sed -E 's|^// ([A-Za-z_][A-Za-z0-9_]*) +@.*$|\1|' |
  LC_ALL=C sort > "$TMP_DIR/reference-functions"

awk -F '[|]' '
  NR == 1 {
    if ($1 != "function" || $2 != "family" || NF != 2) {
      print "invalid inventory header" > "/dev/stderr"
      exit 1
    }
    next
  }
  NF != 2 {
    print "invalid field count at line " NR > "/dev/stderr"
    exit 1
  }
  $1 !~ /^[A-Za-z_][A-Za-z0-9_]*$/ {
    print "invalid function name at line " NR > "/dev/stderr"
    exit 1
  }
  $2 !~ /^(control-lifecycle|page-ownership|dirty-patch|pte-transaction|state-transition|fault-routing|gup-routing|fork-exit-routing|brk-step|write-command|symbol-resolution|layout-scanning|compat-helpers|import-stubs)$/ {
    print "invalid family at line " NR > "/dev/stderr"
    exit 1
  }
  {
    print $1
  }
' "$INVENTORY" | LC_ALL=C sort > "$TMP_DIR/inventory-functions" ||
  fail "inventory format is invalid"

function_count=$(wc -l < "$TMP_DIR/inventory-functions" | tr -d '[:space:]')
[ "$function_count" = "$EXPECTED_FUNCTIONS" ] ||
  fail "function count mismatch: expected=$EXPECTED_FUNCTIONS actual=$function_count"

duplicates=$(
  tail -n +2 "$INVENTORY" |
    cut -d '|' -f 1 |
    LC_ALL=C sort |
    uniq -d
)
[ -z "$duplicates" ] || fail "duplicate functions: $duplicates"

if ! diff -u "$TMP_DIR/reference-functions" "$TMP_DIR/inventory-functions" \
  > "$TMP_DIR/function-set.diff"; then
  cat "$TMP_DIR/function-set.diff" >&2
  fail "inventory function set does not match the pinned reference"
fi

family_count=$(
  tail -n +2 "$INVENTORY" |
    cut -d '|' -f 2 |
    LC_ALL=C sort -u |
    wc -l |
    tr -d '[:space:]'
)
[ "$family_count" = "$EXPECTED_FAMILIES" ] ||
  fail "family count mismatch: expected=$EXPECTED_FAMILIES actual=$family_count"

tail -n +2 "$INVENTORY" |
  cut -d '|' -f 2 |
  LC_ALL=C sort -u > "$TMP_DIR/inventory-families"

sed -nE \
  's/^\| `([^`]+)` \| (Adapted|Partial|Rejected|Reference-only|Not applicable) \|.*$/\1/p' \
  "$COVERAGE" |
  LC_ALL=C sort > "$TMP_DIR/matrix-families"

matrix_family_count=$(
  wc -l < "$TMP_DIR/matrix-families" |
    tr -d '[:space:]'
)
[ "$matrix_family_count" = "$EXPECTED_FAMILIES" ] ||
  fail "matrix family count mismatch: expected=$EXPECTED_FAMILIES actual=$matrix_family_count"

if ! diff -u "$TMP_DIR/inventory-families" "$TMP_DIR/matrix-families" \
  > "$TMP_DIR/family-set.diff"; then
  cat "$TMP_DIR/family-set.diff" >&2
  fail "coverage matrix families do not match the inventory"
fi

printf 'wxshadow_reference_inventory=pass functions=%s families=%s exact_set=1 matrix_set=1 result=pass\n' \
  "$function_count" "$family_count"
