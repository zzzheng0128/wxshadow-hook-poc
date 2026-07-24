#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
HISTORICAL_B0=${RAW_OBSERVER_HISTORICAL_BASELINE_LOG:-"$ROOT/build/evidence/raw-hold-lifetime-matrix-20260724-071931.log"}
HISTORICAL_B1=${RAW_OBSERVER_HISTORICAL_EXTERNAL_LOG:-"$ROOT/build/evidence/raw-live-pte-snapshot-20260724-080322.log"}

fail() {
  printf '%s\n' "raw observer aggregate failure: $*" >&2
  exit 1
}

require_file() {
  path=$1
  [ -f "$path" ] || fail "evidence file is missing: $path"
}

count_text() {
  path=$1
  text=$2
  grep -F -c -- "$text" "$path" 2>/dev/null || true
}

require_text() {
  path=$1
  text=$2
  grep -F -- "$text" "$path" >/dev/null ||
    fail "required evidence is missing from $path: $text"
}

require_once() {
  path=$1
  text=$2
  count=$(count_text "$path" "$text")
  [ "$count" -eq 1 ] ||
    fail "expected one evidence row in $path, found $count: $text"
}

reject_text() {
  path=$1
  text=$2
  if grep -F -- "$text" "$path" >/dev/null; then
    fail "rejected evidence is present in $path: $text"
  fi
}

require_text_before() {
  path=$1
  first=$2
  second=$3
  first_line=$(
    grep -n -F -- "$first" "$path" | sed -n '1s/:.*//p'
  )
  second_line=$(
    grep -n -F -- "$second" "$path" | sed -n '1s/:.*//p'
  )
  [ -n "$first_line" ] || fail "ordered evidence is missing: $first"
  [ -n "$second_line" ] || fail "ordered evidence is missing: $second"
  [ "$first_line" -lt "$second_line" ] ||
    fail "evidence order is invalid in $path: $first before $second"
}

validate_historical_b0() {
  path=$1
  require_file "$path"
  require_once "$path" \
    'raw mode=raw-hold-lifetime failures=0 exit_mmap_armed=0 target_state=source_uxn slots=1 raw_slots=1 page_records=1'
  require_once "$path" \
    'classification=D4-R3e-L1-single-source-uxn-unstable'
  require_once "$path" \
    'phase=d4_r3e_l1_source_single_preserve active_hold=1 cleanup=not_run'
  require_text "$path" \
    'post_hold_status=not_used boot_id_reader=not_used proc_maps=not_used cleanup=preserve'
}

validate_historical_b1() {
  path=$1
  stable='classification=D4-R3e-L2-live-pte-stable'
  blocked='classification=D4-R3e-L2-setup-or-cleanup-blocked'

  require_file "$path"
  require_once "$path" \
    'raw mode=raw-hold-live-pte failures=0 exit_mmap_armed=0 target_state=source_uxn slots=1 raw_slots=1 page_records=1'
  require_once "$path" \
    'snapshot_stage=after_arm walk_rc=0'
  require_once "$path" \
    'live_match=1 live_pfn='
  require_once "$path" \
    'live_state=source_uxn stored_state=source_uxn'
  require_once "$path" \
    'pte_lock=held mmap_lock=read'
  require_once "$path" \
    'record_backend=raw_two_pfn record_state=source_uxn record_match=1'
  require_once "$path" "$stable"
  require_once "$path" "$blocked"
  require_text "$path" \
    'active_hold=1 cleanup=not_run reason=trap'
  require_text_before "$path" "$stable" "$blocked"
}

validate_new_log() {
  path=$1
  expected_run=$2
  expected_sample=$3
  expected_variant=$4
  expected_live_walk=$5
  expected_phase=$6
  stable_classification=$7
  unstable_classification=$8

  require_file "$path"
  reject_text "$path" 'D4-R3e-L3-setup-blocked'
  reject_text "$path" 'D4-R3e-L3-B1-live-pte-walk-failed'
  reject_text "$path" 'D4-R3e-L3-B1-live-pte-mismatch'
  require_once "$path" 'result=classified'
  require_text "$path" "paired_run=$expected_run"
  require_text "$path" "sample_index=$expected_sample"
  require_text "$path" "variant=$expected_variant"
  require_text "$path" "live_walk=$expected_live_walk"
  require_text "$path" 'source_tree=clean clean_boot_confirmed=1'
  require_once "$path" \
    "phase=${expected_phase}_established result=pass paired_run=$expected_run sample_index=$expected_sample variant=$expected_variant live_walk=$expected_live_walk target_state=source_uxn raw_slots=1 page_records=1"
  require_once "$path" \
    'active_hold=1 cleanup=not_run'
  require_text "$path" \
    'post_hold_status=not_used boot_id_reader=not_used proc_maps=not_used'

  if [ "$expected_variant" = baseline ]; then
    require_once "$path" \
      'raw mode=raw-hold-lifetime failures=0 exit_mmap_armed=0 target_state=source_uxn slots=1 raw_slots=1 page_records=1'
    reject_text "$path" 'raw mode=raw-hold-live-pte'
  else
    require_once "$path" \
      'raw mode=raw-hold-live-pte failures=0 exit_mmap_armed=0 target_state=source_uxn slots=1 raw_slots=1 page_records=1'
    require_once "$path" \
      'snapshot_stage=after_arm live_match=1'
    require_once "$path" \
      'walk_rc=0'
    require_once "$path" \
      'pte_lock=held mmap_lock=read'
    require_once "$path" \
      'record_backend=raw_two_pfn record_state=source_uxn record_match=1'
  fi

  terminal=$(
    grep -F -- 'result=classified' "$path"
  )
  case "$terminal" in
    *"classification=$stable_classification "*)
      printf '%s\n' stable
      ;;
    *"classification=$unstable_classification "*)
      printf '%s\n' unstable
      ;;
    *)
      fail "unexpected terminal classification in $path: $terminal"
      ;;
  esac
}

[ "$#" -eq 4 ] ||
  fail "usage: $0 <B0-S2.log> <B1-S2.log> <B0-S3.log> <B1-S3.log>"

validate_historical_b0 "$HISTORICAL_B0"
validate_historical_b1 "$HISTORICAL_B1"

b0_s2=$(validate_new_log "$1" 1 2 baseline 0 d4_r3e_l3_b0_s2 \
  D4-R3e-L3-B0-baseline-stable \
  D4-R3e-L3-B0-baseline-unstable)
b1_s2=$(validate_new_log "$2" 2 2 external 1 d4_r3e_l3_b1_s2 \
  D4-R3e-L3-B1-external-snapshot-stable \
  D4-R3e-L3-B1-external-snapshot-unstable)
b0_s3=$(validate_new_log "$3" 3 3 baseline 0 d4_r3e_l3_b0_s3 \
  D4-R3e-L3-B0-baseline-stable \
  D4-R3e-L3-B0-baseline-unstable)
b1_s3=$(validate_new_log "$4" 4 3 external 1 d4_r3e_l3_b1_s3 \
  D4-R3e-L3-B1-external-snapshot-stable \
  D4-R3e-L3-B1-external-snapshot-unstable)

if [ "$b0_s2" = unstable ] && [ "$b0_s3" = unstable ] &&
   [ "$b1_s2" = stable ] && [ "$b1_s3" = stable ]; then
  classification=D4-R3e-L3-observer-correlated
elif [ "$b0_s2" = stable ] && [ "$b1_s2" = stable ] &&
     [ "$b0_s3" = stable ] && [ "$b1_s3" = stable ]; then
  classification=D4-R3e-L3-environment-drift
elif [ "$b0_s2" = "$b1_s2" ] && [ "$b0_s3" = "$b1_s3" ]; then
  classification=D4-R3e-L3-observer-not-correlated
else
  classification=D4-R3e-L3-repeat-nondeterministic
fi

printf 'raw_observer_aggregate=pass classification=%s historical_b0=unstable historical_b1=stable b0=unstable,%s,%s b1=stable,%s,%s new_pairs=%s/%s,%s/%s inputs=4 majority_vote=not_used result=pass\n' \
  "$classification" "$b0_s2" "$b0_s3" "$b1_s2" "$b1_s3" \
  "$b0_s2" "$b1_s2" "$b0_s3" "$b1_s3"
