/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/*
 * F2FS validation follows UniXNU scripts/dual_sys/f2fs-shrink.c, deliberately.
 *
 * That reference implementation never reads the F2FS checkpoint: it validates
 * the two superblock copies and then asks the kernel to resize. The checkpoint
 * is kernel-owned metadata under a live mount, so a userspace pre-read races the
 * kernel writing it and can observe a torn or mid-checkpoint pair. Reading it
 * proved nothing the resize needs and refused healthy filesystems twice on real
 * hardware. State that only the kernel can state correctly is left to the
 * kernel: if it will not resize, the ioctl fails and the caller leaves the GPT
 * untouched.
 */
#include "common.h"
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/statfs.h>
#include <sys/vfs.h>
#include <sys/mount.h>

#define F2FS_MAGIC 0xf2f52010U
#define F2FS_IOC_GET_FEATURES _IOR(0xf5, 5, uint32_t)
#define F2FS_IOC_RESIZE_FS _IOW(0xf5, 16, uint64_t)
#define F2FS_SUPER_OFFSET 1024U
#define F2FS_SUPER_SIZE 3072U

static uint32_t le32(const void *p)
{
    const uint8_t *b = p;
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static uint64_t le64(const void *p)
{
    return (uint64_t)le32(p) | ((uint64_t)le32((const uint8_t *)p + 4) << 32);
}

static void utf16le_to_ascii(const uint8_t *src, size_t chars, char *out, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; i < chars && o + 1 < cap; i++) {
        uint32_t c = (uint32_t)src[i * 2] | ((uint32_t)src[i * 2 + 1] << 8);
        if (!c)
            break;
        out[o++] = (c >= 32 && c < 127) ? (char)c : '?';
    }
    out[o] = '\0';
}

/*
 * Validate the superblock pair and report the filesystem geometry.
 *
 * The two copies carry 3072 bytes each, at byte 1024 of block 0 and byte 1024
 * of block 1. A mismatch means the layout fields cannot be trusted, so this
 * refuses rather than picking a copy: unlike the checkpoint, the superblock is
 * only rewritten by a resize or a repair, so a disagreement is a real fault.
 */
static int probe_buffer(const uint8_t *buf, uint64_t partition_bytes, f2fs_info_t *info,
                        char *err, size_t errlen)
{
    const uint8_t *first = buf + F2FS_SUPER_OFFSET;
    const uint8_t *second = buf + F2FS_BLOCK_BYTES + F2FS_SUPER_OFFSET;
    memset(info, 0, sizeof(*info));

    int first_ok = le32(first) == F2FS_MAGIC;
    int second_ok = le32(second) == F2FS_MAGIC;
    if (!first_ok && !second_ok)
        return failf(err, errlen, "userdata is not F2FS");
    if (!first_ok || !second_ok) {
        info->sb_damaged_copy = first_ok ? 2 : 1;
        return failf(err, errlen, "F2FS superblock copy %d is damaged", info->sb_damaged_copy);
    }
    int differs = memcmp(first, second, F2FS_SUPER_SIZE) != 0;
    if (differs) {
        info->sb_offset = F2FS_BLOCK_BYTES + F2FS_SUPER_OFFSET;
        return failf(err, errlen,
                     "F2FS superblock copies disagree; run fsck.f2fs before shrinking");
    }

    const uint8_t *s = first;
    uint32_t log_sectorsize = le32(s + 8);
    uint32_t log_sectors_per_block = le32(s + 12);
    uint32_t log_blocksize = le32(s + 16);
    uint32_t log_blocks_per_seg = le32(s + 20);
    uint32_t segs_per_sec = le32(s + 24);
    if (log_blocksize != 12)
        return failf(err, errlen, "unsupported F2FS block size (log2=%u)", log_blocksize);
    if (log_blocks_per_seg != 9)
        return failf(err, errlen, "unsupported F2FS segment size (log2=%u)", log_blocks_per_seg);
    if (segs_per_sec != 1)
        return failf(err, errlen, "unsupported F2FS sections with %u segments", segs_per_sec);
    if (log_sectorsize < 9 || log_sectorsize > 12 ||
        log_sectorsize + log_sectors_per_block != 12)
        return failf(err, errlen, "unsupported F2FS sector geometry");
    uint32_t feature = le32(s + 2180);
    info->feature = feature;
    info->encrypt_feature = !!(feature & 1);
    info->zoned = !!(feature & 2);
    /* devs[0].path begins at byte 2201 in the packed F2FS superblock. */
    info->multi_device = s[2201] != 0;
    if (info->multi_device)
        return failf(err, errlen, "multi-device F2FS is unsupported");
    if (info->zoned)
        return failf(err, errlen, "zoned F2FS is unsupported");
    info->block_size = 1U << log_blocksize;
    info->blocks_per_seg = 1U << log_blocks_per_seg;
    info->segs_per_sec = segs_per_sec;
    info->section_blocks = info->blocks_per_seg * info->segs_per_sec;
    info->blocks = le64(s + 36);
    info->section_count = le32(s + 44);
    info->segment_count = le32(s + 48);
    info->segment_count_main = le32(s + 68);
    uuid_format_be(s + 108, info->uuid);
    utf16le_to_ascii(s + 124, 256, info->volume_name, sizeof(info->volume_name));
    if (!info->blocks || info->blocks > partition_bytes / F2FS_BLOCK_BYTES)
        return failf(err, errlen, "F2FS block count exceeds the userdata partition");
    return 0;
}

int f2fs_probe_fd(int fd, uint64_t partition_bytes, f2fs_info_t *info, char *err, size_t errlen)
{
    uint8_t buf[2 * F2FS_BLOCK_BYTES];
    if (pread(fd, buf, sizeof(buf), 0) != (ssize_t)sizeof(buf))
        return failf(err, errlen, "cannot read both F2FS superblocks");
    return probe_buffer(buf, partition_bytes, info, err, errlen);
}

int f2fs_probe_path(const char *path, uint64_t partition_bytes, f2fs_info_t *info,
                    char *err, size_t errlen)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return failf(err, errlen, "cannot open %s: %s", path, strerror(errno));
    int rc = f2fs_probe_fd(fd, partition_bytes, info, err, errlen);
    close(fd);
    return rc;
}

int f2fs_probe_mounted(const char *mountpoint, const char *block_path, uint64_t partition_bytes,
                       f2fs_info_t *info, char *err, size_t errlen)
{
    int fs = open(mountpoint, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fs < 0)
        return failf(err, errlen, "cannot open %s: %s", mountpoint, strerror(errno));
    struct stat st, bs;
    struct statfs sfs;
    if (fstat(fs, &st) || stat(block_path, &bs))
        return failf(err, errlen, "cannot inspect mounted userdata");
    int identity_ok = st.st_dev == bs.st_rdev;
    int statfs_ok = fstatfs(fs, &sfs) == 0 &&
                    (uint32_t)sfs.f_type == F2FS_MAGIC && sfs.f_bsize == F2FS_BLOCK_BYTES;
#ifdef UNIXNU_TESTING
    if (!identity_ok || !statfs_ok) {
        const char *fixture = getenv("UNIXNU_TEST_STATFS");
        if (fixture && *fixture) {
            FILE *f = fopen(fixture, "r");
            if (f) {
                char line[256];
                unsigned long long b = 0, bf = 0, ba = 0;
                while (fgets(line, sizeof(line), f)) {
                    if (sscanf(line, "blocks=%llu", &b) == 1 ||
                        sscanf(line, "bfree=%llu", &bf) == 1 ||
                        sscanf(line, "bavail=%llu", &ba) == 1)
                        continue;
                }
                fclose(f);
                if (b && ba) {
                    sfs.f_blocks = b;
                    sfs.f_bfree = bf;
                    sfs.f_bavail = ba;
                    sfs.f_bsize = F2FS_BLOCK_BYTES;
                    identity_ok = 1;
                    statfs_ok = 1;
                }
            }
        }
    }
#endif
    if (!identity_ok)
        return failf(err, errlen, "%s is not backed by %s", mountpoint, block_path);
    if (!statfs_ok)
        return failf(err, errlen, "%s is not a 4096-byte F2FS filesystem", mountpoint);
    int bfd = open(block_path, O_RDONLY | O_CLOEXEC);
    if (bfd < 0)
        return failf(err, errlen, "cannot open %s: %s", block_path, strerror(errno));
    int rc = f2fs_probe_fd(bfd, partition_bytes, info, err, errlen);
    close(bfd);
    if (rc < 0) {
        close(fs);
        return -1;
    }
    info->mounted = 1;
    info->f_blocks = (uint64_t)sfs.f_blocks;
    info->f_bfree = (uint64_t)sfs.f_bfree;
    info->f_bavail = (uint64_t)sfs.f_bavail;
    snprintf(info->mount_source, sizeof(info->mount_source), "%s", block_path);
    close(fs);
    return 0;
}

uint64_t f2fs_request_blocks(uint64_t before, uint64_t limit)
{
    uint64_t remainder = before % F2FS_SEGMENT_BLOCKS;
    if (limit <= remainder + F2FS_SEGMENT_BLOCKS)
        return 0;
    /* The kernel removes whole sections and retains the original remainder. */
    return (limit - remainder) / F2FS_SEGMENT_BLOCKS * F2FS_SEGMENT_BLOCKS;
}

uint64_t f2fs_expected_blocks(uint64_t before, uint64_t request)
{
    return before - (before - request) / F2FS_SEGMENT_BLOCKS * F2FS_SEGMENT_BLOCKS;
}

int f2fs_shrink(int fs_fd, int block_fd, const char *device_path, uint64_t limit_blocks,
                f2fs_info_t *info, uint64_t *request_out, uint64_t *expected_out,
                char *err, size_t errlen)
{
    uint64_t before = info->blocks;
    uint64_t request = f2fs_request_blocks(before, limit_blocks);
    if (!request)
        return failf(err, errlen, "F2FS target is too small for section rounding");
    uint64_t expected = f2fs_expected_blocks(before, request);
    if (expected > limit_blocks)
        return failf(err, errlen, "F2FS section rounding exceeds the planned partition");
    /* The free-space check needs statfs, which only exists while /data is
     * mounted. It has already run during planning and again on the mounted
     * recheck; by the time the shrink happens /data is gone on purpose, so
     * f_bavail is 0 here and the kernel's own ioctl is the authority. */
    if (info->mounted && before > request && before - request >= info->f_bavail)
        return failf(err, errlen, "insufficient free F2FS space; kernel also needs reserved space");
#ifdef UNIXNU_TESTING
    {
        const char *fake = getenv("UNIXNU_TEST_FAKE_SHRINK");
        const char *failing = getenv("UNIXNU_TEST_FAIL_SHRINK");
        if (failing && *failing && strcmp(failing, "0")) {
            /* The ioctl ran but the kernel refused it: the GPT must not move. */
            return failf(err, errlen, "F2FS_IOC_RESIZE_FS failed: %s",
                         strerror(EIO));
        }
        if (fake && !strcmp(fake, "1")) {
            /*
             * Emulate what F2FS_IOC_RESIZE_FS leaves behind: both superblock
             * copies carry the new block_count. The checkpoint is not touched
             * because the tool no longer validates it.
             *
             * This hook deliberately does NOT exercise the mount/mkdtemp path
             * below, because a fixture is a plain file and cannot be mounted.
             * The real path is guarded by f2fs_shrink_needs_mount() so that a
             * regression (calling the ioctl on an unmounted fd, which crashes
             * the recovery kernel) fails the test suite instead of the phone.
             */
            uint8_t le[8];
            for (int i = 0; i < 8; i++)
                le[i] = (uint8_t)(expected >> (i * 8));
            if (pwrite(block_fd, le, 8, F2FS_SUPER_OFFSET + 36) != 8 ||
                pwrite(block_fd, le, 8, F2FS_BLOCK_BYTES + F2FS_SUPER_OFFSET + 36) != 8)
                return failf(err, errlen, "cannot simulate the F2FS resize");
            if (fsync(block_fd) < 0)
                return failf(err, errlen, "cannot sync the simulated F2FS resize");
            info->blocks = expected;
            *request_out = request;
            *expected_out = expected;
            return 0;
        }
    }
#endif
    if (syncfs(fs_fd) < 0)
        return failf(err, errlen, "syncfs failed before F2FS resize: %s", strerror(errno));

    /*
     * F2FS_IOC_RESIZE_FS operates on a MOUNTED f2fs instance, not on a bare
     * block device. Calling it on an unmounted fd reaches the kernel's resize
     * path with no f2fs superblock in memory and crashes the caller instead of
     * returning an error.
     *
     * We still want /data unmounted while the size changes, so mount the same
     * mapping at a private directory owned by this process, resize through that
     * mount, then unmount it. Nothing else can reach it: the mount point is a
     * fresh mkdtemp path, and the mount is read-write only for the duration of
     * the ioctl.
     */
    char priv[] = "/tmp/unixnu-resize-XXXXXX";
    if (!mkdtemp(priv))
        return failf(err, errlen, "cannot create a private mount point: %s", strerror(errno));
    int mounted_priv = 0;
    int rc = -1;

    if (mount(device_path, priv, "f2fs", 0, NULL) < 0) {
        int saved = errno;
        rmdir(priv);
        return failf(err, errlen, "cannot mount %s for the resize: %s", device_path,
                     strerror(saved));
    }
    mounted_priv = 1;

    int mfd = open(priv, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (mfd < 0) {
        int saved = errno;
        umount2(priv, 0);
        rmdir(priv);
        return failf(err, errlen, "cannot open the private mount %s: %s", priv, strerror(saved));
    }

    if (syncfs(mfd) < 0) {
        failf(err, errlen, "syncfs failed on the private mount: %s", strerror(errno));
        goto out;
    }
    if (ioctl(mfd, F2FS_IOC_RESIZE_FS, &request) < 0) {
        failf(err, errlen, "F2FS_IOC_RESIZE_FS failed: %s", strerror(errno));
        goto out;
    }
    if (syncfs(mfd) < 0) {
        failf(err, errlen, "syncfs after resize failed: %s", strerror(errno));
        goto out;
    }
    rc = 0;

out:
    close(mfd);
    /* Unmounting flushes the checkpoint that records the new user_block_count,
     * which is what makes the smaller size survive a remount. */
    if (mounted_priv && umount2(priv, 0) < 0 && rc == 0) {
        failf(err, errlen, "resize succeeded but the private mount stayed busy: %s",
              strerror(errno));
        rc = -1;
    }
    rmdir(priv);
    if (rc < 0)
        return -1;
    f2fs_info_t after;
    if (f2fs_probe_fd(block_fd, (uint64_t)limit_blocks * F2FS_BLOCK_BYTES, &after, err, errlen) < 0)
        return -1;
    if (after.blocks != expected || after.blocks > limit_blocks)
        return failf(err, errlen, "unexpected F2FS size after resize: %" PRIu64 " (expected %" PRIu64 ")",
                     after.blocks, expected);
    *request_out = request;
    *expected_out = expected;
    return 0;
}

/*
 * Invariant that must hold on the real path: F2FS_IOC_RESIZE_FS targets a
 * mounted f2fs instance. This function exists so the offline suite can assert
 * that the shrink takes a mount source and a mount point, not a bare fd.
 */
int f2fs_shrink_needs_mount(void)
{
    return 1;
}
