#!/bin/sh
set -eu

# Read-only validation of the existing whitespace-separated evidence protocol.
# Earlier rows may contain expected failures; only the last nonblank row is the
# terminal summary. A summary must have exactly one standalone result=pass.
if [ "$#" -ne 1 ]; then
  printf 'usage: %s EVIDENCE_LOG\n' "$0" >&2
  exit 2
fi
if [ ! -f "$1" ] || [ ! -r "$1" ]; then
  printf 'evidence result failure: not a readable regular file: %s\n' "$1" >&2
  exit 1
fi

LC_ALL=C awk '
  function reject(reason) {
    printf "evidence result failure: line %d: %s\n", summary_line, reason
    exit 1
  }
  {
    sub(/\r$/, "")
    if ($0 ~ /[^[:space:]]/) {
      summary = $0
      summary_line = NR
    }
  }
  END {
    if (!summary_line)
      reject("missing terminal summary")

    $0 = summary
    for (i = 1; i <= NF; i++) {
      token = $i
      if (token ~ /^result=/) {
        result_count++
        if (token != "result=pass")
          invalid_result = 1
      }
      if (token ~ /^(blocked|skip|classified|fail)$/)
        veto = token
      if (token !~ /^[A-Za-z_][A-Za-z0-9_]*=/)
        continue

      separator = index(token, "=")
      key = substr(token, 1, separator - 1)
      value = substr(token, separator + 1)
      # Expected outcomes describe a test input, not its final verdict.
      if (key == "expected" || key ~ /^expected_/)
        continue
      if ((key == "status" && value != "pass") ||
          value ~ /^(blocked|skip|classified|fail)$/)
        veto = token
    }
    if (result_count != 1)
      reject("expected exactly one standalone result field")
    if (invalid_result)
      reject("terminal result is not exactly pass")
    if (veto != "")
      reject("terminal summary contains a non-pass verdict: " veto)
  }
' < "$1" >&2
