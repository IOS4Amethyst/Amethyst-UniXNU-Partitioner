# Hardware and test report

Report date: 2026-10-28

## Summary

| Area | Result |
| --- | --- |
| Host build, self-test and offline fixture suite | **PASS** (84/84 checks) |
| Host ASan/UBSan self-test, inspect, plan and fake apply | **PASS** |
| Amethyst hardware inspect | **NOT RUN** |
| Amethyst hardware plan | **NOT RUN** |
| Amethyst hardware apply (authorized destructive) | **NOT RUN** |
| Amethyst Android boot after split | **NOT RUN** |
| Amethyst `unixnu` formatting/boot integration | **NOT RUN** (out of scope) |

**No physical Amethyst device or recovery was available for this work.** All
results below come from host builds and synthetic fixtures. Nothing in this
report establishes that apply preserves data on real hardware, that the stock
recovery kernel supports `F2FS_IOC_RESIZE_FS`, or that the resulting layout
boots Android or UniXNU.

## Environment

- Host: Linux x86_64.
- Static helper: AArch64, musl, statically linked, stripped, built with the
  Android clang and sysroot listed in `docs/SOURCE-AND-BUILD.md`.
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

## Artifact hashes

Built artifacts for this report:

```
b0bae5bbed86e23ae6ed3b46a46fc449ebcbbf1fe1e845b1ed4d2187e63cfa7b  Amethyst-UniXNU-Partitioner-Apply.zip
9b39d403bc0dd3b70986b33d5e891f0fe5321c903982ab1c7e8bf7eb3bfb4080  Amethyst-UniXNU-Partitioner-Inspect.zip
73fd4a36d20ba57938ebb729d8a5105b2e2eb0a19eedca7901ac3c17d29ccbfa  Amethyst-UniXNU-Partitioner-Plan.zip
bb3fcbb793bf85a7380091f5fcb283f3c5a14c3ae6f99b7881ee028e6c9e6274  bin/partitioner (helper inside each ZIP)
```

`dist/SHA256SUMS` is authoritative for distributed copies. Rebuilding changes
the ZIP hashes; do not mix a plan bundle with a different helper version.

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

Expected final line: `TESTS: 84 passed, 0 failed`. This count covers the offline
suite only and is not evidence of hardware success.
