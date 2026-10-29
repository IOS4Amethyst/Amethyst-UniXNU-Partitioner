#!/usr/bin/env python3
"""Create a synthetic, self-contained Amethyst-like recovery fixture.

This writes only host files under ROOT. It does not touch any block device.
The GPT, F2FS superblock, checkpoint and sysfs shapes are synthetic; they use a
generic Android-like layout with userdata last and contain no device constants
used by the helper. Run it from the repository root:

    python3 tests/make_fixture.py /tmp/amethyst-fixture
"""
import os
import struct
import sys
import uuid
import zlib

SECTOR = 4096
TOTAL = 131072               # 512 MiB
COUNT = 32
ENTRY = 128
BLOCKS = max(16384, COUNT * ENTRY) // SECTOR
FIRST = 2 + BLOCKS
LAST = TOTAL - 1 - BLOCKS - 1
MIB = 1 << 20

USERDATA_INDEX = 3
METADATA_INDEX = 2
USERDATA_START = 2048
USERDATA_END = LAST
FS_BLOCKS = 128000
FS_BFREE = 110000
FS_BAVAIL = 100000
SERIAL = "FIXTURESERIAL"
DISK_GUID = uuid.UUID("11529365-8d60-a8e2-0bcb-b9cc612bb7d2")
USERDATA_GUID = uuid.UUID("6a4afef7-0d3f-453a-8a48-6373f67ffff0")
METADATA_GUID = uuid.UUID("82acc91f-357c-4a68-9c8f-689e1b1a23a1")
BOOT_GUID = uuid.UUID("2c86e742-745e-4fdd-bfd8-b6a7ac638772")
LINUX_DATA = uuid.UUID("0fc63daf-8483-4772-8e79-3d69d8477de4")


def header(lba, other, table_lba, table_crc):
    h = bytearray(92)
    h[0:8] = b"EFI PART"
    struct.pack_into("<I", h, 8, 0x00010000)
    struct.pack_into("<I", h, 12, 92)
    struct.pack_into("<Q", h, 24, lba)
    struct.pack_into("<Q", h, 32, other)
    struct.pack_into("<Q", h, 40, FIRST)
    struct.pack_into("<Q", h, 48, LAST)
    h[56:72] = DISK_GUID.bytes_le
    struct.pack_into("<Q", h, 72, table_lba)
    struct.pack_into("<I", h, 80, COUNT)
    struct.pack_into("<I", h, 84, ENTRY)
    struct.pack_into("<I", h, 88, table_crc)
    struct.pack_into("<I", h, 16, 0)
    struct.pack_into("<I", h, 16, zlib.crc32(h))
    return bytes(h)


def entry(guid, unique, start, end, attrs, name):
    e = bytearray(ENTRY)
    e[0:16] = guid.bytes_le
    e[16:32] = unique.bytes_le
    struct.pack_into("<QQQ", e, 32, start, end, attrs)
    raw = name.encode("utf-16-le")
    e[56:56 + len(raw)] = raw
    return bytes(e)


def build_gpt():
    table = bytearray(COUNT * ENTRY)
    table[0:ENTRY] = entry(BOOT_GUID, uuid.UUID("11111111-1111-4111-8111-111111111111"),
                           FIRST, 63, 0, "boot")
    table[ENTRY:2 * ENTRY] = entry(METADATA_GUID, METADATA_GUID,
                                   64, 127, 0, "metadata")
    table[2 * ENTRY:3 * ENTRY] = entry(LINUX_DATA, USERDATA_GUID,
                                       USERDATA_START, USERDATA_END, 0, "userdata")
    tcrc = zlib.crc32(table)
    primary = bytearray((2 + BLOCKS) * SECTOR)
    primary[446 + 4] = 0xEE
    struct.pack_into("<I", primary, 446 + 8, 1)
    struct.pack_into("<I", primary, 446 + 12, min(TOTAL - 1, 0xFFFFFFFF))
    primary[510:512] = b"\x55\xaa"
    primary[2 * SECTOR:2 * SECTOR + len(table)] = table
    primary[SECTOR:SECTOR + 92] = header(1, TOTAL - 1, 2, tcrc)
    backup = bytearray((1 + BLOCKS) * SECTOR)
    backup[0:len(table)] = table
    backup[-SECTOR:-SECTOR + 92] = header(TOTAL - 1, 1, TOTAL - 1 - BLOCKS, tcrc)
    return bytes(primary), bytes(backup)


def build_f2fs(blocks):
    sb = bytearray(3072)
    struct.pack_into("<I", sb, 0, 0xF2F52010)
    struct.pack_into("<H", sb, 4, 1)
    struct.pack_into("<H", sb, 6, 15)
    struct.pack_into("<I", sb, 8, 9)     # log_sectorsize
    struct.pack_into("<I", sb, 12, 3)    # log_sectors_per_block
    struct.pack_into("<I", sb, 16, 12)   # 4096-byte blocks
    struct.pack_into("<I", sb, 20, 9)    # 512 blocks per segment
    struct.pack_into("<I", sb, 24, 1)    # one segment per section
    struct.pack_into("<Q", sb, 36, blocks)
    struct.pack_into("<I", sb, 44, blocks // 512)
    struct.pack_into("<I", sb, 48, blocks // 512)
    struct.pack_into("<I", sb, 52, 2)
    struct.pack_into("<I", sb, 56, 2)
    struct.pack_into("<I", sb, 60, 2)
    struct.pack_into("<I", sb, 64, 2)
    struct.pack_into("<I", sb, 68, blocks // 512 - 4)
    struct.pack_into("<I", sb, 72, 512)
    struct.pack_into("<I", sb, 76, 2)    # cp_blkaddr
    sb[108:124] = uuid.UUID("12345678-1234-4234-8234-123456789abc").bytes
    name = "fixture-userdata".encode("utf-16-le")
    sb[124:124 + len(name)] = name
    struct.pack_into("<I", sb, 2180, 0)  # features
    sb[2201] = 0                         # single device
    return bytes(sb)


def build_checkpoint(blocks):
    cp = bytearray(4096)
    struct.pack_into("<Q", cp, 0, 7)                    # checkpoint_ver
    struct.pack_into("<Q", cp, 8, blocks - 4096)        # user_block_count
    struct.pack_into("<Q", cp, 16, 1000)                # valid_block_count
    struct.pack_into("<I", cp, 24, 4)
    struct.pack_into("<I", cp, 28, 8)
    struct.pack_into("<I", cp, 32, blocks // 512 - 4)   # free_segment_count
    struct.pack_into("<I", cp, 132, 0)                  # ckpt_flags
    return bytes(cp)


def write(path, data):
    if isinstance(data, str):
        data = data.encode()
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


def symlink(path, target):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    if os.path.lexists(path):
        os.unlink(path)
    os.symlink(target, path)


def rescale(total):
    """Resize the synthetic disk. The GPT stays valid: header, tables and
    userdata end are all derived from TOTAL, so a larger total only grows the
    userdata tail. The filesystem keeps its real ratio of free space (the
    original fixture is ~78% free), which is what a healthy, lightly used
    userdata looks like at any capacity."""
    global TOTAL, LAST, USERDATA_END, FS_BLOCKS, FS_BFREE, FS_BAVAIL
    TOTAL = total
    LAST = TOTAL - 1 - BLOCKS - 1
    USERDATA_END = LAST
    FS_BLOCKS = (USERDATA_END - USERDATA_START + 1)
    FS_BFREE = FS_BLOCKS * 110000 // 128000
    FS_BAVAIL = FS_BLOCKS * 100000 // 128000


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if len(args) != 1:
        print(__doc__, file=sys.stderr)
        print("usage: make_fixture.py ROOT [--sectors N]", file=sys.stderr)
        return 2
    root = os.path.abspath(args[0])
    if os.path.exists(root):
        print(f"refusing to reuse {root}", file=sys.stderr)
        return 2
    for a in sys.argv[1:]:
        if a.startswith("--sectors="):
            rescale(int(a.split("=", 1)[1]))
    os.makedirs(root)

    disk_bytes = TOTAL * SECTOR
    disk = os.path.join(root, "dev/block/sda")
    os.makedirs(os.path.dirname(disk), exist_ok=True)
    with open(disk, "wb") as f:
        f.truncate(disk_bytes)

    primary, backup = build_gpt()
    with open(disk, "r+b") as f:
        f.write(primary)
        f.seek((TOTAL - 1 - BLOCKS) * SECTOR)
        f.write(backup)

    userdata_bytes = (USERDATA_END - USERDATA_START + 1) * SECTOR
    metadata_bytes = 64 * SECTOR
    sb = build_f2fs(FS_BLOCKS)
    cp = build_checkpoint(FS_BLOCKS)

    userdata = os.path.join(root, "dev/block/sda3")
    with open(userdata, "wb") as f:
        f.truncate(userdata_bytes)
        f.seek(1024)
        f.write(sb)
        f.seek(SECTOR + 1024)
        f.write(sb)
        f.seek(2 * SECTOR)
        f.write(cp)
        f.seek((2 + 512) * SECTOR)
        f.write(cp)

    # Hard link so the decrypted mapping shares storage with raw userdata.
    mapped = os.path.join(root, "dev/block/dm-0")
    if os.path.exists(mapped):
        os.unlink(mapped)
    os.link(userdata, mapped)

    metadata = os.path.join(root, "dev/block/sda2")
    with open(metadata, "wb") as f:
        f.write(bytes((i * 37 + 11) & 0xFF for i in range(metadata_bytes)))

    symlink(os.path.join(root, "dev/block/by-name/userdata"), "../sda3")
    symlink(os.path.join(root, "dev/block/by-name/metadata"), "../sda2")

    os.makedirs(os.path.join(root, "data"), exist_ok=True)

    write(os.path.join(root, "proc/mounts"),
          f"/dev/block/dm-0 /data f2fs rw,seclabel,checkpoint=enable 0 0\n")
    write(os.path.join(root, "proc/filesystems"), "nodev\tf2fs\n")
    write(os.path.join(root, "proc/kallsyms"), "ffffffff81000000 t f2fs_ioctl\n")
    write(os.path.join(root, "proc/device-tree/model"), b"Xiaomi amethyst\x00")
    os.makedirs(os.path.join(root, "sys/fs/f2fs"), exist_ok=True)
    write(os.path.join(root, "fixtures/statfs"),
          f"blocks={FS_BLOCKS}\nbfree={FS_BFREE}\nbavail={FS_BAVAIL}\n")
    write(os.path.join(root, "identity.conf"),
          "device=amethyst\n"
          "model=Fixture Amethyst\n"
          f"serial={SERIAL}\n"
          "recovery=twrp\n"
          "twrp=3.7.0-fixture\n"
          "kernel=6.6.0-fixture\n"
          "machine=aarch64\n"
          "boot_completed=0\n")

    block = os.path.join(root, "sys/devices/fixture/block")
    sda = os.path.join(block, "sda")
    write(os.path.join(sda, "queue/logical_block_size"), f"{SECTOR}\n")
    write(os.path.join(sda, "queue/physical_block_size"), f"{SECTOR}\n")
    write(os.path.join(sda, "size"), f"{TOTAL * SECTOR // 512}\n")
    write(os.path.join(sda, "device/serial"), f"{SERIAL}\n")

    def partition(name, index, start, sectors):
        p = os.path.join(sda, name)
        write(os.path.join(p, "partition"), f"{index}\n")
        write(os.path.join(p, "start"), f"{start * SECTOR // 512}\n")
        write(os.path.join(p, "size"), f"{sectors * SECTOR // 512}\n")
        symlink(os.path.join(root, "sys/class/block", name),
                f"../../devices/fixture/block/sda/{name}")

    partition("sda1", 1, FIRST, 63 - FIRST + 1)
    partition("sda2", 2, 64, 64)
    partition("sda3", USERDATA_INDEX, USERDATA_START, USERDATA_END - USERDATA_START + 1)
    os.makedirs(os.path.join(sda, "sda3/holders"), exist_ok=True)
    symlink(os.path.join(sda, "sda3/holders/dm-0"), "../../../dm-0")

    dm = os.path.join(block, "dm-0")
    os.makedirs(os.path.join(dm, "holders"), exist_ok=True)
    os.makedirs(os.path.join(dm, "slaves"), exist_ok=True)
    symlink(os.path.join(dm, "slaves/sda3"), "../../sda/sda3")
    symlink(os.path.join(root, "sys/class/block/dm-0"),
            "../../devices/fixture/block/dm-0")
    symlink(os.path.join(root, "sys/class/block/sda"),
            "../../devices/fixture/block/sda")

    print(f"fixture: {root}")
    print(f"disk: {TOTAL} x {SECTOR} = {disk_bytes} bytes")
    print(f"userdata: LBA {USERDATA_START}..{USERDATA_END} ({userdata_bytes} bytes)")
    print(f"F2FS: {FS_BLOCKS} blocks, bavail={FS_BAVAIL}")
    print(f"serial: {SERIAL}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
