# Example output

These files were generated from the synthetic fixture created by
`tests/make_fixture.py`, not from an Amethyst phone:

| File | Source |
| --- | --- |
| `inspect.txt` | `partitioner-host inspect` on the fixture |
| `inspect.json` | Same run, JSON report |
| `plan.txt` | `partitioner-host plan` on the fixture |
| `plan.json` | Same run, JSON plan |

The fixture root prefix was removed so paths look like recovery paths
(`/dev/block/sda3`, `/data`). Nothing else was rewritten. The fixture is only
512 MiB, so the 64 MiB example split and LBA values are **not** representative
of a real phone; the report and bundle formats are.

The plan ID, session and confirmation phrase in these examples are disposable
fixture values. Do not apply them to any device, and do not use the example
`confirm` value: a real plan has its own random ID, serial, size and bundle
hashes.

`request-plan.conf` and `request-apply.conf` in this directory are templates.
Copy the appropriate one to `/sdcard/Amethyst-UniXNU-Partitioner/request.conf`
and replace every placeholder. Keep the real plan bundle on external SD or
`/tmp`, never on `/sdcard` or `/data`.

Example file SHA-256:

```
5976ee65b9b8dded0ee47a8b1bb52c609b116e8b29a141905c4e034440dbe885  inspect.txt
f4f39abd6e1849a5425aad043466f89408a1e213bdda0e28d1c56521899304b9  plan.txt
e385685646ba981d2e7e2107c156a056c0aaf8f9dec8248534c61de5f9a0ec6d  inspect.json
f5508004c8abfeb5feff74d286d9dd64f4696f4c6f0ddb61a5474e0d416aa4f7  plan.json
```

Regenerate them with the commands in
[`../docs/SOURCE-AND-BUILD.md`](../docs/SOURCE-AND-BUILD.md).
