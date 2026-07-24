#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
NDK=${ANDROID_NDK_HOME:-/Users/ivory/Library/Android/sdk/ndk/29.0.14206865}
OBJDUMP=${OBJDUMP:-$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-objdump}
OBJECT=${1:-$ROOT/kpm/build/r0lab.o}
SYMBOL=r0lab_raw_before_abort_iabt_route

fail() {
  printf '%s\n' "D4-R3l disassembly failure: $*" >&2
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

FRAME_LOADS=$(
  printf '%s\n' "$DISASSEMBLY" |
    grep -Ec '[[:space:]]ldr[[:space:]]+[xw][0-9]+, \[x0, #0x(58|60|68)\]$' ||
    true
)
ADD_COUNT=$(
  printf '%s\n' "$DISASSEMBLY" |
    grep -Ec '[[:space:]]add[[:space:]]+w[0-9]+, w[0-9]+, #0x1$' || true
)
SUB_COUNT=$(
  printf '%s\n' "$DISASSEMBLY" |
    grep -Ec '[[:space:]]sub[[:space:]]+w[0-9]+, w[0-9]+, #0x1$' || true
)
NONSTACK_STORES=$(
  printf '%s\n' "$DISASSEMBLY" |
    grep -E '[[:space:]]str[^[:space:]]*[[:space:]]' |
    grep -Evc '\[sp,' || true
)
INDIRECT_CALLS=$(
  printf '%s\n' "$DISASSEMBLY" |
    grep -Ec '[[:space:]]blr[[:space:]]+x[0-9]+$' || true
)

[ "$FRAME_LOADS" -eq 3 ] ||
  fail "expected FAR/ESR/regs frame loads, found $FRAME_LOADS"
[ "$ADD_COUNT" -eq 1 ] ||
  fail "expected one retained inflight increment, found $ADD_COUNT"
[ "$SUB_COUNT" -eq 1 ] ||
  fail "expected one retained inflight decrement, found $SUB_COUNT"
[ "$NONSTACK_STORES" -eq 2 ] ||
  fail "expected only two non-stack counter stores, found $NONSTACK_STORES"
[ "$INDIRECT_CALLS" -eq 7 ] ||
  fail "unexpected optimized indirect-call shape: $INDIRECT_CALLS"

printf '%s\n' "$DISASSEMBLY" |
  grep -Eq '[[:space:]]ldr[[:space:]]+x[0-9]+, \[x[0-9]+, #0xe0\]$' ||
  fail "token generation read is missing"
printf '%s\n' "$DISASSEMBLY" |
  grep -Eq '[[:space:]]ldr[[:space:]]+x[0-9]+, \[x[0-9]+, #0x100\]$' ||
  fail "pt_regs PC read is missing"
printf '%s\n' "$DISASSEMBLY" |
  grep -Eq '[[:space:]]ldrb[[:space:]]+w[0-9]+, \[x[0-9]+, #0xf7\]$' ||
  fail "immutable IABT-route mode read is missing"

if printf '%s\n' "$DISASSEMBLY" |
    grep -Eq '[[:space:]](dmb|dsb|isb|dc|ic)([[:space:]]|$)'; then
  fail "hardware barrier or cache-maintenance instruction found"
fi

printf '%s\n' \
  "D4-R3l disassembly passed: frame_loads=3 increment=1 decrement=1 nonstack_stores=2 indirect_calls=7 hardware_barriers=0"
