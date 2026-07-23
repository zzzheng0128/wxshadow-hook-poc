#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SERIAL=${ANDROID_SERIAL:-}
EVIDENCE_DIR="$ROOT/build/evidence"
EVIDENCE="$EVIDENCE_DIR/shadow-page-capabilities-$(date +%Y%m%d-%H%M%S).log"
PACKAGE=dev.r0hook.lab
ACTIVITY=dev.r0hook.lab/.MainActivity

adb_device() {
  if [ -n "$SERIAL" ]; then
    adb -s "$SERIAL" "$@"
  else
    adb "$@"
  fi
}

if ! adb_device get-state >/dev/null 2>&1; then
  printf '%s\n' "shadow-page capability probe error: target serial is unavailable: ${SERIAL:-default}" >&2
  exit 2
fi

symbol_state() {
  symbol=$1
  if adb_device shell su -c "/system/bin/grep -q -w $symbol /proc/kallsyms" >/dev/null 2>&1; then
    printf 'present'
  else
    printf 'absent'
  fi
}

app_logs() {
  adb_device logcat -d -v brief -s R0Lab:I '*:S' | tr -d '\r'
}

run_app_command() {
  command=$1
  adb_device logcat -c >/dev/null
  adb_device shell "am start -W -n $ACTIVITY --es r0lab_command '$command'" >/dev/null
  sleep 1
  app_logs
}

mkdir -p "$EVIDENCE_DIR"
"$ROOT/scripts/build_lab_app.sh" >/dev/null
adb_device install -r "$ROOT/build/lab-app/r0lab-debug.apk" >/dev/null
FEATURES=$(adb_device shell "grep -m1 '^Features' /proc/cpuinfo" | tr -d '\r' |
  sed -n 's/^Features[[:space:]]*:[[:space:]]*//p')
case " $FEATURES " in
  *' epan '*) EPAN_CPUINFO=advertised ;;
  *) EPAN_CPUINFO=not_advertised ;;
esac
XOM_OUTPUT=$(run_app_command 'xom probe' || true)
case "$XOM_OUTPUT" in
  *'user_xom_read_fault=proven'*) USER_XOM_READ_FAULT=proven ;;
  *) USER_XOM_READ_FAULT=absent ;;
esac

{
  printf 'serial=%s\n' "${SERIAL:-default}"
  printf 'model=%s\n' "$(adb_device shell getprop ro.product.model | tr -d '\r')"
  printf 'kernel=%s\n' "$(adb_device shell uname -r | tr -d '\r')"
  printf 'page_size=%s\n' "$(adb_device shell getconf PAGE_SIZE | tr -d '\r')"
  printf 'arm64_features=%s\n' "$FEATURES"
  printf 'epan_cpuinfo=%s\n' "$EPAN_CPUINFO"
  printf 'user_xom_read_fault=%s\n' "$USER_XOM_READ_FAULT"
  printf '%s\n' "$XOM_OUTPUT"
  printf '%s\n' 'symbols:'
  for symbol in \
    do_mem_abort \
    pte_offset_map_lock \
    pte_unmap_unlock \
    ptep_set_access_flags \
    set_pte_at \
    flush_tlb_mm_range \
    flush_cache_range \
    follow_page_pte \
    follow_page_mask
  do
    printf '%s=%s\n' "$symbol" "$(symbol_state "$symbol")"
  done
  printf '%s\n' 'leaf_pte_abi=unverified'
  printf '%s\n' 'tlb_icache_abi=unverified'
  printf '%s\n' 'gup_view_switch=excluded'
  printf '%s\n' 'mapping_concealment=excluded'
  printf '%s\n' 'epan_route=separate_linux_xom_route_not_raw_pte_admission'
  printf '%s\n' 'raw_read_cycle_admission=blocked reason=kernel_read_fault_switch_not_implemented'
  printf '%s\n' 'raw_pte_route=blocked reason=run_probe_raw_pte_compat_device'
  printf '%s\n' 'result=blocked reason=leaf_pte_tlb_icache_and_fault_disposition_abi_unverified'
} | tee "$EVIDENCE"
printf '%s\n' "$EVIDENCE"
