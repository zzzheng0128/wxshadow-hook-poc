#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
NDK=${ANDROID_NDK_HOME:-/Users/ivory/Library/Android/sdk/ndk/29.0.14206865}
OBJDUMP=${OBJDUMP:-$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-objdump}
OBJECT=${1:-$ROOT/kpm/build/r0lab.o}
SYMBOL=r0lab_raw_before_abort_iabt_transition

fail() {
  printf '%s\n' "D4-R3m disassembly failure: $*" >&2
  exit 1
}

[ -x "$OBJDUMP" ] || fail "llvm-objdump is unavailable: $OBJDUMP"
[ -f "$OBJECT" ] || fail "KPM object is unavailable: $OBJECT"

DISASSEMBLY=$(
  "$OBJDUMP" -dr --disassemble-symbols="$SYMBOL" "$OBJECT"
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
[ "$ADD_COUNT" -eq 2 ] ||
  fail "expected inflight and activation increments, found $ADD_COUNT"
[ "$SUB_COUNT" -eq 1 ] ||
  fail "expected one retained inflight decrement, found $SUB_COUNT"
[ "$NONSTACK_STORES" -eq 10 ] ||
  fail "unexpected optimized non-stack store shape: $NONSTACK_STORES"
[ "$INDIRECT_CALLS" -eq 10 ] ||
  fail "unexpected optimized indirect-call shape: $INDIRECT_CALLS"

[ "$(
  printf '%s\n' "$DISASSEMBLY" |
    grep -Ec 'R_AARCH64_CALL26[[:space:]]+r0lab_raw_activate_shadow$' || true
)" -eq 1 ] ||
  fail "expected one activate-shadow relocation"
[ "$(
  printf '%s\n' "$DISASSEMBLY" |
    grep -Ec 'R_AARCH64_CALL26[[:space:]]+r0lab_raw_finish_read_cycle$' || true
)" -eq 1 ] ||
  fail "expected one finish-read-cycle relocation"
[ "$(
  printf '%s\n' "$DISASSEMBLY" |
    grep -Ec 'strb[[:space:]]+w[0-9]+, \[x[0-9]+, #0xfb\]$' || true
)" -eq 1 ] ||
  fail "expected one transitioning=true store"
[ "$(
  printf '%s\n' "$DISASSEMBLY" |
    grep -Ec 'strb[[:space:]]+wzr, \[x[0-9]+, #0xfb\]$' || true
)" -eq 1 ] ||
  fail "expected one generation-matched transitioning clear"
[ "$(
  printf '%s\n' "$DISASSEMBLY" |
    grep -Ec 'str[[:space:]]+w[0-9]+, \[x19, #0x8\]$' || true
)" -eq 1 ] ||
  fail "expected one skip_origin=1 store"
[ "$(
  printf '%s\n' "$DISASSEMBLY" |
    grep -Ec 'str[[:space:]]+xzr, \[x19, #0x50\]$' || true
)" -eq 1 ] ||
  fail "expected one ret=0 store"

if printf '%s\n' "$DISASSEMBLY" |
    grep -Eq 'R_AARCH64_CALL26[[:space:]]+r0lab_raw_(begin_fault_read_cycle|restore_original)'; then
  fail "forbidden DABT transition relocation found"
fi
if printf '%s\n' "$DISASSEMBLY" |
    grep -Eq 'R_AARCH64_CALL26[[:space:]]+r0lab_record'; then
  fail "global event-record relocation found"
fi
if printf '%s\n' "$DISASSEMBLY" |
    grep -Eq '[[:space:]](dmb|dsb|isb|dc|ic|tlbi)([[:space:]]|$)'; then
  fail "explicit hardware cache/TLB instruction found"
fi

printf '%s\n' \
  "D4-R3m disassembly passed: frame_loads=3 increments=2 decrement=1 nonstack_stores=10 indirect_calls=10 activate_calls=1 finish_calls=1 transition_stores=2 skip_origin_stores=1 ret_zero_stores=1 hardware_cache_tlb=0"
