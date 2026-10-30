# Amethyst UniXNU Partitioner

Experimental recovery-side tooling that creates a `unixnu` GPT partition beside
the existing Android `userdata` partition on the Xiaomi Redmi Note 14 Pro+ 5G
(`amethyst`, SM7635).

**Status: read-only Inspect has run on Amethyst hardware; Plan and Apply have
not.** Do not treat it as production-ready, and do not assume the split preserves
data until a full hardware test with independent backups has passed. See
[`docs/HARDWARE-TEST-REPORT.md`](docs/HARDWARE-TEST-REPORT.md) and
[`docs/COMPATIBILITY-REPORT.md`](docs/COMPATIBILITY-REPORT.md).

On the real device every structural prerequisite passes: `userdata` (`sda33`) is
the last occupied partition, an unused GPT entry exists, and F2FS uses supported
4096-byte blocks. The first run was refused because `/metadata` was mounted and
the filesystem was unclean; the second because the two F2FS superblock copies
disagreed. The second refusal was a tool defect and is fixed here, but no plan or
apply has been run on hardware.

Inspect reports every blocker as `UNSUPPORTED [fixable-in-recovery]` or
`UNSUPPORTED [hard]`, so it is clear whether the operator can clear it or the
layout cannot be split.

The tool never installs an OS, never formats `unixnu`, and never writes boot
images. It only shrinks F2FS online and rewrites the two GPT copies after a
device-matched plan has been reviewed and explicitly confirmed.

## What it does

1. **Inspect** reads the device identity, storage geometry, GPT, mounts, F2FS
   superblock/checkpoints and recovery capabilities. It writes reports only.
2. **Plan** repeats those checks, computes a tail split, rounds the F2FS size to
   whole sections, and writes a plan bundle containing the original and proposed
   GPT regions. It performs no partition or filesystem writes.
3. **Apply** reloads that bundle, verifies every hash and identity again,
   unmounts `/data`, shrinks F2FS through `F2FS_IOC_RESIZE_FS`, writes and
   verifies the backup GPT, then writes and verifies the primary GPT. It refuses
   to run unless the bundle, device, serial, disk geometry, GPT bytes, F2FS state
   and metadata hash all match and the exact confirmation phrase is supplied.

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
/usb_otg/Amethyst-UniXNU-Partitioner/inspect-<timestamp>.txt
/usb_otg/Amethyst-UniXNU-Partitioner/inspect-<timestamp>.json
```

Require `RESULT: SUPPORTED`. If it says `UNSUPPORTED`, read each line:
`fixable-in-recovery` means the operator can clear it (unmount `/metadata`, mount
decrypted `/data`, shut Android down cleanly, expose a shell); `hard` means the
layout itself cannot be split and this tool will not be able to help.

A `Superblocks match: no` warning is normal after an unclean reboot: F2FS keeps
two superblock copies and one may lag. The report names the active copy, and a
plan records it so apply refuses if it changes. Only a mismatch with no valid
F2FS copy in either block is a refusal.

### 3. Plan

Create `/usb_otg/Amethyst-UniXNU-Partitioner/request.conf`:

> **Use the USB OTG stick, not `/sdcard`.** In recovery `/sdcard` is a bind mount
> of the decrypted `userdata`. Writing there makes every ZIP install create a
> holder that apply must remove before it can unmount `/data`, and apply refuses
> to load a bundle from userdata-backed storage anyway. The installer requires
> `/usb_otg` and stops if it is missing rather than falling back.

```conf
mode = plan
size_gib = 16
reserve_mib = 1024
plan_dir = /usb_otg/Amethyst-UniXNU-Partitioner/plans/amethyst-1
```

`size_gib` must be positive and resolve to whole MiB. `plan_dir` must be an
absolute path on the USB OTG stick or `/tmp`, never on `/sdcard` or `/data`.
Use the actual mount path shown in recovery; `/usb_otg` is where this project
expects it.

`reserve_mib` (default `1024`, retained for layout compatibility) leaves an
**unallocated gap after unixnu**, outside both new partition entries. `0` omits
the extra gap. It does **not** reserve space inside Android F2FS or authorize
raw writes there. Split-device sheng Data persistence and log/export must stay
disabled until a dedicated storage allocation is validated. Do not run Apply
based on the earlier, incorrect claim that this gap protects persistence.
Plan records the value in `plan.conf`, `plan.json` and `plan.env`.

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

The bundle must stay readable when `/data` is unmounted: the USB OTG stick or
`/tmp`. Replace
`/usb_otg/Amethyst-UniXNU-Partitioner/request.conf` with:

```conf
mode = apply
plan = /usb_otg/Amethyst-UniXNU-Partitioner/plans/amethyst-1
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

Unmounting means removing **every** mount at or beneath `/data`, deepest first,
not just `/data` itself. A recovery UI bind-mounts `/data/user/0`, `/sdcard` and
`/mnt/runtime/*` onto the decrypted device; a bare `umount2("/data")` is refused
while those exist, and they are the UI's own mounts, so the operator cannot remove
them by hand. Apply takes them down in the kernel's required order. It never uses
`MS_FORCE` or `MNT_DETACH`, and if any step fails it stops and names the mount and
the process holding it.

The order is deliberate. `/data` is unmounted **before** the shrink, because the
resize ioctl rewrites the F2FS superblock while the checkpoint keeps the old
`user_block_count` until the kernel commits one, and it only commits on a clean
unmount. Shrinking first and unmounting second means a busy `/data` can leave the
superblock and the checkpoint disagreeing, with the shrink already done. Doing
the unmount first makes a busy `/data` cost nothing: apply stops at `revalidated`
with the filesystem untouched.

After the shrink apply verifies F2FS offline, rechecks metadata, writes and reads
back the backup GPT, writes and reads back the primary GPT, and validates both
copies and partition identities before marking the stage `complete`.

There is **no automatic retry and no rollback**, but an interrupted apply can be
**resumed**. Run the same Apply request again: the recorded stage is remeasured
against the device instead of being trusted, so apply either continues from where
it stopped or refuses with the reason. Specifically it re-reads the F2FS
superblock (accepting either the planned size or the target size), classifies the
on-disk GPT as original / backup-only / both-written, and skips every step whose
effect is already present. A shrink that already happened is not repeated.

Run the Apply ZIP in `mode = recover` (or the helper's `recover` command) to see
the last persisted stage and log without touching anything.

### 5. Verify after reboot

Reboot recovery. Set `request.conf` to a read-only mode:

```conf
mode = verify
plan = /usb_otg/Amethyst-UniXNU-Partitioner/plans/amethyst-1
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
- `stage` is neither `planned` nor one of the resumable interrupted stages;
- plan schema/tool/version, plan ID, session or internal consistency is bad;
- confirmation phrase differs by even one character;
- `acknowledge_data_loss` is absent;
- hardware serial, disk path, disk size or sector size changed;
- `userdata`/`metadata` index or LBA range changed;
- `userdata` type/unique GUID or attributes changed;
- `unixnu` already exists;
- primary or backup GPT bytes differ from the bundle's `before` images;
- F2FS is not mounted read-write with checkpoints enabled (fresh runs only; a
  resume requires `/data` to be unmounted);
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

Without an Android clang, build the helper from an Alpine aarch64 minirootfs
through qemu-user:

```sh
ROOTFS=/path/to/alpine-aarch64-rootfs QEMU=/path/to/qemu-aarch64 \
    ./scripts/build-helper-qemu.sh
```

That produces `out/partitioner`; the ZIPs are assembled by `scripts/build.sh`
and the archive hashes are recorded in `dist/SHA256SUMS`.

The offline suite uses synthetic fixtures and must never open a real block
device. Example outputs are in [`examples/`](examples/), including the
disclaimer that they came from a synthetic fixture.

## Known limitations

- Inspect has run on Amethyst hardware; Plan and Apply have both been run
  against Amethyst hardware without modifying the device. The F2FS resize has
  not yet succeeded there, so no destructive operation has completed on a real
  device.
- The Amethyst GPT layout is confirmed to be splittable in shape (`userdata` is
  last, entry 34 is free), but an actual split has never been performed.
- The Amethyst recovery kernel has not been shown to implement
  `F2FS_IOC_RESIZE_FS`. The device's recovery is not TWRP and hides kallsyms, so
  the capability probe is inconclusive; Apply's own ioctl is the real test.
- F2FS validation follows UniXNU `scripts/dual_sys/f2fs-shrink.c`: the two
  superblock copies must be byte-identical, and the checkpoint is never read.
  The checkpoint is kernel-owned metadata that a live mount rewrites, so a
  userspace pre-read races the kernel and can observe a torn pair; reading it
  refused healthy filesystems twice on real hardware and proved nothing the
  resize needs. The resize ioctl is the authority on whether the kernel will
  shrink the filesystem; if it refuses, the GPT is left untouched.
- Because the checkpoint is not inspected, a filesystem carrying `FSCK` or
  `RESIZEFS` flags is not refused in advance. The ioctl fails safely instead,
  and Apply aborts before writing either GPT copy.
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
