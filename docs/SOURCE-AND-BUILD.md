# Source, provenance and build

This directory is self-contained. Nothing here modifies the existing
`UniXNU`, `UniXNU-amethyst` or other project trees.

## Source layout

| Path | Contents |
| --- | --- |
| `src/common.h` | Shared types, constants and prototypes; tool name/version |
| `src/main.c` | CLI parsing and command dispatch |
| `src/util.c` | Allocation, hashing, file I/O, key/value parsing, path helpers, UUID helpers |
| `src/gpt.c` | GPT parsing, CRC/identity validation, layout calculation, before/after image generation |
| `src/f2fs.c` | F2FS superblock/checkpoint probing and section rounding/shrink ioctl |
| `src/device.c` | Identity, sysfs/block discovery, mount and dm topology checks, metadata hash |
| `src/state.c` | Read-only state collection, inspect report assembly |
| `src/report.c` | Report/log output |
| `src/plan.c` | Request parsing, layout planning, plan bundle writing, bundle verification and matching |
| `src/apply.c` | Gated destructive apply, session/stage log, GPT writes, verify and recover |
| `src/selftest.c` | Host self-tests |
| `installer/update-binary.in` | Recovery shell entry point (ZIP mode is substituted at build time) |
| `installer/updater-script` | Present because recovery installers expect it; no logic |
| `installer/README.txt.in` | In-ZIP readme |
| `examples/` | Sample requests plus inspect/plan output from a synthetic fixture |
| `tests/make_fixture.py` | Synthetic Amethyst-like GPT/F2FS/sysfs fixture |
| `tests/corrupt_fixture.py` | Builds deliberately broken fixture variants for failure-path tests |
| `tests/differential_upstream.py` | Compares this tool's plan with vendored upstream `gpt.py` |
| `tests/run_tests.sh` | Offline test suite (no real block devices) |
| `docs/COMPATIBILITY-REPORT.md` | Package, provenance, build and recovery compatibility report |
| `tests/check_after.py` | Independent GPT/F2FS result checker for the fixture |
| `tests/mock_reboot.py` | Simulates the post-reboot kernel partition view |
| `scripts/build.sh` | Host binary, static helper and three ZIPs |
| `scripts/build-helper-qemu.sh` | Static helper from an Alpine aarch64 rootfs under qemu-user (no Android clang) |

## Upstream provenance

The algorithms are ported from the official UniXNU dual-system sources, vendored
unchanged under `vendor/unixnu-dual_sys/` for study and comparison:

- `partition.py`
- `gpt.py`
- `f2fs-shrink.c`
- `twrp.sh`
- `test_dual_sys.py`
- `dual-system-partition.md`

The vendored files carry `SPDX-License-Identifier: BSD-2-Clause-Patent`. This
project is a C reimplementation for recovery use, not a wrapper around the
Python script. It follows the same safety model: preserve `userdata` identity
and start, shrink through the F2FS ioctl rather than editing allocation
metadata, validate both GPT copies, and require a new unused GPT entry.

Material differences from the host-side parent scripts:

- No Python or ADB is used at runtime; the recovery side is one static AArch64 C
  helper plus a small shell entry point.
- Plan and apply are separate ZIPs. Apply consumes a previously reviewed bundle
  instead of regenerating a plan and applying it in one step.
- The bundle must be off `userdata` because apply unmounts `/data`.
- Apply persists a session ID and stage log and never retries automatically.
- `metadata` is content-hashed before planning, before the shrink, after the
  shrink and again after unmount.
- `unixnu` type/unique GUIDs, LBA range, attributes and label are revalidated
  from both GPT copies before completion.
- `stage` is excluded from `SHA256SUMS`; every immutable bundle input is hashed.

## Build prerequisites

- A C compiler for the host test binary (`cc` by default).
- Android clang with AArch64 support, or another clang that can target
  `aarch64-unknown-linux-musl`.
- An AArch64 musl sysroot. The local build used:

  ```
  /home/bluekyeet/AndroidBuilds/EvolutionX/prebuilts/clang/host/linux-x86/clang-r547379/bin/clang
  /home/bluekyeet/AndroidBuilds/EvolutionX/prebuilts/build-tools/sysroots/aarch64-unknown-linux-musl
  ```

- `zip`, `sha256sum`, `od`, `readelf` and `file` for packaging/checks.
- Optional: `qemu-aarch64` for running the static helper's self-test on the host.

If no Android clang or AArch64 musl sysroot is available, the helper can still
be built from an Alpine aarch64 minirootfs containing `gcc`, `binutils` and
`musl-dev`, executed through a static `qemu-aarch64`:

```sh
ROOTFS=/path/to/alpine-aarch64-rootfs QEMU=/path/to/qemu-aarch64 \
    ./scripts/build-helper-qemu.sh
```

Keep the rootfs and qemu binary under a scratch directory you own (for example
`../.scratch/`), not a shared `/tmp`. On many systems `/tmp` is a small tmpfs;
the test suite builds multi-gigabyte sparse fixtures, and a full tmpfs fails
with `No space left on device` in the middle of a run. `tests/run_tests.sh`
honours `TEST_WORKDIR` and otherwise uses a directory beside the repository.

That script does not use GCC's cross driver, because GCC cannot exec `cc1`, `as`
or `collect2` from a foreign architecture. It invokes `cc1` and `as` directly and
links with the aarch64 `ld` and an explicit crt/libc chain, then checks the ELF
class, machine and absence of `PT_INTERP` and runs `selftest` under qemu.

`scripts/build.sh` auto-detects common `$HOME/AndroidBuilds/*` toolchains. Set
`CLANG` and `SYSROOT` explicitly for other installations.

## Build

```sh
cd dual_boot_system
CLANG=/path/to/clang \
SYSROOT=/path/to/aarch64-unknown-linux-musl \
QEMU_AARCH64=/path/to/qemu-aarch64 \
./scripts/build.sh
```

The script:

1. builds `build/partitioner-host` with `-DUNIXNU_TESTING`, runs its self-test;
2. builds `out/partitioner` with:

   ```sh
   clang --target=aarch64-unknown-linux-musl --sysroot="$SYSROOT" \
       -rtlib=compiler-rt -static -s -O2 -Wall -Wextra -Werror -Isrc ...
   ```

3. rejects a dynamically linked helper or a non-AArch64/non-ELF64 result;
4. optionally runs the helper self-test under `qemu-aarch64`;
5. substitutes the helper SHA-256 and mode into the installer;
6. builds the three ZIPs in `dist/` and writes `dist/SHA256SUMS`.

The shipped helper is built **without** `-DUNIXNU_TESTING`, so no test hook is
present. The host test binary includes the hooks and is never packaged.

## Offline tests

```sh
QEMU_AARCH64=/path/to/qemu-aarch64 ./tests/run_tests.sh
```

The suite creates a temporary fixture, then checks:

- host self-test and static helper identity;
- inspect on supported and unsupported layouts, including JSON validity;
- inspect/plan leave the fixture disk, `userdata` and `metadata` unchanged;
- plan bundle contents, hashes, geometry and stage;
- apply refusals for missing acknowledgement, wrong confirmation, tampered
  bundle, wrong stage and wrong serial;
- authorized apply against a fake shrink hook, including stage log, metadata
  byte-identity and an independent GPT/F2FS result check;
- second-apply refusal and recover output;
- simulated reboot plus `verify`, and rejection of a wrong kernel range;
- ZIP layout, shell syntax, pinned helper hash, mode strings and absence of
  network references.

Superblock handling is covered by these cases: two identical copies, one valid
copy plus one damaged copy in either direction (warning naming the damaged copy),
a copy pair that differs with both valid, and two damaged copies (hard refusal).
A plan records the active superblock offset and damaged copy
(`fs_sb_offset`, `fs_sb_damaged_copy`) and apply refuses if either changes.
`tests/run_tests.sh` asserts all four states through both inspect and plan.

The checkpoint pair follows the same rule for the same reason. F2FS picks the
copy with the higher `checkpoint_ver`, but a torn copy can hold any 64-bit value
there, so a copy is only ranked once it looks like a checkpoint at all
(`user_block_count` and `valid_block_count` within the device). A damaged copy is
named by number and the other copy is used; only a pair with neither usable
refuses. The fixtures set `checkpoint_ver` to `1<<62` on a torn copy so a
version-only comparison selects garbage and the test fails.

Two checkpoint states refuse outright, because shrinking moves the allocation
metadata they describe:

| Flag | Meaning | Reported as |
| --- | --- | --- |
| `CKPT_FLAG_RESIZEFS` | an earlier resize did not finish | `F2FS was left mid-resize; run fsck.f2fs or finish the resize first` |
| `CKPT_FLAG_FSCK` | filesystem is marked for repair | `F2FS is marked for fsck; repair it before shrinking` |

The flag constants live in `src/common.h` so the report and the refusal cannot
drift; `state.c` prints every flag it knows about, then `f2fs.c` decides which
ones stop the run.

Two JSON schemas are versioned independently: `PLAN_SCHEMA` in `src/plan.c` and
`INSPECT_SCHEMA` in `src/state.c` are both `2`. Removing `fs_sb_sha256` and
turning `unsupported` entries into objects changed both shapes, so a consumer of
schema 1 must reject the new output rather than silently miss the fields.

Test-only hooks are enabled with environment variables:

| Variable | Effect |
| --- | --- |
| `UNIXNU_TEST_MOUNTS` | Override `/proc/mounts` |
| `UNIXNU_TEST_STATFS` | Override F2FS statfs values |
| `UNIXNU_TEST_FAKE_SHRINK=1` | Edit the synthetic F2FS superblock instead of calling the ioctl |
| `UNIXNU_TEST_FAKE_UNMOUNT=1` | Simulate unmounting `/data` |

These variables have no effect in the shipped helper.

## Regenerating examples

The checked-in examples were generated from the synthetic fixture, not from a
phone:

```sh
rm -rf /tmp/amethyst-example
python3 tests/make_fixture.py /tmp/amethyst-example
mkdir -p /tmp/amethyst-example/plans
printf 'mode = plan\nsize_gib = 0.0625\nplan_dir = /plans/example-64m\n' \
  >/tmp/amethyst-example/plans/request.conf
UNIXNU_TEST_STATFS=/tmp/amethyst-example/fixtures/statfs \
  build/partitioner-host inspect --root /tmp/amethyst-example \
  --identity /identity.conf --out /reports/inspect.txt
UNIXNU_TEST_STATFS=/tmp/amethyst-example/fixtures/statfs \
  build/partitioner-host plan --root /tmp/amethyst-example \
  --identity /identity.conf --request /plans/request.conf
```

The fixture is 512 MiB with a 64 MiB example split so it stays small. That is
why the example geometry is not representative of a real phone; the output
format is.

## Reproducibility

`zip -X` removes extra ZIP metadata, and the helper is stripped and pinned by
hash, but ZIP entry timestamps and the random plan ID in examples still vary.
Byte-identical ZIP reproduction is not claimed. Compare `dist/SHA256SUMS` for
the artifacts you actually distribute.

## License

BSD-2-Clause-Patent; see `LICENSE` and `NOTICE`. Vendored files retain their own
SPDX headers.
