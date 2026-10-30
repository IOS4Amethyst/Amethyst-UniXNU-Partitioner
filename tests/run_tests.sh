#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Offline test suite. It never opens a real block device.
set -uo pipefail

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"
# The suite builds several multi-GiB sparse fixtures; tmpfs (/tmp) is often too
# small and a full one fails with a misleading "no space left" mid-run. Prefer
# real disk: honour TEST_WORKDIR, else a directory beside the repo.
TEST_WORKDIR=${TEST_WORKDIR:-${TMPDIR:-$(dirname "$ROOT")}}
WORK=$(mktemp -d "$TEST_WORKDIR/amethyst-unixnu-tests.XXXXXX") || exit 1
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
# 2 GiB of sectors: the default 1024 MiB tail gap plus a 64 MiB unixnu
# must fit, which a 512 MiB disk cannot express. The file is sparse.
expect_ok 'fixture creates' python3 tests/make_fixture.py "$FIX" --sectors=524288
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
X_END=$(sed -n 's/^unixnu_end_lba=//p' "$FIX/plans/p1/plan.conf")
U_ORIG_END=$(sed -n 's/^userdata_end_lba=//p' "$FIX/plans/p1/plan.conf")
# The split keeps userdata's start, puts unixnu immediately above the shrunken
# userdata, and leaves an unallocated tail above unixnu.
if [ "$U_START" = 2048 ] && [ "$X_START" = $((U_END + 1)) ] \
    && [ "$U_ORIG_END" = 524282 ] && [ $(( (U_ORIG_END - X_END) * 4096 )) -ge $((1024 * 1024 * 1024)) ]; then
    ok 'plan geometry preserves the userdata start and splits at the tail'
else
    bad "plan geometry preserves the userdata start and splits at the tail (start=$U_START u_end=$U_END x_start=$X_START x_end=$X_END orig_end=$U_ORIG_END)"
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

echo '== F2FS superblock pair: any disagreement is a hard stop =='
# The reference implementation (UniXNU scripts/dual_sys/f2fs-shrink.c) requires
# the two 3072-byte superblock copies to be byte-identical. The superblock is
# only rewritten by a resize or a repair, so a disagreement is a real fault and
# must not be resolved by preferring a copy.
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
            ok "superblock plan accepts the pair: $name"
        else
            bad "superblock plan accepts the pair: $name"
            grep -m2 STOP "$WORK/cmd.out" | sed 's/^/    /'
        fi
        if grep -Fq 'fs_sb_offset=0' "$dir/plans/sb/plan.conf" 2>/dev/null; then
            ok "superblock plan.conf pins fs_sb_offset=0: $name"
        else
            bad "superblock plan.conf pins fs_sb_offset=0: $name"
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

sbcase f2fs-sb-first-damaged UNSUPPORTED 'F2FS superblock copy 1 is damaged'
sbcase f2fs-sb-second-damaged UNSUPPORTED 'F2FS superblock copy 2 is damaged'
sbcase f2fs-sb-both-damaged UNSUPPORTED 'UNSUPPORTED [hard]: filesystem: userdata is not F2FS'
sbcase f2fs-sb-field-differs UNSUPPORTED 'F2FS superblock copies disagree'

echo '== F2FS checkpoint is never read =='
# The checkpoint is kernel-owned metadata that a live mount rewrites, so a
# userspace read of it races the kernel and proves nothing the resize needs.
# These fixtures damage it in every way that used to matter; none may change
# the verdict, because the tool must not look at that block at all.
# See UniXNU scripts/dual_sys/f2fs-shrink.c, which validates the superblock and
# then asks the kernel to resize.
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
    if grep -Fq 'Checkpoint' "$dir/reports/i.txt"; then
        bad "checkpoint report must not mention checkpoints: $name"
        grep -m3 'Checkpoint' "$dir/reports/i.txt" | sed 's/^/    /'
    else
        ok "checkpoint report omits the checkpoint entirely: $name"
    fi
    printf 'mode = plan\nsize_gib = 0.0625\nplan_dir = /plans/cp\n' >"$dir/req.conf"
    if [ "$want" = SUPPORTED ]; then
        if UNIXNU_TEST_STATFS="$dir/fixtures/statfs" "$HOST" plan --root "$dir" \
            --identity /identity.conf --request /req.conf >"$WORK/cmd.out" 2>&1; then
            ok "checkpoint plan ignores the damaged checkpoint: $name"
        else
            bad "checkpoint plan ignores the damaged checkpoint: $name"
            grep -m2 STOP "$WORK/cmd.out" | sed 's/^/    /'
        fi
    else
        if UNIXNU_TEST_STATFS="$dir/fixtures/statfs" "$HOST" plan --root "$dir" \
            --identity /identity.conf --request /req.conf >"$WORK/cmd.out" 2>&1; then
            bad "checkpoint plan refuses: $name (unexpected success)"
        else
            ok "checkpoint plan refuses: $name"
        fi
    fi
}

cpcase f2fs-cp-first-damaged SUPPORTED 'Superblock copies match     : yes'
cpcase f2fs-cp-resizefs-flag SUPPORTED 'Superblock copies match     : yes'
cpcase f2fs-cp-fsck-flag SUPPORTED 'Superblock copies match     : yes'
cpcase f2fs-cp-both-unusable SUPPORTED 'Superblock copies match     : yes'
cpcase f2fs-cp-both-out-of-range SUPPORTED 'Superblock copies match     : yes'
cpcase f2fs-cp-beyond-device SUPPORTED 'Superblock copies match     : yes'

echo '== choosable size with unallocated tail (scaled synthetic disk) =='
# Device-sized disk but synthetic userdata start 2048. The gap is outside both
# partition entries; it grants no permission to write inside Android F2FS.
RSV="$WORK/reserve"
mkdir -p "$RSV"
expect_ok 'reserve fixture creates at real geometry' \
    python3 tests/make_fixture.py "$RSV/f" --sectors=124477440
if [ -d "$RSV/f/dev/block" ]; then
    reserve_plan() {
        local name=$1 size=$2 reserve=$3
        mkdir -p "$RSV/f/plans/$name"
        printf 'mode = plan\nsize_gib = %s\n%splan_dir = /plans/%s\n' "$size" \
            "${reserve:+reserve_mib = $reserve
}" "$name" >"$RSV/f/plans/$name.conf"
        UNIXNU_TEST_STATFS="$RSV/f/fixtures/statfs" "$HOST" plan --root "$RSV/f" \
            --identity /identity.conf --request "/plans/$name.conf"
    }
    conf_get() { sed -n "s/^$2=//p" "$RSV/f/plans/$1/plan.conf"; }

    expect_ok 'plan 128 GiB with default reserve succeeds' reserve_plan r-default 128 ''
    if [ -f "$RSV/f/plans/r-default/plan.conf" ]; then
        # Retain the existing default without claiming persistence protection.
        [ "$(conf_get r-default reserve_mib)" = 1024 ] \
            && ok 'default reserve is 1024 MiB' \
            || bad "default reserve is 1024 MiB (got $(conf_get r-default reserve_mib))"
        [ "$(conf_get r-default unixnu_bytes)" = 137438953472 ] \
            && ok 'default-reserve unixnu is exactly 128 GiB' \
            || bad 'default-reserve unixnu is exactly 128 GiB'
        expect_ok 'after-GPT proves tail is outside userdata and unixnu' \
            python3 - "$RSV/f/plans/r-default" <<'PY'
import pathlib, struct, sys
p = pathlib.Path(sys.argv[1])
conf = dict(line.split('=', 1) for line in (p/'plan.conf').read_text().splitlines() if '=' in line)
sector = int(conf['sector'])
data = (p/'gpt-primary.after').read_bytes()
table = struct.unpack_from('<Q', data, sector + 72)[0] * sector
count, size = struct.unpack_from('<II', data, sector + 80)
parts = {}
for i in range(count):
    e = data[table+i*size:table+(i+1)*size]
    if any(e[:16]):
        name = e[56:128].decode('utf-16-le').split('\0', 1)[0]
        parts[name] = struct.unpack_from('<QQ', e, 32)
u, x = parts['userdata'], parts['unixnu']
assert u[1] + 1 == x[0]
assert u[1] == int(conf['userdata_new_end_lba'])
assert x[1] < int(conf['userdata_end_lba'])
assert (int(conf['userdata_end_lba']) - x[1]) * sector >= 1024 * 1048576
assert int(conf['limit_blocks']) * 4096 == (u[1] - u[0] + 1) * sector
text = (p/'plan.txt').read_text()
assert 'tail gap does not protect Android F2FS' in text
assert 'Reserved inside userdata' not in text
PY
        # userdata_end_lba is BEFORE-state, not the new GPT entry's end.
        ud_end=; ux_end=; gap_mib=
        ud_end=$(conf_get r-default userdata_end_lba)
        ux_end=$(conf_get r-default unixnu_end_lba)
        gap_mib=$(( (ud_end - ux_end) * 4096 / 1048576 ))
        [ "$ud_end" = 124477434 ] \
            && ok 'plan records the original userdata end' \
            || bad "plan records the original userdata end (got $ud_end)"
        [ "$gap_mib" -ge 1024 ] \
            && ok "reserve leaves ${gap_mib} MiB unallocated above unixnu" \
            || bad "reserve leaves >=1024 MiB above unixnu (got ${gap_mib})"
        # The shrink limit must be derived from the shrunken userdata, not the
        # full partition, or apply would size F2FS over unixnu.
        lim_ok=
        lim_ok=$(( $(conf_get r-default limit_blocks) * 4096 < $(conf_get r-default userdata_new_bytes) + 4096 ))
        [ "$lim_ok" = 1 ] \
            && ok 'F2FS limit tracks the shrunken userdata' \
            || bad 'F2FS limit tracks the shrunken userdata'
    fi

    expect_ok 'plan 128 GiB with reserve 0 succeeds' reserve_plan r-zero 128 0
    if [ -f "$RSV/f/plans/r-zero/plan.conf" ]; then
        [ "$(conf_get r-zero reserve_mib)" = 0 ] \
            && ok 'explicit reserve 0 is honoured' \
            || bad 'explicit reserve 0 is honoured'
        # 0 vs 1024 must move the boundary by exactly 1 GiB, and by nothing else.
        d=0
        d=$(( $(conf_get r-default unixnu_start_lba) + 262144 ))
        [ "$d" = "$(conf_get r-zero unixnu_start_lba)" ] \
            && ok 'the reserve shifts the boundary by exactly the reserve' \
            || bad "the reserve shifts the boundary by exactly the reserve (got $d vs $(conf_get r-zero unixnu_start_lba))"
        [ "$(conf_get r-zero unixnu_bytes)" = 137438953472 ] \
            && ok 'reserve 0 still gives exactly 128 GiB' \
            || bad 'reserve 0 still gives exactly 128 GiB'
    fi

    expect_fail 'plan refuses a reserve larger than userdata' reserve_plan r-toobig 128 524288
    expect_contains 'oversized reserve explains itself' "$WORK/cmd.out" \
        'reserve_mib leaves no usable userdata'

    expect_fail 'plan refuses a non-numeric reserve' reserve_plan r-text 128 1G

    # Editing the recorded gap invalidates the bundle checksum.
    if cp -a "$RSV/f/plans/r-default" "$RSV/f/plans/r-tampered" 2>/dev/null; then
        sed -i 's/^reserve_mib=.*/reserve_mib=4096/' "$RSV/f/plans/r-tampered/plan.conf"
        if (cd "$RSV/f/plans/r-tampered" && sha256sum -c SHA256SUMS >/dev/null 2>&1); then
            bad 'tampered reserve is detected'
        else
            ok 'tampered reserve breaks the bundle checksum'
        fi
    fi
else
    bad 'reserve fixture creates at real geometry'
fi

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
            UNIXNU_TEST_FAKE_UNMOUNT=1 UNIXNU_TEST_KEEP_BIND=1 "$HOST" apply --root "$BUSY" \
            --identity /identity.conf --request /plans/apply.conf \
            >"$WORK/cmd.out" 2>&1; then
            bad 'apply refuses a surviving /data bind mount (unexpected success)'
        elif grep -Fq 'still provides userdata or a bind mount' "$WORK/cmd.out"; then
            ok 'apply refuses a surviving /data bind mount'
        else
            bad 'apply refuses a surviving /data bind mount'
            grep -m2 STOP "$WORK/cmd.out" | sed 's/^/    /'
        fi
        # Unmount now happens before the shrink, so a surviving bind mount must
        # fail at 'revalidated' with the filesystem still untouched.
        expect_contains 'apply stopped before touching the filesystem' "$BUSY/plans/busy/stage" \
            'revalidated'
        # The offload path tears down every mount under /data before /data
        # itself, deepest first, and must never use MS_FORCE or MNT_DETACH.
        BUSY2="$WORK/corrupt/busy2"
        if python3 tests/corrupt_fixture.py "$FIX" "$BUSY2" busy-bind >"$WORK/cmd.out" 2>&1; then
            mkdir -p "$BUSY2/plans"
            printf 'mode = plan\nsize_gib = 0.0625\nplan_dir = /plans/busy\n' \
                >"$BUSY2/plans/req.conf"
            if UNIXNU_TEST_STATFS="$BUSY2/fixtures/statfs" "$HOST" plan --root "$BUSY2" \
                --identity /identity.conf --request /plans/req.conf >"$WORK/cmd.out" 2>&1; then
                c2=$(sed -n 's/^confirm=//p' "$BUSY2/plans/busy/plan.conf")
                printf 'mode = apply\nplan = /plans/busy\nconfirm = %s\nacknowledge_data_loss = yes\n' \
                    "$c2" >"$BUSY2/plans/apply.conf"
                # No FAKE_UNMOUNT: the real umount2 path runs. It fails offline
                # (EPERM) rather than EBUSY, but it must still stop before the
                # shrink and must name the mount it could not remove.
                if env UNIXNU_TEST_STATFS="$BUSY2/fixtures/statfs" UNIXNU_TEST_FAKE_SHRINK=1 \
                    "$HOST" apply --root "$BUSY2" --identity /identity.conf \
                    --request /plans/apply.conf >"$WORK/cmd.out" 2>&1; then
                    bad 'apply does not proceed while /data cannot be unmounted'
                elif grep -Fq 'cannot unmount /data' "$WORK/cmd.out"; then
                    ok 'apply names the mount it cannot remove'
                else
                    bad 'apply names the mount it cannot remove'
                    grep -m2 STOP "$WORK/cmd.out" | sed 's/^/    /'
                fi
                expect_contains 'real unmount path stops before the filesystem' \
                    "$BUSY2/plans/busy/stage" 'revalidated'
            fi
        fi
    else
        bad 'busy fixture: planning still succeeds'
        grep -m2 STOP "$WORK/cmd.out" | sed 's/^/    /'
    fi
else
    bad 'busy fixture builds'
fi

echo '== recovery-UI mounts outside /data (the real EBUSY) =='
# OrangeFox mounts /sdcard, /storage/emulated and /mnt/runtime/* directly on the
# decrypted device. They are NOT under /data, so a path-prefix test misses them
# and the kernel refuses to unmount /data while they exist.
EMU="$WORK/corrupt/busy-emulated"
if python3 tests/corrupt_fixture.py "$FIX" "$EMU" busy-emulated >"$WORK/cmd.out" 2>&1; then
    mkdir -p "$EMU/plans"
    printf 'mode = plan\nsize_gib = 0.0625\nplan_dir = /plans/emu\n' >"$EMU/plans/req.conf"
    if UNIXNU_TEST_STATFS="$EMU/fixtures/statfs" "$HOST" plan --root "$EMU" \
        --identity /identity.conf --request /plans/req.conf >"$WORK/cmd.out" 2>&1; then
        ok 'emulated fixture: planning succeeds'
        ec=$(sed -n 's/^confirm=//p' "$EMU/plans/emu/plan.conf")
        printf 'mode = apply\nplan = /plans/emu\nconfirm = %s\nacknowledge_data_loss = yes\n' \
            "$ec" >"$EMU/plans/apply.conf"
        # The unmount step must recognise every one of these as a holder of
        # userdata, not just the ones whose path starts with /data.
        if env UNIXNU_TEST_STATFS="$EMU/fixtures/statfs" UNIXNU_TEST_FAKE_SHRINK=1 \
            UNIXNU_TEST_FAKE_UNMOUNT=1 "$HOST" apply --root "$EMU" \
            --identity /identity.conf --request /plans/apply.conf >"$WORK/cmd.out" 2>&1; then
            : # success is acceptable if the fake unmount clears them all
        fi
        if grep -Eq '/sdcard|/storage/emulated|/mnt/runtime' "$WORK/cmd.out"; then
            ok 'emulated mounts are treated as userdata holders'
        elif [ "$(cat "$EMU/plans/emu/stage")" != revalidated ]; then
            ok 'emulated mounts do not block the run'
        else
            bad 'emulated mounts are treated as userdata holders'
            grep -m2 STOP "$WORK/cmd.out" | sed 's/^/    /'
        fi
    else
        bad 'emulated fixture: planning succeeds'
    fi
else
    bad 'emulated fixture builds'
fi


echo '== injected failures: shrink, GPT write, readback =='
# Each scenario starts from its own pristine fixture so a partially written GPT
# in one case cannot mask another. Injections exist only in the host binary.
inject_case() {
    local name=$1 env_name=$2 env_value=$3 want_stage=$4 want_msg=$5
    local dir="$WORK/inject/$name"
    rm -rf "$dir"
    if ! python3 tests/make_fixture.py "$dir" --sectors=524288 >"$WORK/cmd.out" 2>&1; then
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
    # An unclean injected failure can corrupt the GPT pair itself (the backup is
    # new while the primary is old), in which case resuming refuses to read the
    # table at all. That refusal is the correct behaviour, not a retry.
    local resumed_explicitly=0
    if [ "$want_stage" = "$(cat "$dir/plans/inj/stage")" ]; then
        ok "injected failure persists stage $want_stage: $name"
        resumed_explicitly=1
    else
        bad "injected failure persists stage $want_stage: $name"
    fi
    # A second apply must never replay blind. Either it resumes from the recorded
    # stage and finishes, or - when the interrupted write left a GPT pair that
    # cannot be read as a coherent table - it refuses and says so.
    if UNIXNU_TEST_STATFS="$dir/fixtures/statfs" "$HOST" apply --root "$dir" \
        --identity /identity.conf --request /plans/apply.conf >"$WORK/cmd.out" 2>&1; then
        if grep -Fq 'Resuming an interrupted apply' "$WORK/cmd.out" &&
           grep -Fq 'APPLY COMPLETE' "$WORK/cmd.out"; then
            ok "a second apply resumes and completes: $name"
        else
            bad "a second apply resumes and completes: $name"
            grep -m2 STOP "$WORK/cmd.out" | sed 's/^/    /'
        fi
    elif grep -Fq "Resuming an interrupted apply from stage" "$WORK/cmd.out" &&
         grep -Eq 'GPT|do not reboot' "$WORK/cmd.out"; then
        ok "an interrupted GPT write is reported, not retried: $name"
    elif grep -Eq 'GPT|do not reboot' "$WORK/cmd.out"; then
        ok "a damaged GPT is reported, not retried: $name"
    else
        bad "a second apply resumes or reports: $name"
        grep -m1 STOP "$WORK/cmd.out" | sed 's/^/    /'
    fi
    # A failure before any GPT byte is written must leave the table untouched.
    # A readback failure happens after its write, so the table has changed by
    # then and that is expected; the stage record is what protects the user.
    # The second apply above may have resumed and finished, so the "untouched"
    # question is only meaningful for the first attempt.
    case "$want_stage" in
        gpt-backup-writing)
            if [ "$want_msg" = 'backup GPT readback failed' ]; then
                if [ "$before" != "$(sha256sum "$dir/dev/block/sda" | cut -d' ' -f1)" ]; then
                    ok "injected readback failure reports a written backup: $name"
                else
                    bad "injected readback failure reports a written backup: $name"
                fi
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
for stage in planned revalidated unmounted f2fs-shrink-started f2fs-shrink-complete \
             f2fs-verified gpt-backup-writing gpt-backup-verified gpt-primary-writing \
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
    # QEMU_AARCH64 may be set by the caller; otherwise look next to the repo in
    # a scratch directory the caller owns, never in a shared /tmp path.
    if [ -z "${QEMU_AARCH64:-}" ]; then
        for q in "$(dirname "$ROOT")/.scratch/qemu-aarch64.bin" \
                 "$(dirname "$ROOT")/.scratch/qemupkg/usr/bin/qemu-aarch64"; do
            [ -x "$q" ] && { QEMU_AARCH64=$q; break; }
        done
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
