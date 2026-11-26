# Amethyst UniXNU Partitioner

Experimental recovery-side tooling that creates a `unixnu` GPT partition beside
the existing Android `userdata` partition on the Xiaomi Redmi Note 14 Pro+ 5G
(`amethyst`, SM7635).

**Status:** version 0.5.1 (the helper embedded in `dist/amethyst-unixnu.sh`)
completed inspect, plan and apply on an Amethyst. It is still experimental: keep
independent backups of `userdata` and `metadata` before every apply. See
[`docs/HARDWARE-TEST-REPORT.md`](docs/HARDWARE-TEST-REPORT.md) and
[`docs/COMPATIBILITY-REPORT.md`](docs/COMPATIBILITY-REPORT.md).

The whole tool is now **one file**, `dist/amethyst-unixnu.sh`, run from a root
shell inside recovery (`adb shell` or the recovery's terminal). It is not a
flashable ZIP. A guided run does inspect, plan and apply in one go and asks
before it writes anything.

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
  root shell, `sha256sum`, `od`, `tail`, `awk` and a working decrypted `/data`.
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

## The script

`dist/amethyst-unixnu.sh` is a shell front end with the statically linked
AArch64 helper appended to it. On start it copies itself to `/tmp` (so it never
holds `/sdcard` open), extracts the helper to RAM, checks it is ELF64/AArch64
and matches the pinned SHA-256, and only then runs it. It never downloads
anything.

| Command | What it does | Writes partitions? |
| --- | --- | --- |
| `sh amethyst-unixnu.sh` | Guided: inspect, plan, then apply after you type `APPLY` | Only after confirmation |
| `sh amethyst-unixnu.sh inspect` | Read-only report | No |
| `sh amethyst-unixnu.sh plan [--size GIB] [--reserve MIB]` | Create a plan bundle | No |
| `sh amethyst-unixnu.sh apply [--plan DIR]` | Apply a plan (newest plan by default) | Yes |
| `sh amethyst-unixnu.sh verify [--plan DIR]` | Read-only check after reboot | No |
| `sh amethyst-unixnu.sh recover [--plan DIR]` | Read-only stage/log summary | No |
| `sh amethyst-unixnu.sh selftest` | Helper's built-in tests | No |

Reports, plans and a session log go to
`<dir>/Amethyst-UniXNU-Partitioner/`, where `<dir>` is `--dir DIR` if given,
otherwise `/usb_otg`, then `/external_sd`, then `/tmp`. `/sdcard` and `/data`
are refused: in recovery they are the decrypted `userdata`, which apply
unmounts. A plan in `/tmp` is lost on reboot, so use a USB OTG stick or SD card
if you want to verify or resume after a reboot.

## Quick start

### 1. Back up first

Save the boot/recovery images and, if possible, offline images of `userdata` and
`metadata`. A GPT backup contains no file contents. Restoring an old GPT cannot
undo an F2FS shrink.

### 2. Boot recovery and get a shell

In recovery: decrypt `/data` and make sure it is mounted read-write, unmount
`/metadata` if the recovery mounted it, and mount the USB OTG stick if you have
one. Then from the PC:

```sh
adb push dist/amethyst-unixnu.sh /tmp/
adb shell
cd /
sh /tmp/amethyst-unixnu.sh
```

Or copy the file to the OTG stick and run `sh /usb_otg/amethyst-unixnu.sh` from
the recovery's terminal. Stay out of `/sdcard` and `/data` in that shell (`cd /`
first): apply cannot unmount `/data` while your shell is sitting in it, and the
script warns if it is.

### 3. Follow the guided run

1. **Inspect** runs read-only and prints `RESULT: SUPPORTED` or the blockers.
   `fixable-in-recovery` means you can clear it (unmount `/metadata`, mount
   decrypted `/data`, shut Android down cleanly); `hard` means the layout cannot
   be split. A `Superblocks match: no` warning after an unclean reboot is normal.
2. **Plan** asks for the `unixnu` size in GiB (16 suggested; fractions must
   resolve to whole MiB) and writes a bundle under `plans/plan-<timestamp>/`.
   It makes no partition or filesystem writes. Review the printed layout.
3. **Apply** shows `plan.txt` and the plan's confirmation phrase. Type `APPLY`
   to run it; anything else cancels with nothing changed. Keep power connected
   and do not touch the recovery UI until it finishes.

You can also run each step on its own (`inspect`, `plan`, `apply`), for example
to plan now and apply later. `--size` and `--reserve` skip the prompts in
`plan`. `reserve_mib` (default `1024`) leaves an unallocated gap after `unixnu`;
it does **not** reserve space inside Android F2FS, and split-device sheng Data
persistence must stay disabled until a dedicated allocation is validated.

### How apply works

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

The resize itself needs a **mounted** F2FS instance: `F2FS_IOC_RESIZE_FS` acts on
the in-memory filesystem object, not on a bare block device, and calling it on an
unmounted fd reaches the kernel's resize path with no superblock to work on. So
apply mounts the decrypted mapping at a private `mkdtemp` directory, resizes
through that mount, and unmounts it. `/data` stays unmounted throughout, and the
unmount is what commits the checkpoint that makes the new size survive a remount.

After the shrink apply verifies F2FS offline, rechecks metadata, writes and reads
back the backup GPT, writes and reads back the primary GPT, and validates both
copies and partition identities before marking the stage `complete`.

There is **no automatic retry and no rollback**, but an interrupted apply can be
**resumed**. Run `apply` again with the same plan: the recorded stage is remeasured
against the device instead of being trusted, so apply either continues from where
it stopped or refuses with the reason. Specifically it re-reads the F2FS
superblock (accepting either the planned size or the target size), classifies the
on-disk GPT as original / backup-only / both-written, and skips every step whose
effect is already present. A shrink that already happened is not repeated.

`sh amethyst-unixnu.sh recover` shows the last persisted stage and log without
touching anything. If apply stops, the script prints that summary for you.

### 4. Verify after reboot

Reboot to recovery and run:

```sh
sh /tmp/amethyst-unixnu.sh verify --plan /usb_otg/Amethyst-UniXNU-Partitioner/plans/plan-<timestamp>
```

`verify` checks both GPT copies, partition identities, kernel partition ranges
and the resized F2FS superblock. It does not format or create a filesystem in
`unixnu`.

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
  SHA-256 in the script header.
- The script validates ELF64/AArch64 before execution and performs no network
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

That produces `out/partitioner`. `scripts/build.sh` (or, for an existing
helper, `scripts/make-terminal-script.sh`) appends it to
`installer/amethyst-unixnu.sh.in` to make `dist/amethyst-unixnu.sh`, and
records its hash in `dist/SHA256SUMS`.

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
