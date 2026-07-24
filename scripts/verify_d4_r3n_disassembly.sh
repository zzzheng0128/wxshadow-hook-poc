#!/bin/sh
set -eu

ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
NDK=${ANDROID_NDK_HOME:-/Users/ivory/Library/Android/sdk/ndk/29.0.14206865}
OBJDUMP=${OBJDUMP:-$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-objdump}
OBJECT=${1:-$ROOT/kpm/build/r0lab.o}
FULL_SYMBOL=r0lab_raw_before_abort
TOKEN_SYMBOL=r0lab_raw_hook_page_token_acquire_full_abort_locked
EXIT_SYMBOL=r0lab_raw_exit_mmap_before

fail() {
  printf '%s\n' "D4-R3n disassembly failure: $*" >&2
  exit 1
}

count_fixed() {
  text=$1
  pattern=$2
  printf '%s\n' "$text" | grep -F -c -- "$pattern" || true
}

count_regex() {
  text=$1
  pattern=$2
  printf '%s\n' "$text" | grep -E -c -- "$pattern" || true
}

disassemble() {
  symbol=$1
  "$OBJDUMP" -dr --disassemble-symbols="$symbol" "$OBJECT"
}

require_symbol() {
  text=$1
  symbol=$2
  printf '%s\n' "$text" | grep -F "<$symbol>:" >/dev/null ||
    fail "callback symbol is missing: $symbol"
}

reject_hardware_ops() {
  text=$1
  symbol=$2
  if printf '%s\n' "$text" |
      grep -Eq '[[:space:]](dmb|dsb|isb|dc|ic|tlbi)([[:space:]]|$)'; then
    fail "explicit hardware cache/TLB instruction found in $symbol"
  fi
}

[ -x "$OBJDUMP" ] || fail "llvm-objdump is unavailable: $OBJDUMP"
[ -f "$OBJECT" ] || fail "KPM object is unavailable: $OBJECT"

FULL_DISASSEMBLY=$(disassemble "$FULL_SYMBOL") ||
  fail "could not disassemble $FULL_SYMBOL"
TOKEN_DISASSEMBLY=$(disassemble "$TOKEN_SYMBOL") ||
  fail "could not disassemble $TOKEN_SYMBOL"
EXIT_DISASSEMBLY=$(disassemble "$EXIT_SYMBOL") ||
  fail "could not disassemble $EXIT_SYMBOL"

require_symbol "$FULL_DISASSEMBLY" "$FULL_SYMBOL"
require_symbol "$TOKEN_DISASSEMBLY" "$TOKEN_SYMBOL"
require_symbol "$EXIT_DISASSEMBLY" "$EXIT_SYMBOL"

for primitive in \
  r0lab_raw_activate_shadow \
  r0lab_raw_finish_read_cycle \
  r0lab_raw_begin_fault_read_cycle \
  r0lab_raw_restore_original
do
  [ "$(count_regex "$FULL_DISASSEMBLY" \
      "R_AARCH64_CALL26[[:space:]]+$primitive$")" -eq 1 ] ||
    fail "expected one full-callback relocation to $primitive"
done

TRANSITION_SET_COUNT=$(
  count_regex "$FULL_DISASSEMBLY" \
    'strb[[:space:]]+w[0-9]+, \[x[0-9]+, #0xfb\]$'
)
TRANSITION_CLEAR_COUNT=$(
  count_regex "$FULL_DISASSEMBLY" \
    'strb[[:space:]]+wzr, \[x[0-9]+, #0xfb\]$'
)
SKIP_ORIGIN_COUNT=$(
  count_regex "$FULL_DISASSEMBLY" \
    'str[[:space:]]+w[0-9]+, \[x21, #0x8\]$'
)
RET_ZERO_COUNT=$(
  count_regex "$FULL_DISASSEMBLY" \
    'str[[:space:]]+xzr, \[x21, #0x50\]$'
)
EXIT_RESTORE_COUNT=$(
  count_regex "$EXIT_DISASSEMBLY" \
    'R_AARCH64_CALL26[[:space:]]+r0lab_raw_restore_original$'
)

[ "$TRANSITION_SET_COUNT" -eq 3 ] ||
  fail "expected three branch transition-set stores, found $TRANSITION_SET_COUNT"
[ "$TRANSITION_CLEAR_COUNT" -eq 3 ] ||
  fail "expected three generation-matched transition clears, found $TRANSITION_CLEAR_COUNT"
[ "$SKIP_ORIGIN_COUNT" -eq 2 ] ||
  fail "expected paired IABT/read skip_origin stores, found $SKIP_ORIGIN_COUNT"
[ "$RET_ZERO_COUNT" -eq 2 ] ||
  fail "expected paired IABT/read ret=0 stores, found $RET_ZERO_COUNT"
[ "$EXIT_RESTORE_COUNT" -eq 3 ] ||
  fail "unexpected optimized two-slot restore shape: $EXIT_RESTORE_COUNT relocations"

if printf '%s\n' "$TOKEN_DISASSEMBLY" |
    grep -q 'R_AARCH64_CALL26'; then
  fail "no-accounting token helper unexpectedly calls another function"
fi

if printf '%s\n' "$EXIT_DISASSEMBLY" |
    grep -Eq 'R_AARCH64_CALL26[[:space:]]+r0lab_raw_(unhook|wait|reset|close)'; then
  fail "exit callback contains forbidden destruction/lifecycle call"
fi

for disassembly in \
  "$FULL_DISASSEMBLY" \
  "$TOKEN_DISASSEMBLY" \
  "$EXIT_DISASSEMBLY"
do
  reject_hardware_ops "$disassembly" "R3n callback"
done

printf '%s\n' \
  "D4-R3n disassembly passed: primitives=4 transition_sets=3 transition_clears=3 skip_origin_stores=2 ret_zero_stores=2 exit_restore_relocations=3 token_helper_calls=0 hardware_cache_tlb=0"
