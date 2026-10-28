#!/usr/bin/env python3
"""Independently verify the GPT and F2FS results of an offline apply.

Reads only the fixture disk and the plan bundle. It deliberately reimplements
the small amount of GPT layout decoding needed here instead of calling the
helper, so the test can catch a helper bug.
"""
import os
import struct
import sys


def read_plan(plan_dir):
    values = {}
    with open(os.path.join(plan_dir, "plan.conf"), encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, value = line.split("=", 1)
            values[key.strip()] = value.strip()
    return values


def read_bytes(path, offset, length):
    with open(path, "rb") as f:
        f.seek(offset)
        data = f.read(length)
    if len(data) != length:
        raise ValueError(f"short read from {path}")
    return data


def entries(table, count):
    out = {}
    for i in range(count):
        e = table[i * 128:(i + 1) * 128]
        if not any(e[:16]):
            continue
        out[i + 1] = {
            "raw": e,
            "type": e[0:16],
            "unique": e[16:32],
            "start": struct.unpack_from("<Q", e, 32)[0],
            "end": struct.unpack_from("<Q", e, 40)[0],
            "attrs": struct.unpack_from("<Q", e, 48)[0],
            "name": e[56:128],
        }
    return out


def table_of(image, sector, backup):
    if backup:
        return image[0:len(image) - sector]
    return image[2 * sector:]


def main():
    if len(sys.argv) != 3:
        print("usage: check_after.py FIXTURE_ROOT PLAN_DIR", file=sys.stderr)
        return 2
    root = os.path.abspath(sys.argv[1])
    plan_dir = os.path.abspath(sys.argv[2])
    plan = read_plan(plan_dir)
    sector = int(plan["sector"])
    plen = int(plan["gpt_primary_len"])
    blen = int(plan["gpt_backup_len"])
    backup_lba = int(plan["gpt_backup_lba"])

    disk = os.path.join(root, "dev/block/sda")
    primary_before = read_bytes(os.path.join(plan_dir, "gpt-primary.before"), 0, plen)
    backup_before = read_bytes(os.path.join(plan_dir, "gpt-backup.before"), 0, blen)
    primary_after = read_bytes(os.path.join(plan_dir, "gpt-primary.after"), 0, plen)
    backup_after = read_bytes(os.path.join(plan_dir, "gpt-backup.after"), 0, blen)
    disk_primary = read_bytes(disk, 0, plen)
    disk_backup = read_bytes(disk, backup_lba * sector, blen)

    failures = []

    def check(condition, message):
        if not condition:
            failures.append(message)

    check(disk_primary == primary_after, "disk primary GPT differs from gpt-primary.after")
    check(disk_backup == backup_after, "disk backup GPT differs from gpt-backup.after")

    count = struct.unpack_from("<I", primary_before, sector + 80)[0]
    before = entries(table_of(primary_before, sector, False), count)
    after = entries(table_of(primary_after, sector, False), count)
    ub = before[int(plan["userdata_index"])]
    ua = after[int(plan["userdata_index"])]
    for field in ("type", "unique", "attrs", "name"):
        check(ub[field] == ua[field], f"userdata {field} changed")
    check(ua["start"] == int(plan["userdata_start_lba"]), "userdata start changed")
    check(ua["end"] == int(plan["userdata_new_end_lba"]), "userdata end is wrong")

    x = after.get(int(plan["unixnu_index"]))
    if x is None:
        failures.append("unixnu GPT entry is missing")
        x = {"start": -1, "end": -1, "type": b"", "name": b""}
    check(x["start"] == int(plan["unixnu_start_lba"]), "unixnu start is wrong")
    check(x["end"] == int(plan["unixnu_end_lba"]), "unixnu end is wrong")
    linux_type = bytes.fromhex("af3dc60f838472478e793d69d8477de4")
    check(x["type"] == linux_type, "unixnu does not use the Linux filesystem data type GUID")
    check(x["name"] == "unixnu".encode("utf-16-le").ljust(72, b"\0"), "unixnu name is wrong")

    metadata_index = int(plan["metadata_index"])
    for index in before:
        if index in (int(plan["userdata_index"]), int(plan["unixnu_index"])):
            continue
        check(before[index]["raw"] == after[index]["raw"], f"unrelated GPT entry {index} changed")
    check(metadata_index in before and metadata_index in after, "metadata GPT entry is missing")
    check(before[metadata_index]["raw"] == after[metadata_index]["raw"],
          "metadata GPT entry changed")

    expected_blocks = int(plan["expected_blocks"])
    raw = os.path.join(root, "dev/block/sda3")
    for offset in (1024 + 36, 4096 + 1024 + 36):
        blocks = struct.unpack("<Q", read_bytes(raw, offset, 8))[0]
        check(blocks == expected_blocks,
              f"F2FS superblock at {offset} has {blocks} blocks, expected {expected_blocks}")

    if failures:
        for message in failures:
            print("FAIL:", message)
        return 1
    print("PASS: independent GPT/F2FS result check")
    return 0


if __name__ == "__main__":
    sys.exit(main())
