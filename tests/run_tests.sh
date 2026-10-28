#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Offline test suite. It never opens a real block device.
set -uo pipefail

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"
WORK=$(mktemp -d "${TMPDIR:-/tmp}/amethyst-unixnu-tests.XXXXXX") || exit 1
FIX="$WORK/fixture"
HOST="build/partitioner-host"
PASS=0
FAIL=0

cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT

ok() { PASS=$((PASS + 1)); printf 'ok %d - %s\n' "$PASS" "$1"; }
bad() { FAIL=$((FAIL + 1)); printf 'not ok %d - %s\n' "$FAIL" "$1"; }
skip() { printf 'skip - %s\n' "$1"; }

expect_ok() {
    local desc=$1
    shift
    if "$@" >"$WORK/cmd.out" 2>&1; then
        ok "$desc"
    else
        bad "$desc"
        sed 's/^/    /' "$WORK/cmd.out" | tail -20
    fi
}

expect_fail() {
    local desc=$1
    shift
    if "$@" >"$WORK/cmd.out" 2>&1; then
        bad "$desc (unexpected success)"
    else
        ok "$desc"
    fi
}

expect_contains() {
    local desc=$1 file=$2 text=$3
    if grep -Fq -- "$text" "$file"; then
        ok "$desc"
    else
        bad "$desc (missing: $text)"
        tail -10 "$file" | sed 's/^/    /'
    fi
}

host() { "$HOST" "$@"; }
fixture_env() { UNIXNU_TEST_STATFS="$FIX/fixtures/statfs" "$@"; }

printf '# Amethyst UniXNU Partitioner offline tests\n'

echo '== build host binary =='
expect_ok 'host binary builds' \
    cc -DUNIXNU_TESTING -O2 -Wall -Wextra -Werror -Isrc \
    src/main.c src/selftest.c src/util.c src/gpt.c src/f2fs.c src/report.c \
    src/device.c src/state.c src/plan.c src/apply.c -o "$HOST"
expect_ok 'host selftest passes' "$HOST" selftest
expect_contains 'host selftest prints PASS' "$WORK/cmd.out" 'SELFTEST PASS'

echo '== fixture =='
expect_ok 'fixture creates' python3 tests/make_fixture.py "$FIX"
if [ ! -d "$FIX/dev/block" ]; then
    echo 'fixture missing; aborting'
    exit 1
fi
sha256sum "$FIX/dev/block/sda" "$FIX/dev/block/sda3" "$FIX/dev/block/sda2" >"$WORK/pristine.sha"

echo '== read-only inspect =='
expect_ok 'inspect runs on supported layout' \
    fixture_env "$HOST" inspect --root "$FIX" --identity /identity.conf \
    --out /reports/inspect.txt
expect_contains 'inspect reports SUPPORTED' "$FIX/reports/inspect.txt" 'RESULT: SUPPORTED'
if python3 -m json.tool "$FIX/reports/inspect.json" >/dev/null 2>&1; then
    ok 'inspect JSON parses'
else
    bad 'inspect JSON parses'
fi

cat >"$FIX/id-x86.conf" <<'EOF'
device=amethyst
model=Fixture Amethyst
serial=FIXTURESERIAL
recovery=twrp
kernel=6.6.0-fixture
machine=x86_64
boot_completed=0
EOF
expect_ok 'inspect returns a report for unsupported layout' \
    fixture_env "$HOST" inspect --root "$FIX" --identity /id-x86.conf \
    --out /reports/unsupported.txt
expect_contains 'unsupported inspect still prints a verdict' \
    "$FIX/reports/unsupported.txt" 'RESULT: UNSUPPORTED'
if sha256sum -c "$WORK/pristine.sha" >/dev/null 2>&1; then
    ok 'inspect wrote nothing to disk, userdata or metadata'
else
    bad 'inspect wrote nothing to disk, userdata or metadata'
fi

echo '== plan =='
mkdir -p "$FIX/plans"
printf 'mode = plan\nsize_gib = 1000\nplan_dir = /plans/too-big\n' >"$FIX/plans/bad.conf"
expect_fail 'plan refuses an oversized request' \
    fixture_env "$HOST" plan --root "$FIX" --identity /identity.conf --request /plans/bad.conf
if [ ! -e "$FIX/plans/too-big/plan.conf" ]; then
    ok 'oversized plan created no bundle'
else
    bad 'oversized plan created no bundle'
fi

printf 'mode = plan\nsize_gib = 0.0625\nplan_dir = /plans/p1\n' >"$FIX/plans/plan.conf"
expect_ok 'plan creates a bundle' \
    fixture_env "$HOST" plan --root "$FIX" --identity /identity.conf --request /plans/plan.conf
for f in plan.conf plan.txt plan.json plan.env session stage \
         gpt-primary.before gpt-backup.before gpt-primary.after gpt-backup.after SHA256SUMS; do
    if [ -f "$FIX/plans/p1/$f" ]; then
        ok "bundle contains $f"
    else
        bad "bundle contains $f"
    fi
done
if (cd "$FIX/plans/p1" && sha256sum -c SHA256SUMS >/dev/null 2>&1); then
    ok 'bundle SHA256SUMS verifies'
else
    bad 'bundle SHA256SUMS verifies'
fi
expect_contains 'plan stage is planned' "$FIX/plans/p1/stage" 'planned'
if sha256sum -c "$WORK/pristine.sha" >/dev/null 2>&1; then
    ok 'plan wrote nothing to disk, userdata or metadata'
else
    bad 'plan wrote nothing to disk, userdata or metadata'
fi
U_START=$(sed -n 's/^userdata_start_lba=//p' "$FIX/plans/p1/plan.conf")
U_END=$(sed -n 's/^userdata_new_end_lba=//p' "$FIX/plans/p1/plan.conf")
X_START=$(sed -n 's/^unixnu_start_lba=//p' "$FIX/plans/p1/plan.conf")
if [ "$U_START" = 2048 ] && [ "$U_END" = 114431 ] && [ "$X_START" = 114432 ]; then
    ok 'plan geometry preserves the userdata start and splits at the tail'
else
    bad 'plan geometry preserves the userdata start and splits at the tail'
fi

CONFIRM=$(sed -n 's/^confirm=//p' "$FIX/plans/p1/plan.conf")
[ -n "$CONFIRM" ] || bad 'plan contains a confirmation phrase'

echo '== apply refusals (no writes) =='
cp -a "$FIX/plans/p1" "$FIX/plans/tamper"
printf 'X' | dd of="$FIX/plans/tamper/gpt-primary.after" bs=1 seek=200 conv=notrunc status=none
cp -a "$FIX/plans/p1" "$FIX/plans/stage"
printf 'complete\n' >"$FIX/plans/stage/stage"

expect_fail 'apply refuses without data-loss acknowledgement' \
    fixture_env "$HOST" apply --root "$FIX" --identity /identity.conf \
    --plan /plans/p1 --confirm "$CONFIRM"
expect_fail 'apply refuses a wrong confirmation phrase' \
    fixture_env "$HOST" apply --root "$FIX" --identity /identity.conf \
    --plan /plans/p1 --confirm 'unixnu 0 FIXTURESERIAL 1' --i-understand-data-loss
expect_fail 'apply refuses a tampered bundle' \
    fixture_env "$HOST" apply --root "$FIX" --identity /identity.conf \
    --plan /plans/tamper --confirm "$CONFIRM" --i-understand-data-loss
expect_fail 'apply refuses a bundle whose stage is not planned' \
    fixture_env "$HOST" apply --root "$FIX" --identity /identity.conf \
    --plan /plans/stage --confirm "$CONFIRM" --i-understand-data-loss
cat >"$FIX/id-other.conf" <<'EOF'
device=amethyst
model=Fixture Amethyst
serial=OTHERSERIAL
recovery=twrp
kernel=6.6.0-fixture
machine=aarch64
boot_completed=0
EOF
expect_fail 'apply refuses a different hardware serial' \
    fixture_env "$HOST" apply --root "$FIX" --identity /id-other.conf \
    --plan /plans/p1 --confirm "$CONFIRM" --i-understand-data-loss
if sha256sum -c "$WORK/pristine.sha" >/dev/null 2>&1; then
    ok 'all apply refusals left disk, userdata and metadata unchanged'
else
    bad 'all apply refusals left disk, userdata and metadata unchanged'
fi

echo '== authorized destructive apply (offline fake shrink) =='
cat >"$FIX/plans/apply.conf" <<EOF
mode = apply
plan = /plans/p1
confirm = $CONFIRM
acknowledge_data_loss = yes
EOF
if env UNIXNU_TEST_STATFS="$FIX/fixtures/statfs" UNIXNU_TEST_FAKE_SHRINK=1 \
    UNIXNU_TEST_FAKE_UNMOUNT=1 "$HOST" apply --root "$FIX" --identity /identity.conf \
    --request /plans/apply.conf >"$WORK/apply.out" 2>&1; then
    ok 'apply completes with a device-matched plan'
else
    bad 'apply completes with a device-matched plan'
    tail -20 "$WORK/apply.out" | sed 's/^/    /'
fi
expect_contains 'apply reports completion' "$WORK/apply.out" 'APPLY COMPLETE'
expect_contains 'apply stage is complete' "$FIX/plans/p1/stage" 'complete'
for stage in planned revalidated f2fs-shrink-started f2fs-shrink-complete f2fs-verified \
             unmounted gpt-backup-writing gpt-backup-verified gpt-primary-writing \
             gpt-primary-verified complete; do
    if grep -Fq -- " $stage" "$FIX/plans/p1/apply.log"; then
        ok "apply log records $stage"
    else
        bad "apply log records $stage"
    fi
done
METADATA_BEFORE=$(grep 'sda2$' "$WORK/pristine.sha" | cut -d' ' -f1)
METADATA_AFTER=$(sha256sum "$FIX/dev/block/sda2" | cut -d' ' -f1)
if [ "$METADATA_BEFORE" = "$METADATA_AFTER" ]; then
    ok 'metadata contents are byte-identical after apply'
else
    bad 'metadata contents are byte-identical after apply'
fi
if python3 tests/check_after.py "$FIX" "$FIX/plans/p1" >"$WORK/check.out" 2>&1; then
    ok 'independent GPT/F2FS result check passes'
else
    bad 'independent GPT/F2FS result check passes'
    sed 's/^/    /' "$WORK/check.out"
fi
expect_fail 'second apply refuses a completed bundle' \
    env UNIXNU_TEST_STATFS="$FIX/fixtures/statfs" UNIXNU_TEST_FAKE_SHRINK=1 \
    UNIXNU_TEST_FAKE_UNMOUNT=1 "$HOST" apply --root "$FIX" --identity /identity.conf \
    --request /plans/apply.conf
expect_ok 'recover reads the completed session' \
    "$HOST" recover --root "$FIX" --plan /plans/p1
expect_contains 'recover reports completion' "$WORK/cmd.out" 'Last persisted stage: complete'

echo '== post-reboot verify =='
expect_ok 'mock reboot updates the kernel partition view' \
    python3 tests/mock_reboot.py "$FIX" "$FIX/plans/p1"
expect_ok 'verify passes after the simulated reboot' \
    fixture_env "$HOST" verify --root "$FIX" --identity /identity.conf --plan /plans/p1
expect_contains 'verify reports matching GPT copies' "$WORK/cmd.out" \
    'PASS: GPT copies, partition identities and kernel ranges match the plan.'
printf '%s\n' "$((114432 * 8 + 1))" >"$FIX/sys/devices/fixture/block/sda/sda4/start"
expect_fail 'verify rejects a mismatched kernel unixnu range' \
    fixture_env "$HOST" verify --root "$FIX" --identity /identity.conf --plan /plans/p1

echo '== static helper and ZIP artifacts =='
if [ -x out/partitioner ]; then
    ok 'static helper exists'
    if [ "$(od -An -tu1 -j4 -N1 out/partitioner | tr -d ' \n')" = 2 ] &&
       [ "$(od -An -tx1 -j18 -N2 out/partitioner | tr -d ' \n')" = b700 ]; then
        ok 'helper is ELF64 AArch64'
    else
        bad 'helper is ELF64 AArch64'
    fi
    if command -v readelf >/dev/null 2>&1 && readelf -l out/partitioner | grep -q INTERP; then
        bad 'helper is static (no PT_INTERP)'
    else
        ok 'helper is static (no PT_INTERP)'
    fi
    if [ -z "${QEMU_AARCH64:-}" ] && [ -x /tmp/opencode/qemupkg/usr/bin/qemu-aarch64 ]; then
        QEMU_AARCH64=/tmp/opencode/qemupkg/usr/bin/qemu-aarch64
    fi
    if [ -n "${QEMU_AARCH64:-}" ] && [ -x "$QEMU_AARCH64" ]; then
        expect_ok 'static helper selftest runs under qemu' "$QEMU_AARCH64" out/partitioner selftest
        expect_contains 'static helper selftest passes' "$WORK/cmd.out" 'SELFTEST PASS'
    else
        skip 'qemu-aarch64 is not available'
    fi
else
    bad 'static helper exists (run scripts/build.sh)'
fi

HELPER_SHA=$(sha256sum out/partitioner 2>/dev/null | cut -d' ' -f1)
for mode in Inspect Plan Apply; do
    zip="dist/Amethyst-UniXNU-Partitioner-$mode.zip"
    if [ -f "$zip" ]; then
        ok "ZIP exists: $zip"
        dest="$WORK/zip-$mode"
        rm -rf "$dest"
        mkdir -p "$dest"
        unzip -q "$zip" -d "$dest" || bad "ZIP extracts: $zip"
        if [ -f "$dest/META-INF/com/google/android/update-binary" ] &&
           [ -f "$dest/META-INF/com/google/android/updater-script" ] &&
           [ -f "$dest/bin/partitioner" ]; then
            ok "ZIP layout is complete: $mode"
        else
            bad "ZIP layout is complete: $mode"
        fi
        if sh -n "$dest/META-INF/com/google/android/update-binary" 2>/dev/null; then
            ok "installer shell syntax: $mode"
        else
            bad "installer shell syntax: $mode"
        fi
        if [ -n "$HELPER_SHA" ] &&
           grep -Fq "EXPECTED_HELPER_SHA256='$HELPER_SHA'" \
               "$dest/META-INF/com/google/android/update-binary"; then
            ok "installer pins the helper hash: $mode"
        else
            bad "installer pins the helper hash: $mode"
        fi
        if [ "$(sha256sum "$dest/bin/partitioner" | cut -d' ' -f1)" = "$HELPER_SHA" ]; then
            ok "ZIP payload matches the built helper: $mode"
        else
            bad "ZIP payload matches the built helper: $mode"
        fi
        if grep -Fq "MODE='$(printf '%s' "$mode" | tr 'A-Z' 'a-z')'" \
               "$dest/META-INF/com/google/android/update-binary"; then
            ok "ZIP has the expected mode: $mode"
        else
            bad "ZIP has the expected mode: $mode"
        fi
        if grep -Eqi 'curl|wget|http://|https://' "$dest/META-INF/com/google/android/update-binary"; then
            bad "installer performs no network access: $mode"
        else
            ok "installer performs no network access: $mode"
        fi
    else
        bad "ZIP exists: $zip (run scripts/build.sh)"
    fi
done
if [ -f dist/SHA256SUMS ] && (cd dist && sha256sum -c SHA256SUMS >/dev/null 2>&1); then
    ok 'dist/SHA256SUMS verifies'
else
    bad 'dist/SHA256SUMS verifies'
fi

echo
printf 'TESTS: %d passed, %d failed\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
