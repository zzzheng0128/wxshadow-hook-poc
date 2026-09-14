#!/bin/sh
# Generate the vendored kernel header trees under references/kernel-src/.
#
# The KPM raw-compat layer (kpm/r0lab_raw_compat.c) compiles against kernel
# headers that are ABI-bound to the target kernel version (struct mm_struct /
# pte_t / generated asm-offsets.h). The full kernel source is NOT needed --
# only include/ and arch/arm64/include/ (with their generated/ subdirs).
#
# This script downloads the pinned kernel source, runs `make prepare` to emit
# the generated headers, then vendors just the header subtrees into
# references/kernel-src/, so the build no longer depends on an external host.
#
# Targets:
#   oriole -> aosp-mirror/kernel_common   android14-6.1-lts      (6.1.99)
#   redfin -> aosp-mirror/kernel_msm      android-msm-redbull-4.19-android14
#
# Usage:
#   scripts/gen_kernel_headers.sh [oriole|redfin|all]
#
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUT_ROOT="$ROOT/references/kernel-src"
WORK_ROOT="${TMPDIR:-/tmp}/wxshadow-kernel-headers"

# Cross toolchain for `make prepare`. The prepare step only builds host-side
# scripts + generates headers; it needs an arm64 compiler for a few arch
# checks. Prefer an aarch64-linux-gnu- toolchain, fall back to NDK clang.
CROSS_COMPILE="${CROSS_COMPILE:-}"
if [ -z "$CROSS_COMPILE" ]; then
    if command -v aarch64-linux-gnu-gcc >/dev/null 2>&1; then
        CROSS_COMPILE="aarch64-linux-gnu-"
    else
        echo "error: no aarch64 cross compiler found (aarch64-linux-gnu-gcc)" >&2
        echo "       install it, or set CROSS_COMPILE=/path/to/prefix-" >&2
        exit 1
    fi
fi

gen_tree() {
    target="$1"
    case "$target" in
    oriole)
        repo="https://github.com/aosp-mirror/kernel_common.git"
        branch="android14-6.1-lts"
        outname="kernel-common-6.1"
        ;;
    redfin)
        repo="https://github.com/aosp-mirror/kernel_msm.git"
        branch="android-msm-redbull-4.19-android14"
        outname="kernel-msm-4.19"
        ;;
    *)
        echo "unknown target: $target (want oriole|redfin)" >&2
        return 1
        ;;
    esac

    work="$WORK_ROOT/$outname"
    out="$OUT_ROOT/$outname"

    echo "==> [$target] clone $branch from $repo"
    rm -rf "$work"
    # Single-branch shallow clone keeps this fast (~200M instead of ~2G).
    git clone --depth 1 --branch "$branch" "$repo" "$work"

    echo "==> [$target] configure + make prepare"
    cd "$work"
    # 4.19 (and some GKI trees) fail `make prepare` on TRIM_UNUSED_KSYMS without
    # a GKI abi_symbollist.raw whitelist; disable it.
    if [ -f .config ] || [ -x scripts/config ]; then
        scripts/config --disable TRIM_UNUSED_KSYMS 2>/dev/null || true
    fi
    # Non-interactive olddefconfig, then generate headers.
    ARCH=arm64 CROSS_COMPILE="$CROSS_COMPILE" \
        make O="$work" ARCH=arm64 olddefconfig >/dev/null 2>&1 || \
        ARCH=arm64 CROSS_COMPILE="$CROSS_COMPILE" \
        yes "" | make ARCH=arm64 olddefconfig >/dev/null 2>&1 || true
    ARCH=arm64 CROSS_COMPILE="$CROSS_COMPILE" \
        make ARCH=arm64 prepare scripts >/dev/null 2>&1 || {
        echo "error: make prepare failed for $target" >&2
        return 1
    }

    echo "==> [$target] vendor header subtrees into $out"
    rm -rf "$out"
    mkdir -p "$out"
    for sub in include arch/arm64/include; do
        if [ -d "$work/$sub" ]; then
            mkdir -p "$(dirname "$out/$sub")"
            cp -R "$work/$sub" "$out/$sub"
        fi
    done

    echo "==> [$target] done: $out"
}

case "${1:-all}" in
all)
    gen_tree oriole
    gen_tree redfin
    ;;
oriole | redfin)
    gen_tree "$1"
    ;;
*)
    echo "usage: $0 [oriole|redfin|all]" >&2
    exit 1
    ;;
esac

echo ""
echo "Header trees ready under $OUT_ROOT"
du -sh "$OUT_ROOT"/* 2>/dev/null || true
