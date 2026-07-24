#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CLASSIFIER="$ROOT/scripts/classify_raw_observer_perturbation_evidence.sh"
TMP_DIR=$(mktemp -d "${TMPDIR:-/tmp}/r0lab-observer-aggregate.XXXXXX")

cleanup() {
  rm -rf "$TMP_DIR"
}

fail() {
  printf '%s\n' "raw observer aggregate host test failure: $*" >&2
  exit 1
}

require_contains() {
  haystack=$1
  needle=$2
  printf '%s\n' "$haystack" | LC_ALL=C grep -F -- "$needle" >/dev/null ||
    fail "expected output not found: $needle"
}

write_historical_logs() {
  cat > "$TMP_DIR/historical-b0.log" <<'EOF'
raw mode=raw-hold-lifetime failures=0 exit_mmap_armed=0 target_state=source_uxn slots=1 raw_slots=1 page_records=1
phase=d4_r3e_l1_source_single_idle evidence=begin idle_seconds=15 post_hold_status=not_used boot_id_reader=not_used proc_maps=not_used cleanup=preserve
phase=d4_r3e_l1_source_single_idle result=fail classification=D4-R3e-L1-single-source-uxn-unstable
phase=d4_r3e_l1_source_single_preserve active_hold=1 cleanup=not_run
EOF
  cat > "$TMP_DIR/historical-b1.log" <<'EOF'
raw mode=raw-hold-live-pte failures=0 exit_mmap_armed=0 target_state=source_uxn slots=1 raw_slots=1 page_records=1
raw_slot_live_pte snapshot_stage=after_arm walk_rc=0 live_match=1 live_pfn=123 expected_pfn=123 live_state=source_uxn stored_state=source_uxn pte_lock=held mmap_lock=read record_backend=raw_two_pfn record_state=source_uxn record_match=1
phase=d4_r3e_l2_idle result=pass classification=D4-R3e-L2-live-pte-stable idle_seconds=15
classification=D4-R3e-L2-setup-or-cleanup-blocked result=classified active_hold=1 cleanup=not_run reason=trap
EOF
}

row_metadata() {
  run=$1
  case "$run" in
    1)
      sample=2
      variant=baseline
      live_walk=0
      phase=d4_r3e_l3_b0_s2
      stable=D4-R3e-L3-B0-baseline-stable
      unstable=D4-R3e-L3-B0-baseline-unstable
      ;;
    2)
      sample=2
      variant=external
      live_walk=1
      phase=d4_r3e_l3_b1_s2
      stable=D4-R3e-L3-B1-external-snapshot-stable
      unstable=D4-R3e-L3-B1-external-snapshot-unstable
      ;;
    3)
      sample=3
      variant=baseline
      live_walk=0
      phase=d4_r3e_l3_b0_s3
      stable=D4-R3e-L3-B0-baseline-stable
      unstable=D4-R3e-L3-B0-baseline-unstable
      ;;
    4)
      sample=3
      variant=external
      live_walk=1
      phase=d4_r3e_l3_b1_s3
      stable=D4-R3e-L3-B1-external-snapshot-stable
      unstable=D4-R3e-L3-B1-external-snapshot-unstable
      ;;
    *)
      fail "invalid synthetic run: $run"
      ;;
  esac
}

write_new_log() {
  path=$1
  run=$2
  state=$3
  row_metadata "$run"

  case "$state" in
    stable) classification=$stable ;;
    unstable) classification=$unstable ;;
    *) fail "invalid synthetic state: $state" ;;
  esac

  {
    printf 'paired_run=%s sample_index=%s variant=%s live_walk=%s source_tree=clean clean_boot_confirmed=1\n' \
      "$run" "$sample" "$variant" "$live_walk"
    if [ "$variant" = baseline ]; then
      printf '%s\n' \
        'raw mode=raw-hold-lifetime failures=0 exit_mmap_armed=0 target_state=source_uxn slots=1 raw_slots=1 page_records=1'
    else
      printf '%s\n' \
        'raw mode=raw-hold-live-pte failures=0 exit_mmap_armed=0 target_state=source_uxn slots=1 raw_slots=1 page_records=1 snapshot_stage=after_arm live_match=1 walk_rc=0 pte_lock=held mmap_lock=read record_backend=raw_two_pfn record_state=source_uxn record_match=1'
    fi
    printf 'phase=%s_established result=pass paired_run=%s sample_index=%s variant=%s live_walk=%s target_state=source_uxn raw_slots=1 page_records=1 post_hold_status=not_used boot_id_reader=not_used proc_maps=not_used\n' \
      "$phase" "$run" "$sample" "$variant" "$live_walk"
    printf 'classification=%s result=classified paired_run=%s sample_index=%s variant=%s live_walk=%s active_hold=1 cleanup=not_run reason=synthetic post_hold_status=not_used boot_id_reader=not_used proc_maps=not_used\n' \
      "$classification" "$run" "$sample" "$variant" "$live_walk"
  } > "$path"
}

run_valid_case() {
  name=$1
  expected=$2
  state1=$3
  state2=$4
  state3=$5
  state4=$6
  case_dir="$TMP_DIR/$name"
  mkdir -p "$case_dir"
  write_new_log "$case_dir/run1.log" 1 "$state1"
  write_new_log "$case_dir/run2.log" 2 "$state2"
  write_new_log "$case_dir/run3.log" 3 "$state3"
  write_new_log "$case_dir/run4.log" 4 "$state4"
  output=$(
    RAW_OBSERVER_HISTORICAL_BASELINE_LOG="$TMP_DIR/historical-b0.log" \
    RAW_OBSERVER_HISTORICAL_EXTERNAL_LOG="$TMP_DIR/historical-b1.log" \
      "$CLASSIFIER" \
      "$case_dir/run1.log" "$case_dir/run2.log" \
      "$case_dir/run3.log" "$case_dir/run4.log"
  )
  require_contains "$output" "classification=$expected"
  require_contains "$output" 'majority_vote=not_used'
  printf 'case=%s result=pass classification=%s\n' "$name" "$expected"
}

run_rejected_case() {
  name=$1
  expected=$2
  shift 2
  set +e
  output=$(
    RAW_OBSERVER_HISTORICAL_BASELINE_LOG="$TMP_DIR/historical-b0.log" \
    RAW_OBSERVER_HISTORICAL_EXTERNAL_LOG="$TMP_DIR/historical-b1.log" \
      "$CLASSIFIER" "$@" 2>&1
  )
  rc=$?
  set -e
  [ "$rc" -ne 0 ] || fail "$name unexpectedly passed"
  require_contains "$output" "$expected"
  printf 'case=%s result=pass rejected=1\n' "$name"
}

trap cleanup EXIT INT TERM
write_historical_logs

run_valid_case correlated D4-R3e-L3-observer-correlated \
  unstable stable unstable stable
run_valid_case environment-drift D4-R3e-L3-environment-drift \
  stable stable stable stable
run_valid_case observer-not-correlated D4-R3e-L3-observer-not-correlated \
  unstable unstable unstable unstable
run_valid_case nondeterministic D4-R3e-L3-repeat-nondeterministic \
  unstable stable stable stable

reject_dir="$TMP_DIR/rejected"
mkdir -p "$reject_dir"
write_new_log "$reject_dir/run1.log" 1 unstable
write_new_log "$reject_dir/run2.log" 2 stable
write_new_log "$reject_dir/run3.log" 3 unstable
write_new_log "$reject_dir/run4.log" 4 stable

run_rejected_case malformed-order 'paired_run=1' \
  "$reject_dir/run2.log" "$reject_dir/run1.log" \
  "$reject_dir/run3.log" "$reject_dir/run4.log"

printf '%s\n' \
  'classification=D4-R3e-L3-B0-baseline-unstable result=classified paired_run=1 sample_index=2 variant=baseline live_walk=0 active_hold=1 cleanup=not_run' \
  >> "$reject_dir/run1.log"
run_rejected_case duplicate-terminal 'expected one evidence row' \
  "$reject_dir/run1.log" "$reject_dir/run2.log" \
  "$reject_dir/run3.log" "$reject_dir/run4.log"

write_new_log "$reject_dir/run1.log" 1 unstable
printf '%s\n' \
  'classification=D4-R3e-L3-setup-blocked result=classified' \
  >> "$reject_dir/run1.log"
run_rejected_case non-sample 'rejected evidence is present' \
  "$reject_dir/run1.log" "$reject_dir/run2.log" \
  "$reject_dir/run3.log" "$reject_dir/run4.log"

real_historical=skipped
if [ -f "$ROOT/build/evidence/raw-hold-lifetime-matrix-20260724-071931.log" ] &&
   [ -f "$ROOT/build/evidence/raw-live-pte-snapshot-20260724-080322.log" ]; then
  output=$(
    "$CLASSIFIER" \
      "$reject_dir/run1.log" "$reject_dir/run2.log" \
      "$reject_dir/run3.log" "$reject_dir/run4.log" 2>&1 || true
  )
  case "$output" in
    *'D4-R3e-L3-setup-blocked'*) real_historical=anchors-valid ;;
    *) fail "real historical anchors did not reach new-log rejection" ;;
  esac
fi

printf 'raw_observer_aggregate_host=pass valid_cases=4 rejected_cases=3 real_historical=%s result=pass\n' \
  "$real_historical"
