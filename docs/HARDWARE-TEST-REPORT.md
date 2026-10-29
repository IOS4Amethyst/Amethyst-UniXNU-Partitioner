# Hardware and test report

Report date: 2026-10-28

## 1. Amethyst hardware: read-only Inspect runs

Two read-only Inspect runs were executed on the real device. Nothing was
written, resized or repartitioned, and no plan or apply was attempted.

Device and recovery as reported by the helper:

```
Device                  : amethyst
Model                   : Redmi Note 14 Pro+ 5G
Hardware serial         : 42fe0af2
Recovery                : OrangeFox R12.0 (reports itself as "recovery")
Kernel                  : 6.1.138-android14-Wild, aarch64
Storage                 : /dev/block/sda, 4096-byte logical sectors
Disk size               : 509859594240 bytes (124477440 sectors)
userdata                : /dev/block/sda33, index 33, LBA 2539520..124477434
metadata                : /dev/block/sda21, index 21, LBA 16384..32767
GPT                     : 33 of 64 entries, first unused entry 34
```

Every structural prerequisite for the split passed on hardware:

| Check | Result |
| --- | --- |
| `userdata` is the last occupied partition | PASS (`sda33` ends at the last usable LBA) |
| `userdata` start alignment | PASS (4096-byte aligned) |
| `unixnu` entry already exists | PASS (no) |
| Unused GPT entry | PASS (entry 34) |
| Primary/backup GPT CRCs and entry array CRC | PASS (identical across both runs) |
| F2FS geometry | PASS (4096-byte blocks, 512 blocks/segment, 1 segment/section) |
| `userdata` encryption footer vs mapping length | PASS (no footer: the mapping equals the partition) |
| Disk and sector geometry | PASS (4096-byte sectors, whole-sector disk size) |

### Run 1: mounted metadata and a dirty filesystem

`RESULT: UNSUPPORTED`. Blockers, in the helper's own wording:

```
WARNING: F2FS checkpoint copies differ; the filesystem may be unclean
WARNING: F2FS encrypt feature is set; recovery must decrypt /data before planning
WARNING: the read-only F2FS resize capability probe was inconclusive
UNSUPPORTED: metadata is mounted
```

`/metadata` was mounted at `/dev/block/sda21` because OrangeFox had mounted it,
and the F2FS checkpoint copies disagreed, which means Android was not shut down
cleanly before recovery started. Two further probes reported `unknown`:

```
F2FS ioctl namespace        : unknown
GET_FEATURES ioctl          : no/unknown
resize symbol in kallsyms   : no/unknown
```

The recovery image is not TWRP (`getprop ro.twrp.version` is empty), so the
kallsyms probe and the `F2FS_IOC_GET_FEATURES` probe cannot be trusted as
evidence either way. Neither is a statement about `F2FS_IOC_RESIZE_FS`.

### Run 2: unmounted, but the filesystem could not be read

`RESULT: UNSUPPORTED`, for exactly one reason:

```
UNSUPPORTED: filesystem: F2FS superblock copies disagree
```

This run had `/data` unmounted (`Decrypted /data mapping : none`) and
`/metadata` unmounted. The helper then read the raw `userdata` block device, and
the two superblock copies at offsets 1024 and 5120 did not agree. Because
version 0.1.0 compared both copies and refused on any difference, the helper
reported the filesystem as unreadable instead of analysing it.

That refusal is a defect in the tool, not a property of the phone. The Linux
F2FS mount path only requires the second superblock to carry the magic number
and prefers the newer copy; it does not require the copies to be identical.
`vendor/unixnu-dual_sys/f2fs-shrink.c`, the upstream reference this project is
validated against, rejects mismatched superblocks, so the stricter behaviour was
intentional, but it produces an unusable verdict rather than an actionable one.

### Run 3: the fixed helper reads the filesystem

After the superblock fix, a third read-only Inspect completed with
`RESULT: SUPPORTED WITH WARNINGS`. No `UNSUPPORTED` condition remained:

```text
Superblock copies match     : yes
Checkpoint copies match     : no
Active checkpoint copy      : first (higher checkpoint_ver)
Checkpoint version          : 393237468
Checkpoint flags            : 0x00000044
Features                    : 0x00001499 encrypt
```

The superblocks are in fact byte-identical, so Run 2's refusal was the tool
reading a stale view of the pair, not a damaged device. `0x00000044` decodes as
`CP_COMPACT_SUM_FLAG|CP_CRC_RECOVERY_FLAG`, both benign, so the new
`RESIZEFS`/`FSCK` refusals would not have fired here.

The three warnings are the ones that remain, and all three are expected for an
unclean shutdown: checkpoint copies differ, the encrypt feature is set, and the
resize capability probe is inconclusive because OrangeFox hides kallsyms.

Capacity on this device is comfortable: 465.156 GiB of `userdata` with
112,279,460 blocks available (~428 GiB free), against 9,066,588 valid blocks
(~34.5 GiB) of real data.

### Tool changes prompted by these runs

- A superblock mismatch with a valid second copy is now a warning that names the
  active offset (`fs_sb_offset`, `Superblocks match`), not a refusal. A plan
  records the active offset and apply refuses if it changes.
- A mismatch where neither copy carries the F2FS magic is still a hard refusal
  (`userdata is not F2FS`), and one damaged copy is reported separately.
- Every unsupported condition is now tagged `fixable-in-recovery` (operator can
  clear it: mount state, serial, unreadable device) or `hard` (layout cannot
  support the split), in both the text and JSON reports.
- The F2FS checkpoint copy is chosen by validity before version, and a damaged
  copy is named by number, for the same reason as the superblock pair.
- `CKPT_FLAG_RESIZEFS` and `CKPT_FLAG_FSCK` now refuse rather than being defined
  and ignored, since both describe allocation metadata that shrinking moves.

### Fourth run: the checkpoint fix verified on hardware

Run 4 (`2026-10-29T06:29:54Z`) was the first with the validity-before-version
checkpoint selection. It chose the other copy, confirming the fix against real
bytes:

| | Run 3 (05:26) | Run 4 (06:29) |
| --- | --- | --- |
| Active checkpoint copy | first (stale) | **second** |
| Checkpoint version | 393237468 | 393237689 |
| Valid blocks | 9,066,588 | 9,059,756 |
| Free blocks | 112,279,460 | 112,286,292 |

Run 3 was selecting a copy 221 checkpoints stale. Android wrote between the
runs, so the version advanced and valid blocks dropped by ~6,800, as expected
for an idle recovery. Result stayed `SUPPORTED WITH WARNINGS` with no
`UNSUPPORTED` condition. `Checkpoint user blocks` (121,378,816) is the
filesystem's *total* capacity counter, not free space; statfs `available`
(112,286,292) is the free figure.

### Correction: `reserve_mib` is an unallocated tail, not persistence protection

`size_gib` is chosen by the operator. `reserve_mib` (default 1024) leaves an
unallocated gap above unixnu, not space inside the new userdata entry. The
previous statement that this protects sheng's Data/log writes was incorrect.
Those writes would target Android F2FS. Split-device persistence is now disabled
in the UFS component pending a dedicated validated allocation.
The following offline numbers used a scaled synthetic fixture with start LBA
2048, NOT the device's actual userdata start 2539520:

| Request | `unixnu` LBAs | `unixnu` | userdata left |
| --- | --- | --- | --- |
| 128 GiB, reserve 0 | 90922752..124477183 | 128.000000 GiB | 346.83 GiB |
| 128 GiB, reserve 1024 | 90660608..124215039 | 128.000000 GiB | 345.83 GiB |

Both shrink userdata's end to `unixnu_start - 1`. The gap after unixnu is outside
both partitions. The original end 124477434 is recorded only as before-state.
`reserve_mib = 0` reproduces the upstream reference layout byte-for-byte.

### What is still unproven on hardware

| Area | Result |
| --- | --- |
| Inspect on hardware | **PASS** (read-only, both runs) |
| Plan on hardware | **NOT RUN** |
| Apply on hardware | **NOT RUN** |
| Android boot after a split | **NOT RUN** |
| `unixnu` formatting/boot integration | **NOT RUN** (out of scope) |

A hardware pass still requires a clean shutdown, a Plan bundle reviewed off
`userdata`, one authorized Apply, a post-reboot `verify`, and an Android boot
with files intact. Until then this project makes no data-preserving claim.

## 2. Host and offline results

| Area | Result |
| --- | --- |
| Host build, self-test and offline fixture suite | **PASS** (195/195 checks) |
| Host ASan/UBSan self-test, inspect, plan and fake apply | **PASS** |
| Amethyst hardware inspect | **PASS**, read-only, `SUPPORTED WITH WARNINGS` (§1 Run 3) |
| Amethyst hardware plan | **NOT RUN** |
| Amethyst hardware apply (authorized destructive) | **NOT RUN** |
| Amethyst Android boot after split | **NOT RUN** |
| Amethyst `unixnu` formatting/boot integration | **NOT RUN** (out of scope) |

**No destructive operation has run on an Amethyst device.** The hardware results
in §1 are read-only Inspect runs. Nothing in this report establishes that apply
preserves data on real hardware, that the Amethyst recovery kernel implements
`F2FS_IOC_RESIZE_FS`, or that the resulting layout boots Android or UniXNU.

## Environment

- Host: Linux x86_64.
- Static helper: AArch64, musl, statically linked, stripped. It is built with
  the Android clang if one is present, otherwise with the Alpine aarch64
  toolchain under `qemu-user` as described in `docs/SOURCE-AND-BUILD.md`.
- Helper self-test executed under `qemu-aarch64`.
- Fixture: synthetic 512 MiB disk, 4096-byte sectors, 32 GPT entries,
  `boot`/`metadata`/`userdata`, single-device F2FS with checkpoints, serial
  `FIXTURESERIAL`.
- No commands in the suite open a real block device.

## Host and offline results

| Check | Result | Evidence |
| --- | --- | --- |
| Host build with `-Wall -Wextra -Werror` | PASS | `scripts/build.sh` |
| Host `selftest` | PASS | prints `SELFTEST PASS` |
| Static helper is ELF64 AArch64 and has no `PT_INTERP` | PASS | `file`/`readelf` in build script and tests |
| Static helper `selftest` under QEMU | PASS | prints `SELFTEST PASS` |
| ASan/UBSan self-test | PASS | no leaks/errors after fixing self-test cleanup |
| ASan/UBSan inspect + plan + fake apply | PASS | no sanitizer reports |
| Inspect on supported fixture | PASS | `RESULT: SUPPORTED` |
| Inspect JSON parses | PASS | `python3 -m json.tool` |
| Inspect on unsupported identity still writes a report | PASS | `RESULT: UNSUPPORTED` |
| Inspect leaves disk, `userdata`, `metadata` byte-identical | PASS | SHA-256 comparison |
| Plan on supported fixture | PASS | complete bundle and `stage=planned` |
| Plan leaves disk, `userdata`, `metadata` byte-identical | PASS | SHA-256 comparison |
| Plan geometry preserves `userdata` start and splits the tail | PASS | fixture LBA 2048, split at 114432 |
| Plan refuses oversized request and creates no bundle | PASS | refusal test |
| Apply refuses missing data-loss acknowledgement | PASS | no disk writes |
| Apply refuses wrong confirmation phrase | PASS | no disk writes |
| Apply refuses tampered bundle | PASS | `SHA256SUMS` mismatch |
| Apply refuses non-`planned` stage / completed bundle | PASS | stage check |
| Apply refuses wrong hardware serial | PASS | identity check |
| All apply refusals leave disk, `userdata`, `metadata` unchanged | PASS | SHA-256 comparison |
| Authorized fake-shrink apply completes | PASS | stage `complete` |
| Apply log records every required stage | PASS | `apply.log` |
| `metadata` byte-identical after apply | PASS | SHA-256 comparison |
| Independent GPT/F2FS result checker | PASS | `tests/check_after.py` |
| Second apply refuses completed bundle | PASS | no retry |
| `recover` reports the completed session | PASS | stage log |
| Simulated reboot + `verify` | PASS | both GPT copies, kernel ranges, F2FS block count |
| `verify` rejects wrong kernel `unixnu` range | PASS | refusal test |
| ZIP layout, shell syntax, helper hash pinning, mode strings | PASS | all three ZIPs |
| No network references in installer | PASS | grep test |
| `dist/SHA256SUMS` verifies | PASS | `sha256sum -c` |

The offline apply uses `UNIXNU_TEST_FAKE_SHRINK=1` and
`UNIXNU_TEST_FAKE_UNMOUNT=1`; it edits a synthetic F2FS superblock and does not
call the real ioctl or unmount a real filesystem. It proves control flow, gating,
GPT image handling and independent consistency checks, not kernel behavior.

## Upstream agreement and failure-path results

| Check | Result | Evidence |
| --- | --- | --- |
| Vendored upstream files match pinned revision `3ef9721` | PASS | SHA-256 manifest check |
| Upstream `test_dual_sys.py` | PASS (12 tests) | run from `vendor/unixnu-dual_sys` |
| Upstream `f2fs-shrink.c` self-test | PASS | prints `PASS: F2FS section rounding` |
| Upstream `twrp.sh` parses | PASS | `sh -n` |
| Plan equals upstream `gpt.py` for identical geometry | PASS | 23/23 checks, both GPT images byte-identical |
| Plan agrees with upstream across 1–430 MiB | PASS | 0 disagreements, including matching refusals |
| Corrupt primary/backup GPT header CRC | PASS | refuses, disk unchanged |
| Mismatching primary and backup GPT | PASS | refuses, disk unchanged |
| Missing protective MBR signature | PASS | refuses, disk unchanged |
| Existing `unixnu` entry | PASS | refuses, disk unchanged |
| No free GPT entry | PASS | refuses, disk unchanged |
| `userdata` not the last occupied partition | PASS | refuses, disk unchanged |
| Ext4 instead of F2FS | PASS | refuses, disk unchanged |
| Unsupported F2FS block/segment/section geometry | PASS | refuses, disk unchanged |
| Multi-device F2FS | PASS | refuses, disk unchanged |
| F2FS block count larger than the partition | PASS | refuses, disk unchanged |
| One F2FS superblock copy damaged | PASS | warns, names the active copy, continues read-only |
| Both F2FS superblock copies damaged | PASS | refuses, disk unchanged |
| One F2FS checkpoint copy torn, with a higher version | PASS | uses the valid copy, reports its version |
| F2FS checkpoint flagged mid-resize | PASS | refuses before any write |
| F2FS checkpoint flagged for fsck | PASS | refuses before any write |
| Unsupported condition tagged `fixable-in-recovery` or `hard` | PASS | text and JSON reports |
| Surviving `/data` bind mount | PASS | apply stops at `f2fs-verified`, GPT untouched |
| Injected F2FS resize failure | PASS | stage `f2fs-shrink-started`, GPT untouched, no retry |
| Injected backup GPT write failure | PASS | stage `gpt-backup-writing`, GPT untouched, no retry |
| Injected backup GPT readback failure | PASS | stage `gpt-backup-writing`, no retry |
| Injected primary GPT write failure | PASS | stage `gpt-primary-writing`, no retry |
| Shipped helper contains no test hooks | PASS | `strings` check |

## Artifact hashes

Built artifacts for this report:

```
95a8c849c4ceaf77832451017bdef0b835dec8e3bd701c56fe51af9352d0dedb  Amethyst-UniXNU-Partitioner-Apply.zip
a34be661aa3e6222158c57cb4bc10e6544caa34059a7a743f306e7b882e5ac77  Amethyst-UniXNU-Partitioner-Inspect.zip
5ca924e86c0a610e762735f56cfa0b944186b8737c9b3eb61606557783dfaf42  Amethyst-UniXNU-Partitioner-Plan.zip
9227c8beefbf9da3dfaef3816db4a4c3a78f11e7d928f42a6be209459de3907f  bin/partitioner (helper inside each ZIP)
```

Helper bytes: 198352. This build came from the Alpine aarch64 toolchain under
`qemu-user`, so its hash differs from the Android-clang build recorded in the
previous revision of this report even though the source is the same plus the
changes in §1. Rebuild with `scripts/build-helper-qemu.sh` (or `scripts/build.sh`
where an Android clang exists) and publish the new `dist/SHA256SUMS` with it.

The helper hash is reproducible. The ZIP hashes are not: ZIP entry timestamps
vary between builds, so a rebuild produces different archive bytes with
identical contents. Compare the helper hash, and `dist/SHA256SUMS` for the
archives you actually distribute. Do not mix a plan bundle with a different
helper version.

## What must still be tested on hardware

Perform these steps only on an Amethyst with independent backups and a recovery
image you can restore. Record serial, recovery version, kernel version, exact
commands and outputs.

1. **Recovery inventory.** Confirm TWRP-like recovery, root shell, required
   commands, `/data` decryption and read-write mount. Save `getprop`,
   `/proc/mounts`, `uname -r`, `blockdev --getss` and `--getsize64`.
2. **Inspect only.** Install the Inspect ZIP. Require `RESULT: SUPPORTED` and
   retain the text and JSON reports. Confirm the reported serial, disk, sector
   size and LBA ranges match the device. Inspect must not change any SHA-256 of
   the first and last GPT regions or of `metadata`.
3. **Plan only.** Create a small trial request, install the Plan ZIP, verify
   `SHA256SUMS`, and independently review `plan.txt`, `plan.json`,
   `gpt-primary.after` and `gpt-backup.after`. Confirm `userdata` keeps its start,
   type GUID, unique GUID and attributes, and that `metadata` and all unrelated
   entries are unchanged. Do not apply yet.
4. **Authorized destructive apply.** With backups verified, copy the bundle off
   `userdata`, set the exact confirmation phrase and
   `acknowledge_data_loss = yes`, and run Apply once. Do not interrupt it.
5. **Post-failure behavior.** If apply stops, do not retry. Capture `recover`
   output and the full `apply.log`, keep the recovery session, and determine
   whether the shrink completed and which GPT copy was written.
6. **Post-reboot verify.** Reboot recovery, run `mode = verify`, and independently
   compare GPT and F2FS results.
7. **Android boot and file check.** Boot Android, confirm `userdata` mounts and
   read known files. Confirm `metadata` mounts and the device behaves normally.
8. **UniXNU step.** Create and test the `unixnu` filesystem and boot
   configuration separately. The partitioner leaves `unixnu` unformatted.

A hardware pass requires all of: inspect supported with exact identity match;
plan geometry/identity review; apply completion with both GPT copies and F2FS
verified; post-reboot verify pass; Android boot with intact files; and a record
of independent backup/restore. Until then, this project makes no
data-preserving or production-ready claim.

## Known failure modes and limits

- F2FS shrink may fail after moving data. The GPT is unchanged at that point,
  but file contents may already be affected; there is no rollback.
- The GPT backup contains partition-table bytes, not file contents. Restoring it
  cannot undo an F2FS shrink.
- Interrupting GPT writes can leave mismatched primary/backup copies. The tool
  writes and verifies the backup first, then the primary, but power loss can
  still interrupt either write.
- The tool refuses if Android or any process changed F2FS, GPT, metadata or the
  plan bundle between planning and apply.
- The tool does not support Ext4, multi-device/zoned F2FS, stacked mappings,
  encryption footers, multiple segments per section, intervening partitions, or
  a `unixnu` that already exists.
- The stock Amethyst GPT layout was not available for confirmation. The tool
  requires `userdata` to be the last occupied partition and all other
  partitions, including `metadata`, to be before it. If that is not the case on
  the phone, inspect reports `UNSUPPORTED` and no split is attempted.
- The tool does not format, populate or boot `unixnu`.

## Reproduce the offline results

```sh
cd dual_boot_system
QEMU_AARCH64=/path/to/qemu-aarch64 ./scripts/build.sh
QEMU_AARCH64=/path/to/qemu-aarch64 ./tests/run_tests.sh
```

Expected final line: `TESTS: 195 passed, 0 failed`. This count covers the offline
suite only and is not evidence of hardware success.
