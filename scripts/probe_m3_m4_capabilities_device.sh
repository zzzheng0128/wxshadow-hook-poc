#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SERIAL=${ANDROID_SERIAL:-}
EVIDENCE_DIR="$ROOT/build/evidence"
EVIDENCE="$EVIDENCE_DIR/m3-m4-capabilities-$(date +%Y%m%d-%H%M%S).log"

adb_device() {
  if [ -n "$SERIAL" ]; then
    adb -s "$SERIAL" "$@"
  else
    adb "$@"
  fi
}

mkdir -p "$EVIDENCE_DIR"
{
  printf 'serial=%s\n' "${SERIAL:-default}"
  printf 'model=%s\n' "$(adb_device shell getprop ro.product.model | tr -d '\r')"
  printf 'android_release=%s\n' "$(adb_device shell getprop ro.build.version.release | tr -d '\r')"
  printf 'fingerprint=%s\n' "$(adb_device shell getprop ro.build.fingerprint | tr -d '\r')"
  printf 'cpu_abi=%s\n' "$(adb_device shell getprop ro.product.cpu.abi | tr -d '\r')"
  printf 'kernel=%s\n' "$(adb_device shell uname -a | tr -d '\r')"
  printf 'page_size=%s\n' "$(adb_device shell getconf PAGE_SIZE | tr -d '\r')"
  printf 'arm64_features=%s\n' "$(adb_device shell cat /proc/cpuinfo | tr -d '\r' | sed -n 's/^Features[[:space:]]*:[[:space:]]*//p' | sed -n '1p')"
  printf 'arm64_cpu_parts=%s\n' "$(adb_device shell cat /proc/cpuinfo | tr -d '\r' | sed -n 's/^CPU part[[:space:]]*:[[:space:]]*//p' | sort -u | paste -sd, -)"
  printf 'kernelpatch_supercall_abi=0x0d03\n'
  printf 'folkpatch_runtime_version=%s\n' "$(adb_device shell su -c '/system/bin/truncate su version' | tr -d '\r')"
  printf 'module_list=%s\n' "$(adb_device shell su -c '/system/bin/truncate su module list' | tr -d '\r')"
  printf '%s\n' 'symbols:'
  for symbol in \
    do_mem_abort \
    handle_mm_fault \
    find_vma \
    get_task_mm \
    mmput \
    vm_mmap \
    vm_munmap \
    ptep_set_access_flags \
    set_pte_at \
    flush_tlb_mm_range \
    flush_cache_range \
    copy_to_user_page \
    bpf_get_smp_processor_id \
    register_die_notifier \
    unregister_die_notifier
  do
    if adb_device shell su -c "/system/bin/grep -q -w $symbol /proc/kallsyms" >/dev/null 2>&1; then
      printf '%s=present\n' "$symbol"
    else
      printf '%s=absent\n' "$symbol"
    fi
  done
  printf 'result=read_only_probe\n'
} | tee "$EVIDENCE"
printf '%s\n' "$EVIDENCE"
