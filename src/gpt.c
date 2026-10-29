/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* GPT parsing and planning adapted from UniXNU scripts/dual_sys/gpt.py. */
#include "common.h"

typedef struct {
    uint32_t revision;
    uint32_t header_size;
    uint32_t header_crc;
    uint32_t reserved;
    uint64_t my_lba;
    uint64_t alternate_lba;
    uint64_t first_usable;
    uint64_t last_usable;
    uint8_t disk_guid[16];
    uint64_t entries_lba;
    uint32_t count;
    uint32_t entry_size;
    uint32_t entries_crc;
    uint32_t blocks;
} parsed_header_t;

static uint32_t le32(const void *p)
{
    const uint8_t *b = p;
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static uint64_t le64(const void *p)
{
    return (uint64_t)le32(p) | ((uint64_t)le32((const uint8_t *)p + 4) << 32);
}

static void put32(void *p, uint32_t v)
{
    uint8_t *b = p;
    b[0] = (uint8_t)v;
    b[1] = (uint8_t)(v >> 8);
    b[2] = (uint8_t)(v >> 16);
    b[3] = (uint8_t)(v >> 24);
}

static void put64(void *p, uint64_t v)
{
    put32(p, (uint32_t)v);
    put32((uint8_t *)p + 4, (uint32_t)(v >> 32));
}

static int is_zero(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (p[i])
            return 0;
    return 1;
}

static void utf16le_to_utf8(const uint8_t *src, size_t bytes, char *out, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; i + 1 < bytes && o + 1 < cap; i += 2) {
        uint32_t c = (uint32_t)src[i] | ((uint32_t)src[i + 1] << 8);
        if (!c)
            break;
        if (c < 0x80) {
            out[o++] = (char)c;
        } else if (c < 0x800 && o + 2 < cap) {
            out[o++] = (char)(0xc0 | (c >> 6));
            out[o++] = (char)(0x80 | (c & 0x3f));
        } else if (o + 3 < cap) {
            out[o++] = (char)(0xe0 | (c >> 12));
            out[o++] = (char)(0x80 | ((c >> 6) & 0x3f));
            out[o++] = (char)(0x80 | (c & 0x3f));
        }
    }
    out[o] = '\0';
}

static int parse_header(const uint8_t *data, size_t len, uint32_t sector, uint64_t lba,
                        uint64_t total_sectors, parsed_header_t *h, char *err, size_t errlen)
{
    if (sector != 512 && sector != 4096)
        return failf(err, errlen, "unsupported logical sector size %u", sector);
    if (len < sector)
        return failf(err, errlen, "short GPT header region");
    if (memcmp(data, "EFI PART", 8))
        return failf(err, errlen, "missing EFI PART signature at LBA %" PRIu64, lba);
    h->revision = le32(data + 8);
    h->header_size = le32(data + 12);
    h->header_crc = le32(data + 16);
    h->reserved = le32(data + 20);
    h->my_lba = le64(data + 24);
    h->alternate_lba = le64(data + 32);
    h->first_usable = le64(data + 40);
    h->last_usable = le64(data + 48);
    memcpy(h->disk_guid, data + 56, 16);
    h->entries_lba = le64(data + 72);
    h->count = le32(data + 80);
    h->entry_size = le32(data + 84);
    h->entries_crc = le32(data + 88);
    if (h->revision != 0x00010000U || h->header_size != 92 || h->reserved != 0)
        return failf(err, errlen, "unsupported GPT header at LBA %" PRIu64, lba);
    uint8_t check[92];
    memcpy(check, data, 92);
    put32(check + 16, 0);
    if (crc32_buf(check, 92) != h->header_crc)
        return failf(err, errlen, "GPT header CRC mismatch at LBA %" PRIu64, lba);
    if (h->my_lba != lba)
        return failf(err, errlen, "GPT header my_lba %" PRIu64 " != %" PRIu64, h->my_lba, lba);
    uint64_t other = lba == 1 ? total_sectors - 1 : 1;
    if (h->alternate_lba != other)
        return failf(err, errlen, "GPT alternate_lba mismatch at LBA %" PRIu64, lba);
    if (h->entry_size != GPT_ENTRY_SIZE || h->count < 1 || h->count > MAX_GPT_ENTRIES)
        return failf(err, errlen, "unsupported GPT entry geometry");
    uint64_t table_bytes = (uint64_t)h->count * h->entry_size;
    if (table_bytes < GPT_MIN_TABLE_BYTES)
        table_bytes = GPT_MIN_TABLE_BYTES;
    h->blocks = (uint32_t)((table_bytes + sector - 1) / sector);
    if (total_sectors <= (uint64_t)h->blocks + 2)
        return failf(err, errlen, "disk is too small for the GPT entry table");
    uint64_t expect = lba == 1 ? 2 : total_sectors - 1 - h->blocks;
    if (h->entries_lba != expect)
        return failf(err, errlen, "non-contiguous or misplaced GPT entry table");
    if (2 + h->blocks > h->first_usable || h->first_usable > h->last_usable ||
        h->last_usable >= total_sectors - 1 - h->blocks)
        return failf(err, errlen, "GPT usable range is invalid");
    if (is_zero(h->disk_guid, 16))
        return failf(err, errlen, "GPT disk GUID is zero");
    return 0;
}

static int parse_table(gpt_t *gpt, char *err, size_t errlen)
{
    gpt->entries = xcalloc(gpt->count, sizeof(*gpt->entries));
    gpt->n_entries = 0;
    for (uint32_t i = 0; i < gpt->count; i++) {
        const uint8_t *e = gpt->table + (size_t)i * GPT_ENTRY_SIZE;
        if (is_zero(e, 16)) {
            if (!is_zero(e, GPT_ENTRY_SIZE))
                return failf(err, errlen, "GPT entry %u has a zero type but nonzero data", i + 1);
            continue;
        }
        uint64_t start = le64(e + 32), end = le64(e + 40), attrs = le64(e + 48);
        if (start > end || start < gpt->first_usable || end > gpt->last_usable)
            return failf(err, errlen, "GPT entry %u lies outside the usable range", i + 1);
        if (is_zero(e + 16, 16))
            return failf(err, errlen, "GPT entry %u has a zero unique GUID", i + 1);
        gpt_entry_t *g = &gpt->entries[gpt->n_entries++];
        g->index = i + 1;
        memcpy(g->type_guid, e, 16);
        memcpy(g->unique_guid, e + 16, 16);
        g->start = start;
        g->end = end;
        g->attrs = attrs;
        memcpy(g->raw, e, GPT_ENTRY_SIZE);
        utf16le_to_utf8(e + 56, 72, g->name, sizeof(g->name));
    }
    for (size_t i = 0; i < gpt->n_entries; i++) {
        for (size_t j = i + 1; j < gpt->n_entries; j++) {
            if (!memcmp(gpt->entries[i].unique_guid, gpt->entries[j].unique_guid, 16))
                return failf(err, errlen, "duplicate GPT unique GUIDs");
            if (!(gpt->entries[i].end < gpt->entries[j].start ||
                  gpt->entries[j].end < gpt->entries[i].start))
                return failf(err, errlen, "GPT partitions overlap");
        }
    }
    return 0;
}

int gpt_parse(const uint8_t *primary, size_t primary_len, const uint8_t *backup, size_t backup_len,
              uint32_t sector, uint64_t total_sectors, gpt_t *gpt, char *err, size_t errlen)
{
    memset(gpt, 0, sizeof(*gpt));
    gpt->sector = sector;
    gpt->total_sectors = total_sectors;
    if (primary_len < 2ULL * sector || backup_len < 2ULL * sector)
        return failf(err, errlen, "short GPT region");
    parsed_header_t h, b;
    if (parse_header(primary + sector, primary_len - sector, sector, 1, total_sectors, &h, err, errlen) < 0)
        return -1;
    if (parse_header(backup + backup_len - sector, sector, sector, total_sectors - 1,
                     total_sectors, &b, err, errlen) < 0)
        return -1;
    if (h.first_usable != b.first_usable || h.last_usable != b.last_usable ||
        memcmp(h.disk_guid, b.disk_guid, 16) || h.count != b.count ||
        h.blocks != b.blocks || h.entries_crc != b.entries_crc)
        return failf(err, errlen, "primary and backup GPT headers disagree");
    size_t plen = (size_t)(2 + h.blocks) * sector;
    size_t blen = (size_t)(1 + h.blocks) * sector;
    if (primary_len < plen || backup_len < blen)
        return failf(err, errlen, "short GPT tables");
    if (primary[510] != 0x55 || primary[511] != 0xaa)
        return failf(err, errlen, "missing protective MBR signature");
    int ee = 0;
    for (int i = 0; i < 4; i++) {
        uint8_t type = primary[446 + i * 16 + 4];
        if (type == 0xee)
            ee++;
        else if (type != 0)
            return failf(err, errlen, "hybrid MBR is unsupported");
    }
    if (ee != 1)
        return failf(err, errlen, "protective MBR must contain exactly one 0xee entry");
    const uint8_t *ptable = primary + 2ULL * sector;
    size_t table_len = (size_t)h.count * GPT_ENTRY_SIZE;
    if (memcmp(ptable, backup, table_len))
        return failf(err, errlen, "primary and backup GPT tables differ");
    if (crc32_buf(ptable, table_len) != h.entries_crc)
        return failf(err, errlen, "GPT entry array CRC mismatch");
    gpt->primary = xmalloc(plen);
    gpt->backup = xmalloc(blen);
    memcpy(gpt->primary, primary, plen);
    memcpy(gpt->backup, backup, blen);
    gpt->primary_len = plen;
    gpt->backup_len = blen;
    gpt->count = h.count;
    gpt->entry_size = h.entry_size;
    gpt->blocks = h.blocks;
    gpt->first_usable = h.first_usable;
    gpt->last_usable = h.last_usable;
    memcpy(gpt->disk_guid, h.disk_guid, 16);
    gpt->primary_crc = h.header_crc;
    gpt->backup_crc = b.header_crc;
    gpt->table_crc = h.entries_crc;
    gpt->table = xmalloc(table_len);
    memcpy(gpt->table, ptable, table_len);
    return parse_table(gpt, err, errlen);
}

int gpt_read_fd(int fd, uint32_t sector, uint64_t total_sectors, gpt_t *gpt, char *err, size_t errlen)
{
    if (sector != 512 && sector != 4096)
        return failf(err, errlen, "unsupported logical sector size %u", sector);
    if (total_sectors < 8)
        return failf(err, errlen, "disk is too small for GPT");
    uint8_t *hdr = xmalloc(sector);
    ssize_t n = pread(fd, hdr, sector, (off_t)sector);
    if (n != (ssize_t)sector) {
        free(hdr);
        return failf(err, errlen, "cannot read primary GPT header: %s", strerror(errno));
    }
    parsed_header_t h;
    if (parse_header(hdr, sector, sector, 1, total_sectors, &h, err, errlen) < 0) {
        free(hdr);
        return -1;
    }
    free(hdr);
    size_t plen = (size_t)(2 + h.blocks) * sector;
    size_t blen = (size_t)(1 + h.blocks) * sector;
    uint8_t *primary = xmalloc(plen);
    uint8_t *backup = xmalloc(blen);
    if (pread(fd, primary, plen, 0) != (ssize_t)plen ||
        pread(fd, backup, blen, (off_t)(total_sectors - 1 - h.blocks) * sector) != (ssize_t)blen) {
        free(primary);
        free(backup);
        return failf(err, errlen, "cannot read complete GPT regions: %s", strerror(errno));
    }
    int rc = gpt_parse(primary, plen, backup, blen, sector, total_sectors, gpt, err, errlen);
    free(primary);
    free(backup);
    return rc;
}

void gpt_free(gpt_t *gpt)
{
    free(gpt->primary);
    free(gpt->backup);
    free(gpt->table);
    free(gpt->entries);
    memset(gpt, 0, sizeof(*gpt));
}

const gpt_entry_t *gpt_entry_by_index(const gpt_t *gpt, uint32_t index)
{
    for (size_t i = 0; i < gpt->n_entries; i++)
        if (gpt->entries[i].index == index)
            return &gpt->entries[i];
    return NULL;
}

const gpt_entry_t *gpt_entry_by_name(const gpt_t *gpt, const char *name, int ci)
{
    for (size_t i = 0; i < gpt->n_entries; i++) {
        if (ci) {
            if (!strcasecmp(gpt->entries[i].name, name))
                return &gpt->entries[i];
        } else if (!strcmp(gpt->entries[i].name, name)) {
            return &gpt->entries[i];
        }
    }
    return NULL;
}

int gpt_entry_index(const gpt_t *gpt, uint32_t index)
{
    for (size_t i = 0; i < gpt->n_entries; i++)
        if (gpt->entries[i].index == index)
            return (int)i;
    return -1;
}

int gpt_entry_count_used(const gpt_t *gpt)
{
    return (int)gpt->n_entries;
}

int gpt_free_entry(const gpt_t *gpt, uint32_t *index)
{
    uint8_t *used = xcalloc(gpt->count, 1);
    for (size_t i = 0; i < gpt->n_entries; i++)
        used[gpt->entries[i].index - 1] = 1;
    int found = 0;
    for (uint32_t i = 0; i < gpt->count; i++) {
        if (!used[i]) {
            *index = i + 1;
            found = 1;
            break;
        }
    }
    free(used);
    return found ? 0 : -1;
}

int gpt_validate_identity(const gpt_t *gpt, const device_t *dev, char *err, size_t errlen)
{
    const gpt_entry_t *u = gpt_entry_by_index(gpt, (uint32_t)dev->userdata_index);
    if (!u || strcmp(u->name, "userdata"))
        return failf(err, errlen, "GPT userdata entry does not match the by-name device");
    if (u->start != dev->userdata_start_lba || u->end != dev->userdata_end_lba)
        return failf(err, errlen, "GPT userdata range does not match the kernel partition view");
    const gpt_entry_t *m = gpt_entry_by_index(gpt, (uint32_t)dev->metadata_index);
    if (!m || strcmp(m->name, "metadata"))
        return failf(err, errlen, "GPT metadata entry does not match the by-name device");
    if (m->start != dev->metadata_start_lba || m->end != dev->metadata_end_lba)
        return failf(err, errlen, "GPT metadata range does not match the kernel partition view");
    return 0;
}

int gpt_check_basic_layout(const gpt_t *gpt, int userdata_index, char *detail, size_t detail_len,
                           char *err, size_t errlen)
{
    const gpt_entry_t *u = gpt_entry_by_index(gpt, (uint32_t)userdata_index);
    if (!u || strcmp(u->name, "userdata"))
        return failf(err, errlen, "userdata identity does not match GPT");
    uint64_t max_end = 0;
    for (size_t i = 0; i < gpt->n_entries; i++)
        max_end = MAX(max_end, gpt->entries[i].end);
    if (u->end != max_end)
        return failf(err, errlen, "userdata is not the last occupied partition");
    if (gpt_entry_by_name(gpt, "unixnu", 1))
        return failf(err, errlen, "unixnu already exists; no resize is allowed");
    if (u->start * gpt->sector % F2FS_BLOCK_BYTES)
        return failf(err, errlen, "userdata start is not aligned to a 4096-byte F2FS block");
    uint32_t free_index = 0;
    if (gpt_free_entry(gpt, &free_index) < 0)
        return failf(err, errlen, "no unused GPT entry is available");
    if (detail && detail_len)
        snprintf(detail, detail_len, "userdata is last; free GPT entry %u; no unixnu", free_index);
    return 0;
}

int gpt_make_layout(const gpt_t *gpt, int userdata_index, uint64_t requested_bytes,
                    uint64_t reserve_bytes, uint64_t min_userdata_bytes, layout_t *layout,
                    uint8_t **primary_out, size_t *primary_len_out,
                    uint8_t **backup_out, size_t *backup_len_out,
                    char *err, size_t errlen)
{
    memset(layout, 0, sizeof(*layout));
    if (!requested_bytes || requested_bytes % MIB)
        return failf(err, errlen, "requested unixnu size must use whole MiB");
    if (reserve_bytes % MIB)
        return failf(err, errlen, "reserve_mib must resolve to whole MiB");
    if (requested_bytes % gpt->sector)
        return failf(err, errlen, "requested size is not sector aligned");
    const gpt_entry_t *u = gpt_entry_by_index(gpt, (uint32_t)userdata_index);
    if (!u || strcmp(u->name, "userdata"))
        return failf(err, errlen, "userdata identity does not match GPT");
    if (gpt_entry_by_name(gpt, "unixnu", 1))
        return failf(err, errlen, "unixnu already exists; no resize performed");
    uint64_t max_end = 0;
    for (size_t i = 0; i < gpt->n_entries; i++)
        max_end = MAX(max_end, gpt->entries[i].end);
    if (u->end != max_end)
        return failf(err, errlen, "userdata must be the last occupied partition");
    if (u->start * gpt->sector % F2FS_BLOCK_BYTES)
        return failf(err, errlen, "userdata start must be aligned to a 4096-byte F2FS block");
    uint32_t empty = 0;
    if (gpt_free_entry(gpt, &empty) < 0)
        return failf(err, errlen, "no free GPT entry");
    uint64_t align = MIB / gpt->sector;
    if (!align)
        return failf(err, errlen, "sector size is larger than the alignment unit");
    uint64_t end = ((u->end + 1) / align) * align;
    if (!end)
        return failf(err, errlen, "cannot align the end of userdata");
    end -= 1;
    /* Leave an unallocated tail above unixnu, outside both new GPT entries. */
    uint64_t reserve_sectors = reserve_bytes / gpt->sector;
    if (reserve_sectors >= end + 1 - u->start)
        return failf(err, errlen, "reserve_mib leaves no usable userdata");
    end -= reserve_sectors;
    uint64_t request_sectors = requested_bytes / gpt->sector;
    if (request_sectors > end + 1)
        return failf(err, errlen, "requested size is larger than the available tail");
    uint64_t start = end + 1 - request_sectors;
    if (start <= u->start)
        return failf(err, errlen, "requested size leaves no usable userdata");
    uint64_t new_userdata_bytes = (start - u->start) * gpt->sector;
    if (new_userdata_bytes < min_userdata_bytes)
        return failf(err, errlen, "requested size leaves less than the minimum userdata size");
    if ((start * gpt->sector) % F2FS_BLOCK_BYTES)
        return failf(err, errlen, "calculated unixnu start is not F2FS-block aligned");
    if (end >= gpt->last_usable + 1)
        return failf(err, errlen, "calculated unixnu end exceeds the GPT usable range");
    for (size_t i = 0; i < gpt->n_entries; i++) {
        if (gpt->entries[i].index == u->index)
            continue;
        if (!(end < gpt->entries[i].start || start > gpt->entries[i].end))
            return failf(err, errlen, "calculated unixnu range overlaps %s", gpt->entries[i].name);
    }
    uint8_t type_guid[16] = {
        0xaf, 0x3d, 0xc6, 0x0f, 0x83, 0x84, 0x72, 0x47,
        0x8e, 0x79, 0x3d, 0x69, 0xd8, 0x47, 0x7d, 0xe4
    };
    uint8_t unique_guid[16];
    if (uuid4(unique_guid) < 0)
        return failf(err, errlen, "cannot generate a partition GUID");
    uint8_t *primary = xmalloc(gpt->primary_len);
    uint8_t *backup = xmalloc(gpt->backup_len);
    memcpy(primary, gpt->primary, gpt->primary_len);
    memcpy(backup, gpt->backup, gpt->backup_len);
    uint8_t *ptable = primary + 2ULL * gpt->sector;
    uint8_t *btable = backup;
    put64(ptable + (size_t)(u->index - 1) * GPT_ENTRY_SIZE + 40, start - 1);
    put64(btable + (size_t)(u->index - 1) * GPT_ENTRY_SIZE + 40, start - 1);
    uint8_t entry[GPT_ENTRY_SIZE];
    memset(entry, 0, sizeof(entry));
    memcpy(entry, type_guid, 16);
    memcpy(entry + 16, unique_guid, 16);
    put64(entry + 32, start);
    put64(entry + 40, end);
    put64(entry + 48, 0);
    {
        uint8_t name[GPT_ENTRY_SIZE - 56];
        memset(name, 0, sizeof(name));
        static const char label[] = "unixnu";
        for (size_t i = 0; i < sizeof(label) - 1; i++)
            name[i * 2] = (uint8_t)label[i];
        memcpy(entry + 56, name, sizeof(name));
    }
    memcpy(ptable + (size_t)(empty - 1) * GPT_ENTRY_SIZE, entry, GPT_ENTRY_SIZE);
    memcpy(btable + (size_t)(empty - 1) * GPT_ENTRY_SIZE, entry, GPT_ENTRY_SIZE);
    uint32_t table_crc = crc32_buf(ptable, (size_t)gpt->count * GPT_ENTRY_SIZE);
    for (int which = 0; which < 2; which++) {
        uint8_t *img = which ? backup : primary;
        size_t hdr_off = which ? gpt->backup_len - gpt->sector : gpt->sector;
        put32(img + hdr_off + 88, table_crc);
        put32(img + hdr_off + 16, 0);
        put32(img + hdr_off + 16, crc32_buf(img + hdr_off, 92));
    }
    gpt_t check;
    if (gpt_parse(primary, gpt->primary_len, backup, gpt->backup_len, gpt->sector,
                  gpt->total_sectors, &check, err, errlen) < 0) {
        free(primary);
        free(backup);
        return -1;
    }
    gpt_free(&check);
    layout->userdata_index = u->index;
    layout->unixnu_index = (int)empty;
    layout->userdata_start = u->start;
    layout->userdata_end = start - 1;
    layout->unixnu_start = start;
    layout->unixnu_end = end;
    layout->old_userdata_bytes = (u->end - u->start + 1) * gpt->sector;
    layout->new_userdata_bytes = new_userdata_bytes;
    layout->unixnu_bytes = requested_bytes;
    layout->tail_gap_bytes = (u->end - end) * gpt->sector;
    layout->sector = gpt->sector;
    memcpy(layout->unixnu_type_guid, type_guid, 16);
    memcpy(layout->unixnu_unique_guid, unique_guid, 16);
    memcpy(layout->userdata_type_guid, u->type_guid, 16);
    memcpy(layout->userdata_unique_guid, u->unique_guid, 16);
    layout->userdata_attrs = u->attrs;
    *primary_out = primary;
    *primary_len_out = gpt->primary_len;
    *backup_out = backup;
    *backup_len_out = gpt->backup_len;
    return 0;
}

int gpt_compare_entry_identity(const gpt_entry_t *a, const gpt_entry_t *b,
                               char *detail, size_t detail_len)
{
    if (!a || !b) {
        if (detail && detail_len)
            snprintf(detail, detail_len, "missing partition entry");
        return -1;
    }
    if (memcmp(a->type_guid, b->type_guid, 16) || memcmp(a->unique_guid, b->unique_guid, 16) ||
        a->start != b->start || a->attrs != b->attrs || memcmp(a->raw + 56, b->raw + 56, 72)) {
        if (detail && detail_len)
            snprintf(detail, detail_len, "partition identity changed for %s", a->name);
        return -1;
    }
    return 0;
}
