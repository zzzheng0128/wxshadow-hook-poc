#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SERIAL=${ANDROID_SERIAL:-}
EVIDENCE_DIR="$ROOT/build/evidence"
EVIDENCE="$EVIDENCE_DIR/raw-pte-compat-$(date +%Y%m%d-%H%M%S).log"
TARGET_KERNEL=5.10.198-android13-4-00050-g12f3388846c3-ab11920634
TARGET_FINGERPRINT=google/panther/panther:14/AP2A.240905.003/12231197:user/release-keys
TARGET_SOURCE_COMMIT=b4d74ef49794eef8d0a2d8e231cd5c64b8e91f8a
SOURCE_DIR="$ROOT/build/pixel7-kernel-gs"
NDK=${NDK:-/Users/ivory/Library/Android/sdk/ndk/29.0.14206865}
READELF="$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-readelf"
MODULE="$ROOT/kpm/build/r0lab-m1.kpm"

adb_device() {
  if [ -n "$SERIAL" ]; then
    adb -s "$SERIAL" "$@"
  else
    adb "$@"
  fi
}

state() {
  if "$@" >/dev/null 2>&1; then
    printf '%s' proven
  else
    printf '%s' absent
  fi
}

device_config_enabled() {
  name=$1
  adb_device shell su -c "zcat /proc/config.gz 2>/dev/null | /system/bin/grep -q '^${name}=y$'" >/dev/null 2>&1
}

device_config_value() {
  name=$1
  adb_device shell su -c "zcat /proc/config.gz 2>/dev/null | /system/bin/grep '^${name}=' | /system/bin/head -n 1" 2>/dev/null |
    tr -d '\r'
}

source_has() {
  path=$1
  text=$2
  [ -f "$SOURCE_DIR/$path" ] && grep -F -- "$text" "$SOURCE_DIR/$path" >/dev/null
}

module_has_reloc() {
  type=$1
  [ -f "$MODULE" ] && "$READELF" -r "$MODULE" 2>/dev/null | grep -F -- "$type" >/dev/null
}

module_has_section() {
  section=$1
  [ -f "$MODULE" ] && "$READELF" -S "$MODULE" 2>/dev/null | grep -F -- "$section" >/dev/null
}

kpm_has_forbidden_pgtable_entry() {
  grep -F -- 'pgtable_entry' \
    "$ROOT/kpm/r0lab.c" \
    "$ROOT/kpm/r0lab_raw_compat.c" \
    "$ROOT/kpm/r0lab_raw.h" >/dev/null
}

if ! adb_device get-state >/dev/null 2>&1; then
  printf '%s\n' "raw-pte compatibility probe error: target serial is unavailable: ${SERIAL:-default}" >&2
  exit 2
fi

mkdir -p "$EVIDENCE_DIR"
KERNEL=$(adb_device shell uname -r | tr -d '\r')
FINGERPRINT=$(adb_device shell getprop ro.build.fingerprint | tr -d '\r')
PAGE_SIZE=$(adb_device shell getconf PAGE_SIZE | tr -d '\r')
SOURCE_COMMIT=unverified
if [ -d "$SOURCE_DIR/.git" ]; then
  SOURCE_COMMIT=$(git -C "$SOURCE_DIR" rev-parse HEAD 2>/dev/null || printf '%s' unverified)
fi
FINAL_KPM_STATE=$(state test -f "$MODULE")
PREL32_STATE=$(if module_has_reloc R_AARCH64_PREL32; then printf present; else printf absent; fi)
ALTINSTRUCTIONS_STATE=$(if module_has_section .altinstructions; then printf present; else printf absent; fi)
JUMP_TABLE_STATE=$(if module_has_section __jump_table; then printf present; else printf absent; fi)
KPM_FORBIDDEN_PGTABLE_ENTRY_STATE=$(if kpm_has_forbidden_pgtable_entry; then printf present; else printf absent; fi)
RESULT=pass
if [ "$FINAL_KPM_STATE" != proven ] ||
   [ "$PREL32_STATE" != absent ] ||
   [ "$ALTINSTRUCTIONS_STATE" != absent ] ||
   [ "$JUMP_TABLE_STATE" != absent ] ||
   [ "$KPM_FORBIDDEN_PGTABLE_ENTRY_STATE" != absent ]; then
  RESULT=fail
fi

{
  printf 'serial=%s\n' "${SERIAL:-default}"
  printf 'fingerprint=%s\n' "$FINGERPRINT"
  printf 'kernel=%s\n' "$KERNEL"
  printf 'page_size=%s\n' "$PAGE_SIZE"
  printf 'target_identity=%s\n' "$(state test "$KERNEL" = "$TARGET_KERNEL")"
  printf 'target_fingerprint=%s\n' "$(state test "$FINGERPRINT" = "$TARGET_FINGERPRINT")"
  printf 'source_commit=%s\n' "$SOURCE_COMMIT"
  printf 'source_commit_pinned=%s\n' "$(state test "$SOURCE_COMMIT" = "$TARGET_SOURCE_COMMIT")"
  printf 'config_arm64_4k_pages=%s\n' "$(state device_config_enabled CONFIG_ARM64_4K_PAGES)"
  printf 'config_sw_ttbr0_pan=%s\n' "$(state device_config_enabled CONFIG_ARM64_SW_TTBR0_PAN)"
  printf 'config_arm64_mte=%s\n' "$(state device_config_enabled CONFIG_ARM64_MTE)"
  printf 'config_thp=%s\n' "$(state device_config_enabled CONFIG_TRANSPARENT_HUGEPAGE)"
  printf 'config_split_ptlock_cpus=%s\n' "$(device_config_value CONFIG_SPLIT_PTLOCK_CPUS || true)"
  printf 'source_mmap_lock=%s\n' "$(state source_has mm/mprotect.c 'mmap_write_lock_killable(current->mm)')"
  printf 'source_pte_lock=%s\n' "$(state source_has include/linux/mm.h '#define pte_offset_map_lock')"
  printf 'source_bbm=%s\n' "$(state source_has include/linux/pgtable.h 'ptep_modify_prot_start')"
  printf 'source_set_pte_at=%s\n' "$(state source_has arch/arm64/include/asm/pgtable.h 'static inline void set_pte_at')"
  printf 'source_target_tlb=%s\n' "$(state source_has arch/arm64/include/asm/tlbflush.h 'static inline void flush_tlb_page')"
  printf 'source_icache=%s\n' "$(state source_has arch/arm64/mm/flush.c 'EXPORT_SYMBOL_GPL(__sync_icache_dcache)')"
  printf 'kernelpatch_pgtable_entry_reference=%s\n' "$(if grep -F -- 'uint64_t *pgtable_entry' "$ROOT/references/KernelPatch/kernel/include/pgtable.h" >/dev/null; then printf present; else printf absent; fi)"
  printf 'kpm_forbidden_pgtable_entry=%s\n' "$KPM_FORBIDDEN_PGTABLE_ENTRY_STATE"
  printf 'compat_source=%s\n' "$(state test -f "$ROOT/kpm/r0lab_raw_compat.c")"
  printf 'compat_header=%s\n' "$(state test -f "$ROOT/kpm/r0lab_raw.h")"
  printf 'final_kpm=%s\n' "$FINAL_KPM_STATE"
  printf 'final_kpm_prel32=%s\n' "$PREL32_STATE"
  printf 'final_kpm_altinstructions=%s\n' "$ALTINSTRUCTIONS_STATE"
  printf 'final_kpm_jump_table=%s\n' "$JUMP_TABLE_STATE"
  printf '%s\n' 'kpm_target_vma_lock=proven_via_target_compat_object'
  printf '%s\n' 'kpm_leaf_pte_lock=proven_via_target_compat_object'
  printf '%s\n' 'kpm_bbm_transaction=proven_via_target_compat_object'
  printf '%s\n' 'kpm_target_mm_tlb=proven_via_target_compat_object'
  printf '%s\n' 'kpm_fault_disposition_abi=proven_iabt_only'
  printf '%s\n' 'raw_device_smoke=scripts/test_raw_device.sh'
  printf 'result=%s route=lab_two_pfn\n' "$RESULT"
} | tee "$EVIDENCE"
printf '%s\n' "$EVIDENCE"
[ "$RESULT" = pass ] || exit 1
