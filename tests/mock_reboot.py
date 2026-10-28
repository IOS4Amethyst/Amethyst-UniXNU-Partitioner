#!/usr/bin/env python3
"""Simulate the kernel partition view after rebooting recovery.

Only host fixture files are changed. This does not touch a block device.
The new sysfs sizes are expressed in 512-byte units, as Linux exposes them.
"""
import os
import re
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


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(str(data) + "\n")


def main():
    if len(sys.argv) != 3:
        print("usage: mock_reboot.py FIXTURE_ROOT PLAN_DIR", file=sys.stderr)
        return 2
    root = os.path.abspath(sys.argv[1])
    plan = read_plan(sys.argv[2])
    sector = int(plan["sector"])
    ratio = sector // 512
    disk = os.path.join(root, "sys/devices/fixture/block/sda")

    ustart = int(plan["userdata_start_lba"])
    uend = int(plan["userdata_new_end_lba"])
    write(os.path.join(disk, "sda3/size"), (uend - ustart + 1) * ratio)

    index = int(plan["unixnu_index"])
    xstart = int(plan["unixnu_start_lba"])
    xend = int(plan["unixnu_end_lba"])
    node = f"sda{index}"
    write(os.path.join(disk, node, "partition"), index)
    write(os.path.join(disk, node, "start"), xstart * ratio)
    write(os.path.join(disk, node, "size"), (xend - xstart + 1) * ratio)

    class_block = os.path.join(root, "sys/class/block", node)
    os.makedirs(os.path.dirname(class_block), exist_ok=True)
    if os.path.lexists(class_block):
        os.unlink(class_block)
    os.symlink(f"../../devices/fixture/block/sda/{node}", class_block)

    dev = os.path.join(root, "dev/block", node)
    os.makedirs(os.path.dirname(dev), exist_ok=True)
    if not os.path.exists(dev):
        open(dev, "wb").close()
    by_name = os.path.join(root, "dev/block/by-name/unixnu")
    if os.path.lexists(by_name):
        os.unlink(by_name)
    os.symlink(f"../{node}", by_name)

    # Reboot starts with no /data or /sdcard mount.
    write(os.path.join(root, "proc/mounts"), "")
    print(f"reboot view: userdata end LBA {uend}, unixnu {node} LBA {xstart}..{xend}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
