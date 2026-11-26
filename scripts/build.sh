#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Build the host test binary, the static AArch64 helper and the single-file
# terminal script dist/amethyst-unixnu.sh.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"
VERSION=$(sed -n 's/^#define TOOL_VERSION "\(.*\)"/\1/p' src/common.h)
[ -n "$VERSION" ] || { echo "cannot read TOOL_VERSION" >&2; exit 1; }

CC=${CC:-cc}
HOST_CFLAGS=${HOST_CFLAGS:-"-DUNIXNU_TESTING -O2 -Wall -Wextra -Werror -Isrc"}
TARGET_CFLAGS=${TARGET_CFLAGS:-"-O2 -Wall -Wextra -Werror -Isrc"}

SOURCES="src/main.c src/selftest.c src/util.c src/gpt.c src/f2fs.c \
         src/report.c src/device.c src/state.c src/plan.c src/apply.c"

find_clang() {
    if [ -n "${CLANG:-}" ]; then
        printf '%s\n' "$CLANG"
        return
    fi
    for c in "$HOME"/AndroidBuilds/*/prebuilts/clang/host/linux-x86/clang-r547379/bin/clang \
             "$HOME"/AndroidBuilds/*/prebuilts/clang/host/linux-x86/clang-*/bin/clang; do
        [ -x "$c" ] && { printf '%s\n' "$c"; return; }
    done
    command -v clang 2>/dev/null || true
}

find_sysroot() {
    if [ -n "${SYSROOT:-}" ]; then
        printf '%s\n' "$SYSROOT"
        return
    fi
    for s in "$HOME"/AndroidBuilds/*/prebuilts/build-tools/sysroots/aarch64-unknown-linux-musl \
             "$HOME"/AndroidBuilds/*/prebuilts/build-tools/sysroots/aarch64-linux-musl; do
        [ -d "$s" ] && { printf '%s\n' "$s"; return; }
    done
    true
}

TARGET_CLANG=$(find_clang)
MUSL_SYSROOT=$(find_sysroot)
if [ -z "$TARGET_CLANG" ] || [ -z "$MUSL_SYSROOT" ]; then
    cat >&2 <<'EOF'
error: an Android clang and an AArch64 musl sysroot are required.

Set them explicitly, for example:
  CLANG=/path/to/prebuilts/clang/host/linux-x86/clang-r547379/bin/clang \
  SYSROOT=/path/to/prebuilts/build-tools/sysroots/aarch64-unknown-linux-musl \
  ./scripts/build.sh
EOF
    exit 1
fi

mkdir -p build out dist
rm -f dist/*.zip dist/SHA256SUMS

echo "== host test binary (UNIXNU_TESTING) =="
# shellcheck disable=SC2086
$CC $HOST_CFLAGS $SOURCES -o build/partitioner-host
build/partitioner-host selftest

echo "== static AArch64 helper =="
# shellcheck disable=SC2086
"$TARGET_CLANG" --target=aarch64-unknown-linux-musl --sysroot="$MUSL_SYSROOT" \
    -rtlib=compiler-rt -static -s $TARGET_CFLAGS $SOURCES -o out/partitioner

if command -v file >/dev/null 2>&1; then
    file out/partitioner
fi
if command -v readelf >/dev/null 2>&1; then
    if readelf -l out/partitioner | grep -q INTERP; then
        echo "error: helper is dynamically linked" >&2
        exit 1
    fi
fi
MACHINE=$(od -An -tx1 -j18 -N2 out/partitioner | tr -d ' \n')
[ "$MACHINE" = "b700" ] || { echo "error: helper is not AArch64" >&2; exit 1; }
[ "$(od -An -tu1 -j4 -N1 out/partitioner | tr -d ' \n')" = "2" ] || {
    echo "error: helper is not ELF64" >&2; exit 1;
}

if [ -n "${QEMU_AARCH64:-}" ]; then
    echo "== helper selftest under qemu =="
    "$QEMU_AARCH64" out/partitioner selftest
fi

HELPER_SHA256=$(sha256sum out/partitioner | cut -d' ' -f1)
[ -n "$HELPER_SHA256" ] || { echo "error: cannot hash helper" >&2; exit 1; }

echo "== terminal script =="
rm -f dist/amethyst-unixnu.sh
HELPER=out/partitioner OUT=dist/amethyst-unixnu.sh sh scripts/make-terminal-script.sh
echo
echo "== artifacts =="
cat dist/SHA256SUMS
echo "helper: out/partitioner ($HELPER_SHA256)"
