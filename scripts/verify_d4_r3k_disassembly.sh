#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
NDK=${ANDROID_NDK_HOME:-/Users/ivory/Library/Android/sdk/ndk/29.0.14206865}
OBJDUMP=${OBJDUMP:-$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-objdump}
OBJECT=${1:-$ROOT/kpm/build/r0lab.o}
SYMBOL=r0lab_raw_before_abort_inflight_passthrough

fail() {
  printf '%s\n' "D4-R3k disassembly failure: $*" >&2
  exit 1
}

[ -x "$OBJDUMP" ] || fail "llvm-objdump is unavailable: $OBJDUMP"
[ -f "$OBJECT" ] || fail "KPM object is unavailable: $OBJECT"

DISASSEMBLY=$(
  "$OBJDUMP" -d --disassemble-symbols="$SYMBOL" "$OBJECT"
) || fail "could not disassemble $SYMBOL"

printf '%s\n' "$DISASSEMBLY" |
  grep -F "<$SYMBOL>:" >/dev/null ||
  fail "callback symbol is missing"

ADD_COUNT=$(
  printf '%s\n' "$DISASSEMBLY" |
    grep -Ec '[[:space:]]add[[:space:]]+w[0-9]+, w[0-9]+, #0x1$' || true
)
SUB_COUNT=$(
  printf '%s\n' "$DISASSEMBLY" |
    grep -Ec '[[:space:]]sub[[:space:]]+w[0-9]+, w[0-9]+, #0x1$' || true
)
STORE_COUNT=$(
  printf '%s\n' "$DISASSEMBLY" |
    grep -Ec '[[:space:]]str[[:space:]]+w[0-9]+, \[x[0-9]+\]$' || true
)

[ "$ADD_COUNT" -eq 1 ] ||
  fail "expected one retained increment, found $ADD_COUNT"
[ "$SUB_COUNT" -eq 1 ] ||
  fail "expected one retained decrement, found $SUB_COUNT"
[ "$STORE_COUNT" -eq 2 ] ||
  fail "expected two retained counter stores, found $STORE_COUNT"

VISIBLE_WINDOW=$(
  printf '%s\n' "$DISASSEMBLY" |
    awk '
      /[[:space:]]add[[:space:]]+w[0-9]+, w[0-9]+, #0x1$/ {
        visible = 1
      }
      visible {
        print
      }
      /[[:space:]]sub[[:space:]]+w[0-9]+, w[0-9]+, #0x1$/ {
        exit
      }
    '
)
INTERVENING_CALLS=$(
  printf '%s\n' "$VISIBLE_WINDOW" |
    grep -Ec '[[:space:]]blr[[:space:]]+x[0-9]+$' || true
)
[ "$INTERVENING_CALLS" -eq 3 ] ||
  fail "expected unlock, mmput, and second-lock calls between counter operations; found $INTERVENING_CALLS"

printf '%s\n' "$VISIBLE_WINDOW" |
  grep -Eq '[[:space:]]str[[:space:]]+w[0-9]+, \[x[0-9]+\]$' ||
  fail "increment store is missing before the visible interval"

if printf '%s\n' "$DISASSEMBLY" |
    grep -Eq '[[:space:]](dmb|dsb|isb)([[:space:]]|$)'; then
  fail "hardware barrier instruction found"
fi

printf '%s\n' \
  "D4-R3k disassembly passed: increment=1 decrement=1 stores=2 intervening_calls=3 hardware_barriers=0"
