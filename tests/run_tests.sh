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

echo '== upstream reference (vendored, unmodified) =='
# The reference implementation is the source of truth for layout and F2FS
# math. Run its own suite and helper self-test, then prove this tool produces
# the same geometry and GPT bytes for identical input.
VENDOR=vendor/unixnu-dual_sys
PINNED_SHA256=(
    '8e4a756951f691849a4c95d0a775d26cdafaa9dd638aeb8b417425ce74b91aa7  f2fs-shrink.c'
    'd0864fd6f7caf3ffa54d9a52ab93e3eb1a2a4a02c5263ee2119be71f37d33498  gpt.py'
    '4f5686539ba3f88276efc7c0f2b72e190e70f05d4ec0884ad0618732845b7d9a  partition.py'
    '29f2de54c0a9e2122ddb84d3d510f704c001adee7ff93b1916cccda17cef903c  test_dual_sys.py'
    'f51641521a6fe88d024b98c8f75aa49e157cff54b85a8d888c110f29349ae5d0  twrp.sh'
)
printf '%s\n' "${PINNED_SHA256[@]}" >"$WORK/vendor.sha"
if (cd "$VENDOR" && sha256sum -c "$WORK/vendor.sha" >/dev/null 2>&1); then
    ok 'vendored upstream files match the pinned upstream revision'
else
    bad 'vendored upstream files match the pinned upstream revision'
    (cd "$VENDOR" && sha256sum -c "$WORK/vendor.sha") 2>&1 | sed 's/^/    /'
fi
if python3 --version >/dev/null 2>&1; then
    expect_ok 'upstream test_dual_sys.py passes' bash -c \
        'cd vendor/unixnu-dual_sys && python3 test_dual_sys.py'
    expect_ok 'upstream f2fs-shrink self-test builds' \
        cc -O2 -Wall -Wextra -Werror vendor/unixnu-dual_sys/f2fs-shrink.c \
        -o "$WORK/f2fs-shrink-test"
    expect_ok 'upstream f2fs-shrink self-test passes' \
        "$WORK/f2fs-shrink-test" self-test
    expect_contains 'upstream self-test prints PASS' \
        "$WORK/cmd.out" 'PASS: F2FS section rounding'
    expect_ok 'upstream twrp.sh parses' sh -n vendor/unixnu-dual_sys/twrp.sh
    expect_ok 'differential plan matches upstream at 64 MiB' \
        python3 tests/differential_upstream.py "$FIX" $((64 * 1024 * 1024))
else
    skip 'python3 unavailable; upstream differential tests skipped'
fi

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

echo '== corrupted fixtures: inspect must report, plan/apply must refuse =='
# Build each variant from the pristine fixture, then require the exact refusal
# that variant is meant to exercise. None of these touch a real block device.
cfixture() {
    local name=$1 mode=$2 want=$3
    local dir="$WORK/corrupt/$name"
    if ! python3 tests/corrupt_fixture.py "$FIX" "$dir" "$mode" >"$WORK/cmd.out" 2>&1; then
        bad "corrupt fixture builds: $name"
        tail -5 "$WORK/cmd.out" | sed 's/^/    /'
        return
    fi
    ok "corrupt fixture builds: $name"
    local before after
    before=$(sha256sum "$dir/dev/block/sda" "$dir/dev/block/sda3" "$dir/dev/block/sda2" \
        | sha256sum)
    mkdir -p "$dir/reports" "$dir/plans"
    printf 'mode = plan\nsize_gib = 0.0625\nplan_dir = /plans/c\n' \
        >"$dir/plans/request.conf"

    UNIXNU_TEST_STATFS="$dir/fixtures/statfs" "$HOST" inspect --root "$dir" \
        --identity /identity.conf --out /reports/i.txt >"$WORK/cmd.out" 2>&1
    if grep -Fq 'RESULT: UNSUPPORTED' "$dir/reports/i.txt"; then
        ok "inspect reports UNSUPPORTED: $name"
    else
        bad "inspect reports UNSUPPORTED: $name"
    fi

    if UNIXNU_TEST_STATFS="$dir/fixtures/statfs" "$HOST" plan --root "$dir" \
        --identity /identity.conf --request /plans/request.conf \
        >"$WORK/cmd.out" 2>&1; then
        bad "plan refuses: $name (unexpected success)"
    elif grep -Fq -- "$want" "$WORK/cmd.out"; then
        ok "plan refuses with '$want': $name"
    else
        bad "plan refuses with '$want': $name"
        grep -m2 STOP "$WORK/cmd.out" | sed 's/^/    /'
    fi

    after=$(sha256sum "$dir/dev/block/sda" "$dir/dev/block/sda3" "$dir/dev/block/sda2" \
        | sha256sum)
    if [ "$before" = "$after" ]; then
        ok "corrupted fixture left unchanged: $name"
    else
        bad "corrupted fixture left unchanged: $name"
    fi
}

cfixture primary-crc primary-crc 'GPT header CRC mismatch'
cfixture backup-crc backup-crc 'GPT header CRC mismatch'
cfixture primary-table primary-table 'primary and backup GPT headers disagree'
cfixture backup-disagree backup-disagree 'primary and backup GPT headers disagree'
cfixture mbr-missing mbr-missing 'missing protective MBR signature'
cfixture add-unixnu add-unixnu 'unixnu already exists'
cfixture fill-entries fill-entries 'no unused GPT entry is available'
cfixture userdata-not-last userdata-not-last 'userdata is not the last occupied partition'
cfixture f2fs-ext4 f2fs-ext4 'userdata is not F2FS'
cfixture f2fs-bad-block f2fs-bad-block 'unsupported F2FS block size'
cfixture f2fs-bad-seg f2fs-bad-seg 'unsupported F2FS segment size'
cfixture f2fs-multi-seg f2fs-multi-seg 'unsupported F2FS sections'
cfixture f2fs-multi-dev f2fs-multi-dev 'multi-device F2FS is unsupported'
cfixture f2fs-oversize f2fs-oversize 'exceeds the userdata partition'

echo '== F2FS superblock pair: one damaged copy recovers, none refuses =='
# Mounting needs only one valid copy and the kernel prefers the second, so a
# torn write on either copy must be a warning, not a refusal. Only "neither
# copy carries the magic" may be a hard stop.
sbcase() {
    local name=$1 want=$2 want_text=$3
    local dir="$WORK/corrupt/$name"
    if ! python3 tests/corrupt_fixture.py "$FIX" "$dir" "$name" >"$WORK/cmd.out" 2>&1; then
        bad "superblock fixture builds: $name"
        tail -5 "$WORK/cmd.out" | sed 's/^/    /'
        return
    fi
    ok "superblock fixture builds: $name"
    mkdir -p "$dir/reports"
    UNIXNU_TEST_STATFS="$dir/fixtures/statfs" "$HOST" inspect --root "$dir" \
        --identity /identity.conf --out /reports/i.txt >"$WORK/cmd.out" 2>&1
    if grep -Fq "RESULT: $want" "$dir/reports/i.txt"; then
        ok "superblock inspect result is $want: $name"
    else
        bad "superblock inspect result is $want: $name"
        grep -m3 'RESULT:\|UNSUPPORTED' "$dir/reports/i.txt" | sed 's/^/    /'
    fi
    if [ -n "$want_text" ] && ! grep -Fq -- "$want_text" "$dir/reports/i.txt"; then
        bad "superblock report says '$want_text': $name"
    elif [ -n "$want_text" ]; then
        ok "superblock report says '$want_text': $name"
    fi
    printf 'mode = plan\nsize_gib = 0.0625\nplan_dir = /plans/sb\n' \
        >"$dir/req.conf"
    if [ "$want" = SUPPORTED ]; then
        if UNIXNU_TEST_STATFS="$dir/fixtures/statfs" "$HOST" plan --root "$dir" \
            --identity /identity.conf --request /req.conf >"$WORK/cmd.out" 2>&1; then
            ok "superblock plan records the active copy: $name"
        else
            bad "superblock plan records the active copy: $name"
            grep -m2 STOP "$WORK/cmd.out" | sed 's/^/    /'
        fi
        if grep -Fq 'fs_sb_offset=' "$dir/plans/sb/plan.conf" 2>/dev/null; then
            ok "superblock plan.conf carries fs_sb_offset: $name"
        else
            bad "superblock plan.conf carries fs_sb_offset: $name"
        fi
    else
        if UNIXNU_TEST_STATFS="$dir/fixtures/statfs" "$HOST" plan --root "$dir" \
            --identity /identity.conf --request /req.conf >"$WORK/cmd.out" 2>&1; then
            bad "superblock plan refuses: $name (unexpected success)"
        else
            ok "superblock plan refuses: $name"
        fi
    fi
}

sbcase f2fs-sb-first-damaged SUPPORTED 'Damaged superblock copy     : 1'
sbcase f2fs-sb-second-damaged SUPPORTED 'Damaged superblock copy     : 2'
sbcase f2fs-sb-both-damaged UNSUPPORTED 'UNSUPPORTED [hard]: filesystem: userdata is not F2FS'
sbcase f2fs-sb-field-differs SUPPORTED 'Active superblock copy'

echo '== F2FS checkpoint pair: validity beats version, bad states refuse =='
# A torn copy can hold any version number, so the copy with the higher
# checkpoint_ver is only trusted once it looks like a checkpoint at all.
cpcase() {
    local name=$1 want=$2 want_text=$3
    local dir="$WORK/corrupt/$name"
    if ! python3 tests/corrupt_fixture.py "$FIX" "$dir" "$name" >"$WORK/cmd.out" 2>&1; then
        bad "checkpoint fixture builds: $name"
        tail -5 "$WORK/cmd.out" | sed 's/^/    /'
        return
    fi
    ok "checkpoint fixture builds: $name"
    mkdir -p "$dir/reports"
    UNIXNU_TEST_STATFS="$dir/fixtures/statfs" "$HOST" inspect --root "$dir" \
        --identity /identity.conf --out /reports/i.txt >"$WORK/cmd.out" 2>&1
    if grep -Fq "RESULT: $want" "$dir/reports/i.txt"; then
        ok "checkpoint inspect result is $want: $name"
    else
        bad "checkpoint inspect result is $want: $name"
        grep -m3 'RESULT:\|UNSUPPORTED' "$dir/reports/i.txt" | sed 's/^/    /'
    fi
    if ! grep -Fq -- "$want_text" "$dir/reports/i.txt"; then
        bad "checkpoint report says '$want_text': $name"
    else
        ok "checkpoint report says '$want_text': $name"
    fi
    if [ "$want" = SUPPORTED ]; then
        # The damaged copy's version outranks the good one; the good version
        # must still be the one reported, or a garbage block was selected.
        if grep -Fq 'Checkpoint version          : 7' "$dir/reports/i.txt"; then
            ok "checkpoint uses the valid copy's version: $name"
        else
            bad "checkpoint uses the valid copy's version: $name"
            grep 'Checkpoint version' "$dir/reports/i.txt" | sed 's/^/    /'
        fi
    fi
}

cpcase f2fs-cp-first-damaged SUPPORTED 'Damaged checkpoint copy     : 1'
cpcase f2fs-cp-resizefs-flag UNSUPPORTED 'UNSUPPORTED [hard]: filesystem: F2FS was left mid-resize'
cpcase f2fs-cp-fsck-flag UNSUPPORTED 'UNSUPPORTED [hard]: filesystem: F2FS is marked for fsck'

echo '== busy userdata: apply must stop before writing GPT =='
# A bind mount of the decrypted device survives the /data unmount. Apply must
# notice it and stop with the GPT untouched.
BUSY="$WORK/corrupt/busy"
if python3 tests/corrupt_fixture.py "$FIX" "$BUSY" busy-bind >"$WORK/cmd.out" 2>&1; then
    mkdir -p "$BUSY/plans"
    printf 'mode = plan\nsize_gib = 0.0625\nplan_dir = /plans/busy\n' \
        >"$BUSY/plans/req.conf"
    if UNIXNU_TEST_STATFS="$BUSY/fixtures/statfs" "$HOST" plan --root "$BUSY" \
        --identity /identity.conf --request /plans/req.conf >"$WORK/cmd.out" 2>&1; then
        ok 'busy fixture: planning still succeeds'
        busy_confirm=$(sed -n 's/^confirm=//p' "$BUSY/plans/busy/plan.conf")
        printf 'mode = apply\nplan = /plans/busy\nconfirm = %s\nacknowledge_data_loss = yes\n' \
            "$busy_confirm" >"$BUSY/plans/apply.conf"
        if env UNIXNU_TEST_STATFS="$BUSY/fixtures/statfs" UNIXNU_TEST_FAKE_SHRINK=1 \
            UNIXNU_TEST_FAKE_UNMOUNT=1 "$HOST" apply --root "$BUSY" \
            --identity /identity.conf --request /plans/apply.conf \
            >"$WORK/cmd.out" 2>&1; then
            bad 'apply refuses a surviving /data bind mount (unexpected success)'
        elif grep -Fq 'still provides userdata or a bind mount' "$WORK/cmd.out"; then
            ok 'apply refuses a surviving /data bind mount'
        else
            bad 'apply refuses a surviving /data bind mount'
            grep -m2 STOP "$WORK/cmd.out" | sed 's/^/    /'
        fi
        expect_contains 'apply stopped before writing the GPT' "$BUSY/plans/busy/stage" \
            'f2fs-verified'
    else
        bad 'busy fixture: planning still succeeds'
        grep -m2 STOP "$WORK/cmd.out" | sed 's/^/    /'
    fi
else
    bad 'busy fixture builds'
fi


echo '== injected failures: shrink, GPT write, readback =='
# Each scenario starts from its own pristine fixture so a partially written GPT
# in one case cannot mask another. Injections exist only in the host binary.
inject_case() {
    local name=$1 env_name=$2 env_value=$3 want_stage=$4 want_msg=$5
    local dir="$WORK/inject/$name"
    rm -rf "$dir"
    if ! python3 tests/make_fixture.py "$dir" >"$WORK/cmd.out" 2>&1; then
        bad "injection fixture builds: $name"
        return
    fi
    mkdir -p "$dir/plans"
    printf 'mode = plan\nsize_gib = 0.0625\nplan_dir = /plans/inj\n' >"$dir/plans/req.conf"
    if ! UNIXNU_TEST_STATFS="$dir/fixtures/statfs" "$HOST" plan --root "$dir" \
        --identity /identity.conf --request /plans/req.conf >"$WORK/cmd.out" 2>&1; then
        bad "injection plan builds: $name"
        tail -5 "$WORK/cmd.out" | sed 's/^/    /'
        return
    fi
    local confirm
    confirm=$(sed -n 's/^confirm=//p' "$dir/plans/inj/plan.conf")
    printf 'mode = apply\nplan = /plans/inj\nconfirm = %s\nacknowledge_data_loss = yes\n' \
        "$confirm" >"$dir/plans/apply.conf"

    local before
    before=$(sha256sum "$dir/dev/block/sda" | cut -d' ' -f1)
    if env UNIXNU_TEST_STATFS="$dir/fixtures/statfs" UNIXNU_TEST_FAKE_SHRINK=1 \
        UNIXNU_TEST_FAKE_UNMOUNT=1 "$env_name"=1 "$HOST" apply --root "$dir" \
        --identity /identity.conf --request /plans/apply.conf >"$WORK/cmd.out" 2>&1; then
        bad "injected failure stops apply: $name (unexpected success)"
    elif grep -Fq -- "$want_msg" "$WORK/cmd.out"; then
        ok "injected failure reports '$want_msg': $name"
    else
        bad "injected failure reports '$want_msg': $name"
        grep -m2 STOP "$WORK/cmd.out" | sed 's/^/    /'
    fi
    if [ "$want_stage" = "$(cat "$dir/plans/inj/stage")" ]; then
        ok "injected failure persists stage $want_stage: $name"
    else
        bad "injected failure persists stage $want_stage: $name"
    fi
    # No automatic retry: the same bundle must refuse on a second attempt.
    if UNIXNU_TEST_STATFS="$dir/fixtures/statfs" "$HOST" apply --root "$dir" \
        --identity /identity.conf --request /plans/apply.conf >"$WORK/cmd.out" 2>&1; then
        bad "injected failure is not retried automatically: $name"
    elif grep -Fq 'do not retry, run recover' "$WORK/cmd.out"; then
        ok "injected failure is not retried automatically: $name"
    else
        bad "injected failure is not retried automatically: $name"
        grep -m1 STOP "$WORK/cmd.out" | sed 's/^/    /'
    fi
    # A failure before any GPT byte is written must leave the table untouched.
    # A readback failure happens after its write, so the table has changed by
    # then and that is expected; the stage record is what protects the user.
    case "$want_stage" in
        f2fs-shrink-started|gpt-backup-writing)
            if [ "$want_msg" = 'backup GPT readback failed' ]; then
                if [ "$before" != "$(sha256sum "$dir/dev/block/sda" | cut -d' ' -f1)" ]; then
                    ok "injected readback failure reports a written backup: $name"
                else
                    bad "injected readback failure reports a written backup: $name"
                fi
            elif [ "$before" = "$(sha256sum "$dir/dev/block/sda" | cut -d' ' -f1)" ]; then
                ok "injected failure left the GPT untouched: $name"
            else
                bad "injected failure left the GPT untouched: $name"
            fi
            ;;
    esac
}

inject_case shrink-failure UNIXNU_TEST_FAIL_SHRINK 1 \
    'f2fs-shrink-started' 'F2FS resize failed'
inject_case backup-write UNIXNU_TEST_FAIL_BACKUP_WRITE 1 \
    'gpt-backup-writing' 'backup GPT write failed'
inject_case backup-readback UNIXNU_TEST_FAIL_BACKUP_READBACK 1 \
    'gpt-backup-writing' 'backup GPT readback failed'
inject_case primary-write UNIXNU_TEST_FAIL_PRIMARY_WRITE 1 \
    'gpt-primary-writing' 'primary GPT write failed'

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
    # The shipped helper must never contain the offline test hooks.
    if command -v strings >/dev/null 2>&1; then
        if [ "$(strings out/partitioner | grep -c 'UNIXNU_TEST_')" -eq 0 ]; then
            ok 'shipped helper contains no UNIXNU_TEST_ hooks'
        else
            bad 'shipped helper contains no UNIXNU_TEST_ hooks'
        fi
    else
        skip 'strings is not available'
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
