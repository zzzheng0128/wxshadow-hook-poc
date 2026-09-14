#!/bin/sh

# The caller supplies ROOT and adb_device. Capture boot on both sides of the
# counter read so a reboot cannot silently create an inconsistent baseline.
r0lab_device_ready() {
  r0lab_device_state=$(adb_device get-state 2>/dev/null) || return 1
  [ "$r0lab_device_state" = device ]
}

r0lab_device_snapshot() {
  r0lab_boot_before_raw=$(adb_device shell cat /proc/sys/kernel/random/boot_id) || return 1
  r0lab_warn_raw=$(adb_device shell su -c cat /sys/kernel/warn_count) || return 1
  r0lab_boot_after_raw=$(adb_device shell cat /proc/sys/kernel/random/boot_id) || return 1
  R0LAB_SNAPSHOT_BOOT=$(printf '%s\n' "$r0lab_boot_before_raw" | awk '{ sub(/\r$/, ""); print }')
  R0LAB_SNAPSHOT_WARN=$(printf '%s\n' "$r0lab_warn_raw" | awk '{ sub(/\r$/, ""); print }')
  r0lab_boot_after=$(printf '%s\n' "$r0lab_boot_after_raw" | awk '{ sub(/\r$/, ""); print }')
  "$ROOT/scripts/verify_run_continuity.sh" \
    "$R0LAB_SNAPSHOT_BOOT" "$r0lab_boot_after" \
    "$R0LAB_SNAPSHOT_WARN" "$R0LAB_SNAPSHOT_WARN"
}
