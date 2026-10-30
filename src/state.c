/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "common.h"

static void json_string(FILE *f, const char *s)
{
    fputc('"', f);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '"': fputs("\\\"", f); break;
        case '\\': fputs("\\\\", f); break;
        case '\n': fputs("\\n", f); break;
        case '\r': fputs("\\r", f); break;
        case '\t': fputs("\\t", f); break;
        default:
            if (*p < 0x20)
                fprintf(f, "\\u%04x", *p);
            else
                fputc(*p, f);
        }
    }
    fputc('"', f);
}

void state_free(state_t *s)
{
    gpt_free(&s->gpt);
    mounts_free(&s->mounts);
    memset(s, 0, sizeof(*s));
}

/* Tell the operator whether recovery can fix a condition or the layout cannot. */
static const char *unsup_action(const char *text)
{
    static const char *const recovery[] = {
        "metadata is mounted", "/data is mounted", "no hardware serial was found",
        "filesystem: cannot open", "identity: cannot", "storage: cannot",
        "the recovery kernel does not list f2fs support",
    };
    for (size_t i = 0; i < ARRAY_LEN(recovery); i++)
        if (strstr(text, recovery[i]))
            return "fixable-in-recovery";
    return "hard";
}

static void add_unsup(state_t *s, const char *fmt, ...)
{
    if (s->n_unsupported >= (int)ARRAY_LEN(s->unsupported))
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s->unsupported[s->n_unsupported], 320, fmt, ap);
    va_end(ap);
    s->unsupported_action[s->n_unsupported] = unsup_action(s->unsupported[s->n_unsupported]);
    s->n_unsupported++;
}

static void add_warn(state_t *s, const char *fmt, ...)
{
    if (s->n_warnings >= (int)ARRAY_LEN(s->warnings))
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s->warnings[s->n_warnings], 320, fmt, ap);
    va_end(ap);
    s->n_warnings++;
}

int state_collect(const paths_t *p, state_t *s, int strict, char *err, size_t errlen)
{
    memset(s, 0, sizeof(*s));
    char local[512];

    if (identity_load(p, &s->id, local, sizeof(local)) < 0) {
        if (strict)
            return failf(err, errlen, "%s", local);
        add_unsup(s, "identity: %s", local);
    } else if (identity_validate(p, &s->id, local, sizeof(local)) < 0) {
        if (strict)
            return failf(err, errlen, "%s", local);
        add_unsup(s, "identity: %s", local);
    }

    if (device_discover(p, &s->id, &s->dev, local, sizeof(local)) < 0) {
        if (strict)
            return failf(err, errlen, "%s", local);
        add_unsup(s, "storage: %s", local);
        return 0;
    }

    int fd = open(s->dev.disk, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (strict)
            return failf(err, errlen, "cannot open %s: %s", s->dev.disk, strerror(errno));
        add_unsup(s, "cannot open storage device %s: %s", s->dev.disk, strerror(errno));
        return 0;
    }
    int rc = gpt_read_fd(fd, s->dev.sector, s->dev.total_sectors, &s->gpt, local, sizeof(local));
    close(fd);
    if (rc < 0) {
        if (strict)
            return failf(err, errlen, "%s", local);
        add_unsup(s, "GPT: %s", local);
    } else {
        s->have_gpt = 1;
        if (gpt_validate_identity(&s->gpt, &s->dev, local, sizeof(local)) < 0) {
            if (strict)
                return failf(err, errlen, "%s", local);
            add_unsup(s, "GPT identity: %s", local);
        }
    }

    if (mounts_scan(p, &s->dev, &s->mounts, local, sizeof(local)) < 0) {
        if (strict)
            return failf(err, errlen, "%s", local);
        add_unsup(s, "mounts: %s", local);
    } else {
        s->have_mounts = 1;
    }

    if (s->have_gpt) {
        char detail[512];
        if (gpt_check_basic_layout(&s->gpt, s->dev.userdata_index, detail, sizeof(detail),
                                   local, sizeof(local)) < 0) {
            if (strict)
                return failf(err, errlen, "%s", local);
            add_unsup(s, "layout: %s", local);
        }
    }

    char ferr[512] = "";
    int fs_rc = -1;
    char data_path[PATH_MAX];
    int have_data_path = root_join(p, data_path, sizeof(data_path), "/data") == 0;
    if (s->have_mounts && s->mounts.data_count == 1 && s->dev.mapped[0] && have_data_path)
        fs_rc = f2fs_probe_mounted(data_path, s->dev.mapped, s->dev.userdata_bytes,
                                   &s->fs, ferr, sizeof(ferr));
    else if (s->dev.raw[0])
        fs_rc = f2fs_probe_path(s->dev.raw, s->dev.userdata_bytes, &s->fs, ferr, sizeof(ferr));
    if (fs_rc == 0) {
        s->have_fs = 1;
    } else if (ferr[0]) {
        if (strict)
            return failf(err, errlen, "%s", ferr);
        add_unsup(s, "filesystem: %s", ferr);
    }
    caps_check(p, &s->id, &s->dev, s->have_fs ? &s->fs : NULL, &s->caps, local, sizeof(local));

    if (s->id.serial[0] == 0 || !strcmp(s->id.serial, "unknown"))
        add_unsup(s, "no hardware serial was found; device matching would be ambiguous");
    if (s->have_fs && (s->fs.sb_offset || s->fs.sb_damaged_copy))
        add_unsup(s, "F2FS superblock copies disagree or are damaged; run fsck.f2fs");
    if (s->have_fs && s->fs.encrypt_feature)
        add_warn(s, "F2FS encrypt feature is set; recovery must decrypt /data before planning");
    if (s->have_mounts && s->mounts.data_count == 1 && !s->mounts.data_rw)
        add_unsup(s, "/data is mounted read-only; planning requires decrypted read-write F2FS");
    if (s->have_mounts && s->mounts.data_count == 1 && !s->mounts.data_checkpoint_enabled)
        add_unsup(s, "/data is mounted with checkpoint=disable");
    if (s->have_mounts && s->mounts.metadata_mounts)
        add_unsup(s, "metadata is mounted");
    if (!s->caps.f2fs_filesystem)
        add_unsup(s, "the recovery kernel does not list f2fs support");
    if (s->have_fs && !s->caps.all_ok)
        add_warn(s, "the read-only F2FS resize capability probe was inconclusive");
    if (s->dev.sector != 4096 && s->dev.sector != 512)
        add_unsup(s, "unsupported sector size %u", s->dev.sector);

    if (strict && s->n_unsupported)
        return failf(err, errlen, "unsupported condition: %s", s->unsupported[0]);
    return 0;
}

static void print_gpt_entries(report_t *r, const state_t *s)
{
    rp(r, "%5s  %-24s %14s %14s %14s  %s\n", "Index", "Name", "First LBA", "Last LBA", "Size", "Type GUID");
    for (size_t i = 0; i < s->gpt.n_entries; i++) {
        const gpt_entry_t *e = &s->gpt.entries[i];
        char size[64], type[37], unique[37];
        fmt_size((e->end - e->start + 1) * s->gpt.sector, size, sizeof(size));
        uuid_format_le(e->type_guid, type);
        uuid_format_le(e->unique_guid, unique);
        rp(r, "%5u  %-24s %14" PRIu64 " %14" PRIu64 " %14s  %s\n",
           e->index, e->name, e->start, e->end, size, type);
        if (e->index == (uint32_t)s->dev.userdata_index || e->index == (uint32_t)s->dev.metadata_index)
            rp(r, "       unique GUID %s, attributes 0x%016" PRIx64 "\n", unique, e->attrs);
    }
}

static void print_mounts(report_t *r, const state_t *s)
{
    rp_section(r, "Mounts and mappings");
    if (!s->have_mounts) {
        rp(r, "Mount table was not available; see unsupported conditions.\n");
        return;
    }
    if (!s->mounts.n) {
        rp(r, "No mounts were readable.\n");
        return;
    }
    rp(r, "%-28s %-24s %-10s %s\n", "Target", "Source", "Type", "Options");
    for (size_t i = 0; i < s->mounts.n; i++) {
        const mount_t *m = &s->mounts.items[i];
        if (m->is_data || m->is_userdata || m->is_metadata)
            rp(r, "%-28s %-24s %-10s %s%s%s\n", m->target, m->source, m->fstype, m->opts,
               m->is_data ? " [DATA]" : "", m->is_userdata ? " [USERDATA]" : "",
               m->is_metadata ? " [METADATA]" : "");
    }
    rp_kv(r, "Decrypted /data mapping", "%s", s->dev.mapped[0] ? s->dev.mapped : "none");
    rp_kv(r, "userdata raw device", "%s", s->dev.raw);
    rp_kv(r, "metadata device", "%s", s->dev.metadata);
    rp_kv(r, "metadata mounted", "%s", s->mounts.metadata_mounts ? "yes" : "no");
}

static void print_fs(report_t *r, const state_t *s)
{
    rp_section(r, "Filesystem");
    if (!s->have_fs) {
        rp(r, "F2FS probe did not succeed; see unsupported conditions.\n");
        return;
    }
    const f2fs_info_t *fs = &s->fs;
    rp_kv(r, "Type", "F2FS");
    rp_kv(r, "Mounted", "%s", fs->mounted ? "yes" : "no");
    if (fs->mounted) {
        rp_kv(r, "Mounted source", "%s", fs->mount_source);
        rp_kv(r, "statfs blocks", "%" PRIu64, fs->f_blocks);
        rp_kv(r, "statfs free blocks", "%" PRIu64, fs->f_bfree);
        rp_kv(r, "statfs available blocks", "%" PRIu64, fs->f_bavail);
    }
    rp_kv(r, "Block size", "%u", fs->block_size);
    rp_kv(r, "Blocks per segment", "%u", fs->blocks_per_seg);
    rp_kv(r, "Segments per section", "%u", fs->segs_per_sec);
    rp_kv(r, "Section blocks", "%u", fs->section_blocks);
    rp_kv(r, "Block count", "%" PRIu64, fs->blocks);
    rp_kv(r, "Section count", "%u", fs->section_count);
    rp_kv(r, "Segment count", "%u", fs->segment_count);
    rp_kv(r, "Main segments", "%u", fs->segment_count_main);
    rp_kv(r, "UUID", "%s", fs->uuid);
    rp_kv(r, "Volume name", "%s", fs->volume_name);
    rp_kv(r, "Features", "0x%08x%s%s", fs->feature,
          fs->encrypt_feature ? " encrypt" : "", fs->zoned ? " zoned" : "");
    /*
     * Only the superblock is reported. The checkpoint is kernel-owned and is
     * rewritten while /data is mounted, so a userspace read of it is a race,
     * not evidence; the resize ioctl is the authority on whether the kernel
     * will shrink this filesystem.
     */
    rp_kv(r, "Superblock copies match", "%s", fs->sb_offset ? "no" : "yes");
    if (fs->sb_damaged_copy)
        rp_kv(r, "Damaged superblock copy", "%d", fs->sb_damaged_copy);
    else if (fs->sb_offset)
        rp_kv(r, "Superblock disagreement", "layout fields differ between copies");
}

static void print_gpt(report_t *r, const state_t *s)
{
    rp_section(r, "GPT");
    if (!s->have_gpt) {
        rp(r, "GPT was not readable; see unsupported conditions.\n");
        return;
    }
    char guid[37];
    uuid_format_le(s->gpt.disk_guid, guid);
    rp_kv(r, "Disk GUID", "%s", guid);
    rp_kv(r, "Entries", "%u of %u (entry size %u)", (unsigned)s->gpt.n_entries,
          s->gpt.count, s->gpt.entry_size);
    rp_kv(r, "Entry table blocks", "%u", s->gpt.blocks);
    rp_kv(r, "Usable LBA range", "%" PRIu64 "..%" PRIu64, s->gpt.first_usable, s->gpt.last_usable);
    rp_kv(r, "Primary header CRC", "0x%08x", s->gpt.primary_crc);
    rp_kv(r, "Backup header CRC", "0x%08x", s->gpt.backup_crc);
    rp_kv(r, "Entry array CRC", "0x%08x", s->gpt.table_crc);
    uint8_t h[32];
    char hex[65];
    sha256_buf(s->gpt.primary, s->gpt.primary_len, h);
    hex_encode(h, 32, hex);
    rp_kv(r, "Primary region SHA-256", "%s", hex);
    sha256_buf(s->gpt.backup, s->gpt.backup_len, h);
    hex_encode(h, 32, hex);
    rp_kv(r, "Backup region SHA-256", "%s", hex);
    rp(r, "\n");
    print_gpt_entries(r, s);
}

static void print_device(report_t *r, const state_t *s)
{
    rp_section(r, "Device identity");
    rp_kv(r, "Device", "%s", s->id.device[0] ? s->id.device : "(not reported)");
    rp_kv(r, "Model", "%s", s->id.model[0] ? s->id.model : "(not reported)");
    rp_kv(r, "Hardware serial", "%s", s->id.serial[0] ? s->id.serial : "(not found)");
    rp_kv(r, "Recovery", "%s%s%s", s->id.recovery,
          s->id.twrp[0] ? " " : "", s->id.twrp);
    rp_kv(r, "Kernel", "%s", s->id.kernel);
    rp_kv(r, "Architecture", "%s", s->id.machine);
    rp_kv(r, "Device-tree model", "%s", s->id.dt_model[0] ? s->id.dt_model : "(not found)");
    rp_kv(r, "Android boot completed", "%s", s->id.boot_completed ? "yes" : "no");
    rp_kv(r, "USB config", "%s", s->id.usb_config[0] ? s->id.usb_config : "(not reported)");
    rp_kv(r, "USB state", "%s", s->id.usb_state[0] ? s->id.usb_state : "(not reported)");
}

static void print_storage(report_t *r, const state_t *s)
{
    rp_section(r, "Storage and partition geometry");
    if (!s->dev.disk[0]) {
        rp(r, "Storage could not be discovered; see unsupported conditions.\n");
        return;
    }
    rp_kv(r, "Storage device", "%s", s->dev.disk);
    rp_kv(r, "Logical sector size", "%u bytes", s->dev.sector);
    rp_kv(r, "Physical sector size", "%u bytes", s->dev.physical_sector);
    rp_kv(r, "Disk size", "%" PRIu64 " bytes", s->dev.disk_bytes);
    rp_kv(r, "Total logical sectors", "%" PRIu64, s->dev.total_sectors);
    rp_kv(r, "userdata by-name", "%s", s->dev.raw);
    rp_kv(r, "userdata index", "%d", s->dev.userdata_index);
    rp_kv(r, "userdata start LBA", "%" PRIu64, s->dev.userdata_start_lba);
    rp_kv(r, "userdata end LBA", "%" PRIu64, s->dev.userdata_end_lba);
    rp_kv(r, "userdata size", "%" PRIu64 " bytes", s->dev.userdata_bytes);
    rp_kv(r, "metadata by-name", "%s", s->dev.metadata);
    rp_kv(r, "metadata index", "%d", s->dev.metadata_index);
    rp_kv(r, "metadata start LBA", "%" PRIu64, s->dev.metadata_start_lba);
    rp_kv(r, "metadata end LBA", "%" PRIu64, s->dev.metadata_end_lba);
    rp_kv(r, "metadata size", "%" PRIu64 " bytes", s->dev.metadata_bytes);
    rp_kv(r, "Disk serial", "%s", s->dev.disk_serial);
}

static void print_caps(report_t *r, const state_t *s)
{
    rp_section(r, "Recovery kernel capability checks");
    rp_kv(r, "f2fs listed", "%s", s->caps.f2fs_filesystem ? "yes" : "no");
    rp_kv(r, "f2fs sysfs", "%s", s->caps.f2fs_sysfs ? "yes" : "no");
    rp_kv(r, "F2FS ioctl namespace", "%s", s->caps.ioctl_namespace ? "yes" : "unknown");
    rp_kv(r, "GET_FEATURES ioctl", "%s", s->caps.getfeatures_ok ? "yes" : "no/unknown");
    rp_kv(r, "resize symbol in kallsyms", "%s", s->caps.resize_symbol ? "yes" : "no/unknown");
    rp_kv(r, "Kernel version check", "%s", s->caps.kernel_ok ? "pass" : "fail");
    rp_kv(r, "Read-only capability verdict", "%s", s->caps.all_ok ? "available" : "unverified");
    rp_kv(r, "Details", "%s", s->caps.details);
}

static void print_layout_checks(report_t *r, const state_t *s)
{
    rp_section(r, "Layout checks");
    if (!s->have_gpt) {
        rp(r, "Layout checks need a valid GPT.\n");
        return;
    }
    const gpt_entry_t *u = gpt_entry_by_index(&s->gpt, (uint32_t)s->dev.userdata_index);
    uint64_t max_end = 0;
    for (size_t i = 0; i < s->gpt.n_entries; i++)
        max_end = MAX(max_end, s->gpt.entries[i].end);
    uint32_t free_index = 0;
    int free_ok = gpt_free_entry(&s->gpt, &free_index) == 0;
    rp_kv(r, "userdata is last occupied", "%s", u && u->end == max_end ? "yes" : "no");
    rp_kv(r, "userdata start alignment", "%s",
          u && (u->start * s->gpt.sector) % F2FS_BLOCK_BYTES == 0 ? "4096-byte aligned" : "bad");
    rp_kv(r, "unixnu exists", "%s", gpt_entry_by_name(&s->gpt, "unixnu", 1) ? "yes" : "no");
    rp_kv(r, "Unused GPT entry", "%s", free_ok ? "available" : "none");
    if (free_ok)
        rp_kv(r, "First unused entry", "%u", free_index);
    uint64_t usable_bytes = (max_end - (u ? u->start : 0) + 1) * s->gpt.sector;
    rp_kv(r, "Current userdata range", "%" PRIu64 " bytes", usable_bytes);
    if (s->have_fs)
        rp_kv(r, "Current F2FS blocks", "%" PRIu64 " (%" PRIu64 " bytes)",
              s->fs.blocks, s->fs.blocks * F2FS_BLOCK_BYTES);
}

static void print_result(report_t *r, state_t *s)
{
    rp_section(r, "Warnings and unsupported conditions");
    if (!s->n_warnings && !s->n_unsupported)
        rp(r, "None.\n");
    for (int i = 0; i < s->n_warnings; i++)
        rp(r, "WARNING: %s\n", s->warnings[i]);
    for (int i = 0; i < s->n_unsupported; i++)
        rp(r, "UNSUPPORTED [%s]: %s\n", s->unsupported_action[i] ? s->unsupported_action[i] : "hard",
           s->unsupported[i]);
    rp_section(r, "Verdict");
    rp(r, "RESULT: %s\n", state_verdict(s));
    rp(r, "\nDATA-LOSS WARNING: GPT and filesystem operations can destroy data. This inspect\n"
          "report is read-only. A GPT backup does not contain file contents, and restoring an\n"
          "old GPT cannot reverse a failed F2FS shrink. Keep an independent backup.\n");
}

/* The inspect JSON shape changed when unsupported reasons gained actions and
 * the filesystem block gained the superblock fields, so consumers of schema 1
 * must refuse rather than silently miss the new keys. */
#define INSPECT_SCHEMA 2

static void write_inspect_json(const state_t *s, const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    fprintf(f, "{\n  \"schema\": %d,\n  \"tool\": \"" TOOL_NAME "\",\n  \"version\": \"" TOOL_VERSION "\",\n",
            INSPECT_SCHEMA);
    fprintf(f, "  \"result\": ");
    json_string(f, state_verdict(s));
    fprintf(f, ",\n  \"identity\": {\n    \"device\": ");
    json_string(f, s->id.device);
    fprintf(f, ",\n    \"model\": ");
    json_string(f, s->id.model);
    fprintf(f, ",\n    \"serial\": ");
    json_string(f, s->id.serial);
    fprintf(f, ",\n    \"recovery\": ");
    json_string(f, s->id.recovery);
    fprintf(f, ",\n    \"kernel\": ");
    json_string(f, s->id.kernel);
    fprintf(f, ",\n    \"machine\": ");
    json_string(f, s->id.machine);
    fprintf(f, ",\n    \"dt_model\": ");
    json_string(f, s->id.dt_model);
    fprintf(f, "\n  },\n  \"device\": {\n    \"disk\": ");
    json_string(f, s->dev.disk);
    fprintf(f, ",\n    \"sector\": %u,\n    \"disk_bytes\": %" PRIu64 ",\n    \"total_sectors\": %" PRIu64,
            s->dev.sector, s->dev.disk_bytes, s->dev.total_sectors);
    fprintf(f, ",\n    \"userdata\": {\"raw\": ");
    json_string(f, s->dev.raw);
    fprintf(f, ", \"index\": %d, \"start_lba\": %" PRIu64 ", \"end_lba\": %" PRIu64 ", \"bytes\": %" PRIu64 "}",
            s->dev.userdata_index, s->dev.userdata_start_lba, s->dev.userdata_end_lba, s->dev.userdata_bytes);
    fprintf(f, ",\n    \"metadata\": {\"raw\": ");
    json_string(f, s->dev.metadata);
    fprintf(f, ", \"index\": %d, \"start_lba\": %" PRIu64 ", \"end_lba\": %" PRIu64 ", \"bytes\": %" PRIu64 "}",
            s->dev.metadata_index, s->dev.metadata_start_lba, s->dev.metadata_end_lba, s->dev.metadata_bytes);
    fprintf(f, "\n  },\n  \"filesystem\": ");
    if (s->have_fs) {
        fprintf(f, "{\"type\":\"f2fs\",\"blocks\":%" PRIu64 ",\"block_size\":%u,\"blocks_per_seg\":%u,"
                "\"segs_per_sec\":%u,\"section_blocks\":%u,\"mounted\":%s,\"available_blocks\":%" PRIu64 ","
                "\"encrypt_feature\":%s,\"sb_offset\":%u,\"sb_damaged_copy\":%d}",
                s->fs.blocks, s->fs.block_size, s->fs.blocks_per_seg, s->fs.segs_per_sec,
                s->fs.section_blocks, s->fs.mounted ? "true" : "false", s->fs.f_bavail,
                s->fs.encrypt_feature ? "true" : "false",
                s->fs.sb_offset, s->fs.sb_damaged_copy);
    } else {
        fprintf(f, "null");
    }
    if (s->have_gpt)
        fprintf(f, ",\n  \"gpt\": {\"count\":%u,\"used\":%zu,\"blocks\":%u,\"first_usable\":%" PRIu64 ","
                "\"last_usable\":%" PRIu64 "},\n", s->gpt.count, s->gpt.n_entries, s->gpt.blocks,
                s->gpt.first_usable, s->gpt.last_usable);
    else
        fprintf(f, ",\n  \"gpt\": null,\n");
    fprintf(f, "  \"warnings\": [");
    for (int i = 0; i < s->n_warnings; i++) {
        if (i) fprintf(f, ", ");
        json_string(f, s->warnings[i]);
    }
    fprintf(f, "],\n  \"unsupported\": [");
    for (int i = 0; i < s->n_unsupported; i++) {
        if (i) fprintf(f, ", ");
        fprintf(f, "{\"reason\": ");
        json_string(f, s->unsupported[i]);
        fprintf(f, ", \"action\": ");
        json_string(f, s->unsupported_action[i] ? s->unsupported_action[i] : "hard");
        fprintf(f, "}");
    }
    fprintf(f, "]\n}\n");
    fclose(f);
}

int inspect_run(const paths_t *p, const char *out_path, char *err, size_t errlen)
{
    state_t s;
    if (state_collect(p, &s, 0, err, errlen) < 0) {
        state_free(&s);
        return -1;
    }
    char path[PATH_MAX];
    if (out_path && out_path[0]) {
        snprintf(path, sizeof(path), "%s", out_path);
    } else {
        if (root_join(p, path, sizeof(path), "/sdcard/Amethyst-UniXNU-Partitioner/inspect.txt") < 0)
            return failf(err, errlen, "path too long");
    }
    char dir[PATH_MAX];
    if (path_dirname(path, dir, sizeof(dir)) < 0 || mkdir_p(dir, 0755) < 0)
        return failf(err, errlen, "cannot create report directory %s: %s", dir, strerror(errno));
    report_t r;
    rp_open(&r, path, 1);
    rp(&r, "Amethyst UniXNU Partitioner - read-only inspect\n");
    rp(&r, "Tool: " TOOL_NAME " " TOOL_VERSION "\n");
    char now[64];
    now_utc(now, sizeof(now));
    rp(&r, "Generated: %s\n", now);
    print_device(&r, &s);
    print_storage(&r, &s);
    print_mounts(&r, &s);
    print_fs(&r, &s);
    print_gpt(&r, &s);
    print_caps(&r, &s);
    print_layout_checks(&r, &s);
    print_result(&r, &s);
    rp_close(&r);
    char json[PATH_MAX];
    snprintf(json, sizeof(json), "%s", path);
    char *dot = strrchr(json, '.');
    if (dot && !strcmp(dot, ".txt"))
        snprintf(dot, sizeof(json) - (size_t)(dot - json), ".json");
    else
        snprintf(json + strlen(json), sizeof(json) - strlen(json), ".json");
    write_inspect_json(&s, json);
    state_free(&s);
    return 0;
}
