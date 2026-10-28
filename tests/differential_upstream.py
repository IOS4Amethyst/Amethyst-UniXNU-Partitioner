#!/usr/bin/env python3
"""Differential test: this tool's plan must match upstream partition.py exactly.

Both implementations are run against the same synthetic GPT/sysfs fixture, and
the resulting geometry and GPT images are compared byte for byte. The upstream
sources are used unmodified from vendor/unixnu-dual_sys.

This never touches a block device. It does call upstream `gpt.py` directly
(the same code `partition.py plan` uses for layout and image generation); the
only host-specific step it skips is ADB discovery, which is replaced by the
fixture's sysfs/serial values.

Usage:
    python3 tests/differential_upstream.py ROOT
where ROOT is a fresh fixture directory from tests/make_fixture.py.
"""
import hashlib
import importlib.util
import json
import os
import struct
import subprocess
import sys
import uuid
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
VENDOR = REPO / 'vendor' / 'unixnu-dual_sys'
MIB = 1 << 20

sys.path.insert(0, str(VENDOR))
from gpt import GPT, size_bytes  # noqa: E402  (vendored upstream code)


def fixture_module():
    spec = importlib.util.spec_from_file_location('make_fixture', HERE / 'make_fixture.py')
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def load_upstream_gpt(disk, sector, total):
    """Read the fixture disk exactly as upstream read_gpt would over ADB."""
    data = disk.read_bytes()

    def read(lba, count):
        off = lba * sector
        chunk = data[off:off + count * sector]
        if len(chunk) != count * sector:
            raise SystemExit(f'short read at lba {lba} count {count}')
        return chunk

    blocks = max(16384, 32 * 128) // sector
    primary = read(0, 2 + blocks)
    backup = read(total - 1 - blocks, 1 + blocks)
    gpt = GPT(primary, backup, sector, total)
    return gpt, primary, backup


def upstream_plan(gpt, requested, userdata_index):
    primary, backup, layout = gpt.split(requested, userdata_index)
    return layout, primary, backup


def expected_blocks_upstream(before, limit):
    """Upstream partition.expected_blocks, reproduced verbatim."""
    if before <= limit:
        return before
    request = (limit - before % 512) // 512 * 512
    if request <= 0:
        raise SystemExit('upstream: userdata target too small')
    return request + before % 512


def run_tool(root, host_bin, env, *args):
    cmd = [host_bin, *args, '--root', str(root), '--identity', '/identity.conf']
    result = subprocess.run(cmd, capture_output=True, text=True, env=env)
    return result


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    root = Path(sys.argv[1]).resolve()
    requested = int(sys.argv[2])
    fx = fixture_module()
    host_bin = str(REPO / 'build' / 'partitioner-host')
    disk = root / 'dev' / 'block' / 'sda'
    sector = fx.SECTOR
    total = fx.TOTAL

    # --- upstream layout for exactly this starting GPT ---------------------
    gpt, p_before, b_before = load_upstream_gpt(disk, sector, total)
    layout, p_after, b_after = upstream_plan(gpt, requested, fx.USERDATA_INDEX)

    # --- this tool's plan --------------------------------------------------
    plans = root / 'plans'
    plans.mkdir(exist_ok=True)
    plan_dir = plans / 'differential'
    if plan_dir.exists():
        raise SystemExit(f'{plan_dir} already exists')
    req = plans / 'request.conf'
    gib = requested / (1 << 30)
    req.write_text(f'mode = plan\nsize_gib = {gib!r}\nplan_dir = /plans/differential\n')

    env = dict(os.environ)
    env['UNIXNU_TEST_STATFS'] = str(root / 'fixtures' / 'statfs')
    result = run_tool(root, host_bin, env, 'plan', '--request', '/plans/request.conf')
    if result.returncode != 0:
        raise SystemExit(f'tool plan failed:\n{result.stdout}\n{result.stderr}')

    conf = {}
    for line in (plan_dir / 'plan.conf').read_text().splitlines():
        if line and not line.startswith('#') and '=' in line:
            k, v = line.split('=', 1)
            conf[k.strip()] = v.strip()

    checks = []

    def check(name, got, want):
        ok = got == want
        checks.append((ok, name, got, want))

    check('unixnu_start_lba', int(conf['unixnu_start_lba']), layout['unixnu_start'])
    check('unixnu_end_lba', int(conf['unixnu_end_lba']), layout['unixnu_end'])
    check('userdata_start_lba', int(conf['userdata_start_lba']), layout['userdata_start'])
    check('userdata_new_end_lba', int(conf['userdata_new_end_lba']), layout['userdata_end'])
    check('userdata_new_bytes', int(conf['userdata_new_bytes']), layout['new_userdata_bytes'])
    check('unixnu_bytes', int(conf['unixnu_bytes']), layout['unixnu_bytes'])
    check('tail_gap_bytes', int(conf['tail_gap_bytes']), layout['tail_gap_bytes'])
    check('unixnu_index', int(conf['unixnu_index']), layout['unixnu_index'])
    check('userdata_bytes', int(conf['userdata_bytes']), layout['old_userdata_bytes'])

    limit = layout['new_userdata_bytes'] // 4096
    check('limit_blocks', int(conf['limit_blocks']), limit)
    up_expected = expected_blocks_upstream(fx.FS_BLOCKS, limit)
    check('expected_blocks', int(conf['expected_blocks']), up_expected)
    check('userdata_type_guid', conf['userdata_type_guid'],
          str(uuid.UUID(bytes_le=gpt.table[(fx.USERDATA_INDEX - 1) * 128:
                                           (fx.USERDATA_INDEX - 1) * 128 + 16])))
    check('userdata_unique_guid', conf['userdata_unique_guid'],
          str(uuid.UUID(bytes_le=gpt.table[(fx.USERDATA_INDEX - 1) * 128 + 16:
                                           (fx.USERDATA_INDEX - 1) * 128 + 32])))

    # --- GPT images -------------------------------------------------------
    # The only intended difference is the random unixnu unique GUID (and the two
    # CRCs that cover it). Verify structurally, then rebuild with upstream's GUID
    # and require byte equality.
    mine_p = (plan_dir / 'gpt-primary.after').read_bytes()
    mine_b = (plan_dir / 'gpt-backup.after').read_bytes()
    before_p = (plan_dir / 'gpt-primary.before').read_bytes()
    before_b = (plan_dir / 'gpt-backup.before').read_bytes()

    check('gpt-primary.before bytes', before_p, p_before)
    check('gpt-backup.before bytes', before_b, b_before)
    check('gpt-primary.after length', len(mine_p), len(p_after))
    check('gpt-backup.after length', len(mine_b), len(b_after))

    up_gpt = GPT(p_after, b_after, sector, total)
    my_gpt = GPT(mine_p, mine_b, sector, total)

    def entry_view(g, idx):
        e = g.entries[idx - 1]
        return (e['name'], e['start'], e['end'], e['flags'])

    check('unixnu entry (name/start/end/attrs)',
          entry_view(my_gpt, layout['unixnu_index']),
          entry_view(up_gpt, layout['unixnu_index']))
    check('userdata entry (only end changes)',
          (entry_view(my_gpt, fx.USERDATA_INDEX)[0], entry_view(my_gpt, fx.USERDATA_INDEX)[1],
           entry_view(my_gpt, fx.USERDATA_INDEX)[3]),
          (entry_view(up_gpt, fx.USERDATA_INDEX)[0], entry_view(up_gpt, fx.USERDATA_INDEX)[1],
           entry_view(up_gpt, fx.USERDATA_INDEX)[3]))
    LINUX_DATA = bytes.fromhex('af3dc60f838472478e793d69d8477de4')
    span = slice((layout['unixnu_index'] - 1) * 128, (layout['unixnu_index'] - 1) * 128 + 16)
    check('unixnu type GUID', my_gpt.table[span], LINUX_DATA)
    check('unixnu type GUID vs upstream', my_gpt.table[span], up_gpt.table[span])

    # Rebuild our images with upstream's generated unique GUID. The table lives
    # at LBA 2 in the primary region (after the protective MBR and header) and at
    # offset 0 in the backup region.
    up_unique = up_gpt.table[(layout['unixnu_index'] - 1) * 128 + 16:
                             (layout['unixnu_index'] - 1) * 128 + 32]
    primary = bytearray(mine_p)
    backup = bytearray(mine_b)
    for img, table_off, hdr_off in ((primary, 2 * sector, sector),
                                    (backup, 0, len(backup) - sector)):
        ent = table_off + (layout['unixnu_index'] - 1) * 128
        img[ent + 16:ent + 32] = up_unique
        table = bytes(img[table_off:table_off + 32 * 128])
        struct.pack_into('<I', img, hdr_off + 88, zlib.crc32(table))
        struct.pack_into('<I', img, hdr_off + 16, 0)
        struct.pack_into('<I', img, hdr_off + 16,
                         zlib.crc32(bytes(img[hdr_off:hdr_off + 92])))
    check('gpt-primary.after bytes (upstream unique GUID substituted)', bytes(primary), p_after)
    check('gpt-backup.after bytes (upstream unique GUID substituted)', bytes(backup), b_after)

    failed = [c for c in checks if not c[0]]
    for ok, name, got, want in checks:
        if ok:
            print(f'ok    {name}')
        else:
            print(f'FAIL  {name}: tool={_brief(got)} upstream={_brief(want)}')
    print(f'\nDIFFERENTIAL: {len(checks) - len(failed)} passed, {len(failed)} failed')
    return 1 if failed else 0


def _brief(value, limit=120):
    """Short, readable rendering for pass/fail lines."""
    if isinstance(value, (bytes, bytearray)):
        if len(value) <= 32:
            return value.hex()
        return f'<{len(value)} bytes sha256={hashlib.sha256(value).hexdigest()[:16]}>'
    text = repr(value)
    return text if len(text) <= limit else text[:limit] + '...'


if __name__ == '__main__':
    sys.exit(main())
