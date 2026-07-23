#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
make -C "$ROOT/kpm" clean
make -C "$ROOT/kpm"

READELF=/Users/ivory/Library/Android/sdk/ndk/29.0.14206865/toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-readelf
"$READELF" -h "$ROOT/kpm/build/r0lab-m1.kpm"
"$READELF" -S "$ROOT/kpm/build/r0lab-m1.kpm" | grep -E '\.kpm\.(info|init|ctl0|exit)'
