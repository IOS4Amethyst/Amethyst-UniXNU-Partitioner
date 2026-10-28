/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "common.h"
#include <ctype.h>
#include <dirent.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/utsname.h>
#include <strings.h>

typedef struct {
    identity_t *id;
    int seen_device;
} identity_parse_t;

static int identity_kv(void *arg, const char *key, const char *val)
{
    identity_parse_t *p = arg;
    identity_t *id = p->id;
    if (!strcmp(key, "device")) {
        shell_copy_value(id->device, sizeof(id->device), val);
        p->seen_device = 1;
    } else if (!strcmp(key, "model")) {
        shell_copy_value(id->model, sizeof(id->model), val);
    } else if (!strcmp(key, "serial")) {
        shell_copy_value(id->serial, sizeof(id->serial), val);
    } else if (!strcmp(key, "recovery")) {
        shell_copy_value(id->recovery, sizeof(id->recovery), val);
    } else if (!strcmp(key, "twrp")) {
        shell_copy_value(id->twrp, sizeof(id->twrp), val);
    } else if (!strcmp(key, "kernel")) {
        shell_copy_value(id->kernel, sizeof(id->kernel), val);
    } else if (!strcmp(key, "machine")) {
        shell_copy_value(id->machine, sizeof(id->machine), val);
    } else if (!strcmp(key, "usb_config")) {
        shell_copy_value(id->usb_config, sizeof(id->usb_config), val);
    } else if (!strcmp(key, "usb_state")) {
        shell_copy_value(id->usb_state, sizeof(id->usb_state), val);
    } else if (!strcmp(key, "dt_model")) {
        shell_copy_value(id->dt_model, sizeof(id->dt_model), val);
    } else if (!strcmp(key, "boot_completed")) {
        id->boot_completed = atoi(val) == 1;
    }
    return 0;
}

static void read_dt_model(const paths_t *p, char *out, size_t cap)
{
    char path[PATH_MAX];
    if (root_join(p, path, sizeof(path), "/proc/device-tree/model") < 0)
        return;
    size_t n = 0;
    if (read_file(path, out, cap - 1, &n) < 0)
        return;
    out[n] = '\0';
    for (size_t i = 0; i < n; i++)
        if (out[i] == '\0')
            out[i] = ' ';
    trim(out);
}

int identity_load(const paths_t *p, identity_t *id, char *err, size_t errlen)
{
    memset(id, 0, sizeof(*id));
    identity_parse_t parse = {.id = id, .seen_device = 0};
    if (p->identity[0]) {
        if (parse_kv_file(p->identity, identity_kv, &parse) < 0)
            return failf(err, errlen, "cannot parse identity file %s", p->identity);
    }
    if (!id->machine[0]) {
        struct utsname u;
        if (!uname(&u))
            snprintf(id->machine, sizeof(id->machine), "%s", u.machine);
    }
    if (!id->kernel[0]) {
        struct utsname u;
        if (!uname(&u))
            snprintf(id->kernel, sizeof(id->kernel), "%s", u.release);
    }
    char dt[192];
    read_dt_model(p, dt, sizeof(dt));
    if (!id->dt_model[0])
        snprintf(id->dt_model, sizeof(id->dt_model), "%s", dt);
    return 0;
}

static int contains_ci(const char *haystack, const char *needle)
{
    size_t n = strlen(needle);
    if (!n)
        return 1;
    for (const char *p = haystack; *p; p++) {
        size_t i = 0;
        while (i < n && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i]))
            i++;
        if (i == n)
            return 1;
    }
    return 0;
}

int identity_validate(const paths_t *p, identity_t *id, char *err, size_t errlen)
{
    int prop_amethyst = contains_ci(id->device, "amethyst");
    int dt_amethyst = contains_ci(id->dt_model, "amethyst");
    if (id->device[0] && !prop_amethyst)
        return failf(err, errlen, "device property is '%s', not amethyst", id->device);
    if (!prop_amethyst && !dt_amethyst)
        return failf(err, errlen, "cannot verify Amethyst (device='%s', dt_model='%s')",
                     id->device, id->dt_model);
    if (id->boot_completed)
        return failf(err, errlen, "Android is running; boot a recovery first");
    if (!id->recovery[0]) {
        char rec[PATH_MAX];
        int found = 0;
        const char *candidates[] = {"/sbin/recovery", "/system/bin/recovery", "/system/bin/twrp"};
        for (size_t i = 0; i < ARRAY_LEN(candidates); i++) {
            if (root_join(p, rec, sizeof(rec), candidates[i]) == 0 && access(rec, X_OK) == 0) {
                found = 1;
                break;
            }
        }
        if (!found)
            return failf(err, errlen, "no supported recovery environment was detected");
        snprintf(id->recovery, sizeof(id->recovery), "recovery");
    }
    if (!contains_ci(id->machine, "aarch64") && !contains_ci(id->machine, "arm64"))
        return failf(err, errlen, "unsupported architecture '%s'; AArch64 recovery is required", id->machine);
    if (!id->kernel[0])
        return failf(err, errlen, "cannot determine the recovery kernel release");
    return 0;
}

static int resolve_byname(const paths_t *p, const char *name, char *out, size_t cap,
                          char *err, size_t errlen)
{
    char link[PATH_MAX], tmp[PATH_MAX];
    if (root_join(p, tmp, sizeof(tmp), "/dev/block/by-name") < 0)
        return failf(err, errlen, "path too long");
    if (path_join(link, sizeof(link), tmp, name) < 0)
        return failf(err, errlen, "path too long");
    char resolved[PATH_MAX];
    if (!realpath(link, resolved))
        return failf(err, errlen, "cannot resolve %s: %s", link, strerror(errno));
    if (snprintf(out, cap, "%s", resolved) >= (int)cap)
        return failf(err, errlen, "resolved path is too long");
    return 0;
}

static int sys_node_real(const paths_t *p, const char *node, char *out, size_t cap,
                         char *err, size_t errlen)
{
    char path[PATH_MAX];
    if (root_join(p, path, sizeof(path), "/sys/class/block") < 0 ||
        path_join(path, sizeof(path), path, node) < 0)
        return failf(err, errlen, "path too long");
    char resolved[PATH_MAX];
    if (!realpath(path, resolved))
        return failf(err, errlen, "cannot resolve %s: %s", path, strerror(errno));
    if (snprintf(out, cap, "%s", resolved) >= (int)cap)
        return failf(err, errlen, "sysfs path is too long");
    return 0;
}

static int partition_parent(const paths_t *p, const char *path, char *out, size_t cap,
                            char *err, size_t errlen)
{
    char sys_real[PATH_MAX], dir[PATH_MAX];
    if (sys_node_real(p, path_basename(path), sys_real, sizeof(sys_real), err, errlen) < 0)
        return -1;
    if (path_dirname(sys_real, dir, sizeof(dir)) < 0)
        return failf(err, errlen, "cannot determine parent of %s", path);
    if (snprintf(out, cap, "%s", path_basename(dir)) >= (int)cap)
        return failf(err, errlen, "parent disk name is too long");
    return 0;
}

static int read_sys_u64(const paths_t *p, const char *node, const char *name, uint64_t *value)
{
    char path[PATH_MAX], dir[PATH_MAX];
    if (root_join(p, dir, sizeof(dir), "/sys/class/block") < 0 ||
        path_join(dir, sizeof(dir), dir, node) < 0 ||
        path_join(path, sizeof(path), dir, name) < 0)
        return -1;
    return read_u64_file(path, value);
}

static int read_sys_text(const paths_t *p, const char *node, const char *name, char *out, size_t cap)
{
    char path[PATH_MAX], dir[PATH_MAX];
    if (root_join(p, dir, sizeof(dir), "/sys/class/block") < 0 ||
        path_join(dir, sizeof(dir), dir, node) < 0 ||
        path_join(path, sizeof(path), dir, name) < 0)
        return -1;
    if (read_text(path, out, cap) < 0)
        return -1;
    trim(out);
    return 0;
}

static int partition_info(const paths_t *p, const char *path, const char *want_name,
                          device_t *dev, int *index_out, uint64_t *start_lba,
                          uint64_t *end_lba, uint64_t *bytes, uint64_t *start_512,
                          uint32_t sector, char *err, size_t errlen)
{
    const char *node = path_basename(path);
    char sys_real[PATH_MAX], parent[64], marker[PATH_MAX];
    if (sys_node_real(p, node, sys_real, sizeof(sys_real), err, errlen) < 0)
        return -1;
    char dir[PATH_MAX];
    if (path_dirname(sys_real, dir, sizeof(dir)) < 0)
        return failf(err, errlen, "cannot determine parent disk for %s", node);
    snprintf(parent, sizeof(parent), "%s", path_basename(dir));
    if (!strcmp(parent, node))
        return failf(err, errlen, "%s is a whole disk, not a partition", node);
    if (root_join(p, marker, sizeof(marker), "/sys/class/block") < 0 ||
        path_join(marker, sizeof(marker), marker, parent) < 0 ||
        path_join(marker, sizeof(marker), marker, "partition") < 0)
        return failf(err, errlen, "path too long");
    if (access(marker, F_OK) == 0)
        return failf(err, errlen, "%s does not resolve to a partition", node);
    uint64_t partno = 0, sys_start = 0, sys_size = 0;
    if (read_sys_u64(p, node, "partition", &partno) < 0)
        return failf(err, errlen, "cannot read partition number for %s", node);
    if (read_sys_u64(p, node, "start", &sys_start) < 0 ||
        read_sys_u64(p, node, "size", &sys_size) < 0)
        return failf(err, errlen, "cannot read partition geometry for %s", node);
    uint64_t ratio = sector / 512;
    if (!ratio || sys_start % ratio || sys_size % ratio)
        return failf(err, errlen, "partition %s is not aligned to the logical sector size", node);
    *index_out = (int)partno;
    *start_lba = sys_start / ratio;
    *end_lba = *start_lba + sys_size / ratio - 1;
    *bytes = sys_size * 512;
    *start_512 = sys_start;
    if (!strcmp(want_name, "userdata")) {
        snprintf(dev->raw, sizeof(dev->raw), "%s", path);
        snprintf(dev->raw_node, sizeof(dev->raw_node), "%s", node);
        char blockdir[PATH_MAX];
        if (root_join(p, blockdir, sizeof(blockdir), "/dev/block") < 0 ||
            path_join(dev->disk, sizeof(dev->disk), blockdir, parent) < 0)
            return failf(err, errlen, "disk path is too long");
        snprintf(dev->disk_node, sizeof(dev->disk_node), "%s", parent);
        dev->userdata_index = (int)partno;
        dev->userdata_start_lba = *start_lba;
        dev->userdata_end_lba = *end_lba;
        dev->userdata_bytes = *bytes;
        dev->userdata_start_512 = sys_start;
    } else if (!strcmp(want_name, "metadata")) {
        snprintf(dev->metadata, sizeof(dev->metadata), "%s", path);
        snprintf(dev->metadata_node, sizeof(dev->metadata_node), "%s", node);
        dev->metadata_index = (int)partno;
        dev->metadata_start_lba = *start_lba;
        dev->metadata_end_lba = *end_lba;
        dev->metadata_bytes = *bytes;
        dev->metadata_start_512 = sys_start;
    }
    return 0;
}

int device_discover(const paths_t *p, identity_t *id, device_t *dev, char *err, size_t errlen)
{
    memset(dev, 0, sizeof(*dev));
    char user[PATH_MAX], metadata[PATH_MAX];
    if (resolve_byname(p, "userdata", user, sizeof(user), err, errlen) < 0 ||
        resolve_byname(p, "metadata", metadata, sizeof(metadata), err, errlen) < 0)
        return -1;
    if (strstr(user, "/dm-"))
        return failf(err, errlen, "by-name/userdata resolves to a device-mapper node, not a partition");
    char disk_guess[PATH_MAX], dir[PATH_MAX];
    char sys_real[PATH_MAX];
    if (sys_node_real(p, path_basename(user), sys_real, sizeof(sys_real), err, errlen) < 0)
        return -1;
    if (path_dirname(sys_real, dir, sizeof(dir)) < 0)
        return failf(err, errlen, "cannot determine userdata parent disk");
    snprintf(disk_guess, sizeof(disk_guess), "%s", path_basename(dir));
    char disk_path[PATH_MAX];
    if (root_join(p, disk_path, sizeof(disk_path), "/dev/block") < 0 ||
        path_join(disk_path, sizeof(disk_path), disk_path, disk_guess) < 0)
        return failf(err, errlen, "path too long");
    uint32_t sector = 0, phys = 0;
    uint64_t disk_bytes = 0;
#ifdef UNIXNU_TESTING
    {
        char tmp[PATH_MAX];
        if (root_join(p, tmp, sizeof(tmp), "/sys/class/block") < 0 ||
            path_join(tmp, sizeof(tmp), tmp, disk_guess) < 0 ||
            path_join(tmp, sizeof(tmp), tmp, "queue/logical_block_size") < 0 ||
            read_u32_file(tmp, &sector) < 0)
            return failf(err, errlen, "cannot read logical sector size");
        if (root_join(p, tmp, sizeof(tmp), "/sys/class/block") == 0 &&
            path_join(tmp, sizeof(tmp), tmp, disk_guess) == 0 &&
            path_join(tmp, sizeof(tmp), tmp, "queue/physical_block_size") == 0)
            (void)read_u32_file(tmp, &phys);
        if (root_join(p, tmp, sizeof(tmp), "/sys/class/block") == 0 &&
            path_join(tmp, sizeof(tmp), tmp, disk_guess) == 0 &&
            path_join(tmp, sizeof(tmp), tmp, "size") == 0) {
            uint64_t sz512 = 0;
            if (read_u64_file(tmp, &sz512) < 0)
                return failf(err, errlen, "cannot read disk size");
            disk_bytes = sz512 * 512;
        }
    }
#else
    int dfd = open(disk_path, O_RDONLY | O_CLOEXEC);
    if (dfd < 0)
        return failf(err, errlen, "cannot open %s: %s", disk_path, strerror(errno));
    if (ioctl(dfd, BLKSSZGET, &sector) < 0)
        sector = 0;
    if (ioctl(dfd, BLKPBSZGET, &phys) < 0)
        phys = sector;
    if (ioctl(dfd, BLKGETSIZE64, &disk_bytes) < 0) {
        close(dfd);
        return failf(err, errlen, "cannot read disk size: %s", strerror(errno));
    }
    close(dfd);
    if (!sector) {
        char tmp[PATH_MAX];
        if (root_join(p, tmp, sizeof(tmp), "/sys/class/block") == 0 &&
            path_join(tmp, sizeof(tmp), tmp, disk_guess) == 0 &&
            path_join(tmp, sizeof(tmp), tmp, "queue/logical_block_size") == 0)
            (void)read_u32_file(tmp, &sector);
    }
#endif
    if (sector != 512 && sector != 4096)
        return failf(err, errlen, "unsupported logical sector size %u", sector);
    if (!phys)
        phys = sector;
    if (!disk_bytes || disk_bytes % sector)
        return failf(err, errlen, "unsupported disk geometry");
    dev->sector = sector;
    dev->physical_sector = phys;
    dev->disk_bytes = disk_bytes;
    dev->total_sectors = disk_bytes / sector;
    if (partition_info(p, user, "userdata", dev, &dev->userdata_index,
                       &dev->userdata_start_lba, &dev->userdata_end_lba,
                       &dev->userdata_bytes, &dev->userdata_start_512, sector,
                       err, errlen) < 0)
        return -1;
    char metadata_parent[64];
    if (partition_parent(p, metadata, metadata_parent, sizeof(metadata_parent), err, errlen) < 0)
        return -1;
    if (strcmp(metadata_parent, disk_guess))
        return failf(err, errlen, "metadata is not on the userdata disk");
    if (partition_info(p, metadata, "metadata", dev, &dev->metadata_index,
                       &dev->metadata_start_lba, &dev->metadata_end_lba,
                       &dev->metadata_bytes, &dev->metadata_start_512, sector,
                       err, errlen) < 0)
        return -1;
    if (strcmp(dev->disk, disk_path))
        return failf(err, errlen, "metadata is not on the userdata disk");
    if (dev->metadata_index == dev->userdata_index)
        return failf(err, errlen, "userdata and metadata resolve to the same partition");
    struct stat st;
    if (stat(dev->raw, &st) < 0)
        return failf(err, errlen, "cannot stat %s: %s", dev->raw, strerror(errno));
    dev->raw_rdev = st.st_rdev;
    dev->raw_is_block = S_ISBLK(st.st_mode);
#ifndef UNIXNU_TESTING
    if (!dev->raw_is_block)
        return failf(err, errlen, "userdata is not a block device");
#endif
    if (stat(dev->metadata, &st) < 0)
        return failf(err, errlen, "cannot stat %s: %s", dev->metadata, strerror(errno));
    dev->metadata_rdev = st.st_rdev;
    char serial[192] = "";
    if (read_sys_text(p, disk_guess, "device/serial", serial, sizeof(serial)) < 0 &&
        read_sys_text(p, disk_guess, "device/wwid", serial, sizeof(serial)) < 0 &&
        read_sys_text(p, disk_guess, "device/unique_id", serial, sizeof(serial)) < 0)
        serial[0] = '\0';
    if (!serial[0])
        snprintf(serial, sizeof(serial), "%s", id->serial);
    snprintf(dev->disk_serial, sizeof(dev->disk_serial), "%s", serial[0] ? serial : "unknown");
    if (!id->serial[0])
        snprintf(id->serial, sizeof(id->serial), "%s", dev->disk_serial);
    return 0;
}

static void mount_unescape(char *s)
{
    char *o = s;
    for (size_t i = 0; s[i];) {
        if (s[i] == '\\' && s[i + 1] >= '0' && s[i + 1] <= '3' &&
            s[i + 2] >= '0' && s[i + 2] <= '7' &&
            s[i + 3] >= '0' && s[i + 3] <= '7') {
            *o++ = (char)(((s[i + 1] - '0') << 6) | ((s[i + 2] - '0') << 3) | (s[i + 3] - '0'));
            i += 4;
        } else {
            *o++ = s[i++];
        }
    }
    *o = '\0';
}

static int mk_mount_stat(const paths_t *p, const char *target, dev_t *dev_out)
{
    char path[PATH_MAX];
    if (root_join(p, path, sizeof(path), target) < 0)
        return -1;
    struct stat st;
    if (stat(path, &st) < 0)
        return -1;
    *dev_out = st.st_dev;
    return 0;
}

static int mount_has_opt(const char *opts, const char *opt)
{
    size_t n = strlen(opt);
    const char *p = opts;
    while (p && *p) {
        const char *e = strchr(p, ',');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len == n && !strncmp(p, opt, n))
            return 1;
        p = e ? e + 1 : NULL;
    }
    return 0;
}

static int mounts_path(const paths_t *p, char *path, size_t cap)
{
#ifdef UNIXNU_TESTING
    const char *override = getenv("UNIXNU_TEST_MOUNTS");
    if (override && *override) {
        if (snprintf(path, cap, "%s", override) >= (int)cap)
            return -1;
        return 0;
    }
#endif
    return root_join(p, path, cap, "/proc/mounts");
}

int mounts_scan(const paths_t *p, device_t *dev, mounts_t *mounts, char *err, size_t errlen)
{
    memset(mounts, 0, sizeof(*mounts));
    char path[PATH_MAX];
    if (mounts_path(p, path, sizeof(path)) < 0)
        return failf(err, errlen, "path too long");
    FILE *f = fopen(path, "r");
    if (!f)
        return failf(err, errlen, "cannot read %s: %s", path, strerror(errno));
    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        char *fields[6];
        int nf = 0;
        char *save = NULL;
        for (char *tok = strtok_r(line, " \t\r\n", &save); tok && nf < 6;
             tok = strtok_r(NULL, " \t\r\n", &save))
            fields[nf++] = tok;
        if (nf < 4)
            continue;
        mount_unescape(fields[0]);
        mount_unescape(fields[1]);
        mount_t m;
        memset(&m, 0, sizeof(m));
        snprintf(m.source, sizeof(m.source), "%s", fields[0]);
        snprintf(m.target, sizeof(m.target), "%s", fields[1]);
        snprintf(m.fstype, sizeof(m.fstype), "%s", fields[2]);
        snprintf(m.opts, sizeof(m.opts), "%s", fields[3]);
        mk_mount_stat(p, m.target, &m.st_dev);
        if (!strcmp(m.target, "/data"))
            m.is_data = 1;
        if (dev->sector && (m.st_dev == dev->raw_rdev ||
                            (dev->mapped_rdev && m.st_dev == dev->mapped_rdev)))
            m.is_userdata = 1;
        if (m.st_dev && dev->metadata_rdev && m.st_dev == dev->metadata_rdev)
            m.is_metadata = 1;
#ifdef UNIXNU_TESTING
        if (m.is_data || !strncmp(m.target, "/data/", 6) || !strcmp(m.target, "/metadata"))
            m.is_userdata = !strcmp(m.target, "/metadata") ? m.is_userdata : 1;
        if (!strcmp(m.target, "/metadata") ||
            !strcmp(path_basename(m.source), dev->metadata_node))
            m.is_metadata = 1;
#endif
        mounts->items = xrealloc(mounts->items, (mounts->n + 1) * sizeof(*mounts->items));
        mounts->items[mounts->n++] = m;
        if (m.is_data) {
            mounts->data_count++;
            mounts->data_rw = mount_has_opt(m.opts, "rw") && !mount_has_opt(m.opts, "ro");
            mounts->data_checkpoint_enabled = !strstr(m.opts, "checkpoint=disable");
            if (!strcmp(m.fstype, "f2fs") && m.source[0] == '/') {
                char resolved[PATH_MAX], joined[PATH_MAX];
                resolved[0] = '\0';
                if (!realpath(m.source, resolved) &&
                    root_join(p, joined, sizeof(joined), m.source) == 0) {
                    char *rp = realpath(joined, resolved);
                    (void)rp;
                }
                if (resolved[0]) {
                    snprintf(dev->mapped, sizeof(dev->mapped), "%s", resolved);
                    snprintf(dev->mapped_node, sizeof(dev->mapped_node), "%s",
                             path_basename(resolved));
                    struct stat st;
                    if (stat(resolved, &st) == 0) {
                        dev->mapped_rdev = st.st_rdev;
#ifdef UNIXNU_TESTING
                        /* Fixture block devices are regular files, whose
                         * st_rdev is zero. Keep a stable non-zero identity so
                         * the mapping checks can be exercised offline. */
                        if (!dev->mapped_rdev)
                            dev->mapped_rdev = st.st_dev ? st.st_dev : (dev_t)1;
#endif
                    }
                }
            }
        }
        if (m.is_metadata)
            mounts->metadata_mounts++;
    }
    fclose(f);
    if (mounts->data_count > 1)
        return failf(err, errlen, "multiple /data mounts were found");
    if (mounts->data_count == 1) {
        const mount_t *m = NULL;
        for (size_t i = 0; i < mounts->n; i++)
            if (mounts->items[i].is_data)
                m = &mounts->items[i];
        if (!m || strcmp(m->fstype, "f2fs"))
            return failf(err, errlen, "/data is not mounted as F2FS");
        if (!dev->mapped[0]) {
            snprintf(dev->mapped, sizeof(dev->mapped), "%s", dev->raw);
            snprintf(dev->mapped_node, sizeof(dev->mapped_node), "%s", dev->raw_node);
            dev->mapped_rdev = dev->raw_rdev;
        }
        mounts->data_decrypted_mapping = strcmp(dev->mapped, dev->raw) != 0;
        if (dm_topology_check(p, dev->raw_node, dev->mapped_node, err, errlen) < 0)
            return -1;
    }
    /* Do not clear dev->mapped here when /data is absent. After an apply
     * unmount, mounts_check_unmounted needs the last known decrypted device to
     * detect a bind mount of /data that outlived the /data mount itself. */
    return 0;
}

void mounts_free(mounts_t *mounts)
{
    free(mounts->items);
    memset(mounts, 0, sizeof(*mounts));
}

int mounts_check_for_plan(const paths_t *p, device_t *dev, mounts_t *mounts, char *err, size_t errlen)
{
    (void)p;
    if (mounts->data_count != 1)
        return failf(err, errlen, "mount decrypted /data read-write before planning");
    if (!mounts->data_rw)
        return failf(err, errlen, "/data must be mounted read-write");
    if (!mounts->data_checkpoint_enabled)
        return failf(err, errlen, "/data is mounted with checkpoint=disable, which is unsupported");
    if (mounts->metadata_mounts)
        return failf(err, errlen, "metadata must be unmounted before planning");
    if (!dev->mapped[0] || !dev->mapped_rdev)
        return failf(err, errlen, "cannot identify the decrypted /data mapping");
    return 0;
}

int mounts_check_unmounted(const paths_t *p, device_t *dev, mounts_t *mounts, char *err, size_t errlen)
{
    mounts_free(mounts);
    if (mounts_scan(p, dev, mounts, err, errlen) < 0)
        return -1;
    if (mounts->data_count)
        return failf(err, errlen, "/data is still mounted");
    for (size_t i = 0; i < mounts->n; i++) {
        if (mounts->items[i].is_userdata)
            return failf(err, errlen, "%s still provides userdata or a bind mount",
                         mounts->items[i].target);
        if (mounts->items[i].is_metadata)
            return failf(err, errlen, "%s still provides metadata", mounts->items[i].target);
    }
    return 0;
}

int mounts_unmount_data(const paths_t *p, device_t *dev, mounts_t *mounts, char *err, size_t errlen)
{
#ifdef UNIXNU_TESTING
    const char *fake = getenv("UNIXNU_TEST_FAKE_UNMOUNT");
    if (fake && !strcmp(fake, "1")) {
        /* Remove only the /data mount so a surviving bind mount under /data is
         * still visible, exactly as after a real umount(2) of /data. */
        char path[PATH_MAX];
        if (mounts_path(p, path, sizeof(path)) == 0) {
            FILE *f = fopen(path, "r");
            char kept[8192] = "";
            size_t used = 0;
            if (f) {
                char line[1024];
                while (fgets(line, sizeof(line), f)) {
                    char copy[1024];
                    snprintf(copy, sizeof(copy), "%s", line);
                    char *save = NULL;
                    char *source = strtok_r(copy, " \t\n", &save);
                    char *target = strtok_r(NULL, " \t\n", &save);
                    if (source && target && !strcmp(target, "/data"))
                        continue;
                    size_t len = strlen(line);
                    if (used + len + 1 < sizeof(kept)) {
                        memcpy(kept + used, line, len);
                        used += len;
                        kept[used] = '\0';
                    }
                }
                fclose(f);
            }
            (void)write_text(path, kept, 0644);
        }
    } else
#endif
    {
        char data_path[PATH_MAX];
        if (root_join(p, data_path, sizeof(data_path), "/data") < 0)
            return failf(err, errlen, "path too long");
        if (umount2(data_path, 0) < 0)
            return failf(err, errlen, "cannot unmount /data: %s; do not force or lazily unmount",
                         strerror(errno));
    }
    return mounts_check_unmounted(p, dev, mounts, err, errlen);
}

static int dir_contains_only(const char *path, const char *only, char *err, size_t errlen)
{
    DIR *d = opendir(path);
    if (!d)
        return failf(err, errlen, "cannot inspect %s: %s", path, strerror(errno));
    unsigned count = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        if (only && strcmp(e->d_name, only))
            return failf(err, errlen, "unexpected entry %s in %s", e->d_name, path);
        if (!only)
            return failf(err, errlen, "unexpected holder in %s", path);
        count++;
    }
    closedir(d);
    if (only && count != 1)
        return failf(err, errlen, "missing expected entry %s in %s", only, path);
    return 0;
}

int dm_topology_check(const paths_t *p, const char *raw_node, const char *mapped_node,
                      char *err, size_t errlen)
{
    char base[PATH_MAX], path[PATH_MAX];
    if (root_join(p, base, sizeof(base), "/sys/class/block") < 0)
        return failf(err, errlen, "path too long");
    if (!strcmp(raw_node, mapped_node)) {
        if (path_join(path, sizeof(path), base, raw_node) < 0 ||
            path_join(path, sizeof(path), path, "holders") < 0)
            return failf(err, errlen, "path too long");
        return dir_contains_only(path, NULL, err, errlen);
    }
    if (strncmp(mapped_node, "dm-", 3))
        return failf(err, errlen, "unsupported /data mapping %s", mapped_node);
    if (path_join(path, sizeof(path), base, mapped_node) < 0 ||
        path_join(path, sizeof(path), path, "slaves") < 0)
        return failf(err, errlen, "path too long");
    if (dir_contains_only(path, raw_node, err, errlen) < 0)
        return -1;
    if (path_join(path, sizeof(path), base, raw_node) < 0 ||
        path_join(path, sizeof(path), path, "holders") < 0)
        return failf(err, errlen, "path too long");
    if (dir_contains_only(path, mapped_node, err, errlen) < 0)
        return -1;
    if (path_join(path, sizeof(path), base, mapped_node) < 0 ||
        path_join(path, sizeof(path), path, "holders") < 0)
        return failf(err, errlen, "path too long");
    return dir_contains_only(path, NULL, err, errlen);
}

int open_usage_scan(const paths_t *p, device_t *dev, char *detail, size_t detail_len)
{
    char proc[PATH_MAX];
    if (root_join(p, proc, sizeof(proc), "/proc") < 0)
        return -1;
    DIR *d = opendir(proc);
    if (!d)
        return -1;
    int count = 0;
    pid_t self = getpid();
    struct dirent *e;
    while ((e = readdir(d)) && count < 8) {
        if (!isdigit((unsigned char)e->d_name[0]))
            continue;
        pid_t pid = (pid_t)atoi(e->d_name);
        if (pid == self)
            continue;
        char fddir[PATH_MAX];
        if (path_join(fddir, sizeof(fddir), proc, e->d_name) < 0 ||
            path_join(fddir, sizeof(fddir), fddir, "fd") < 0)
            continue;
        DIR *fds = opendir(fddir);
        if (!fds)
            continue;
        struct dirent *fe;
        while ((fe = readdir(fds))) {
            if (fe->d_name[0] == '.')
                continue;
            char link[PATH_MAX], target[PATH_MAX];
            if (path_join(link, sizeof(link), fddir, fe->d_name) < 0)
                continue;
            ssize_t n = readlink(link, target, sizeof(target) - 1);
            if (n <= 0)
                continue;
            target[n] = '\0';
            if (!strcmp(target, dev->raw) || (dev->mapped[0] && !strcmp(target, dev->mapped)) ||
                !strncmp(target, "/data", 5)) {
                if (detail && detail_len)
                    snprintf(detail, detail_len, "pid %d has %s open", (int)pid, target);
                count++;
                break;
            }
        }
        closedir(fds);
    }
    closedir(d);
    return count;
}

int caps_check(const paths_t *p, const identity_t *id, device_t *dev, f2fs_info_t *fs,
               caps_t *caps, char *err, size_t errlen)
{
    (void)dev;
    memset(caps, 0, sizeof(*caps));
    char path[PATH_MAX], buf[65536];
    if (root_join(p, path, sizeof(path), "/proc/filesystems") < 0 ||
        read_text(path, buf, sizeof(buf)) < 0)
        return failf(err, errlen, "cannot read /proc/filesystems");
    caps->f2fs_filesystem = strstr(buf, "f2fs") != NULL;
    if (root_join(p, path, sizeof(path), "/sys/module/f2fs") == 0 && access(path, F_OK) == 0)
        caps->f2fs_sysfs = 1;
    if (root_join(p, path, sizeof(path), "/sys/fs/f2fs") == 0 && access(path, F_OK) == 0)
        caps->f2fs_sysfs = 1;
    if (root_join(p, path, sizeof(path), "/proc/kallsyms") == 0 &&
        read_text(path, buf, sizeof(buf)) == 0) {
        if (strstr(buf, "f2fs_resize_fs") || strstr(buf, "f2fs_ioctl"))
            caps->resize_symbol = 1;
    }
    int major = 0, minor = 0;
    if (sscanf(id->kernel, "%d.%d", &major, &minor) == 2)
        caps->kernel_ok = major > 3 || (major == 3 && minor >= 8);
    caps->data_mounted = fs && fs->mounted;
    if (caps->data_mounted) {
        char data_path[PATH_MAX];
        int fd = -1;
        if (root_join(p, data_path, sizeof(data_path), "/data") == 0)
            fd = open(data_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd >= 0) {
            uint32_t feat = 0;
            if (ioctl(fd, F2FS_IOC_GET_FEATURES, &feat) == 0) {
                caps->ioctl_namespace = 1;
                caps->getfeatures_ok = 1;
            } else if (errno == EINVAL) {
                caps->ioctl_namespace = 1;
            }
            close(fd);
        }
    }
    caps->all_ok = caps->f2fs_filesystem && caps->kernel_ok &&
                   (caps->getfeatures_ok || caps->resize_symbol);
    snprintf(caps->details, sizeof(caps->details),
             "f2fs_filesystem=%d sysfs=%d getfeatures=%d resize_symbol=%d kernel_ok=%d",
             caps->f2fs_filesystem, caps->f2fs_sysfs, caps->getfeatures_ok,
             caps->resize_symbol, caps->kernel_ok);
    return 0;
}

int device_partition_geometry(const paths_t *p, const char *name, uint32_t sector,
                              int *index, uint64_t *start_lba, uint64_t *end_lba,
                              uint64_t *bytes, char *err, size_t errlen)
{
    char path[PATH_MAX];
    if (resolve_byname(p, name, path, sizeof(path), err, errlen) < 0)
        return -1;
    if (strstr(path, "/dm-"))
        return failf(err, errlen, "%s resolves to a device-mapper node", name);
    device_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    uint64_t start512 = 0;
    if (partition_info(p, path, "__geometry__", &tmp, index, start_lba, end_lba,
                       bytes, &start512, sector, err, errlen) < 0)
        return -1;
    return 0;
}

int metadata_content_hash(const paths_t *p, device_t *dev, uint8_t out[32], char *err, size_t errlen)
{
    (void)p;
    int fd = open(dev->metadata, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return failf(err, errlen, "cannot open metadata: %s", strerror(errno));
    int rc = sha256_region(fd, dev->metadata_bytes, out);
    close(fd);
    if (rc < 0)
        return failf(err, errlen, "cannot read metadata: %s", strerror(errno));
    return 0;
}
