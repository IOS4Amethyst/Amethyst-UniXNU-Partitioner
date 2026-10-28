# Amethyst UniXNU Partitioner

Experimental recovery-side tooling that creates a `unixnu` GPT partition beside
the existing Android `userdata` partition on the Xiaomi Redmi Note 14 Pro+ 5G
(`amethyst`, SM7635).

**Status: software-tested only. It has not been run on Amethyst hardware.**
Do not treat it as production-ready, and do not assume the split preserves data
until a full hardware test with independent backups has passed. See
[`docs/HARDWARE-TEST-REPORT.md`](docs/HARDWARE-TEST-REPORT.md).

The tool never installs an OS, never formats `unixnu`, and never writes boot
images. It only shrinks F2FS online and rewrites the two GPT copies after a
device-matched plan has been reviewed and explicitly confirmed.

## What it does

1. **Inspect** reads the device identity, storage geometry, GPT, mounts, F2FS
   superblock/checkpoints and recovery capabilities. It writes reports only.
2. **Plan** repeats those checks, computes a tail split, rounds the F2FS size to
   whole sections, and writes a plan bundle containing the original and proposed
   GPT regions. It performs no partition or filesystem writes.
3. **Apply** reloads that bundle, verifies every hash and identity again, shrinks
   F2FS through `F2FS_IOC_RESIZE_FS`, unmounts `/data`, writes and verifies the
   backup GPT, then writes and verifies the primary GPT. It refuses to run unless
   the bundle, device, serial, disk geometry, GPT bytes, F2FS state and metadata
   hash all match and the exact confirmation phrase is supplied.

The split preserves the `userdata` start LBA, type GUID, unique GUID and
attributes. `metadata` is never modified. The new partition uses the Linux
filesystem data type GUID `0fc63daf-8483-4772-8e79-3d69d8477de4` and is left
unformatted so that UniXNU tooling can create its filesystem separately.

## Requirements

- Amethyst (`ro.product.device=amethyst`) with a TWRP-like recovery that provides
  root shell, `sha256sum`, `od`, `unzip`, `awk` and a working decrypted `/data`.
- `/data` mounted read-write as single-device F2FS with checkpoints enabled and
  4096-byte blocks, 512 blocks per segment and one segment per section.
- A recovery kernel that implements `F2FS_IOC_RESIZE_FS`.
- `userdata` is the last occupied GPT partition; an unused GPT entry exists;
  no `unixnu` entry exists.
- The plan/apply bundle must live on storage that is **not** backed by
  `userdata`: an external SD card or `/tmp` in recovery RAM. Apply unmounts
  `/data`, so `/sdcard` and `/data` are explicitly refused.
- An independent backup of important data, `userdata` and `metadata` before any
  apply attempt.

Unsupported conditions stop plan/apply. Inspect still writes a report and marks
`RESULT: UNSUPPORTED` so the reason can be reviewed.

## ZIPs

| ZIP | Purpose | Writes partitions? |
| --- | --- | --- |
| `dist/Amethyst-UniXNU-Partitioner-Inspect.zip` | Read-only report | No |
| `dist/Amethyst-UniXNU-Partitioner-Plan.zip` | Create a plan bundle | No |
| `dist/Amethyst-UniXNU-Partitioner-Apply.zip` | Gated destructive apply, plus read-only `verify`/`recover` modes | Only with a valid plan and confirmation |

Each ZIP contains a statically linked AArch64 helper, a small shell entry point,
sample requests and a short in-ZIP README. The installer checks the helper's
architecture and pinned SHA-256 before executing it and never downloads
anything.

## Quick start

### 1. Back up first

Save the boot/recovery images and, if possible, offline images of `userdata` and
`metadata`. A GPT backup contains no file contents. Restoring an old GPT cannot
undo an F2FS shrink.

### 2. Inspect

Boot recovery, decrypt and mount `/data` read-write, then install
`Amethyst-UniXNU-Partitioner-Inspect.zip`. Read the report under:

```
/sdcard/Amethyst-UniXNU-Partitioner/inspect-<timestamp>.txt
/sdcard/Amethyst-UniXNU-Partitioner/inspect-<timestamp>.json
```

Require `RESULT: SUPPORTED`. If it says `UNSUPPORTED`, stop and read the listed
reasons; do not continue.

### 3. Plan

Create `/sdcard/Amethyst-UniXNU-Partitioner/request.conf`:

```conf
mode = plan
size_gib = 16
plan_dir = /external_sd/Amethyst-UniXNU-Partitioner/plans/amethyst-1
```

`size_gib` must be positive and resolve to whole MiB. `plan_dir` must be an
absolute path on external SD, USB OTG or `/tmp`, never on `/sdcard` or `/data`.
Use the actual mount path shown in TWRP; `/external_sd` is only an example.

Install the Plan ZIP. Review the printed layout and then inspect the bundle:

```
plan.conf  plan.txt  plan.json  plan.env
gpt-primary.before  gpt-backup.before
gpt-primary.after   gpt-backup.after
session  stage  SHA256SUMS
```

Do not edit any file in the bundle. A plan is single-use and contains a random
plan ID, a session ID, the hardware serial and the created GPT images.

### 4. Apply

Copy the bundle to storage that stays writable when `/data` is unmounted (for
example an external SD card or `/tmp`). Replace
`/sdcard/Amethyst-UniXNU-Partitioner/request.conf` with:

```conf
mode = apply
plan = /external_sd/Amethyst-UniXNU-Partitioner/plans/amethyst-1
confirm = unixnu <plan_id> <hardware_serial> <unixnu_bytes>
acknowledge_data_loss = yes
```

Copy `confirm` exactly from `plan.txt` or `plan.conf`. Install the Apply ZIP and
keep power connected. Do not use the recovery UI, reboot, or run anything else
while it works.

Apply rechecks, in order: bundle hashes, stage, identity, serial, disk size and
sector size, `userdata`/`metadata` geometry and GUIDs, both GPT images, F2FS
superblock and checkpoint state, metadata content hash, and the plan's exact
confirmation phrase. Only then does it create a session and start the shrink.

After the shrink it verifies F2FS offline, unmounts `/data`, rechecks metadata,
writes and reads back the backup GPT, writes and reads back the primary GPT, and
validates both copies and partition identities before marking the stage
`complete`.

There is **no automatic retry and no rollback**. If apply stops, keep the plan
bundle and recovery session; do not reboot or repartition. Run the Apply ZIP in
`mode = recover` (or the helper's `recover` command) to see the last persisted
stage and log.

### 5. Verify after reboot

Reboot recovery. Set `request.conf` to a read-only mode:

```conf
mode = verify
plan = /external_sd/Amethyst-UniXNU-Partitioner/plans/amethyst-1
```

Install the Apply ZIP again. `verify` checks both GPT copies, partition
identities, kernel partition ranges and the resized F2FS superblock. It does not
format or create a filesystem in `unixnu`.

Then boot Android and confirm `userdata` still mounts and files are intact.
Create and test the UniXNU filesystem separately; that is outside this tool's
scope.

## Before you touch `unixnu`

This tool only adds the GPT entry. Two things on the UniXNU side cause data loss
if you skip them:

- **`unixnu` is not empty.** It is the tail of the old `userdata` partition and
  still contains whatever userdata bytes were there. It holds no valid
  filesystem, and it is not a copy of your files. Format it before use, and do
  not treat its contents as a backup.
- **Build and flash commands may still target `userdata`.** A device config that
  was written before the split can still name Android's `userdata` as its output
  or install target. If you write a UniXNU image to `userdata`, you destroy
  Android data. Check the target device path (`/dev/block/by-name/unixnu`, not
  `.../userdata`) before every flash, and confirm it against
  `readlink -f /dev/block/by-name/unixnu` in recovery.

A GPT backup cannot restore file contents, and restoring the old GPT cannot undo
the F2FS shrink. A full userdata restore needs the original partition capacity
plus a matching `metadata` backup.

If apply is interrupted, do not delete the session file to rerun it and do not
format unreadable `/data`. Save the stage log and the kernel log (`dmesg`) first,
then have the device maintainer assess the captured state.

## Refusals and safety rules

Apply refuses, without writing to the disk, when any of these fail:

- bundle `SHA256SUMS` or a file changed;
- `stage` is not exactly `planned`;
- plan schema/tool/version, plan ID, session or internal consistency is bad;
- confirmation phrase differs by even one character;
- `acknowledge_data_loss` is absent;
- hardware serial, disk path, disk size or sector size changed;
- `userdata`/`metadata` index or LBA range changed;
- `userdata` type/unique GUID or attributes changed;
- `unixnu` already exists;
- primary or backup GPT bytes differ from the bundle's `before` images;
- F2FS is not mounted read-write with checkpoints enabled;
- F2FS UUID, features, block count or superblock hash changed;
- metadata content hash changed;
- plan bundle is on `/data`, `/sdcard`, emulated storage, or the same
  filesystem as `/data`.

Inspect is safe on unsupported devices: it still writes a report. Plan and apply
abort on unsupported conditions.

## Security and integrity

- The helper is static AArch64, built without test hooks, stripped, and pinned by
  SHA-256 in each ZIP's installer.
- The installer validates ELF64/AArch64 before execution and performs no network
  access.
- Plan metadata and GPT images are mode `0600`; the plan directory is mode
  `0700` and `SHA256SUMS` is mode `0644`.
- `stage` is intentionally excluded from `SHA256SUMS` because apply mutates it.
- The apply session file prevents a second apply of the same plan.
- Offline test hooks are compiled only into the host test binary
  (`-DUNIXNU_TESTING`), never into the shipped helper.

## Build and tests

See [`docs/SOURCE-AND-BUILD.md`](docs/SOURCE-AND-BUILD.md). On a host with an
Android clang and an AArch64 musl sysroot:

```sh
QEMU_AARCH64=/path/to/qemu-aarch64 ./scripts/build.sh
QEMU_AARCH64=/path/to/qemu-aarch64 ./tests/run_tests.sh
```

The offline suite uses synthetic fixtures and must never open a real block
device. Example outputs are in [`examples/`](examples/), including the
disclaimer that they came from a synthetic fixture.

## Known limitations

- No Amethyst hardware validation has been performed.
- The stock Amethyst GPT layout has not been confirmed here. The tool requires
  `userdata` to be the last occupied partition, with `metadata` and every other
  partition before it. If the phone has `metadata` or another partition after
  `userdata`, inspect will report `UNSUPPORTED` and this tool will not split it.
- Only single-device F2FS with 4096-byte blocks and one segment per section is
  supported; multi-device, zoned, Ext4, stacked dm and other layouts are
  rejected.
- The tool cannot recover file contents if F2FS shrink fails. Restoring the old
  GPT does not undo the shrink.
- The tool does not create, format or populate `unixnu`, and does not configure
  UniXNU boot.
- Plan/apply require the exact same tool version, device, serial, GPT and F2FS
  state; updates or Android writes between plan and apply cause refusal.
- The apply bundle must be on external or RAM-backed storage; this is a hard
  requirement, not a suggestion.

## License

BSD-2-Clause-Patent. See [`LICENSE`](LICENSE) and [`NOTICE`](NOTICE). The
vendored UniXNU reference sources retain their SPDX headers.
