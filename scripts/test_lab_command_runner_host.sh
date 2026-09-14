#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
WORK=$(mktemp -d "${TMPDIR:-/tmp}/r0lab-command-test.XXXXXX")
trap 'rm -rf "$WORK"' EXIT HUP INT TERM

javac --release 8 -d "$WORK" \
  "$ROOT/lab-app/src/main/java/dev/r0hook/lab/LabCommandRunner.java" \
  "$ROOT/lab-app/src/test/java/dev/r0hook/lab/LabCommandRunnerTest.java"
java -cp "$WORK" dev.r0hook.lab.LabCommandRunnerTest
