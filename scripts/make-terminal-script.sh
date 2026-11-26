#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Package out/partitioner into the single-file terminal script
# dist/amethyst-unixnu.sh (shell front end + appended static helper).
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"
HELPER=${HELPER:-out/partitioner}
OUT=${OUT:-dist/amethyst-unixnu.sh}

VERSION=$(sed -n 's/^#define TOOL_VERSION "\(.*\)"/\1/p' src/common.h)
[ -n "$VERSION" ] || { echo "cannot read TOOL_VERSION" >&2; exit 1; }
[ -f "$HELPER" ] || { echo "missing $HELPER; run scripts/build.sh first" >&2; exit 1; }
[ "$(od -An -tx1 -j18 -N2 "$HELPER" | tr -d ' \n')" = b700 ] || {
    echo "error: $HELPER is not AArch64" >&2; exit 1; }

SHA=$(sha256sum "$HELPER" | cut -d' ' -f1)
SIZE=$(wc -c <"$HELPER" | tr -d ' ')
mkdir -p "$(dirname "$OUT")"
sed -e "s/@HELPER_SHA256@/$SHA/g" -e "s/@HELPER_SIZE@/$SIZE/g" \
    -e "s/@TOOL_VERSION@/$VERSION/g" installer/amethyst-unixnu.sh.in >"$OUT.tmp"
tail -n 1 "$OUT.tmp" | grep -qx '__HELPER_PAYLOAD__' || {
    echo "error: template must end with the __HELPER_PAYLOAD__ line" >&2; exit 1; }
cat "$HELPER" >>"$OUT.tmp"
chmod 755 "$OUT.tmp"
mv "$OUT.tmp" "$OUT"
(cd "$(dirname "$OUT")" && sha256sum "$(basename "$OUT")" >SHA256SUMS)
echo "built $OUT (helper $SHA, $SIZE bytes)"
