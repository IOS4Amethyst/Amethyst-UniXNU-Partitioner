#!/usr/bin/env python3
"""Corrupt a fixture copy in one specific way, for offline failure-path tests.

Every mode writes a fresh copy under DEST first, so the pristine fixture is
never modified. This never opens a real block device: fixtures are ordinary
files created by tests/make_fixture.py.

Usage:
    python3 tests/corrupt_fixture.py SRC DEST MODE

Modes:
    primary-crc       flip a byte inside the primary GPT header CRC field
    backup-crc        flip a byte inside the backup GPT header CRC field
    primary-table     flip a byte in the primary partition entry array
    backup-disagree   change the backup header's disk GUID so copies disagree
    mbr-missing       clear the 0x55AA boot signature in the protective MBR
    add-unixnu        append an existing `unixnu` entry
    fill-entries      occupy every GPT entry so none is free
    userdata-not-last insert an occupied partition after userdata
    f2fs-ext4         replace the F2FS magic so userdata looks like another FS
    f2fs-bad-block    set log_blocksize to 11 (2048-byte blocks)
    f2fs-bad-seg      set log_blocks_per_seg to 10 (1024 blocks per segment)
    f2fs-multi-seg    set segments_per_section to 2
    f2fs-multi-dev    set the devs[0].path field so F2FS looks multi-device
    f2fs-oversize     claim more F2FS blocks than userdata can hold
    f2fs-sb-first-damaged   clear the first superblock copy's magic
    f2fs-sb-second-damaged  clear the second superblock copy's magic
    f2fs-sb-both-damaged    clear both superblock magics
    f2fs-sb-field-differs   both superblocks valid but one harmless byte differs
    f2fs-cp-first-damaged   torn first checkpoint with a higher version
    f2fs-cp-resizefs-flag   set CKPT_FLAG_RESIZEFS (interrupted resize)
    f2fs-cp-fsck-flag       set CKPT_FLAG_FSCK (needs repair)
    busy-bind         add a bind mount of the decrypted device under /data
"""
import os
import shutil
import struct
import sys
import uuid
import zlib
from pathlib import Path

SECTOR = 4096                       # fixture sector size, see tests/make_fixture.py
LINUX_DATA = uuid.UUID('0fc63daf-8483-4772-8e79-3d69d8477de4').bytes_le


def set_entry_name(entry, name):
    """Write a GPT entry name into its fixed 72-byte UTF-16LE name field."""
    if len(name) > 36:
        raise SystemExit(f'partition name too long: {name}')
    data = name.encode('utf-16-le')
    entry[56:56 + len(data)] = data


def read_region(path, offset, length):
    with open(path, 'rb') as f:
        f.seek(offset)
        data = f.read(length)
    if len(data) != length:
        raise SystemExit(f'short read at {offset} ({len(data)} of {length})')
    return bytearray(data)


def write_region(path, offset, data):
    with open(path, 'r+b') as f:
        f.seek(offset)
        f.write(bytes(data))


def locate_checkpoints(path):
    """Return the byte offsets of both F2FS checkpoint copies.

    cp_blkaddr and blocks_per_seg come from the superblock, so the fixture
    follows whatever the generator wrote instead of duplicating its numbers.
    """
    sb = read_region(path, 1024, 3072)
    if struct.unpack_from('<I', sb, 0)[0] != 0xf2f52010:
        raise SystemExit('fixture userdata is not F2FS')
    cp_blkaddr = struct.unpack_from('<I', sb, 76)[0]
    blocks_per_seg = 1 << struct.unpack_from('<I', sb, 20)[0]
    block = 1 << struct.unpack_from('<I', sb, 16)[0]
    first = cp_blkaddr * block
    return cp_blkaddr, (first, first + blocks_per_seg * block)


def load_consts(path):
    """Read geometry out of the fixture's own GPT instead of assuming values."""
    hdr = read_region(path, SECTOR, 92)
    sig, _ver, _len, _crc, _res, _here, _other, first, last, _guid, table, count, \
        width, _tcrc = struct.unpack('<8sIIIIQQQQ16sQIII', hdr)
    if sig != b'EFI PART':
        raise SystemExit('source fixture has no GPT')
    blocks = (max(16384, count * width) + SECTOR - 1) // SECTOR
    total = path.stat().st_size // SECTOR
    return dict(total=total, first=first, last=last, count=count,
                width=width, blocks=blocks, table_lba=table)


def reheader(img, hdr_off, total, table_lba):
    """Recompute header and table CRCs after an edit.

    `img` is the whole primary (MBR + header + table) or the whole backup
    (table + header). The header always sits `SECTOR` bytes from the region
    start for the primary and immediately before the end for the backup, so the
    caller passes the region bytes already positioned.
    """
    struct.pack_into('<Q', img, hdr_off + 24, 1 if hdr_off == SECTOR else total - 1)
    struct.pack_into('<Q', img, hdr_off + 32, total - 1 if hdr_off == SECTOR else 1)
    struct.pack_into('<Q', img, hdr_off + 72, table_lba)
    table_off = 2 * SECTOR if hdr_off == SECTOR else 0
    count_bytes = struct.unpack_from('<I', img, hdr_off + 80)[0] * 128
    table = bytes(img[table_off:table_off + count_bytes])
    struct.pack_into('<I', img, hdr_off + 88, zlib.crc32(table))
    struct.pack_into('<I', img, hdr_off + 16, 0)
    struct.pack_into('<I', img, hdr_off + 16,
                     zlib.crc32(bytes(img[hdr_off:hdr_off + 92])))


def edit_gpt(path, fn):
    """Apply fn(primary, backup, c) to both GPT regions, then write them back."""
    c = load_consts(path)
    total, blocks = c['total'], c['blocks']
    p_off, b_off = 0, (total - 1 - blocks) * SECTOR
    p = read_region(path, p_off, (2 + blocks) * SECTOR)
    b = read_region(path, b_off, (1 + blocks) * SECTOR)
    fn(p, b, c)
    if len(p) != (2 + blocks) * SECTOR or len(b) != (1 + blocks) * SECTOR:
        raise SystemExit('corruption changed a GPT region length')
    write_region(path, p_off, p)
    write_region(path, b_off, b)


def rebuild_both(p, b, c, table):
    """Write a new entry array into both copies and refresh both headers."""
    count_bytes = c['count'] * c['width']
    p[2 * SECTOR:2 * SECTOR + count_bytes] = table
    b[0:count_bytes] = table
    reheader(p, SECTOR, c['total'], c['table_lba'])
    reheader(b, len(b) - SECTOR, c['total'], c['total'] - 1 - c['blocks'])


def first_userdata_index(table, count, width):
    for i in range(count):
        e = table[i * width:(i + 1) * width]
        if e[:16] == bytes(16):
            continue
        name = e[56:56 + 72].decode('utf-16-le', 'ignore').split('\0', 1)[0]
        if name == 'userdata':
            return i
    raise SystemExit('fixture has no userdata entry')


def sparse_copy(src, dest):
    """Copy a fixture tree, preserving sparseness and hard links.

    Fixtures contain a 512 MiB disk image that is almost entirely holes, plus a
    hard link (dm-0) sharing userdata's inode. shutil.copytree would materialise
    every hole and duplicate the link, so the disk image is copied with
    SEEK_DATA/SEEK_HOLE and links are recreated via an inode map.
    """
    src, dest = Path(src), Path(dest)
    dest.mkdir(parents=True)
    links = {}
    sparse_copy_into(src, dest, links)


def sparse_copy_into(src, dest, links):
    for entry in sorted(src.iterdir()):
        target = dest / entry.name
        if entry.is_symlink():
            os.symlink(os.readlink(entry), target)
        elif entry.is_dir():
            target.mkdir()
            sparse_copy_into(entry, target, links)
        else:
            st = entry.stat()
            key = (st.st_dev, st.st_ino)
            if st.st_nlink > 1 and key in links:
                os.link(links[key], target)
                continue
            if st.st_size > (8 << 20):
                copy_sparse_file(entry, target)
            else:
                shutil.copy2(entry, target)
            if st.st_nlink > 1:
                links[key] = target


def copy_sparse_file(src, dest):
    """Copy a mostly-hole file, writing only its data extents."""
    size = src.stat().st_size
    with open(src, 'rb') as fin, open(dest, 'wb') as fout:
        fout.truncate(size)
        try:
            pos = fin.seek(0, os.SEEK_DATA)
        except OSError:
            pos = size                     # whole file is a hole
        while pos < size:
            end = fin.seek(pos, os.SEEK_HOLE)
            fin.seek(pos)
            while pos < end:
                chunk = fin.read(min(1 << 20, end - pos))
                if not chunk:
                    break
                fout.seek(pos)
                fout.write(chunk)
                pos += len(chunk)
            try:
                pos = fin.seek(end, os.SEEK_DATA)
            except OSError:
                break                      # no more data extents


def main():
    if len(sys.argv) != 4:
        print(__doc__, file=sys.stderr)
        return 2
    src, dest, mode = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3]
    if dest.exists():
        print(f'refusing to reuse {dest}', file=sys.stderr)
        return 2
    sparse_copy(src, dest)

    disk = dest / 'dev' / 'block' / 'sda'
    userdata = dest / 'dev' / 'block' / 'sda3'
    metadata = dest / 'dev' / 'block' / 'sda2'

    if mode in ('primary-crc', 'backup-crc'):
        # Flip a byte in the header CRC field itself, leaving everything else
        # intact: the header now claims a CRC it does not have.
        c = load_consts(disk)
        off = SECTOR + 16 if mode == 'primary-crc' else \
            (c['total'] - 1) * SECTOR + 16
        raw = read_region(disk, off, 4)
        raw[0] ^= 0xFF
        write_region(disk, off, raw)

    elif mode == 'primary-table':
        def fn(p, b, c):
            p[2 * SECTOR + 32] ^= 0xFF          # userdata start LBA low byte
            struct.pack_into('<I', p, SECTOR + 88,
                             zlib.crc32(bytes(p[2 * SECTOR:2 * SECTOR + c['count'] * 128])))
            struct.pack_into('<I', p, SECTOR + 16, 0)
            struct.pack_into('<I', p, SECTOR + 16,
                             zlib.crc32(bytes(p[SECTOR:SECTOR + 92])))
        edit_gpt(disk, fn)

    elif mode == 'backup-disagree':
        def fn(p, b, c):
            off = len(b) - SECTOR
            b[off + 56:off + 72] = uuid.uuid4().bytes_le
            struct.pack_into('<I', b, off + 16, 0)
            struct.pack_into('<I', b, off + 16, zlib.crc32(bytes(b[off:off + 92])))
        edit_gpt(disk, fn)

    elif mode == 'mbr-missing':
        def fn(p, b, c):
            p[510:512] = b'\x00\x00'
        edit_gpt(disk, fn)

    elif mode in ('add-unixnu', 'fill-entries'):
        def fn(p, b, c):
            count, width = c['count'], c['width']
            table = bytearray(p[2 * SECTOR:2 * SECTOR + count * width])

            def new_entry(start, end, name):
                e = bytearray(width)
                e[:16] = LINUX_DATA
                e[16:32] = uuid.uuid4().bytes_le
                struct.pack_into('<QQQ', e, 32, start, end, 0)
                set_entry_name(e, name)
                return e

            def shrink_userdata(end):
                """Move userdata's end earlier, freeing real space at the tail."""
                user = first_userdata_index(table, count, width)
                struct.pack_into('<Q', table, user * width + 40, end)

            free = [i for i in range(count) if table[i * width:i * width + 16] == bytes(16)]
            if not free:
                raise SystemExit(f'{mode}: fixture already has no free GPT entry')
            user = first_userdata_index(table, count, width)
            user_end = struct.unpack_from('<Q', table, user * width + 40)[0]

            if mode == 'add-unixnu':
                # An existing `unixnu` in the gap after `metadata`, which is
                # where a previous run would have created it after shrinking
                # userdata. userdata stays last, so the refusal is specifically
                # "unixnu already exists".
                table[free[0] * width:(free[0] + 1) * width] = \
                    new_entry(128, 128 + 16, 'unixnu')
            else:
                # Occupy every remaining free slot with distinct, non-overlapping
                # partitions before userdata, which keeps its identity and stays
                # last. The refusal must be "no unused GPT entry".
                slots = [s for s in free if s != user]
                for n, slot in enumerate(slots):
                    start = 128 + 2 + n * 2
                    if start + 1 >= 2048:
                        raise SystemExit('fill-entries: no room before userdata')
                    table[slot * width:(slot + 1) * width] = \
                        new_entry(start, start + 1, f'pad{n}')
            rebuild_both(p, b, c, table)
        edit_gpt(disk, fn)

    elif mode == 'userdata-not-last':
        def fn(p, b, c):
            count, width = c['count'], c['width']
            table = bytearray(p[2 * SECTOR:2 * SECTOR + count * width])
            free = [i for i in range(count) if table[i * width:i * width + 16] == bytes(16)]
            if not free:
                raise SystemExit('userdata-not-last: fixture has no free GPT entry')
            # An occupied partition that ends after userdata. userdata is still
            # valid F2FS and readable, but is no longer the last occupied
            # partition, so the refusal must come from the placement check.
            entry = bytearray(width)
            entry[:16] = LINUX_DATA
            entry[16:32] = uuid.uuid4().bytes_le
            user = first_userdata_index(table, count, width)
            user_end = struct.unpack_from('<Q', table, user * width + 40)[0]
            # Trim userdata by one LBA and give that LBA to `tail`, so they are
            # adjacent but never overlap, and tail is the last partition.
            struct.pack_into('<Q', table, user * width + 40, user_end - 1)
            struct.pack_into('<QQQ', entry, 32, user_end, user_end, 0)
            set_entry_name(entry, 'tail')
            table[free[0] * width:(free[0] + 1) * width] = entry
            rebuild_both(p, b, c, table)
        edit_gpt(disk, fn)
        # Keep the kernel partition view consistent with the edited GPT so the
        # refusal comes from the placement check rather than a range mismatch.
        c = load_consts(disk)
        table = read_region(disk, 2 * SECTOR, c['count'] * c['width'])
        user = first_userdata_index(table, c['count'], c['width'])
        start = struct.unpack_from('<Q', table, user * c['width'] + 32)[0]
        end = struct.unpack_from('<Q', table, user * c['width'] + 40)[0]
        (dest / 'sys/devices/fixture/block/sda/sda3/size').write_text(
            f'{(end - start + 1) * SECTOR // 512}\n')

    elif mode in ('f2fs-ext4', 'f2fs-bad-block', 'f2fs-bad-seg', 'f2fs-multi-seg',
                  'f2fs-multi-dev', 'f2fs-oversize'):
        # Both copies are edited so the change is a layout value, not a torn
        # write: a single-field difference would make the copy-mismatch warning
        # fire instead of the intended geometry refusal.
        if mode == 'f2fs-ext4':
            field, width, value = 0, '<I', 0xEF53
        elif mode == 'f2fs-bad-block':
            field, width, value = 16, '<I', 11        # log_blocksize -> 2048
        elif mode == 'f2fs-bad-seg':
            field, width, value = 20, '<I', 10        # log_blocks_per_seg -> 1024
        elif mode == 'f2fs-multi-seg':
            field, width, value = 24, '<I', 2         # segments_per_section
        elif mode == 'f2fs-multi-dev':
            field, width, value = 2201, '<B', 1       # devs[0].path[0] set
        elif mode == 'f2fs-oversize':
            field, width, value = 36, '<Q', 1 << 40   # more blocks than userdata
        else:
            print(f'unknown mode {mode}', file=sys.stderr)
            return 2
        blob = bytearray(struct.pack(width, value))
        for base in (1024, SECTOR + 1024):
            write_region(userdata, base + field, blob)

    elif mode == 'f2fs-sb-first-damaged':
        # Magic only in the second copy: the first is a torn write.
        write_region(userdata, 1024, bytearray(struct.pack('<I', 0)))
    elif mode == 'f2fs-sb-second-damaged':
        # Magic only in the first copy.
        write_region(userdata, SECTOR + 1024, bytearray(struct.pack('<I', 0)))
    elif mode == 'f2fs-sb-both-damaged':
        write_region(userdata, 1024, bytearray(struct.pack('<I', 0)))
        write_region(userdata, SECTOR + 1024, bytearray(struct.pack('<I', 0)))
    elif mode == 'f2fs-sb-field-differs':
        # Both magic-valid, layout fields identical, one unrelated byte differs.
        write_region(userdata, SECTOR + 1024 + 3000, bytearray(b'\x01'))

    elif mode == 'f2fs-cp-first-damaged':
        # A torn first checkpoint: its user_block_count exceeds the device,
        # while the version field still outranks the good copy. Validity must
        # decide, not the version, or the garbage copy would be selected.
        off, cp = locate_checkpoints(userdata)
        write_region(userdata, cp[0] + 0, struct.pack('<Q', 1 << 62))
        write_region(userdata, cp[0] + 8, struct.pack('<Q', 1 << 40))
    elif mode == 'f2fs-cp-resizefs-flag':
        # CKPT_FLAG_RESIZEFS set in both copies: an interrupted resize.
        off, cp = locate_checkpoints(userdata)
        for base in cp:
            write_region(userdata, base + 132, struct.pack('<I', 0x00004000))
    elif mode == 'f2fs-cp-fsck-flag':
        # CKPT_FLAG_FSCK set in both copies: filesystem needs repair.
        off, cp = locate_checkpoints(userdata)
        for base in cp:
            write_region(userdata, base + 132, struct.pack('<I', 0x00000010))

    elif mode == 'busy-bind':
        mounts = dest / 'proc' / 'mounts'
        text = mounts.read_text()
        mounts.write_text(text + '/dev/block/dm-0 /data/vendor f2fs rw,checkpoint=enable 0 0\n')

    else:
        print(f'unknown mode {mode}', file=sys.stderr)
        return 2
    return 0


if __name__ == '__main__':
    sys.exit(main())
