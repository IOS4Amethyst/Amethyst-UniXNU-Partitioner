/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Gated destructive apply. Every prerequisite is rechecked against a bundle
 * that was created for this exact device. There is no retry or rollback. */
#include "common.h"
#include <sys/mount.h>

#ifdef UNIXNU_TESTING
/* Host-only failure injection for the offline suite. These are compiled out of
 * the shipped helper, which is always built without -DUNIXNU_TESTING. */
static int test_inject(const char *name)
{
    const char *v = getenv(name);
    return v && *v && strcmp(v, "0");
}
#endif

static int pwrite_all(int fd, const uint8_t *buf, size_t len, off_t off)
{
    size_t done = 0;
    while (done < len) {
        ssize_t n = pwrite(fd, buf + done, len - done, off + (off_t)done);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (!n)
            return -1;
        done += (size_t)n;
    }
    return fsync(fd);
}

static int pread_all(int fd, uint8_t *buf, size_t len, off_t off)
{
    size_t done = 0;
    while (done < len) {
        ssize_t n = pread(fd, buf + done, len - done, off + (off_t)done);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (!n)
            return -1;
        done += (size_t)n;
    }
    return 0;
}

static void apply_log(const char *plan_dir, const char *fmt, ...)
{
    char path[PATH_MAX], line[1024], now[64];
    if (path_join(path, sizeof(path), plan_dir, "apply.log") < 0)
        return;
    now_utc(now, sizeof(now));
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    FILE *f = fopen(path, "a");
    if (!f)
        return;
    fprintf(f, "%s %s\n", now, line);
    fclose(f);
}

static int fail_apply(const char *plan_dir, const char *stage, char *err, size_t errlen,
                      const char *fmt, ...)
{
    va_list ap;
    char msg[512];
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (plan_dir)
        apply_log(plan_dir, "FAILED stage=%s: %s", stage ? stage : "preflight", msg);
    return failf(err, errlen, "%s", msg);
}

static int load_gpt_regions(const char *plan_dir, const char *name, uint8_t **data, size_t *len,
                            char *err, size_t errlen)
{
    return plan_read_file(plan_dir, name, data, len, err, errlen);
}

static int region_matches(const uint8_t *current, size_t current_len,
                          const uint8_t *expected, size_t expected_len, const char *label,
                          char *err, size_t errlen)
{
    if (current_len != expected_len || memcmp(current, expected, expected_len))
        return failf(err, errlen, "%s does not match the plan bundle", label);
    return 0;
}

/* Read the two on-disk GPT regions verbatim, so a resumed apply can classify
 * whatever state an interrupted run left behind instead of trusting its log. */
static int read_gpt_regions(int fd, const state_t *st, uint8_t **primary, size_t *primary_len,
                            uint8_t **backup, size_t *backup_len, char *err, size_t errlen)
{
    gpt_t g;
    memset(&g, 0, sizeof(g));
    if (gpt_read_fd(fd, st->dev.sector, st->dev.total_sectors, &g, err, errlen) < 0)
        return -1;
    if (!g.primary || !g.backup || !g.primary_len || !g.backup_len) {
        gpt_free(&g);
        return failf(err, errlen, "cannot read the on-disk GPT regions");
    }
    *primary = xmalloc(g.primary_len);
    *backup = xmalloc(g.backup_len);
    memcpy(*primary, g.primary, g.primary_len);
    memcpy(*backup, g.backup, g.backup_len);
    *primary_len = g.primary_len;
    *backup_len = g.backup_len;
    gpt_free(&g);
    return 0;
}

static int gpt_verify_after(const gpt_t *after, const gpt_t *before, const plan_info_t *plan,
                            char *err, size_t errlen)
{
    if (memcmp(after->disk_guid, before->disk_guid, 16))
        return failf(err, errlen, "disk GUID changed");
    if (after->count != before->count)
        return failf(err, errlen, "GPT entry geometry changed");
    if (after->first_usable != before->first_usable || after->last_usable != before->last_usable)
        return failf(err, errlen, "GPT usable range changed");
    char guid[37];
    /* plan->userdata_index and plan->unixnu_index are 1-based entry numbers. */
    for (uint32_t i = 1; i <= after->count; i++) {
        const uint8_t *n = after->table + (size_t)(i - 1) * GPT_ENTRY_SIZE;
        const uint8_t *o = before->table + (size_t)(i - 1) * GPT_ENTRY_SIZE;
        if ((int)i == plan->userdata_index) {
            if (memcmp(n, o, 16) || memcmp(n + 16, o + 16, 16) || memcmp(n + 48, o + 48, 80))
                return failf(err, errlen, "userdata type, GUID, attributes or name changed");
            uint64_t start = 0, end = 0;
            for (int b = 0; b < 8; b++) {
                start |= (uint64_t)n[32 + b] << (b * 8);
                end |= (uint64_t)n[40 + b] << (b * 8);
            }
            if (start != plan->userdata_start_lba || end != plan->userdata_new_end_lba)
                return failf(err, errlen, "userdata LBA range does not match the plan");
        } else if ((int)i == plan->unixnu_index) {
            uint64_t start = 0, end = 0;
            for (int b = 0; b < 8; b++) {
                start |= (uint64_t)n[32 + b] << (b * 8);
                end |= (uint64_t)n[40 + b] << (b * 8);
            }
            uuid_format_le(n, guid);
            if (strcmp(guid, plan->unixnu_type_guid))
                return failf(err, errlen, "unixnu type GUID does not match the plan");
            uuid_format_le(n + 16, guid);
            if (strcmp(guid, plan->unixnu_unique_guid))
                return failf(err, errlen, "unixnu unique GUID does not match the plan");
            if (start != plan->unixnu_start_lba || end != plan->unixnu_end_lba)
                return failf(err, errlen, "unixnu LBA range does not match the plan");
            uint64_t attrs = 0;
            for (int b = 0; b < 8; b++)
                attrs |= (uint64_t)n[48 + b] << (b * 8);
            if (attrs != 0)
                return failf(err, errlen, "unixnu attributes are not zero");
            static const char label[] = "unixnu";
            for (size_t c = 0; c < 36; c++) {
                uint8_t lo = n[56 + c * 2], hi = n[56 + c * 2 + 1];
                uint8_t want = c < sizeof(label) - 1 ? (uint8_t)label[c] : 0;
                if (lo != want || hi != 0)
                    return failf(err, errlen, "unixnu name is not the expected label");
            }
        } else if (memcmp(n, o, GPT_ENTRY_SIZE)) {
            return failf(err, errlen, "unrelated GPT entry %u changed", i);
        }
    }
    return 0;
}

static int create_session(const char *plan_dir, const plan_info_t *plan, char *err, size_t errlen)
{
    char path[PATH_MAX], text[80];
    if (path_join(path, sizeof(path), plan_dir, "apply.session") < 0)
        return failf(err, errlen, "plan path is too long");
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
        if (errno == EEXIST)
            return failf(err, errlen,
                         "an apply session already exists for this bundle; do not retry, run recover");
        return failf(err, errlen, "cannot create apply session: %s", strerror(errno));
    }
    snprintf(text, sizeof(text), "%s\n", plan->plan_id);
    ssize_t n = write(fd, text, strlen(text));
    if (n != (ssize_t)strlen(text) || fsync(fd) < 0) {
        close(fd);
        return failf(err, errlen, "cannot persist apply session: %s", strerror(errno));
    }
    close(fd);
    fsync_parent_dir(path);
    return 0;
}

static int probe_constant_metadata(const plan_info_t *plan, const state_t *s,
                                   char *err, size_t errlen)
{
    uint8_t hash[32];
    char hex[65];
    if (metadata_content_hash(NULL, (device_t *)&s->dev, hash, err, errlen) < 0)
        return -1;
    hex_encode(hash, 32, hex);
    if (strcasecmp(hex, plan->metadata_sha256))
        return failf(err, errlen, "metadata content changed since planning");
    return 0;
}

static int plan_location_check(const paths_t *p, const char *plan_dir, char *err, size_t errlen)
{
    /* Offline fixtures intentionally place everything under one host tree. */
    if (p->root[0])
        return 0;
    char real[PATH_MAX];
    if (!realpath(plan_dir, real))
        return failf(err, errlen, "cannot resolve plan directory %s: %s", plan_dir, strerror(errno));
    /* /data is unmounted during apply. A bundle on internal storage would stop
     * being writable exactly when the stage log matters most. */
    static const char *const forbidden[] = {
        "/data", "/sdcard", "/storage/emulated", "/mnt/user", "/mnt/runtime",
    };
    for (size_t i = 0; i < ARRAY_LEN(forbidden); i++) {
        size_t n = strlen(forbidden[i]);
        if (!strncmp(real, forbidden[i], n) && (real[n] == '\0' || real[n] == '/'))
            return failf(err, errlen,
                         "plan bundle is on userdata-backed storage (%s); copy it to an external "
                         "SD card or /tmp and update plan = ... before applying", real);
    }
    struct stat pd, dd;
    char data_path[PATH_MAX];
    if (root_join(p, data_path, sizeof(data_path), "/data") < 0)
        return failf(err, errlen, "path too long");
    if (stat(real, &pd) < 0 || stat(data_path, &dd) < 0)
        return failf(err, errlen, "cannot compare plan and /data filesystems: %s", strerror(errno));
    if (pd.st_dev == dd.st_dev)
        return failf(err, errlen,
                     "plan bundle is on the same filesystem as /data; copy it to an external SD "
                     "card or /tmp and update plan = ... before applying");
    return 0;
}

int apply_run(const paths_t *p, const char *plan_dir, const char *confirm,
              int acknowledge_data_loss, char *err, size_t errlen)
{
    plan_info_t plan;
    if (!plan_dir || !*plan_dir)
        return failf(err, errlen, "apply requires --plan with a directory created by the Plan ZIP");
    if (plan_load(plan_dir, &plan, err, errlen) < 0)
        return -1;
    if (!acknowledge_data_loss)
        return failf(err, errlen,
                     "refusing: apply requires explicit data-loss acknowledgement "
                     "(--i-understand-data-loss)");
    if (!confirm || strcmp(confirm, plan.confirm))
        return failf(err, errlen,
                     "refusing: confirmation phrase does not match this plan exactly");
    if (plan_location_check(p, plan_dir, err, errlen) < 0)
        return -1;
    if (plan_bundle_verify(plan_dir, err, errlen) < 0)
        return -1;
    /* Accept "planned" for a fresh run, or any interrupted stage for a resume.
     * The stage is validated below, once we know which shape it has. */
    char stage[64];
    if (plan_check_stage(plan_dir, NULL, stage, sizeof(stage), err, errlen) < 0)
        return -1;

    state_t s;
    if (state_collect(p, &s, 1, err, errlen) < 0) {
        state_free(&s);
        return -1;
    }
    if (plan_match_state_ex(&plan, &s, strcmp(stage, "planned") != 0, err, errlen) < 0) {
        state_free(&s);
        return -1;
    }
    if (strcmp(stage, "planned") == 0) {
        if (mounts_check_for_plan(p, &s.dev, &s.mounts, err, errlen) < 0) {
            state_free(&s);
            return -1;
        }
    } else {
        /* A resume runs with /data already unmounted; the fresh-plan check that
         * demands a read-write /data cannot apply. */
        if (mounts_scan(p, &s.dev, &s.mounts, err, errlen) < 0) {
            state_free(&s);
            return -1;
        }
    }

    uint8_t *primary_before = NULL, *backup_before = NULL;
    uint8_t *primary_after = NULL, *backup_after = NULL;
    size_t pbl = 0, bbl = 0, pal = 0, bal = 0;
    if (load_gpt_regions(plan_dir, "gpt-primary.before", &primary_before, &pbl, err, errlen) < 0 ||
        load_gpt_regions(plan_dir, "gpt-backup.before", &backup_before, &bbl, err, errlen) < 0 ||
        load_gpt_regions(plan_dir, "gpt-primary.after", &primary_after, &pal, err, errlen) < 0 ||
        load_gpt_regions(plan_dir, "gpt-backup.after", &backup_after, &bal, err, errlen) < 0)
        goto fail;
    if (pbl != plan.gpt_primary_len || bbl != plan.gpt_backup_len ||
        pal != plan.gpt_primary_len || bal != plan.gpt_backup_len) {
        fail_apply(plan_dir, "preflight", err, errlen, "GPT bundle image sizes do not match the plan");
        goto fail;
    }
    if (region_matches(s.gpt.primary, s.gpt.primary_len, primary_before, pbl,
                       "primary GPT", err, errlen) < 0 ||
        region_matches(s.gpt.backup, s.gpt.backup_len, backup_before, bbl,
                       "backup GPT", err, errlen) < 0) {
        apply_log(plan_dir, "FAILED stage=preflight: %s", err);
        goto fail;
    }
    if (probe_constant_metadata(&plan, &s, err, errlen) < 0) {
        apply_log(plan_dir, "FAILED stage=preflight: %s", err);
        goto fail;
    }

    /* Every stage beyond "revalidated" is reachable again: an interrupted run
     * used to abandon the bundle, which turned a transient failure into a lost
     * session. Resuming checks that the world still matches what the stage file
     * claims before continuing, so it is not a blind replay. */
    static const char *const resumable[] = {
        "unmounted", "f2fs-shrink-started", "f2fs-shrink-complete", "f2fs-verified",
        "gpt-backup-writing", "gpt-backup-verified", "gpt-primary-writing",
        "gpt-primary-verified",
    };
    int resumed = strcmp(stage, "planned") != 0;
    if (resumed) {
        int known = 0;
        for (size_t i = 0; i < ARRAY_LEN(resumable); i++)
            if (!strcmp(stage, resumable[i]))
                known = 1;
        if (!known)
            return failf(err, errlen,
                         "plan stage is '%s', expected 'planned' or an interrupted stage; "
                         "create a fresh plan", stage);
        if (mounts_check_unmounted(p, &s.dev, &s.mounts, err, errlen) < 0) {
            state_free(&s);
            return -1;
        }
        printf("Resuming an interrupted apply from stage '%s'.\n", stage);
        fflush(stdout);
    }
    /* Stage ordering, strictly forward. `reached` advances as each step is
     * confirmed, and a step whose work is already proven on disk is skipped. */
    int reached = resumed ? 1 : 0;

    if (!resumed) {
        /* Only after every read-only check succeeded do we create the session
         * and touch anything. */
        if (create_session(plan_dir, &plan, err, errlen) < 0) {
            state_free(&s);
            return -1;
        }
        plan_write_stage(plan_dir, "revalidated", err, errlen);

        /*
         * Unmount first, shrink second.
         *
         * The ioctl rewrites the F2FS superblock, but the checkpoint keeps the
         * old user_block_count until the kernel commits one, which it does on a
         * clean unmount. Shrinking while /data is still mounted leaves the
         * superblock and the checkpoint disagreeing if that unmount then fails
         * with EBUSY, which is exactly what happened on hardware. Doing the
         * unmount first means a busy /data costs nothing: it fails before the
         * filesystem is touched.
         *
         * The filesystem is reached through the dm mapper, which stays open
         * after /data is unmounted, so this is the same fd path as before.
         */
        if (mounts_unmount_data(p, &s.dev, &s.mounts, err, errlen) < 0) {
            fail_apply(plan_dir, "revalidated", err, errlen,
                       "%s; nothing was changed, clear the holders and retry", err);
            goto fail;
        }
        plan_write_stage(plan_dir, "unmounted", err, errlen);
        reached = 2;
    }

    /*
     * /data is unmounted at every point from here on, so metadata can no longer
     * be rewritten by Android. One hash check covers every remaining stage.
     */
    char merr[512] = "";
    if (probe_constant_metadata(&plan, &s, merr, sizeof(merr)) < 0) {
        fail_apply(plan_dir, resumed ? stage : "unmounted", err, errlen,
                   "metadata recheck failed: %s; GPT is unchanged", merr);
        goto fail;
    }
    apply_log(plan_dir, "metadata rechecked (offline)");

    int block_fd = open(s.dev.mapped, O_RDWR | O_CLOEXEC);
    if (block_fd < 0) {
        fail_apply(plan_dir, resumed ? stage : "unmounted", err, errlen,
                   "cannot open the mapped device %s read-write: %s; nothing was changed",
                   s.dev.mapped, strerror(errno));
        goto fail;
    }

    /* Step: the filesystem must be at the planned size. Either it never shrank
     * (still fs_blocks) or it already shrank (already expected_blocks). Anything
     * else is a state this plan cannot describe. */
    f2fs_info_t fs;
    char ferr[512] = "";
    if (f2fs_probe_fd(block_fd, plan.userdata_bytes, &fs, ferr, sizeof(ferr)) < 0) {
        fail_apply(plan_dir, resumed ? stage : "unmounted", err, errlen,
                   "F2FS check failed: %s", ferr);
        close(block_fd);
        goto fail;
    }
    if (fs.blocks != plan.fs_blocks && fs.blocks != plan.expected_blocks) {
        fail_apply(plan_dir, resumed ? stage : "unmounted", err, errlen,
                   "F2FS block count is %" PRIu64 ", plan expects %" PRIu64 " or %" PRIu64
                   "; nothing was changed", fs.blocks, plan.fs_blocks, plan.expected_blocks);
        close(block_fd);
        goto fail;
    }
    uint64_t request_out = fs.blocks, expected_out = fs.blocks;
    if (fs.blocks == plan.expected_blocks) {
        if (reached < 4)
            printf("F2FS is already at %" PRIu64 " blocks; skipping the resize.\n",
                   plan.expected_blocks);
        reached = 4;
    } else {
        uint64_t want = f2fs_request_blocks(fs.blocks, plan.limit_blocks);
        if (!want || f2fs_expected_blocks(fs.blocks, want) != plan.expected_blocks) {
            fail_apply(plan_dir, resumed ? stage : "unmounted", err, errlen,
                       "F2FS section rounding no longer matches the plan; nothing was changed");
            close(block_fd);
            goto fail;
        }
        plan_write_stage(plan_dir, "f2fs-shrink-started", err, errlen);
        printf("Shrinking F2FS from %" PRIu64 " to %" PRIu64 " blocks (limit %" PRIu64 ").\n"
               "Keep power connected and do not use the recovery UI.\n",
               fs.blocks, plan.expected_blocks, plan.limit_blocks);
        fflush(stdout);
        if (f2fs_shrink(block_fd, block_fd, plan.limit_blocks, &fs,
                        &request_out, &expected_out, ferr, sizeof(ferr)) < 0) {
            fail_apply(plan_dir, "f2fs-shrink-started", err, errlen,
                       "F2FS resize failed: %s; GPT is unchanged. Run recover.", ferr);
            close(block_fd);
            goto fail;
        }
        printf("F2FS resized to %" PRIu64 " blocks.\n", expected_out);
        fflush(stdout);
        if (expected_out != plan.expected_blocks) {
            fail_apply(plan_dir, "f2fs-shrink-started", err, errlen,
                       "unexpected F2FS result %" PRIu64 "; GPT is unchanged", expected_out);
            close(block_fd);
            goto fail;
        }
        plan_write_stage(plan_dir, "f2fs-shrink-complete", err, errlen);
        reached = 3;
    }

    if (reached < 4) {
        f2fs_info_t offline;
        if (f2fs_probe_fd(block_fd, plan.userdata_new_bytes, &offline, ferr, sizeof(ferr)) < 0) {
            fail_apply(plan_dir, "f2fs-shrink-complete", err, errlen,
                       "offline F2FS verification failed: %s", ferr);
            close(block_fd);
            goto fail;
        }
        if (offline.blocks != plan.expected_blocks || offline.blocks > plan.limit_blocks) {
            fail_apply(plan_dir, "f2fs-shrink-complete", err, errlen,
                       "F2FS is %" PRIu64 " blocks, expected %" PRIu64,
                       offline.blocks, plan.expected_blocks);
            close(block_fd);
            goto fail;
        }
        plan_write_stage(plan_dir, "f2fs-verified", err, errlen);
        reached = 4;
    }
    close(block_fd);

    int disk_fd = open(s.dev.disk, O_RDWR | O_CLOEXEC);
    if (disk_fd < 0) {
        fail_apply(plan_dir, "f2fs-verified", err, errlen, "cannot open %s read-write: %s; "
                   "the filesystem is already shrunk, rerun apply to resume",
                   s.dev.disk, strerror(errno));
        goto fail;
    }

    /*
     * The GPT may be in three states: untouched, backup-only (interrupted before
     * the primary write), or fully written. Read what is actually there and
     * classify it, instead of assuming the stage file is the truth.
     */
    uint8_t *cur_primary = NULL, *cur_backup = NULL;
    size_t cur_plen = 0, cur_blen = 0;
    if (read_gpt_regions(disk_fd, &s, &cur_primary, &cur_plen, &cur_backup, &cur_blen,
                         err, errlen) < 0) {
        close(disk_fd);
        goto fail;
    }
    if (cur_plen != plan.gpt_primary_len || cur_blen != plan.gpt_backup_len) {
        free(cur_primary);
        free(cur_backup);
        fail_apply(plan_dir, "f2fs-verified", err, errlen,
                   "on-disk GPT region sizes differ from the plan; do not reboot");
        close(disk_fd);
        goto fail;
    }
    int prim_is_before = !memcmp(cur_primary, primary_before, pbl);
    int prim_is_after = !memcmp(cur_primary, primary_after, pal);
    int bak_is_before = !memcmp(cur_backup, backup_before, bbl);
    int bak_is_after = !memcmp(cur_backup, backup_after, bal);
    free(cur_primary);
    free(cur_backup);
    if (!(prim_is_before || prim_is_after) || !(bak_is_before || bak_is_after)) {
        fail_apply(plan_dir, "f2fs-verified", err, errlen,
                   "on-disk GPT matches neither the before nor the after image; "
                   "do not reboot, the filesystem is already shrunk");
        close(disk_fd);
        goto fail;
    }
    if (prim_is_after && bak_is_after) {
        printf("Both GPT copies are already written; nothing left to do.\n");
        close(disk_fd);
    } else if (prim_is_after && !bak_is_after) {
        close(disk_fd);
        fail_apply(plan_dir, "gpt-primary-verified", err, errlen,
                   "primary GPT is new but the backup GPT is old; this ordering is not produced "
                   "by this tool - do not reboot");
        goto fail;
    } else if (bak_is_after && prim_is_before && resumed) {
        apply_log(plan_dir, "resuming: backup GPT already written, primary pending");
        close(disk_fd);
        disk_fd = -1;
        goto gpt_primary_write;
    } else {
        if (!bak_is_before) {
            fail_apply(plan_dir, "f2fs-verified", err, errlen,
                       "backup GPT is not the planned original; do not reboot");
            close(disk_fd);
            goto fail;
        }
        goto gpt_backup_write;
    }

    sync();
    if (plan_write_stage(plan_dir, "complete", err, errlen) < 0)
        goto fail;

    printf("\nAPPLY COMPLETE: both GPT copies were written and verified.\n"
           "Partition identities, userdata start/GUID/attributes and metadata were preserved.\n"
           "userdata is now unmounted. Reboot recovery before using /data.\n"
           "Then run verify against this plan directory.\n"
           "\n"
           "A GPT backup does not contain file contents. Restoring the old GPT cannot\n"
           "undo the F2FS shrink. Do not format userdata or unixnu.\n");
    free(primary_before);
    free(backup_before);
    free(primary_after);
    free(backup_after);
    state_free(&s);
    return 0;

gpt_backup_write:
    {
        off_t backup_off = (off_t)(s.dev.total_sectors - 1 - s.gpt.blocks) * (off_t)s.dev.sector;
        if ((uint64_t)backup_off / s.dev.sector != s.gpt.total_sectors - 1 - s.gpt.blocks) {
            fail_apply(plan_dir, "f2fs-verified", err, errlen, "backup GPT offset is out of range");
            close(disk_fd);
            goto fail;
        }
        plan_write_stage(plan_dir, "gpt-backup-writing", err, errlen);
        sync();
#ifdef UNIXNU_TESTING
        if (test_inject("UNIXNU_TEST_FAIL_BACKUP_WRITE")) {
            errno = EIO;
            fail_apply(plan_dir, "gpt-backup-writing", err, errlen,
                       "backup GPT write failed: %s; do not reboot", strerror(errno));
            close(disk_fd);
            goto fail;
        }
#endif
        if (pwrite_all(disk_fd, backup_after, bal, backup_off) < 0) {
            fail_apply(plan_dir, "gpt-backup-writing", err, errlen,
                       "backup GPT write failed: %s; do not reboot", strerror(errno));
            close(disk_fd);
            goto fail;
        }
        uint8_t *verify = xmalloc(bal);
        if (pread_all(disk_fd, verify, bal, backup_off) < 0 ||
            memcmp(verify, backup_after, bal)
#ifdef UNIXNU_TESTING
            || test_inject("UNIXNU_TEST_FAIL_BACKUP_READBACK")
#endif
        ) {
            free(verify);
            fail_apply(plan_dir, "gpt-backup-writing", err, errlen,
                       "backup GPT readback failed; do not reboot");
            close(disk_fd);
            goto fail;
        }
        free(verify);
        plan_write_stage(plan_dir, "gpt-backup-verified", err, errlen);
    }

gpt_primary_write:
    {
        if (disk_fd < 0) {
            disk_fd = open(s.dev.disk, O_RDWR | O_CLOEXEC);
            if (disk_fd < 0) {
                fail_apply(plan_dir, "gpt-backup-verified", err, errlen,
                           "cannot reopen %s: %s; backup GPT is already new, rerun apply",
                           s.dev.disk, strerror(errno));
                goto fail;
            }
        }
        plan_write_stage(plan_dir, "gpt-primary-writing", err, errlen);
        sync();
#ifdef UNIXNU_TESTING
        if (test_inject("UNIXNU_TEST_FAIL_PRIMARY_WRITE")) {
            errno = EIO;
            fail_apply(plan_dir, "gpt-primary-writing", err, errlen,
                       "primary GPT write failed: %s; do not reboot", strerror(errno));
            close(disk_fd);
            goto fail;
        }
#endif
        if (pwrite_all(disk_fd, primary_after, pal, 0) < 0) {
            fail_apply(plan_dir, "gpt-primary-writing", err, errlen,
                       "primary GPT write failed: %s; do not reboot", strerror(errno));
            close(disk_fd);
            goto fail;
        }
        off_t backup_off = (off_t)(s.dev.total_sectors - 1 - s.gpt.blocks) * (off_t)s.dev.sector;
        uint8_t *primary_read = xmalloc(pal);
        uint8_t *backup_read = xmalloc(bal);
        if (pread_all(disk_fd, primary_read, pal, 0) < 0 ||
            pread_all(disk_fd, backup_read, bal, backup_off) < 0 ||
            memcmp(primary_read, primary_after, pal) ||
            memcmp(backup_read, backup_after, bal)) {
            free(primary_read);
            free(backup_read);
            fail_apply(plan_dir, "gpt-primary-writing", err, errlen,
                       "GPT readback mismatch; do not reboot");
            close(disk_fd);
            goto fail;
        }
        free(primary_read);
        free(backup_read);
        plan_write_stage(plan_dir, "gpt-primary-verified", err, errlen);

        gpt_t before_gpt, after_gpt;
        memset(&before_gpt, 0, sizeof(before_gpt));
        memset(&after_gpt, 0, sizeof(after_gpt));
        if (gpt_parse(primary_before, pbl, backup_before, bbl, s.dev.sector, s.dev.total_sectors,
                      &before_gpt, err, errlen) < 0 ||
            gpt_read_fd(disk_fd, s.dev.sector, s.dev.total_sectors, &after_gpt, err, errlen) < 0) {
            apply_log(plan_dir, "FAILED stage=gpt-primary-verified: %s", err);
            gpt_free(&before_gpt);
            gpt_free(&after_gpt);
            close(disk_fd);
            goto fail;
        }
        if (region_matches(after_gpt.primary, after_gpt.primary_len, primary_after, pal,
                           "primary GPT after readback", err, errlen) < 0 ||
            region_matches(after_gpt.backup, after_gpt.backup_len, backup_after, bal,
                           "backup GPT after readback", err, errlen) < 0 ||
            gpt_verify_after(&after_gpt, &before_gpt, &plan, err, errlen) < 0) {
            apply_log(plan_dir, "FAILED stage=gpt-primary-verified: %s", err);
            gpt_free(&before_gpt);
            gpt_free(&after_gpt);
            close(disk_fd);
            goto fail;
        }
        gpt_free(&before_gpt);
        gpt_free(&after_gpt);
        close(disk_fd);
        sync();
    }

    if (plan_write_stage(plan_dir, "complete", err, errlen) < 0)
        goto fail;

    printf("\nAPPLY COMPLETE: both GPT copies were written and verified.\n"
           "Partition identities, userdata start/GUID/attributes and metadata were preserved.\n"
           "userdata is now unmounted. Reboot recovery before using /data.\n"
           "Then run verify against this plan directory.\n"
           "\n"
           "A GPT backup does not contain file contents. Restoring the old GPT cannot\n"
           "undo the F2FS shrink. Do not format userdata or unixnu.\n");
    free(primary_before);
    free(backup_before);
    free(primary_after);
    free(backup_after);
    state_free(&s);
    return 0;

fail:
    free(primary_before);
    free(backup_before);
    free(primary_after);
    free(backup_after);
    state_free(&s);
    return -1;
}

int verify_run(const paths_t *p, const char *plan_dir, char *err, size_t errlen)
{
    plan_info_t plan;
    if (plan_load(plan_dir, &plan, err, errlen) < 0)
        return -1;
    if (plan_bundle_verify(plan_dir, err, errlen) < 0)
        return -1;
    char stage[64];
    if (plan_check_stage(plan_dir, "complete", stage, sizeof(stage), err, errlen) < 0)
        return -1;

    state_t s;
    if (state_collect(p, &s, 0, err, errlen) < 0) {
        state_free(&s);
        return -1;
    }
    if (!s.have_gpt || !s.dev.disk[0]) {
        state_free(&s);
        return failf(err, errlen, "cannot read the current device state; reboot recovery and retry");
    }

    uint8_t *primary_before = NULL, *backup_before = NULL;
    uint8_t *primary_after = NULL, *backup_after = NULL;
    size_t pbl = 0, bbl = 0, pal = 0, bal = 0;
    int rc = -1;
    if (load_gpt_regions(plan_dir, "gpt-primary.before", &primary_before, &pbl, err, errlen) < 0 ||
        load_gpt_regions(plan_dir, "gpt-backup.before", &backup_before, &bbl, err, errlen) < 0 ||
        load_gpt_regions(plan_dir, "gpt-primary.after", &primary_after, &pal, err, errlen) < 0 ||
        load_gpt_regions(plan_dir, "gpt-backup.after", &backup_after, &bal, err, errlen) < 0)
        goto out;
    if (region_matches(s.gpt.primary, s.gpt.primary_len, primary_after, pal,
                       "current primary GPT", err, errlen) < 0 ||
        region_matches(s.gpt.backup, s.gpt.backup_len, backup_after, bal,
                       "current backup GPT", err, errlen) < 0)
        goto out;
    gpt_t before_gpt, after_gpt;
    memset(&before_gpt, 0, sizeof(before_gpt));
    memset(&after_gpt, 0, sizeof(after_gpt));
    if (gpt_parse(primary_before, pbl, backup_before, bbl, s.dev.sector, s.dev.total_sectors,
                  &before_gpt, err, errlen) < 0 ||
        gpt_parse(primary_after, pal, backup_after, bal, s.dev.sector, s.dev.total_sectors,
                  &after_gpt, err, errlen) < 0 ||
        gpt_verify_after(&after_gpt, &before_gpt, &plan, err, errlen) < 0) {
        gpt_free(&before_gpt);
        gpt_free(&after_gpt);
        goto out;
    }
    gpt_free(&before_gpt);
    gpt_free(&after_gpt);

    if (s.dev.userdata_start_lba != plan.userdata_start_lba ||
        s.dev.userdata_end_lba != plan.userdata_new_end_lba ||
        s.dev.userdata_bytes != plan.userdata_new_bytes) {
        failf(err, errlen,
              "kernel userdata view is stale or wrong (%" PRIu64 "..%" PRIu64
              "); reboot recovery and retry", s.dev.userdata_start_lba, s.dev.userdata_end_lba);
        goto out;
    }
    int uidx = 0;
    uint64_t ustart = 0, uend = 0, ubytes = 0;
    char gerr[512] = "";
    if (device_partition_geometry(p, "unixnu", s.dev.sector, &uidx, &ustart, &uend, &ubytes,
                                  gerr, sizeof(gerr)) < 0) {
        failf(err, errlen, "cannot read unixnu partition geometry: %s", gerr);
        goto out;
    }
    if (uidx != plan.unixnu_index || ustart != plan.unixnu_start_lba ||
        uend != plan.unixnu_end_lba || ubytes != plan.unixnu_bytes) {
        failf(err, errlen, "kernel unixnu view does not match the plan");
        goto out;
    }

    f2fs_info_t fs;
    char ferr[512] = "";
    int fs_rc;
    char data_path[PATH_MAX];
    int have_data_path = root_join(p, data_path, sizeof(data_path), "/data") == 0;
    if (s.have_mounts && s.mounts.data_count == 1 && s.dev.mapped[0] && have_data_path)
        fs_rc = f2fs_probe_mounted(data_path, s.dev.mapped, plan.userdata_new_bytes,
                                   &fs, ferr, sizeof(ferr));
    else
        fs_rc = f2fs_probe_path(s.dev.raw, plan.userdata_new_bytes, &fs, ferr, sizeof(ferr));
    if (fs_rc < 0) {
        failf(err, errlen, "cannot verify F2FS: %s", ferr);
        goto out;
    }
    if (fs.blocks != plan.expected_blocks || fs.blocks > plan.limit_blocks) {
        failf(err, errlen, "F2FS is %" PRIu64 " blocks, expected %" PRIu64,
              fs.blocks, plan.expected_blocks);
        goto out;
    }
    if (!s.mounts.metadata_mounts) {
        uint8_t hash[32];
        char hex[65];
        if (metadata_content_hash(p, &s.dev, hash, err, errlen) < 0)
            goto out;
        hex_encode(hash, 32, hex);
        if (strcasecmp(hex, plan.metadata_sha256)) {
            failf(err, errlen, "metadata content changed");
            goto out;
        }
    } else {
        printf("WARNING: metadata is mounted, so its content hash was not rechecked.\n");
    }

    printf("PASS: GPT copies, partition identities and kernel ranges match the plan.\n");
    printf("PASS: userdata F2FS has %" PRIu64 " blocks (limit %" PRIu64 ").\n",
           fs.blocks, plan.limit_blocks);
    printf("PASS: unixnu exists at LBA %" PRIu64 "..%" PRIu64 " (%" PRIu64 " bytes).\n",
           ustart, uend, ubytes);
    printf("unixnu is unformatted and may contain old userdata bytes.\n"
           "Test Android boot and your files separately. No filesystem was created in unixnu.\n");
    rc = 0;

out:
    free(primary_before);
    free(backup_before);
    free(primary_after);
    free(backup_after);
    state_free(&s);
    return rc;
}

int recover_run(const paths_t *p, const char *plan_dir, char *err, size_t errlen)
{
    (void)p;
    char path[PATH_MAX], stage[128] = "(missing)", session[128] = "(missing)";
    if (path_join(path, sizeof(path), plan_dir, "stage") < 0)
        return failf(err, errlen, "plan path is too long");
    if (read_text(path, stage, sizeof(stage)) < 0)
        snprintf(stage, sizeof(stage), "(missing)");
    trim(stage);
    if (path_join(path, sizeof(path), plan_dir, "apply.session") < 0)
        return failf(err, errlen, "plan path is too long");
    if (read_text(path, session, sizeof(session)) < 0)
        snprintf(session, sizeof(session), "(no apply session was created)");
    trim(session);

    printf("Amethyst UniXNU Partitioner recovery information\n");
    printf("Plan directory: %s\n", plan_dir);
    printf("Session: %s\n", session);
    printf("Last persisted stage: %s\n\n", stage);
    if (path_join(path, sizeof(path), plan_dir, "apply.log") == 0) {
        char log[4096];
        if (read_text(path, log, sizeof(log)) == 0 && log[0]) {
            printf("apply.log:\n%s\n", log);
        }
    }
    if (!strcmp(stage, "complete")) {
        printf("This apply completed. Reboot recovery, decrypt /data, then run verify.\n");
        return 0;
    }
    printf("No automatic retry or rollback is provided. Do not delete apply.session.\n\n");
    printf("Stage guidance:\n");
    if (!strcmp(stage, "planned") || !strcmp(stage, "revalidated"))
        printf("  Storage was not changed yet. Save logs, then create a fresh plan if needed.\n");
    else if (!strcmp(stage, "f2fs-shrink-started") || !strcmp(stage, "f2fs-shrink-complete"))
        printf("  The GPT is still original. Inspect kernel/filesystem errors before any new attempt.\n");
    else if (!strcmp(stage, "f2fs-verified") || !strcmp(stage, "unmounted"))
        printf("  F2FS was resized but the GPT is still original. Inspect open files and bind mounts.\n");
    else if (!strncmp(stage, "gpt-", 4))
        printf("  GPT copies may disagree. Keep recovery running and inspect both raw copies.\n"
               "  Do not reboot or retry blindly.\n");
    else
        printf("  Unrecognized stage. Preserve recovery logs and stop.\n");
    printf("\nA GPT backup does not contain file data. Restoring an old GPT cannot undo an\n"
           "F2FS shrink. Never format unreadable data; keep the device offline for assessment.\n");
    return 1;
}
