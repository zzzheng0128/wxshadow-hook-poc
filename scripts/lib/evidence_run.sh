#!/bin/sh

# Sourced by aggregate runners. This manages host-side evidence only.
evidence_run_init() {
  mkdir -p "$1" || return 1
  RUN_DIR=$(mktemp -d "$1/$2-$3.XXXXXX") || return 1
  MANIFEST="$RUN_DIR/manifest.log"
  EVIDENCE_RUN_COMPLETE=0
  EVIDENCE_RUN_PHASE=setup
  EVIDENCE_RUN_ABORT_REASON=runner_incomplete
  printf 'phase=setup result=incomplete\n' > "$MANIFEST" || return 1
  trap 'evidence_run_on_exit "$?"' EXIT
  trap 'EVIDENCE_RUN_ABORT_REASON=signal_HUP; exit 129' HUP
  trap 'EVIDENCE_RUN_ABORT_REASON=signal_INT; exit 130' INT
  trap 'EVIDENCE_RUN_ABORT_REASON=signal_TERM; exit 143' TERM
}

evidence_run_on_exit() {
  evidence_exit_status=$1
  trap - EXIT HUP INT TERM
  if [ "$EVIDENCE_RUN_COMPLETE" -ne 1 ] || [ "$evidence_exit_status" -ne 0 ]; then
    printf 'result=fail reason=%s phase=%s exit_status=%s\n' \
      "$EVIDENCE_RUN_ABORT_REASON" "$EVIDENCE_RUN_PHASE" "$evidence_exit_status" \
      >> "$MANIFEST" || evidence_exit_status=1
    if [ "$evidence_exit_status" -eq 0 ]; then
      evidence_exit_status=1
    fi
  fi
  exit "$evidence_exit_status"
}

evidence_run_complete() {
  printf 'warn_after=%s result=pass\n' "$1" >> "$MANIFEST" || return 1
  EVIDENCE_RUN_COMPLETE=1
}
