#!/bin/sh
set -eu

# Validate supplied snapshots only. This script never reads a device or a log.
if [ "$#" -ne 4 ]; then
  printf 'usage: %s BASE_BOOT CURRENT_BOOT BASE_WARN CURRENT_WARN\n' "$0" >&2
  exit 2
fi

LC_ALL=C
export LC_ALL

reject() {
  printf 'run continuity failure: %s\n' "$1" >&2
  exit 1
}

validate_boot() {
  case "$2" in
    ????????-????-????-????-????????????) ;;
    *) reject "invalid $1: expected a UUID" ;;
  esac
  case "$2" in
    *[!0123456789abcdefABCDEF-]*) reject "invalid $1: expected a UUID" ;;
  esac
  boot_hex=$(printf '%s' "$2" | tr -d '-')
  # The shape above fixes four separators; reject extra hyphens in hex groups.
  [ "${#boot_hex}" -eq 32 ] || reject "invalid $1: expected a UUID"
  validated_boot=$(printf '%s' "$2" | tr 'ABCDEF' 'abcdef')
}

validate_warn() {
  case "$2" in
    ''|*[!0123456789]*) reject "invalid $1: expected a nonempty decimal count" ;;
  esac
  # Strip leading zeroes using strings, including counts larger than uint64.
  validated_warn=${2#"${2%%[!0]*}"}
  validated_warn=${validated_warn:-0}
}

validate_boot BASE_BOOT "$1"
base_boot=$validated_boot
validate_boot CURRENT_BOOT "$2"
current_boot=$validated_boot
validate_warn BASE_WARN "$3"
base_warn=$validated_warn
validate_warn CURRENT_WARN "$4"
current_warn=$validated_warn

[ "$base_boot" = "$current_boot" ] || reject 'boot ID changed since baseline'
[ "$base_warn" = "$current_warn" ] || reject 'warning count changed since baseline'
