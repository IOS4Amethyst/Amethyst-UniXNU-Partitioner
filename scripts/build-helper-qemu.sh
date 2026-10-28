#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Build the static AArch64 helper without the Android clang / musl sysroot.
#
# This is the fallback for hosts that only have an Alpine aarch64 minirootfs:
# the aarch64 gcc in that rootfs is executed through qemu-user, and the cross
# driver is not used because gcc cannot exec cc1/as/collect2 from a foreign
# architecture. cc1 and as are therefore invoked directly, and the link is done
# by the aarch64 ld with an explicit crt and libc chain.
#
# Environment:
#   ROOTFS    Alpine aarch64 minirootfs with gcc, binutils and musl-dev
#   QEMU      qemu-aarch64 static binary (qemu-user)
#   OUTPUT    helper path (default: out/partitioner)
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"

ROOTFS=${ROOTFS:?set ROOTFS to an Alpine aarch64 minirootfs}
QEMU=${QEMU:?set QEMU to a static qemu-aarch64}
OUTPUT=${OUTPUT:-out/partitioner}

CC1=$(find "$ROOTFS/usr/libexec/gcc" -name cc1 -type f | head -n 1)
AS="$ROOTFS/usr/bin/as"
LD="$ROOTFS/usr/bin/ld"
LIBGCC=$(dirname "$(find "$ROOTFS/usr/lib/gcc" -name crtbeginT.o -type f | head -n 1)")
LIBC="$ROOTFS/usr/lib"
[ -x "$CC1" ] && [ -x "$AS" ] && [ -x "$LD" ] && [ -d "$LIBC" ] || {
    echo "error: $ROOTFS does not contain cc1, as, ld and musl libc" >&2
    exit 1
}

WORK=${TMPDIR:-/tmp}/amethyst-partitioner-obj.$$
trap 'rm -rf "$WORK"' EXIT INT TERM
mkdir -p "$WORK" "$(dirname "$OUTPUT")"

CFLAGS="-O2 -Wall -Wextra -Werror -Isrc"
for f in main selftest util gpt f2fs report device state plan apply; do
    "$QEMU" -L "$ROOTFS" "$CC1" -quiet $CFLAGS src/$f.c -o "$WORK/$f.s"
    "$QEMU" -L "$ROOTFS" "$AS" "$WORK/$f.s" -o "$WORK/$f.o"
done

# The emulated collect2 is unusable (GCC derives the wrong dirname in a
# foreign rootfs), so this is the same link line collect2 would emit, with an
# absolute -B so the right ld is found.
"$QEMU" -L "$ROOTFS" "$LD" -static -s -o "$OUTPUT" \
    "$LIBC/crt1.o" "$LIBC/crti.o" "$LIBGCC/crtbeginT.o" \
    "$WORK/main.o" "$WORK/selftest.o" "$WORK/util.o" "$WORK/gpt.o" \
    "$WORK/f2fs.o" "$WORK/report.o" "$WORK/device.o" "$WORK/state.o" \
    "$WORK/plan.o" "$WORK/apply.o" \
    -L"$LIBGCC" -L"$LIBC" \
    -lssp_nonshared --start-group -lgcc -lgcc_eh -lc --end-group \
    "$LIBGCC/crtend.o" "$LIBC/crtn.o"

[ "$(od -An -tu1 -j4 -N1 "$OUTPUT" | tr -d ' \n')" = 2 ] || {
    echo "error: helper is not ELF64" >&2; exit 1; }
[ "$(od -An -tx1 -j18 -N2 "$OUTPUT" | tr -d ' \n')" = b700 ] || {
    echo "error: helper is not AArch64" >&2; exit 1; }
if command -v readelf >/dev/null 2>&1 && readelf -l "$OUTPUT" | grep -q INTERP; then
    echo "error: helper is dynamically linked" >&2
    exit 1
fi
"$QEMU" -L "$ROOTFS" "$OUTPUT" selftest
echo "helper: $OUTPUT"
sha256sum "$OUTPUT"
