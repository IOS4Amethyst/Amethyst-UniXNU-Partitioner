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
| `tests/run_tests.sh` | Offline test suite (no real block devices) |
| `tests/check_after.py` | Independent GPT/F2FS result checker for the fixture |
| `tests/mock_reboot.py` | Simulates the post-reboot kernel partition view |
| `scripts/build.sh` | Host binary, static helper and three ZIPs |

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
