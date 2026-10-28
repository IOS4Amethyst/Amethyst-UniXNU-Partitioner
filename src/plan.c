/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Plan creation, bundle checks and device matching. Read-only except for files
 * written under the user-selected bundle directory. */
#include "common.h"
#include <ctype.h>

#define PLAN_SCHEMA 1

static int req_kv(void *arg, const char *key, const char *val)
{
    request_t *r = arg;
    if (!strcmp(key, "size_gib") || !strcmp(key, "size")) {
        snprintf(r->size, sizeof(r->size), "%s", val);
    } else if (!strcmp(key, "plan_dir")) {
        snprintf(r->plan_dir, sizeof(r->plan_dir), "%s", val);
    } else if (!strcmp(key, "mode")) {
        snprintf(r->mode, sizeof(r->mode), "%s", val);
    } else if (!strcmp(key, "plan")) {
        snprintf(r->plan, sizeof(r->plan), "%s", val);
    } else if (!strcmp(key, "confirm")) {
        snprintf(r->confirm, sizeof(r->confirm), "%s", val);
    } else if (!strcmp(key, "acknowledge_data_loss")) {
        r->acknowledge_data_loss = !strcasecmp(val, "yes") || !strcasecmp(val, "true") ||
                                   !strcmp(val, "1") || !strcasecmp(val, "i-understand");
    }
    return 0;
}

int read_request_file(const char *path, request_t *req, char *err, size_t errlen)
{
    memset(req, 0, sizeof(*req));
    if (parse_kv_file(path, req_kv, req) < 0)
        return failf(err, errlen, "cannot read request file %s (use key = value lines)", path);
    return 0;
}

static void set_guid(char out[37], const uint8_t guid[16])
{
    uuid_format_le(guid, out);
}

static int plan_path(const char *dir, const char *name, char *out, size_t cap)
{
    return path_join(out, cap, dir, name);
}

int plan_check_stage(const char *plan_dir, const char *expect, char *stage, size_t stage_cap,
                     char *err, size_t errlen)
{
    char path[PATH_MAX];
    if (plan_path(plan_dir, "stage", path, sizeof(path)) < 0)
        return failf(err, errlen, "plan path is too long");
    if (read_text(path, stage, stage_cap) < 0)
        return failf(err, errlen, "plan has no stage file; create a fresh plan");
    trim(stage);
    if (expect && strcmp(stage, expect))
        return failf(err, errlen, "plan stage is '%s', expected '%s'; do not retry, run recover",
                     stage, expect);
    return 0;
}

int plan_write_stage(const char *plan_dir, const char *stage, char *err, size_t errlen)
{
    char path[PATH_MAX], log[PATH_MAX], line[256], now[64];
    if (plan_path(plan_dir, "stage", path, sizeof(path)) < 0 ||
        plan_path(plan_dir, "apply.log", log, sizeof(log)) < 0)
        return failf(err, errlen, "plan path is too long");
    now_utc(now, sizeof(now));
    if (write_atomic_text(path, stage, 0600) < 0)
        return failf(err, errlen, "cannot persist stage %s: %s", path, strerror(errno));
    snprintf(line, sizeof(line), "%s %s\n", now, stage);
    FILE *f = fopen(log, "a");
    if (f) {
        fputs(line, f);
        fclose(f);
    }
    return 0;
}

int plan_bundle_verify(const char *plan_dir, char *err, size_t errlen)
{
    char path[PATH_MAX];
    if (plan_path(plan_dir, "SHA256SUMS", path, sizeof(path)) < 0)
        return failf(err, errlen, "plan path is too long");
    FILE *f = fopen(path, "r");
    if (!f)
        return failf(err, errlen, "cannot open %s: %s", path, strerror(errno));
    char line[1024];
    int checked = 0;
    while (fgets(line, sizeof(line), f)) {
        trim(line);
        if (!*line || *line == '#')
            continue;
        char *space = strstr(line, "  ");
        if (!space)
            space = strchr(line, ' ');
        if (!space)
            return failf(err, errlen, "malformed SHA256SUMS line");
        *space = '\0';
        const char *name = space + 1;
        while (*name == ' ')
            name++;
        if (*name == '*')
            name++;
        if (!*name || strchr(name, '/') || !strcmp(name, "..") || !strcmp(name, "."))
            return failf(err, errlen, "unsafe name in SHA256SUMS");
        char file[PATH_MAX], hex[65];
        if (plan_path(plan_dir, name, file, sizeof(file)) < 0)
            return failf(err, errlen, "plan path is too long");
        uint8_t hash[32];
        if (sha256_file(file, hash) < 0)
            return failf(err, errlen, "cannot hash %s: %s", file, strerror(errno));
        hex_encode(hash, 32, hex);
        if (strcasecmp(hex, line))
            return failf(err, errlen, "bundle file changed: %s", name);
        checked++;
    }
    fclose(f);
    if (checked < 6)
        return failf(err, errlen, "SHA256SUMS is incomplete (%d files)", checked);
    return 0;
}

static int write_sha256sums(const char *plan_dir, const char *const *names, size_t count,
                            char *err, size_t errlen)
{
    char text[16384];
    size_t used = 0;
    text[0] = '\0';
    for (size_t i = 0; i < count; i++) {
        char file[PATH_MAX], hex[65];
        if (plan_path(plan_dir, names[i], file, sizeof(file)) < 0)
            return failf(err, errlen, "plan path is too long");
        uint8_t hash[32];
        if (sha256_file(file, hash) < 0)
            return failf(err, errlen, "cannot hash %s: %s", file, strerror(errno));
        hex_encode(hash, 32, hex);
        int n = snprintf(text + used, sizeof(text) - used, "%s  %s\n", hex, names[i]);
        if (n < 0 || (size_t)n >= sizeof(text) - used)
            return failf(err, errlen, "SHA256SUMS is too large");
        used += (size_t)n;
    }
    char path[PATH_MAX];
    if (plan_path(plan_dir, "SHA256SUMS", path, sizeof(path)) < 0)
        return failf(err, errlen, "plan path is too long");
    if (write_text(path, text, 0644) < 0)
        return failf(err, errlen, "cannot write SHA256SUMS: %s", strerror(errno));
    return 0;
}

int plan_read_file(const char *plan_dir, const char *name, uint8_t **data, size_t *len,
                   char *err, size_t errlen)
{
    char path[PATH_MAX];
    if (plan_path(plan_dir, name, path, sizeof(path)) < 0)
        return failf(err, errlen, "plan path is too long");
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return failf(err, errlen, "cannot open %s: %s", path, strerror(errno));
    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size <= 0) {
        close(fd);
        return failf(err, errlen, "cannot stat %s", path);
    }
    uint8_t *buf = xmalloc((size_t)st.st_size);
    size_t done = 0;
    while (done < (size_t)st.st_size) {
        ssize_t n = read(fd, buf + done, (size_t)st.st_size - done);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            free(buf);
            close(fd);
            return failf(err, errlen, "cannot read %s: %s", path, strerror(errno));
        }
        if (!n)
            break;
        done += (size_t)n;
    }
    close(fd);
    if (done != (size_t)st.st_size) {
        free(buf);
        return failf(err, errlen, "short read from %s", path);
    }
    *data = buf;
    *len = done;
    return 0;
}

static int run_plan(const paths_t *p, const request_t *req, const char *plan_os,
                    char *err, size_t errlen)
{
    uint64_t mib = 0;
    if (!req->size[0])
        return failf(err, errlen, "request must set size_gib");
    if (parse_gib_to_mib(req->size, &mib, err, errlen) < 0)
        return -1;
    uint64_t requested = mib * MIB;

    state_t s;
    if (state_collect(p, &s, 1, err, errlen) < 0) {
        state_free(&s);
        return -1;
    }
    if (!s.have_mounts || s.mounts.data_count != 1) {
        state_free(&s);
        return failf(err, errlen, "mount decrypted /data read-write before planning");
    }
    if (mounts_check_for_plan(p, &s.dev, &s.mounts, err, errlen) < 0) {
        state_free(&s);
        return -1;
    }
    if (s.mounts.metadata_mounts) {
        state_free(&s);
        return failf(err, errlen, "metadata must be unmounted before planning");
    }
    uint8_t metadata_hash[32];
    if (metadata_content_hash(p, &s.dev, metadata_hash, err, errlen) < 0) {
        state_free(&s);
        return -1;
    }

    uint8_t *primary = NULL, *backup = NULL;
    size_t primary_len = 0, backup_len = 0;
    layout_t layout;
    if (gpt_make_layout(&s.gpt, s.dev.userdata_index, requested, MIB, &layout,
                        &primary, &primary_len, &backup, &backup_len, err, errlen) < 0) {
        state_free(&s);
        return -1;
    }

    uint64_t limit_blocks = layout.new_userdata_bytes / F2FS_BLOCK_BYTES;
    uint64_t expected = s.fs.blocks;
    if (s.fs.blocks > limit_blocks) {
        uint64_t request = f2fs_request_blocks(s.fs.blocks, limit_blocks);
        if (!request) {
            free(primary);
            free(backup);
            state_free(&s);
            return failf(err, errlen, "remaining userdata is too small for F2FS section rounding");
        }
        expected = f2fs_expected_blocks(s.fs.blocks, request);
        if (s.fs.blocks - request >= s.fs.f_bavail) {
            free(primary);
            free(backup);
            state_free(&s);
            return failf(err, errlen,
                         "not enough free F2FS space: need %" PRIu64 " blocks, only %" PRIu64
                         " are available to the filesystem",
                         s.fs.blocks - request, s.fs.f_bavail);
        }
    }
    if (expected * F2FS_BLOCK_BYTES > layout.new_userdata_bytes) {
        free(primary);
        free(backup);
        state_free(&s);
        return failf(err, errlen, "F2FS size after section rounding exceeds the planned userdata");
    }

    char plan_id[33];
    uint8_t rnd[16];
    if (uuid4(rnd) < 0) {
        free(primary);
        free(backup);
        state_free(&s);
        return failf(err, errlen, "cannot generate a plan ID");
    }
    hex_encode(rnd, 16, plan_id);

    char confirm[512];
    snprintf(confirm, sizeof(confirm), "unixnu %s %s %" PRIu64, plan_id,
             s.id.serial[0] ? s.id.serial : "unknown", layout.unixnu_bytes);

    char parent[PATH_MAX];
    if (path_dirname(plan_os, parent, sizeof(parent)) < 0 || mkdir_p(parent, 0755) < 0) {
        free(primary);
        free(backup);
        state_free(&s);
        return failf(err, errlen, "cannot create parent of %s: %s", plan_os, strerror(errno));
    }
    if (mkdir(plan_os, 0700) < 0 && errno != EEXIST) {
        free(primary);
        free(backup);
        state_free(&s);
        return failf(err, errlen, "cannot create plan directory %s: %s", plan_os, strerror(errno));
    }
    struct stat pst;
    if (stat(plan_os, &pst) < 0 || !S_ISDIR(pst.st_mode)) {
        free(primary);
        free(backup);
        state_free(&s);
        return failf(err, errlen, "%s is not a directory", plan_os);
    }
    if (access(plan_os, W_OK) < 0) {
        free(primary);
        free(backup);
        state_free(&s);
        return failf(err, errlen, "%s is not writable", plan_os);
    }
    char existing[PATH_MAX];
    if (plan_path(plan_os, "plan.conf", existing, sizeof(existing)) == 0 && access(existing, F_OK) == 0) {
        free(primary);
        free(backup);
        state_free(&s);
        return failf(err, errlen, "plan directory already contains a plan; choose a new directory");
    }

    const char *names[] = {
        "gpt-primary.before", "gpt-backup.before",
        "gpt-primary.after", "gpt-backup.after",
        "plan.conf", "plan.txt", "plan.json", "plan.env", "session",
    };
    char path[PATH_MAX];

    if (plan_path(plan_os, "gpt-primary.before", path, sizeof(path)) < 0 ||
        write_file(path, s.gpt.primary, s.gpt.primary_len, 0600) < 0 ||
        plan_path(plan_os, "gpt-backup.before", path, sizeof(path)) < 0 ||
        write_file(path, s.gpt.backup, s.gpt.backup_len, 0600) < 0 ||
        plan_path(plan_os, "gpt-primary.after", path, sizeof(path)) < 0 ||
        write_file(path, primary, primary_len, 0600) < 0 ||
        plan_path(plan_os, "gpt-backup.after", path, sizeof(path)) < 0 ||
        write_file(path, backup, backup_len, 0600) < 0) {
        free(primary);
        free(backup);
        state_free(&s);
        return failf(err, errlen, "cannot write GPT images: %s", strerror(errno));
    }

    char created[64], disk_guid[37], user_type[37], user_unique[37], unix_type[37], unix_unique[37];
    char metadata_hex[65], fs_sb_hex[65], primary_hex[65], backup_hex[65];
    uint8_t h[32];
    now_utc(created, sizeof(created));
    uuid_format_le(s.gpt.disk_guid, disk_guid);
    set_guid(user_type, layout.userdata_type_guid);
    set_guid(user_unique, layout.userdata_unique_guid);
    set_guid(unix_type, layout.unixnu_type_guid);
    set_guid(unix_unique, layout.unixnu_unique_guid);
    hex_encode(metadata_hash, 32, metadata_hex);
    hex_encode(s.fs.sb_sha256, 32, fs_sb_hex);
    sha256_buf(s.gpt.primary, s.gpt.primary_len, h);
    hex_encode(h, 32, primary_hex);
    sha256_buf(s.gpt.backup, s.gpt.backup_len, h);
    hex_encode(h, 32, backup_hex);
    uint64_t backup_lba = s.gpt.total_sectors - 1 - s.gpt.blocks;

    char conf[8192];
    int n = snprintf(conf, sizeof(conf),
        "schema=%d\n"
        "tool=" TOOL_NAME "\n"
        "version=" TOOL_VERSION "\n"
        "plan_id=%s\n"
        "created=%s\n"
        "size_bytes=%" PRIu64 "\n"
        "serial=%s\n"
        "disk=%s\n"
        "disk_bytes=%" PRIu64 "\n"
        "sector=%u\n"
        "userdata_index=%d\n"
        "userdata_start_lba=%" PRIu64 "\n"
        "userdata_end_lba=%" PRIu64 "\n"
        "userdata_bytes=%" PRIu64 "\n"
        "userdata_new_end_lba=%" PRIu64 "\n"
        "userdata_new_bytes=%" PRIu64 "\n"
        "metadata_index=%d\n"
        "metadata_start_lba=%" PRIu64 "\n"
        "metadata_end_lba=%" PRIu64 "\n"
        "metadata_bytes=%" PRIu64 "\n"
        "metadata_sha256=%s\n"
        "gpt_primary_sha256=%s\n"
        "gpt_backup_sha256=%s\n"
        "gpt_primary_len=%zu\n"
        "gpt_backup_len=%zu\n"
        "gpt_table_lba=%u\n"
        "gpt_backup_lba=%" PRIu64 "\n"
        "gpt_disk_guid=%s\n"
        "fs_blocks=%" PRIu64 "\n"
        "fs_uuid=%s\n"
        "fs_sb_sha256=%s\n"
        "fs_feature=%u\n"
        "limit_blocks=%" PRIu64 "\n"
        "expected_blocks=%" PRIu64 "\n"
        "unixnu_index=%d\n"
        "unixnu_start_lba=%" PRIu64 "\n"
        "unixnu_end_lba=%" PRIu64 "\n"
        "unixnu_bytes=%" PRIu64 "\n"
        "unixnu_type_guid=%s\n"
        "unixnu_unique_guid=%s\n"
        "userdata_type_guid=%s\n"
        "userdata_unique_guid=%s\n"
        "userdata_attrs=%" PRIu64 "\n"
        "tail_gap_bytes=%" PRIu64 "\n"
        "confirm=%s\n",
        PLAN_SCHEMA, plan_id, created, layout.unixnu_bytes, s.id.serial, s.dev.disk,
        s.dev.disk_bytes, s.dev.sector, layout.userdata_index, layout.userdata_start,
        s.dev.userdata_end_lba, layout.old_userdata_bytes, layout.userdata_end,
        layout.new_userdata_bytes, s.dev.metadata_index, s.dev.metadata_start_lba,
        s.dev.metadata_end_lba, s.dev.metadata_bytes, metadata_hex, primary_hex, backup_hex,
        primary_len, backup_len, 2u, backup_lba, disk_guid, s.fs.blocks, s.fs.uuid,
        fs_sb_hex, s.fs.feature, limit_blocks, expected, layout.unixnu_index,
        layout.unixnu_start, layout.unixnu_end, layout.unixnu_bytes, unix_type,
        unix_unique, user_type, user_unique, layout.userdata_attrs, layout.tail_gap_bytes,
        confirm);
    if (n < 0 || (size_t)n >= sizeof(conf)) {
        free(primary);
        free(backup);
        state_free(&s);
        return failf(err, errlen, "plan.conf is too large");
    }
    if (plan_path(plan_os, "plan.conf", path, sizeof(path)) < 0 ||
        write_file(path, conf, (size_t)n, 0600) < 0) {
        free(primary);
        free(backup);
        state_free(&s);
        return failf(err, errlen, "cannot write plan.conf: %s", strerror(errno));
    }

    char env[8192];
    n = snprintf(env, sizeof(env),
        "PLAN_ID='%s'\n"
        "CONFIRM='%s'\n"
        "SIZE_BYTES='%" PRIu64 "'\n"
        "USERDATA_INDEX='%d'\n"
        "USERDATA_START_LBA='%" PRIu64 "'\n"
        "USERDATA_NEW_BYTES='%" PRIu64 "'\n"
        "UNIXNU_INDEX='%d'\n"
        "UNIXNU_START_LBA='%" PRIu64 "'\n"
        "UNIXNU_END_LBA='%" PRIu64 "'\n"
        "UNIXNU_BYTES='%" PRIu64 "'\n"
        "LIMIT_BLOCKS='%" PRIu64 "'\n"
        "EXPECTED_BLOCKS='%" PRIu64 "'\n"
        "DISK='%s'\n"
        "DISK_BYTES='%" PRIu64 "'\n"
        "SECTOR='%u'\n"
        "HARDWARE_SERIAL='%s'\n"
        "GPT_BACKUP_LBA='%" PRIu64 "'\n",
        plan_id, confirm, layout.unixnu_bytes, layout.userdata_index, layout.userdata_start,
        layout.new_userdata_bytes, layout.unixnu_index, layout.unixnu_start, layout.unixnu_end,
        layout.unixnu_bytes, limit_blocks, expected, s.dev.disk, s.dev.disk_bytes, s.dev.sector,
        s.id.serial, backup_lba);
    if (n < 0 || (size_t)n >= sizeof(env) ||
        plan_path(plan_os, "plan.env", path, sizeof(path)) < 0 ||
        write_file(path, env, (size_t)n, 0600) < 0) {
        free(primary);
        free(backup);
        state_free(&s);
        return failf(err, errlen, "cannot write plan.env: %s", strerror(errno));
    }

    char txt[8192];
    char old_sz[64], new_sz[64], unix_sz[64], gap_sz[64];
    fmt_bytes(layout.old_userdata_bytes, old_sz, sizeof(old_sz));
    fmt_bytes(layout.new_userdata_bytes, new_sz, sizeof(new_sz));
    fmt_bytes(layout.unixnu_bytes, unix_sz, sizeof(unix_sz));
    fmt_bytes(layout.tail_gap_bytes, gap_sz, sizeof(gap_sz));
    n = snprintf(txt, sizeof(txt),
        "Amethyst UniXNU partition plan\n"
        "Tool: " TOOL_NAME " " TOOL_VERSION "\n"
        "Created: %s\n"
        "Plan ID: %s\n"
        "\n"
        "Device: %s\n"
        "Hardware serial: %s\n"
        "Storage: %s (%" PRIu64 " bytes, %u-byte logical sectors)\n"
        "userdata: index %d, LBA %" PRIu64 "..%" PRIu64 ", %s\n"
        "metadata: index %d, LBA %" PRIu64 "..%" PRIu64 " (unchanged)\n"
        "\n"
        "Planned unixnu: index %d, LBA %" PRIu64 "..%" PRIu64 ", %s\n"
        "Remaining userdata: LBA %" PRIu64 "..%" PRIu64 ", %s\n"
        "Alignment gap after unixnu: %s\n"
        "unixnu type GUID: %s\n"
        "unixnu unique GUID: %s\n"
        "\n"
        "F2FS blocks: %" PRIu64 " -> %" PRIu64 " (partition limit %" PRIu64 ")\n"
        "Available F2FS blocks reported by statfs: %" PRIu64 "\n"
        "\n"
        "Bundle contents: plan.conf, plan.txt, plan.json, plan.env, session, stage,\n"
        "gpt-primary.before, gpt-backup.before, gpt-primary.after, gpt-backup.after,\n"
        "SHA256SUMS.\n"
        "\n"
        "BUNDLE LOCATION FOR APPLY\n"
        "Apply unmounts /data, so it refuses a bundle stored on /data, /sdcard or other\n"
        "userdata-backed internal storage. Copy this directory to an external SD card\n"
        "(or /tmp for a single recovery session) and set plan = that path in the Apply\n"
        "request. The stage log and apply session must stay writable until <data> is\n"
        "unmounted and both GPT copies are verified.\n"
        "\n"
        "To apply, run the Apply ZIP with mode = apply, plan = this directory,\n"
        "acknowledge_data_loss = yes and this exact confirm value:\n"
        "\n"
        "%s\n"
        "\n"
        "DATA-LOSS WARNING\n"
        "Shrinking F2FS moves file data and rewrites the partition table. A GPT backup\n"
        "does not contain file contents, and restoring the old GPT cannot undo an F2FS\n"
        "shrink. Keep a separate backup of userdata and metadata. Do not format either\n"
        "partition and do not retry an interrupted apply.\n",
        created, plan_id, s.id.model[0] ? s.id.model : "(model not reported)",
        s.id.serial, s.dev.disk, s.dev.disk_bytes, s.dev.sector,
        layout.userdata_index, layout.userdata_start, s.dev.userdata_end_lba, old_sz,
        s.dev.metadata_index, s.dev.metadata_start_lba, s.dev.metadata_end_lba,
        layout.unixnu_index, layout.unixnu_start, layout.unixnu_end, unix_sz,
        layout.userdata_start, layout.userdata_end, new_sz, gap_sz,
        unix_type, unix_unique, s.fs.blocks, expected, limit_blocks, s.fs.f_bavail, confirm);
    if (n < 0 || (size_t)n >= sizeof(txt) ||
        plan_path(plan_os, "plan.txt", path, sizeof(path)) < 0 ||
        write_file(path, txt, (size_t)n, 0600) < 0) {
        free(primary);
        free(backup);
        state_free(&s);
        return failf(err, errlen, "cannot write plan.txt: %s", strerror(errno));
    }

    char json[8192];
    n = snprintf(json, sizeof(json),
        "{\n"
        "  \"schema\": %d,\n"
        "  \"tool\": \"" TOOL_NAME "\",\n"
        "  \"version\": \"" TOOL_VERSION "\",\n"
        "  \"plan_id\": \"%s\",\n"
        "  \"created\": \"%s\",\n"
        "  \"identity\": {\"serial\": \"%s\", \"model\": \"%s\", \"device\": \"%s\",\n"
        "               \"kernel\": \"%s\", \"machine\": \"%s\", \"recovery\": \"%s\"},\n"
        "  \"device\": {\"disk\": \"%s\", \"disk_bytes\": %" PRIu64 ", \"sector\": %u},\n"
        "  \"userdata_before\": {\"index\": %d, \"start_lba\": %" PRIu64 ", \"end_lba\": %" PRIu64
        ", \"bytes\": %" PRIu64 "},\n"
        "  \"userdata_after\": {\"index\": %d, \"start_lba\": %" PRIu64 ", \"end_lba\": %" PRIu64
        ", \"bytes\": %" PRIu64 "},\n"
        "  \"unixnu\": {\"index\": %d, \"start_lba\": %" PRIu64 ", \"end_lba\": %" PRIu64
        ", \"bytes\": %" PRIu64 ", \"type_guid\": \"%s\", \"unique_guid\": \"%s\"},\n"
        "  \"filesystem\": {\"blocks_before\": %" PRIu64 ", \"blocks_after\": %" PRIu64
        ", \"limit_blocks\": %" PRIu64 ", \"uuid\": \"%s\"},\n"
        "  \"confirm\": \"%s\"\n"
        "}\n",
        PLAN_SCHEMA, plan_id, created, s.id.serial, s.id.model, s.id.device, s.id.kernel,
        s.id.machine, s.id.recovery, s.dev.disk, s.dev.disk_bytes, s.dev.sector,
        layout.userdata_index, layout.userdata_start, s.dev.userdata_end_lba,
        layout.old_userdata_bytes, layout.userdata_index, layout.userdata_start,
        layout.userdata_end, layout.new_userdata_bytes, layout.unixnu_index,
        layout.unixnu_start, layout.unixnu_end, layout.unixnu_bytes, unix_type, unix_unique,
        s.fs.blocks, expected, limit_blocks, s.fs.uuid, confirm);
    if (n < 0 || (size_t)n >= sizeof(json) ||
        plan_path(plan_os, "plan.json", path, sizeof(path)) < 0 ||
        write_file(path, json, (size_t)n, 0600) < 0) {
        free(primary);
        free(backup);
        state_free(&s);
        return failf(err, errlen, "cannot write plan.json: %s", strerror(errno));
    }

    char idline[80];
    snprintf(idline, sizeof(idline), "%s\n", plan_id);
    if (plan_path(plan_os, "session", path, sizeof(path)) < 0 ||
        write_file(path, idline, strlen(idline), 0600) < 0 ||
        plan_write_stage(plan_os, "planned", err, errlen) < 0) {
        free(primary);
        free(backup);
        state_free(&s);
        return failf(err, errlen, "cannot write session files: %s", strerror(errno));
    }
    if (write_sha256sums(plan_os, names, ARRAY_LEN(names), err, errlen) < 0) {
        free(primary);
        free(backup);
        state_free(&s);
        return -1;
    }

    char plan_display[PATH_MAX];
    if (p->root[0] && !strncmp(plan_os, p->root, strlen(p->root)))
        snprintf(plan_display, sizeof(plan_display), "%s", plan_os + strlen(p->root));
    else
        snprintf(plan_display, sizeof(plan_display), "%s", plan_os);

    printf("\nPLAN CREATED (no partition or filesystem writes)\n");
    printf("Plan directory: %s\n", plan_display);
    printf("Plan ID: %s\n", plan_id);
    printf("unixnu: LBA %" PRIu64 "..%" PRIu64 " (%s)\n",
           layout.unixnu_start, layout.unixnu_end, unix_sz);
    printf("Remaining userdata: LBA %" PRIu64 "..%" PRIu64 " (%s)\n",
           layout.userdata_start, layout.userdata_end, new_sz);
    printf("F2FS blocks: %" PRIu64 " -> %" PRIu64 " (limit %" PRIu64 ")\n",
           s.fs.blocks, expected, limit_blocks);
    printf("Alignment gap after unixnu: %s\n", gap_sz);
    printf("\nTo apply, use this exact confirmation phrase:\n%s\n", confirm);
    printf("\nBUNDLE LOCATION: apply unmounts /data and refuses a bundle stored on\n"
           "userdata-backed internal storage (/data, /sdcard). Copy this directory to an\n"
           "external SD card or /tmp before applying.\n");
    printf("\nDATA-LOSS WARNING: A GPT backup does not contain file data, and restoring the\n"
           "old GPT cannot undo an F2FS shrink. Keep an independent userdata/metadata backup.\n");

    free(primary);
    free(backup);
    state_free(&s);
    return 0;
}

int plan_run(const paths_t *p, const char *request_path, char *err, size_t errlen)
{
    request_t req;
    if (read_request_file(request_path, &req, err, errlen) < 0)
        return -1;
    if (req.mode[0] && strcmp(req.mode, "plan"))
        return failf(err, errlen, "request mode is '%s'; the Plan ZIP requires mode = plan",
                     req.mode);
    char logical[PATH_MAX], plan_os[PATH_MAX];
    if (req.plan_dir[0]) {
        if (snprintf(logical, sizeof(logical), "%s", req.plan_dir) >= (int)sizeof(logical))
            return failf(err, errlen, "plan_dir is too long");
    } else {
        char stamp[64];
        now_stamp(stamp, sizeof(stamp));
        if (snprintf(logical, sizeof(logical), "%s/plan-%s", p->base, stamp) >= (int)sizeof(logical))
            return failf(err, errlen, "default plan_dir is too long");
    }
    if (root_join(p, plan_os, sizeof(plan_os), logical) < 0)
        return failf(err, errlen, "plan_dir is too long");
    return run_plan(p, &req, plan_os, err, errlen);
}

typedef struct {
    plan_info_t *plan;
    int seen_schema;
} plan_parse_t;

#define PLAN_STR(field) \
    if (!strcmp(key, #field)) { snprintf(plan->field, sizeof(plan->field), "%s", val); return 0; }
#define PLAN_U64(field) \
    if (!strcmp(key, #field)) { if (parse_u64(val, &plan->field) < 0) return -1; return 0; }
#define PLAN_I32(field) \
    if (!strcmp(key, #field)) { uint64_t v; if (parse_u64(val, &v) < 0 || v > INT32_MAX) return -1; \
                                plan->field = (int)v; return 0; }
#define PLAN_U32(field) \
    if (!strcmp(key, #field)) { uint64_t v; if (parse_u64(val, &v) < 0 || v > UINT32_MAX) return -1; \
                                plan->field = (uint32_t)v; return 0; }

static int plan_kv(void *arg, const char *key, const char *val)
{
    plan_parse_t *pp = arg;
    plan_info_t *plan = pp->plan;
    if (!strcmp(key, "schema")) {
        uint64_t v;
        if (parse_u64(val, &v) < 0)
            return -1;
        pp->seen_schema = (int)v;
        return 0;
    }
    PLAN_STR(tool)
    PLAN_STR(version)
    PLAN_STR(plan_id)
    PLAN_STR(created)
    PLAN_STR(serial)
    PLAN_STR(disk)
    PLAN_STR(metadata_sha256)
    PLAN_STR(gpt_primary_sha256)
    PLAN_STR(gpt_backup_sha256)
    PLAN_STR(gpt_disk_guid)
    PLAN_STR(fs_uuid)
    PLAN_STR(fs_sb_sha256)
    PLAN_STR(unixnu_type_guid)
    PLAN_STR(unixnu_unique_guid)
    PLAN_STR(userdata_type_guid)
    PLAN_STR(userdata_unique_guid)
    PLAN_STR(confirm)
    PLAN_U64(size_bytes)
    PLAN_U64(disk_bytes)
    PLAN_U32(sector)
    PLAN_I32(userdata_index)
    PLAN_U64(userdata_start_lba)
    PLAN_U64(userdata_end_lba)
    PLAN_U64(userdata_bytes)
    PLAN_U64(userdata_new_end_lba)
    PLAN_U64(userdata_new_bytes)
    PLAN_I32(metadata_index)
    PLAN_U64(metadata_start_lba)
    PLAN_U64(metadata_end_lba)
    PLAN_U64(metadata_bytes)
    PLAN_U64(gpt_primary_len)
    PLAN_U64(gpt_backup_len)
    PLAN_U64(gpt_table_lba)
    PLAN_U64(gpt_backup_lba)
    PLAN_U64(fs_blocks)
    PLAN_U32(fs_feature)
    PLAN_U64(limit_blocks)
    PLAN_U64(expected_blocks)
    PLAN_I32(unixnu_index)
    PLAN_U64(unixnu_start_lba)
    PLAN_U64(unixnu_end_lba)
    PLAN_U64(unixnu_bytes)
    PLAN_U64(userdata_attrs)
    PLAN_U64(tail_gap_bytes)
    return 0;
}

int plan_load(const char *plan_dir, plan_info_t *plan, char *err, size_t errlen)
{
    memset(plan, 0, sizeof(*plan));
    char path[PATH_MAX];
    if (plan_path(plan_dir, "plan.conf", path, sizeof(path)) < 0)
        return failf(err, errlen, "plan path is too long");
    plan_parse_t pp = {.plan = plan, .seen_schema = -1};
    if (parse_kv_file(path, plan_kv, &pp) < 0)
        return failf(err, errlen, "cannot parse %s", path);
    if (pp.seen_schema != PLAN_SCHEMA)
        return failf(err, errlen, "unsupported plan schema %d", pp.seen_schema);
    if (strcmp(plan->tool, TOOL_NAME))
        return failf(err, errlen, "plan was created by another tool: %s", plan->tool);
    if (strcmp(plan->version, TOOL_VERSION))
        return failf(err, errlen, "plan version %s does not match tool version %s",
                     plan->version, TOOL_VERSION);
    if (!plan->plan_id[0] || strlen(plan->plan_id) != 32 || !plan->serial[0] ||
        !plan->disk[0] || !plan->confirm[0] || !plan->gpt_primary_sha256[0] ||
        !plan->gpt_backup_sha256[0] || !plan->metadata_sha256[0])
        return failf(err, errlen, "plan is incomplete");
    if (plan->sector != 512 && plan->sector != 4096)
        return failf(err, errlen, "plan has an unsupported sector size");
    if (plan->gpt_primary_len == 0 || plan->gpt_backup_len == 0 ||
        plan->gpt_primary_len % plan->sector || plan->gpt_backup_len % plan->sector)
        return failf(err, errlen, "plan has invalid GPT region sizes");
    if (plan->userdata_new_end_lba + 1 != plan->unixnu_start_lba ||
        plan->unixnu_end_lba < plan->unixnu_start_lba)
        return failf(err, errlen, "plan has inconsistent userdata/unixnu boundaries");
    if (plan->unixnu_bytes != (plan->unixnu_end_lba - plan->unixnu_start_lba + 1) * plan->sector)
        return failf(err, errlen, "plan has inconsistent unixnu size");
    if (plan->userdata_new_bytes != (plan->userdata_new_end_lba - plan->userdata_start_lba + 1) * plan->sector)
        return failf(err, errlen, "plan has inconsistent userdata size");
    if (plan->limit_blocks != plan->userdata_new_bytes / F2FS_BLOCK_BYTES)
        return failf(err, errlen, "plan has inconsistent F2FS limit");
    return 0;
}

static int hash_region_hex(const uint8_t *data, size_t len, char out[65])
{
    uint8_t h[32];
    sha256_buf(data, len, h);
    hex_encode(h, 32, out);
    return 0;
}

int plan_identity_match(const plan_info_t *plan, const state_t *s, char *err, size_t errlen)
{
    if (!s->id.serial[0] || strcmp(plan->serial, s->id.serial))
        return failf(err, errlen, "hardware serial does not match the plan");
    if (strcmp(plan->disk, s->dev.disk))
        return failf(err, errlen, "storage device does not match the plan");
    if (plan->disk_bytes != s->dev.disk_bytes || plan->sector != s->dev.sector)
        return failf(err, errlen, "storage geometry does not match the plan");
    if (plan->userdata_index != s->dev.userdata_index ||
        plan->userdata_start_lba != s->dev.userdata_start_lba ||
        plan->userdata_end_lba != s->dev.userdata_end_lba ||
        plan->userdata_bytes != s->dev.userdata_bytes)
        return failf(err, errlen, "kernel userdata geometry does not match the plan");
    if (plan->metadata_index != s->dev.metadata_index ||
        plan->metadata_start_lba != s->dev.metadata_start_lba ||
        plan->metadata_end_lba != s->dev.metadata_end_lba ||
        plan->metadata_bytes != s->dev.metadata_bytes)
        return failf(err, errlen, "metadata geometry does not match the plan");
    const gpt_entry_t *u = gpt_entry_by_index(&s->gpt, (uint32_t)s->dev.userdata_index);
    const gpt_entry_t *m = gpt_entry_by_index(&s->gpt, (uint32_t)s->dev.metadata_index);
    const gpt_entry_t *x = gpt_entry_by_index(&s->gpt, (uint32_t)plan->unixnu_index);
    if (!u || !m)
        return failf(err, errlen, "GPT identity entries are missing");
    char uid[37];
    uuid_format_le(u->unique_guid, uid);
    if (strcmp(uid, plan->userdata_unique_guid))
        return failf(err, errlen, "userdata unique GUID changed");
    uuid_format_le(u->type_guid, uid);
    if (strcmp(uid, plan->userdata_type_guid))
        return failf(err, errlen, "userdata type GUID changed");
    if (u->attrs != plan->userdata_attrs)
        return failf(err, errlen, "userdata attributes changed");
    if (x)
        return failf(err, errlen, "unixnu already exists");
    return 0;
}

int plan_match_state(const plan_info_t *plan, const state_t *s, char *err, size_t errlen)
{
    if (plan_identity_match(plan, s, err, errlen) < 0)
        return -1;
    char hex[65];
    hash_region_hex(s->gpt.primary, s->gpt.primary_len, hex);
    if (strcasecmp(hex, plan->gpt_primary_sha256))
        return failf(err, errlen, "primary GPT changed since planning");
    hash_region_hex(s->gpt.backup, s->gpt.backup_len, hex);
    if (strcasecmp(hex, plan->gpt_backup_sha256))
        return failf(err, errlen, "backup GPT changed since planning");
    if (!s->have_fs)
        return failf(err, errlen, "filesystem was not readable; keep decrypted /data mounted");
    if (plan->fs_blocks != s->fs.blocks)
        return failf(err, errlen, "F2FS block count changed since planning");
    if (strcmp(plan->fs_uuid, s->fs.uuid))
        return failf(err, errlen, "F2FS UUID changed since planning");
    if (plan->fs_feature != s->fs.feature)
        return failf(err, errlen, "F2FS features changed since planning");
    hex_encode(s->fs.sb_sha256, 32, hex);
    if (strcasecmp(plan->fs_sb_sha256, hex))
        return failf(err, errlen, "F2FS superblock changed since planning");
    uint8_t metadata[32];
    if (metadata_content_hash(NULL, (device_t *)&s->dev, metadata, err, errlen) < 0)
        return -1;
    hex_encode(metadata, 32, hex);
    if (strcasecmp(hex, plan->metadata_sha256))
        return failf(err, errlen, "metadata changed since planning");
    return 0;
}
