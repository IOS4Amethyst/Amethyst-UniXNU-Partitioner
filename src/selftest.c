/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Host-only self tests for pure arithmetic, hashing and GPT layout code. */
#include "common.h"

static int failures;

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put64(uint8_t *p, uint64_t v)
{
    put32(p, (uint32_t)v);
    put32(p + 4, (uint32_t)(v >> 32));
}

typedef struct {
    uint32_t sector;
    uint64_t total;
    uint32_t blocks;
    uint64_t first;
    uint64_t last;
    uint8_t guid[16];
    size_t plen;
    size_t blen;
    uint8_t *primary;
    uint8_t *backup;
} fake_gpt_t;

static void gpt_header(uint8_t *h, uint64_t lba, uint64_t other, uint64_t first, uint64_t last,
                      const uint8_t guid[16], uint64_t table_lba, uint32_t count,
                      uint32_t table_crc)
{
    memset(h, 0, 92);
    memcpy(h, "EFI PART", 8);
    put32(h + 8, 0x00010000);
    put32(h + 12, 92);
    put64(h + 24, lba);
    put64(h + 32, other);
    put64(h + 40, first);
    put64(h + 48, last);
    memcpy(h + 56, guid, 16);
    put64(h + 72, table_lba);
    put32(h + 80, count);
    put32(h + 84, 128);
    put32(h + 88, table_crc);
    put32(h + 16, 0);
    put32(h + 16, crc32_buf(h, 92));
}

static int fake_gpt_build(fake_gpt_t *f, uint32_t sector, uint64_t total, const char *user_name,
                         int duplicate_guid, int overlap)
{
    memset(f, 0, sizeof(*f));
    f->sector = sector;
    f->total = total;
    uint32_t count = 128;
    f->blocks = (uint32_t)((MAX(GPT_MIN_TABLE_BYTES, count * 128) + sector - 1) / sector);
    f->first = 2 + f->blocks;
    f->last = total - 1 - f->blocks - 1;
    for (int i = 0; i < 16; i++)
        f->guid[i] = (uint8_t)(0x10 + i);
    f->plen = (size_t)(2 + f->blocks) * sector;
    f->blen = (size_t)(1 + f->blocks) * sector;
    f->primary = xcalloc(1, f->plen);
    f->backup = xcalloc(1, f->blen);
    uint8_t *table = xcalloc(1, (size_t)count * 128);
    uint8_t *p = f->primary;
    p[510] = 0x55;
    p[511] = 0xaa;
    p[446 + 4] = 0xee;
    put32(p + 446 + 8, 1);
    put32(p + 446 + 12, (uint32_t)MIN(total - 1, UINT32_MAX));
    uint64_t start = 2048;
    uint64_t end = f->last;
    if (overlap) {
        uint64_t second = start + 64;
        uint8_t *e = table + 128;
        memcpy(e, table, 16);
        uint8_t ug[16];
        for (int i = 0; i < 16; i++)
            ug[i] = (uint8_t)(0x40 + i);
        if (duplicate_guid)
            memcpy(ug, table + 16, 16);
        memcpy(e + 16, ug, 16);
        put64(e + 32, second);
        put64(e + 40, end);
        static const char label[] = "overlap";
        for (size_t i = 0; i < sizeof(label) - 1; i++)
            e[56 + i * 2] = (uint8_t)label[i];
    }
    uint8_t type[16] = {0xaf, 0x3d, 0xc6, 0x0f, 0x83, 0x84, 0x72, 0x47,
                        0x8e, 0x79, 0x3d, 0x69, 0xd8, 0x47, 0x7d, 0xe4};
    memcpy(table, type, 16);
    uint8_t unique[16];
    for (int i = 0; i < 16; i++)
        unique[i] = (uint8_t)(0x80 + i);
    memcpy(table + 16, unique, 16);
    put64(table + 32, start);
    put64(table + 40, end);
    put64(table + 48, 0x8000000000000001ULL);
    size_t nl = strlen(user_name);
    for (size_t i = 0; i < nl && i < 36; i++)
        table[56 + i * 2] = (uint8_t)user_name[i];
    uint32_t tcrc = crc32_buf(table, (size_t)count * 128);
    memcpy(f->primary + 2ULL * sector, table, (size_t)count * 128);
    memcpy(f->backup, table, (size_t)count * 128);
    gpt_header(f->primary + sector, 1, total - 1, f->first, f->last, f->guid, 2, count, tcrc);
    gpt_header(f->backup + f->blen - sector, total - 1, 1, f->first, f->last, f->guid,
               total - 1 - f->blocks, count, tcrc);
    free(table);
    return 0;
}

static void fake_gpt_free(fake_gpt_t *f)
{
    free(f->primary);
    free(f->backup);
    memset(f, 0, sizeof(*f));
}

static void test_sha(void)
{
    static const uint8_t abc[32] = {
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
        0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
        0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
        0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad,
    };
    static const uint8_t empty[32] = {
        0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14,
        0x9a, 0xfb, 0xf4, 0xc8, 0x99, 0x6f, 0xb9, 0x24,
        0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b, 0x93, 0x4c,
        0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55,
    };
    uint8_t h[32];
    sha256_buf("abc", 3, h);
    CHECK(!memcmp(h, abc, 32));
    sha256_buf("", 0, h);
    CHECK(!memcmp(h, empty, 32));
    CHECK(crc32_buf((const uint8_t *)"123456789", 9) == 0xcbf43926U);
}

static void test_parse(void)
{
    uint64_t mib = 0;
    char err[256];
    CHECK(parse_gib_to_mib("32", &mib, err, sizeof(err)) == 0 && mib == 32768);
    CHECK(parse_gib_to_mib("32.5", &mib, err, sizeof(err)) == 0 && mib == 33280);
    CHECK(parse_gib_to_mib("0.0009765625", &mib, err, sizeof(err)) == 0 && mib == 1);
    CHECK(parse_gib_to_mib("32.0001", &mib, err, sizeof(err)) < 0);
    CHECK(parse_gib_to_mib("0", &mib, err, sizeof(err)) < 0);
    CHECK(parse_gib_to_mib("-1", &mib, err, sizeof(err)) < 0);
    CHECK(parse_gib_to_mib("abc", &mib, err, sizeof(err)) < 0);
    uint64_t v = 0;
    CHECK(parse_u64("4096", &v) == 0 && v == 4096);
    CHECK(parse_u64("", &v) < 0);
    CHECK(parse_u64("12x", &v) < 0);
}

static void test_uuid(void)
{
    uint8_t g[16] = {0xaf, 0x3d, 0xc6, 0x0f, 0x83, 0x84, 0x72, 0x47,
                     0x8e, 0x79, 0x3d, 0x69, 0xd8, 0x47, 0x7d, 0xe4};
    char out[37];
    uuid_format_le(g, out);
    CHECK(!strcmp(out, "0fc63daf-8483-4772-8e79-3d69d8477de4"));
}

static void test_f2fs_rounding(void)
{
    CHECK(f2fs_request_blocks(57685263, 40907796) == 40907264);
    CHECK(f2fs_expected_blocks(57685263, 40907264) == 40907535);
    for (uint64_t r = 0; r < 512; r++) {
        uint64_t before = 20000 * 512 + r;
        uint64_t request = f2fs_request_blocks(before, 9000 * 512 + 101);
        CHECK(request % 512 == 0);
        CHECK(request > 0);
        CHECK(f2fs_expected_blocks(before, request) <= 9000 * 512 + 101);
    }
}

static void test_gpt_layout(void)
{
    fake_gpt_t f;
    gpt_t g;
    char err[512];
    uint8_t *pa = NULL, *ba = NULL;
    size_t pal = 0, bal = 0;
    layout_t layout;

    fake_gpt_build(&f, 512, 4 * 1024 * 1024, "userdata", 0, 0);
    CHECK(gpt_parse(f.primary, f.plen, f.backup, f.blen, 512, f.total, &g,
                    err, sizeof(err)) == 0);
    if (failures) {
        gpt_free(&g);
        fake_gpt_free(&f);
        return;
    }
    CHECK(g.n_entries == 1);
    CHECK(g.entries[0].start == 2048);
    CHECK(g.entries[0].end == f.last);
    CHECK(!strcmp(g.entries[0].name, "userdata"));

    memset(&layout, 0, sizeof(layout));
    CHECK(gpt_make_layout(&g, 1, 128 * MIB, 0, MIB, &layout, &pa, &pal, &ba, &bal,
                          err, sizeof(err)) == 0);
    if (!failures) {
        CHECK(layout.unixnu_start == layout.userdata_end + 1);
        CHECK(layout.unixnu_bytes == 128 * MIB);
        CHECK(layout.new_userdata_bytes == (layout.userdata_end - layout.userdata_start + 1) * 512);
        CHECK(layout.unixnu_start * 512 % 4096 == 0);
        gpt_t after;
        CHECK(gpt_parse(pa, pal, ba, bal, 512, f.total, &after, err, sizeof(err)) == 0);
        const gpt_entry_t *u = gpt_entry_by_index(&after, 1);
        const gpt_entry_t *x = gpt_entry_by_name(&after, "unixnu", 0);
        CHECK(u && x);
        if (u && x) {
            CHECK(u->start == 2048);
            CHECK(u->end == layout.unixnu_start - 1);
            CHECK(!memcmp(u->type_guid, g.entries[0].type_guid, 16));
            CHECK(!memcmp(u->unique_guid, g.entries[0].unique_guid, 16));
            CHECK(u->attrs == g.entries[0].attrs);
            CHECK(x->start == layout.unixnu_start);
            CHECK(x->end == layout.unixnu_end);
            char guid[37];
            uuid_format_le(x->type_guid, guid);
            CHECK(!strcmp(guid, LINUX_FS_DATA_GUID));
        }
        gpt_free(&after);
    }
    free(pa);
    free(ba);
    gpt_free(&g);
    fake_gpt_free(&f);

    /* duplicate unique GUID */
    fake_gpt_build(&f, 512, 4 * 1024 * 1024, "userdata", 1, 1);
    CHECK(gpt_parse(f.primary, f.plen, f.backup, f.blen, 512, f.total, &g,
                    err, sizeof(err)) < 0);
    gpt_free(&g);
    fake_gpt_free(&f);

    /* corruption: flip one byte in the primary table */
    fake_gpt_build(&f, 512, 4 * 1024 * 1024, "userdata", 0, 0);
    f.primary[2ULL * 512 + 20] ^= 0xff;
    CHECK(gpt_parse(f.primary, f.plen, f.backup, f.blen, 512, f.total, &g,
                    err, sizeof(err)) < 0);
    gpt_free(&g);
    fake_gpt_free(&f);

    /* hybrid MBR */
    fake_gpt_build(&f, 512, 4 * 1024 * 1024, "userdata", 0, 0);
    f.primary[446 + 4] = 0x0c;
    CHECK(gpt_parse(f.primary, f.plen, f.backup, f.blen, 512, f.total, &g,
                    err, sizeof(err)) < 0);
    gpt_free(&g);
    fake_gpt_free(&f);
}

/*
 * Regression guard: the 0.5.0 runtime called F2FS_IOC_RESIZE_FS on an unmounted
 * block device and the recovery kernel crashed. The ioctl only works on a
 * mounted f2fs instance, so the shrink must mount the mapping privately. If
 * anyone collapses that back to a bare-fd ioctl, this assertion fails.
 */
static void test_resize_requires_mount(void)
{
    CHECK(f2fs_shrink_needs_mount() == 1);
}

int selftest_run(void)
{
    failures = 0;
    test_sha();
    test_parse();
    test_uuid();
    test_f2fs_rounding();
    test_resize_requires_mount();
    test_gpt_layout();
    if (failures)
        fprintf(stderr, "SELFTEST FAILED: %d checks\n", failures);
    else
        printf("SELFTEST PASS\n");
    return failures ? 1 : 0;
}
