# Amethyst compatibility report

Report date: 2026-10-28

Scope: the three recovery-flashable packages and the static helper they contain,
reviewed against the official UniXNU dual-system implementation for the Xiaomi
Redmi Note 14 Pro+ 5G (`amethyst`, Qualcomm SM7635/Volcano).

This report covers packaging, provenance, build reproducibility, and
recovery/filesystem compatibility. Hardware results are deliberately separated
into read-only and destructive work and are kept in
[`HARDWARE-TEST-REPORT.md`](HARDWARE-TEST-REPORT.md).

**Read this first:** read-only Inspect has now run on a real Amethyst and passed
every structural prerequisite; Plan and Apply have not run anywhere but a
fixture. Nothing here establishes that apply preserves data on real hardware,
that the Amethyst recovery kernel implements `F2FS_IOC_RESIZE_FS`, or that the
resulting layout boots either system. See
[`HARDWARE-TEST-REPORT.md`](HARDWARE-TEST-REPORT.md) §1 for the device results.

## 1. Upstream reference under review

| Item | Value |
| --- | --- |
| Repository | `UniXNU/UniXNU` |
| Commit | `3ef972192279bacbfe91577ab942b5742d5efcc5` (`bootimg: verify header version 4 images`) |
| Files reviewed | `scripts/dual_sys/{partition.py,gpt.py,f2fs-shrink.c,twrp.sh,test_dual_sys.py}`, `docs/porting/dual-system-partition.md` |
| Doc last touched by | `9fd45ef fix(storage): make backups optional and support macOS self-tests` |

The copies in `vendor/unixnu-dual_sys/` are byte-identical to that revision and
are checked against pinned SHA-256 values by `tests/run_tests.sh`:

```
8e4a756951f691849a4c95d0a775d26cdafaa9dd638aeb8b417425ce74b91aa7  f2fs-shrink.c
d0864fd6f7caf3ffa54d9a52ab93e3eb1a2a4a02c5263ee2119be71f37d33498  gpt.py
4f5686539ba3f88276efc7c0f2b72e190e70f05d4ec0884ad0618732845b7d9a  partition.py
29f2de54c0a9e2122ddb84d3d510f704c001adee7ff93b1916cccda17cef903c  test_dual_sys.py
f51641521a6fe88d024b98c8f75aa49e157cff54b85a8d888c110f29349ae5d0  twrp.sh
573f2e2257a6caaa62be4582ea20589f8e717999912eaa7a1e52299c079a0b56  dual-system-partition.md
```

## 2. Official flow vs. this implementation

This section records what the upstream code actually does, and where a recovery
ZIP cannot reproduce it.

| Concern | Official implementation | This implementation |
| --- | --- | --- |
| Transport | Host Python drives the device over ADB (`ADB` class in `partition.py`) | Runs entirely inside recovery; no host or network involvement |
| Identity | `adb devices` serial plus `getprop ro.serialno` fallback | Same `ro.serialno`/`ro.boot.serialno` sources, plus `device-tree/model`, chosen recovery bus, kernel release and a user-supplied identity file |
| Helper | `f2fs-shrink.c` pushed to `/data/local/tmp` and copied into the bundle | Statically linked AArch64 helper embedded in each ZIP at `bin/partitioner` |
| Plan → apply coupling | `apply` **regenerates** a fresh plan, then executes it in the same run | `apply` consumes the reviewed, hash-manifested bundle produced by Plan and refuses if the live device differs |
| Confirmation | `f"{adb.serial} unixnu {bytes}"` typed on the host | `unixnu <plan_id> <serial> <unixnu_bytes>` plus `acknowledge_data_loss = yes`; a ZIP install alone never triggers apply |
| GPT layout math | `gpt.py` `GPT.split()` | Reimplemented in `src/gpt.c`; proven byte-identical to upstream (see §4) |
| F2FS shrink | `f2fs-shrink.c` ioctl + section rounding | Same algorithms in `src/f2fs.c`; rounding functions match upstream exactly |
| Superblock pair | `f2fs-shrink.c` refuses unless both copies are byte-identical | Same refusal where neither copy is valid; a mismatch with one valid copy is a warning that names the active copy, because Linux mounts that filesystem |
| Backups | Optional, per `9fd45ef` | Apply refuses to run unless the operator confirms a verified off-device backup exists |
| Sequencing | `twrp.sh`: shrink, verify, unmount, write backup GPT, verify, write primary GPT, verify | Same order, with a persisted `stage` file and a post-unmount metadata re-hash before any GPT byte is written |
| Interruption | Documented as unrecoverable; no automatic rollback | Same stance; stage and errors are persisted and the bundle refuses a second run |

Checks that upstream performs on the host (ADB device selection, multi-device
ambiguity) have no recovery-side equivalent and are therefore replaced by
on-device equivalents: a single resolved `by-name` link, a single decrypted
mapping backed only by `userdata`, and an explicit identity file. Nothing that
upstream does on the host is silently dropped.

## 3. Package inventory

All three ZIPs are TWRP-style flashable archives and contain the same helper;
only the mode the installer requests differs.

| ZIP | SHA-256 | Mode |
| --- | --- | --- |
| `Amethyst-UniXNU-Partitioner-Inspect.zip` | `685eec24396e98d0f6837ddf6d500d1ee4d6dbd93773c97e9d5870da6e462975` | read-only inspect |
| `Amethyst-UniXNU-Partitioner-Plan.zip` | `9bba09bd70e58b3caed335ed6b211d3c0396c944822c176c6965b8e673fcb9d7` | plan bundle only |
| `Amethyst-UniXNU-Partitioner-Apply.zip` | `2734fa37694d979203559b36e5f973a1482a451c3a63edcc5ee7e93508a1f13f` | gated destructive apply |

These are the hashes of the archives in this commit. `dist/SHA256SUMS` is the
file to check against for copies you distribute.

`dist/SHA256SUMS` is authoritative for distributed copies. ZIP hashes are not
reproducible because entry timestamps vary between builds; the helper hash and
the archive contents are stable.

### Contents of every ZIP

```
META-INF/com/google/android/updater-script    flashable entry point
META-INF/com/google/android/update-binary     mounts the log, extracts, runs
bin/partitioner                               static AArch64 helper
README.txt                                    in-ZIP instructions
samples/request-plan.conf                     request template
samples/request-apply.conf                    request template
```

The in-ZIP README points at `samples/`, which is where the archive really stores
the templates. The project repository keeps the same files under `examples/`;
`installer/README.txt.in` now states both paths explicitly, so the earlier
mismatch is resolved.

### Helper binary

```
SHA-256 : 5ab4821747debb40bd140d90818adfc95dcd859ae96acb7d027fcb5aff4b34cc
file    : ELF 64-bit LSB executable, ARM aarch64, version 1 (SYSV),
          statically linked, stripped
size    : 198352 bytes
```

The installer pins this hash and verifies `bin/partitioner` before executing it.
Because the helper is stripped, the source and build recipe below are the only
attribution; both are in this repository.

## 4. Reproducible build

### Source

```
src/common.h src/util.c    shared types, parsing, formatting, hashing helpers
src/gpt.c                  GPT parse/validate, layout generation, images
src/f2fs.c                 F2FS superblock/checkpoint validation, rounding, shrink
src/device.c               identity, block-device discovery, mount inspection
src/report.c               human-readable and JSON reports
src/state.c                consolidated device state and capability checks
src/plan.c                 plan bundle creation and hashing
src/apply.c                gated destructive apply, stage persistence
src/main.c                 argument handling and mode dispatch
src/selftest.c             in-binary self-test (rounding, CRC, layout)
```

### Toolchain

| Component | Version |
| --- | --- |
| Compiler | Android clang `r547379`, `clang version 20.0.0` (`b718bcaf8c198c82f3021447d943401e3ab5bd54`) |
| Sysroot | `aarch64-unknown-linux-musl` from Android prebuilts |
| Host compiler | `cc` (Ubuntu 15.2.0-16ubuntu1) for the test binary only |
| Python | 3.14.4 (offline tests and fixtures only) |

### Commands

Shipped helper (no test hooks, `-static -s`, no `-DUNIXNU_TESTING`):

```sh
CLANG=/path/to/clang-r547379/bin/clang
SYSROOT=/path/to/prebuilts/build-tools/sysroots/aarch64-unknown-linux-musl

$CLANG --target=aarch64-unknown-linux-musl --sysroot=$SYSROOT \
    -rtlib=compiler-rt -static -s -O2 -Wall -Wextra -Werror -Isrc \
    src/main.c src/selftest.c src/util.c src/gpt.c src/f2fs.c src/report.c \
    src/device.c src/state.c src/plan.c src/apply.c -o out/partitioner
```

Host test binary (hooks enabled):

```sh
cc -DUNIXNU_TESTING -O2 -Wall -Wextra -Werror -Isrc \
    src/main.c src/selftest.c src/util.c src/gpt.c src/f2fs.c src/report.c \
    src/device.c src/state.c src/plan.c src/apply.c -o build/partitioner-host
```

Or run everything at once:

```sh
QEMU_AARCH64=/path/to/qemu-aarch64 ./scripts/build.sh
QEMU_AARCH64=/path/to/qemu-aarch64 ./tests/run_tests.sh
```

`scripts/build.sh` verifies the ELF class, machine, absence of `PT_INTERP`,
runs the helper self-test under QEMU when available, and assembles all three
ZIPs with the helper hash pinned in the installer.

### Test hooks never ship

Failure-injection and fixture hooks (`UNIXNU_TEST_MOUNTS`,
`UNIXNU_TEST_STATFS`, `UNIXNU_TEST_FAKE_SHRINK`, `UNIXNU_TEST_FAKE_UNMOUNT`,
`UNIXNU_TEST_FAIL_SHRINK`, `UNIXNU_TEST_FAIL_BACKUP_WRITE`,
`UNIXNU_TEST_FAIL_BACKUP_READBACK`, `UNIXNU_TEST_FAIL_PRIMARY_WRITE`) are guarded
by `#ifdef UNIXNU_TESTING`. The suite asserts that `strings out/partitioner`
contains no `UNIXNU_TEST_` symbol, so the shipped helper cannot be steered by an
environment variable.

## 5. Agreement with the official calculations

`tests/differential_upstream.py` runs the vendored upstream `gpt.py` directly on
the same fixture geometry and compares results with this implementation. It
performs 23 checks per run:

- `unixnu` start/end LBA, `userdata` start and new end, sizes, tail gap, entry index
- `userdata` type GUID and unique GUID preservation
- `limit_blocks` and `expected_blocks`
- both `before` GPT images, byte for byte
- both `after` GPT images, byte for byte once the randomly generated `unixnu`
  unique GUID (and the two CRCs covering it) is normalised to upstream's value
- `unixnu` type GUID equals upstream's `Linux data` GUID
- entry name/start/end/attributes for both `unixnu` and `userdata`

Results:

| Request | Upstream | This tool |
| --- | --- | --- |
| 1, 2, 16, 64, 128, 256, 300 MiB | plan | plan, 23/23 identical |
| 400, 401, 410, 420, 430 MiB | refuse (insufficient free F2FS space) | refuse, same reason |

Zero disagreements across the sweep. The shrink formula is upstream's
`floor((L - B % S) / S) * S` with `S = 512`, implemented as
`f2fs_request_blocks()` and confirmed equal to upstream's `expected_blocks()`
for every case tested.

## 6. Amethyst recovery compatibility

### Identity

The fstab supplied for Amethyst names `bootdevice/by-name/userdata` at `/data`,
F2FS with file-based and inline encryption, metadata encryption at
`/metadata/vold/metadata_encryption`, a separate F2FS `metadata` partition, and
UFS behind `1d84000.ufshc`.

The fstab is treated as evidence only. At runtime the tool resolves identity
from `ro.serialno`/`ro.boot.serialno`, `device-tree/model`, the recovery in use,
`uname -r`, and the resolved `by-name/userdata` and `by-name/metadata` links,
and compares all of them against the identity recorded in the plan bundle.

### Encryption and device mapper

Accepted: direct `userdata`, or exactly one full-length device-mapper mapping
whose only backing device is the resolved `userdata` partition.

Rejected: stacked mappings, mappings with additional slaves, mappings shorter
than the partition, multiple `/data` mounts, and any encryption footer. The
implementation does not attempt to understand the key material; it only checks
the mapping topology and the F2FS superblock geometry.

### F2FS

Accepted geometry matches the official guide: 4096-byte blocks, 512 blocks per
segment, one segment per section, checkpoints enabled and matching.

Rejected: Ext4 or any non-F2FS magic, mismatched superblock copies, block
size ≠ 4096, segment size ≠ 512 blocks, more than one segment per section,
multi-device F2FS, zoned F2FS, and a block count larger than the partition.

`F2FS_IOC_RESIZE_FS` support cannot be proven without the Amethyst recovery
kernel. The tool only checks that F2FS is present in `/proc/filesystems` and
`/sys/fs/f2fs`, and reports the capability verdict as advisory. The ioctl is
issued once during apply, with no retry.

### UFS

UFS is not treated specially. Discovery works from sysfs block topology so that
either `/dev/block/sdX` or `/dev/block/ufsX` naming resolves; no Amethyst-specific
controller path is hard-coded. This is what lets the same helper run on the
synthetic fixture and on a real device.

### Stock GPT layout

The stock Amethyst partition table was **not** available for confirmation. The
tool requires `userdata` to be the last occupied partition and `metadata` plus
everything else to sit before it. If the real layout differs, inspect reports
`RESULT: UNSUPPORTED` and no split is attempted. Forcing a layout designed for a
different device is explicitly out of scope.

## 7. Differences from the official implementation, stated plainly

1. **Apply consumes a reviewed bundle** instead of regenerating a plan. This is a
   deliberate safety difference: the operator reviews exact GPT images before
   anything is written, and apply refuses if the device moved since planning.
2. **Backups are mandatory.** Upstream made them optional; here apply requires
   `acknowledge_data_loss = yes` and an operator-confirmed off-device backup.
3. **The bundle must live off `userdata`.** Apply refuses a plan directory on
   `/data`, `/sdcard`, emulated storage, `/mnt/user` or `/mnt/runtime`, or on a
   filesystem sharing `st_dev` with `userdata`, because apply unmounts `/data`.
4. **Confirmation phrase includes a per-plan random ID,** so a phrase copied from
   an older run cannot authorise a different plan.
5. **Stage is persisted after every step** and excluded from `SHA256SUMS` because
   apply legitimately mutates it.
6. **Metadata is re-hashed after unmount,** immediately before the first GPT
   write, to catch a change that happened while unmounting.
7. **Only the shipped AArch64 helper runs in recovery.** No Python is required on
   the device, unlike the upstream host-driven flow.

Nothing here changes partition arithmetic or F2FS rounding; §5 shows those are
identical.

## 8. Offline test coverage

`tests/run_tests.sh` runs 165 checks. Relevant to this report:

| Area | Coverage |
| --- | --- |
| Upstream reference | vendored hashes pinned, upstream `test_dual_sys.py` (12 tests), upstream `f2fs-shrink.c` self-test, `sh -n twrp.sh`, differential comparison |
| Inspect | supported/unsupported layouts, JSON output, byte-identical disk afterwards |
| Plan | bundle completeness, `SHA256SUMS`, geometry, no writes |
| Apply gating | missing acknowledgement, wrong phrase, tampered bundle, wrong stage, wrong serial |
| Corrupted GPT | primary/backup header CRC, table/backup disagreement, missing protective MBR, existing `unixnu`, no free GPT entry, `userdata` not last |
| Corrupted F2FS | Ext4 magic, bad block size, bad segment size, multi-segment sections, multi-device, oversized block count |
| Busy userdata | surviving `/data` bind mount detected before any GPT write |
| Injected failures | shrink failure, backup write failure, backup readback failure, primary write failure — each stops, persists its stage, and refuses a retry |
| Artifacts | ELF class/machine, static linkage, no test hooks in the shipped helper, ZIP layout and hash pinning, `dist/SHA256SUMS` |

Every corrupted-fixture case asserts that `sda`, `sda3` and `sda2` are unchanged
by inspect and plan.

## 9. Hardware results

| Stage | Result |
| --- | --- |
| Amethyst inspect (read-only) | **NOT RUN** |
| Amethyst plan (read-only) | **NOT RUN** |
| Amethyst apply (destructive) | **NOT RUN** |
| Post-reboot verify | **NOT RUN** |
| Android boot after split | **NOT RUN** |

Details, required evidence and the acceptance criteria for each stage are in
[`HARDWARE-TEST-REPORT.md`](HARDWARE-TEST-REPORT.md).

## 10. Verdict

- **Inspect** reproduces the official layout view without writing anything on
  every offline case tested, and reports `UNSUPPORTED` with a written report
  whenever a precondition fails. Ready for read-only hardware testing.
- **Plan** reproduces the official geometry and F2FS rounding exactly, including
  matching refusals. Ready for hardware comparison against `partition.py plan`.
- **Apply** implements the required operation order, persisting stage and
  refusing retries and mismatched bundles, and it is covered by offline failure
  injection. It remains **not cleared for real devices**: the shrink ioctl,
  decryption layout, unmount behaviour and boot result are unverified on
  Amethyst hardware.

No data-preserving or production-ready claim is made.
