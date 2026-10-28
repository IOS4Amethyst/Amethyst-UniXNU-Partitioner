/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef UNIXNU_COMMON_H
#define UNIXNU_COMMON_H

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define TOOL_NAME "amethyst-unixnu-partitioner"
#define TOOL_VERSION "0.1.0"
#define MIB UINT64_C(1048576)
#define GIB UINT64_C(1073741824)
#define GPT_ENTRY_SIZE 128U
#define GPT_MIN_TABLE_BYTES 16384U
#define MAX_GPT_ENTRIES 4096U
#define F2FS_BLOCK_BYTES 4096U
#define F2FS_SEGMENT_BLOCKS 512U
#define LINUX_FS_DATA_GUID "0fc63daf-8483-4772-8e79-3d69d8477de4"
#define F2FS_IOC_GET_FEATURES _IOR(0xf5, 5, uint32_t)
#define F2FS_IOC_RESIZE_FS _IOW(0xf5, 16, uint64_t)

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

typedef struct {
    char root[PATH_MAX];
    char base[PATH_MAX];
    char request[PATH_MAX];
    char identity[PATH_MAX];
    int testing;
} paths_t;

typedef struct {
    char device[96];
    char model[192];
    char serial[192];
    char recovery[64];
    char twrp[64];
    char kernel[192];
    char machine[96];
    char usb_config[192];
    char usb_state[192];
    char dt_model[192];
    int boot_completed;
} identity_t;

typedef struct {
    char raw[PATH_MAX];
    char raw_node[64];
    char mapped[PATH_MAX];
    char mapped_node[64];
    char metadata[PATH_MAX];
    char metadata_node[64];
    char disk[PATH_MAX];
    char disk_node[64];
    int userdata_index;
    int metadata_index;
    uint32_t sector;
    uint32_t physical_sector;
    uint64_t disk_bytes;
    uint64_t total_sectors;
    uint64_t userdata_start_lba;
    uint64_t userdata_end_lba;
    uint64_t userdata_bytes;
    uint64_t userdata_start_512;
    uint64_t metadata_start_lba;
    uint64_t metadata_end_lba;
    uint64_t metadata_bytes;
    uint64_t metadata_start_512;
    char disk_serial[192];
    dev_t raw_rdev;
    dev_t mapped_rdev;
    dev_t metadata_rdev;
    int raw_is_block;
    int mapped_is_block;
} device_t;

typedef struct {
    uint32_t index;
    uint8_t type_guid[16];
    uint8_t unique_guid[16];
    uint64_t start;
    uint64_t end;
    uint64_t attrs;
    char name[80];
    uint8_t raw[GPT_ENTRY_SIZE];
} gpt_entry_t;

typedef struct {
    uint32_t sector;
    uint64_t total_sectors;
    uint32_t count;
    uint32_t entry_size;
    uint32_t blocks;
    uint64_t first_usable;
    uint64_t last_usable;
    uint8_t disk_guid[16];
    uint8_t *primary;
    size_t primary_len;
    uint8_t *backup;
    size_t backup_len;
    uint8_t *table;
    uint32_t table_crc;
    uint32_t primary_crc;
    uint32_t backup_crc;
    gpt_entry_t *entries;
    size_t n_entries;
} gpt_t;

typedef struct {
    uint64_t blocks;
    uint32_t block_size;
    uint32_t blocks_per_seg;
    uint32_t segs_per_sec;
    uint32_t section_blocks;
    uint32_t section_count;
    uint32_t segment_count;
    uint32_t segment_count_main;
    uint32_t feature;
    int encrypt_feature;
    int zoned;
    int multi_device;
    uint32_t ckpt_flags;
    uint64_t cp_version;
    uint64_t cp_user_blocks;
    uint64_t cp_valid_blocks;
    uint32_t cp_free_segments;
    uint32_t cp_total_segments;
    int cp_copies_match;
    int cp_present;
    char uuid[37];
    char volume_name[64];
    uint8_t sb_sha256[32];
    int mounted;
    uint64_t f_blocks;
    uint64_t f_bfree;
    uint64_t f_bavail;
    char mount_source[PATH_MAX];
} f2fs_info_t;

typedef struct {
    char source[PATH_MAX];
    char target[PATH_MAX];
    char fstype[64];
    char opts[512];
    dev_t st_dev;
    int is_data;
    int is_userdata;
    int is_metadata;
} mount_t;

typedef struct {
    mount_t *items;
    size_t n;
    int data_count;
    int metadata_mounts;
    int data_rw;
    int data_checkpoint_enabled;
    int data_decrypted_mapping;
} mounts_t;

typedef struct {
    int f2fs_filesystem;
    int f2fs_sysfs;
    int ioctl_namespace;
    int getfeatures_ok;
    int resize_symbol;
    int kernel_ok;
    int data_mounted;
    int all_ok;
    char details[512];
} caps_t;

typedef struct {
    int userdata_index;
    int unixnu_index;
    uint64_t userdata_start;
    uint64_t userdata_end;
    uint64_t unixnu_start;
    uint64_t unixnu_end;
    uint64_t old_userdata_bytes;
    uint64_t new_userdata_bytes;
    uint64_t unixnu_bytes;
    uint64_t tail_gap_bytes;
    uint32_t sector;
    uint8_t unixnu_type_guid[16];
    uint8_t unixnu_unique_guid[16];
    uint8_t userdata_type_guid[16];
    uint8_t userdata_unique_guid[16];
    uint64_t userdata_attrs;
} layout_t;

typedef struct {
    identity_t id;
    device_t dev;
    gpt_t gpt;
    f2fs_info_t fs;
    mounts_t mounts;
    caps_t caps;
    int have_gpt;
    int have_fs;
    int have_mounts;
    char warnings[64][320];
    int n_warnings;
    char unsupported[64][320];
    int n_unsupported;
} state_t;

typedef struct {
    FILE *f;
    int console;
} report_t;

/* util.c */
void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t m);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
void die(const char *fmt, ...) __attribute__((noreturn));
void warn_msg(const char *fmt, ...);
int failf(char *err, size_t errlen, const char *fmt, ...);
void trim(char *s);
int read_file(const char *path, void *buf, size_t cap, size_t *out_len);
int read_text(const char *path, char *buf, size_t cap);
int write_file(const char *path, const void *buf, size_t len, mode_t mode);
int write_text(const char *path, const char *text, mode_t mode);
int mkdir_p(const char *path, mode_t mode);
int copy_file(const char *src, const char *dst, mode_t mode);
int path_join(char *dst, size_t cap, const char *a, const char *b);
int root_join(const paths_t *p, char *dst, size_t cap, const char *path);
const char *path_basename(const char *path);
int path_dirname(const char *path, char *out, size_t cap);
void fsync_parent_dir(const char *path);
int read_u64_file(const char *path, uint64_t *value);
int read_u32_file(const char *path, uint32_t *value);
int parse_u64(const char *s, uint64_t *value);
int parse_gib_to_mib(const char *s, uint64_t *mib, char *err, size_t errlen);
int parse_mib(const char *s, uint64_t *mib, char *err, size_t errlen);
void fmt_bytes(uint64_t bytes, char *out, size_t cap);
void fmt_size(uint64_t bytes, char *out, size_t cap);
void now_utc(char *out, size_t cap);
void now_stamp(char *out, size_t cap);
int random_bytes(void *buf, size_t len);
int uuid4(uint8_t out[16]);
void uuid_format_le(const uint8_t guid[16], char out[37]);
void uuid_format_be(const uint8_t guid[16], char out[37]);
void hex_encode(const uint8_t *buf, size_t len, char *out);
int hex_decode(const char *hex, uint8_t *out, size_t cap, size_t *len);
uint32_t crc32_buf(const uint8_t *data, size_t len);
int sha256_file(const char *path, uint8_t out[32]);
void sha256_buf(const void *data, size_t len, uint8_t out[32]);
int parse_kv_file(const char *path, int (*cb)(void *, const char *, const char *), void *arg);
int shell_copy_value(char *dst, size_t cap, const char *src);

/* report.c */
void rp_open(report_t *r, const char *path, int console);
void rp_close(report_t *r);
void rp(report_t *r, const char *fmt, ...);
void rp_section(report_t *r, const char *name);
void rp_kv(report_t *r, const char *key, const char *fmt, ...);
void rp_warn(state_t *s, report_t *r, const char *fmt, ...);
void rp_unsupported(state_t *s, report_t *r, const char *fmt, ...);
const char *state_verdict(const state_t *s);

/* gpt.c */
int gpt_read_fd(int fd, uint32_t sector, uint64_t total_sectors, gpt_t *gpt, char *err, size_t errlen);
int gpt_parse(const uint8_t *primary, size_t primary_len, const uint8_t *backup, size_t backup_len,
              uint32_t sector, uint64_t total_sectors, gpt_t *gpt, char *err, size_t errlen);
void gpt_free(gpt_t *gpt);
const gpt_entry_t *gpt_entry_by_index(const gpt_t *gpt, uint32_t index);
const gpt_entry_t *gpt_entry_by_name(const gpt_t *gpt, const char *name, int ci);
int gpt_entry_index(const gpt_t *gpt, uint32_t index);
int gpt_entry_count_used(const gpt_t *gpt);
int gpt_free_entry(const gpt_t *gpt, uint32_t *index);
int gpt_validate_identity(const gpt_t *gpt, const device_t *dev, char *err, size_t errlen);
int gpt_make_layout(const gpt_t *gpt, int userdata_index, uint64_t requested_bytes,
                    uint64_t min_userdata_bytes, layout_t *layout,
                    uint8_t **primary_out, size_t *primary_len_out,
                    uint8_t **backup_out, size_t *backup_len_out,
                    char *err, size_t errlen);
int gpt_check_basic_layout(const gpt_t *gpt, int userdata_index, char *detail, size_t detail_len,
                           char *err, size_t errlen);
int gpt_compare_entry_identity(const gpt_entry_t *a, const gpt_entry_t *b,
                               char *detail, size_t detail_len);

/* f2fs.c */
int f2fs_probe_fd(int fd, uint64_t partition_bytes, f2fs_info_t *info, char *err, size_t errlen);
int f2fs_probe_path(const char *path, uint64_t partition_bytes, f2fs_info_t *info, char *err, size_t errlen);
int f2fs_probe_mounted(const char *mountpoint, const char *block_path, uint64_t partition_bytes,
                       f2fs_info_t *info, char *err, size_t errlen);
uint64_t f2fs_request_blocks(uint64_t before, uint64_t limit);
uint64_t f2fs_expected_blocks(uint64_t before, uint64_t request);
int f2fs_shrink(int fs_fd, int block_fd, uint64_t limit_blocks, f2fs_info_t *info,
                uint64_t *request_out, uint64_t *expected_out, char *err, size_t errlen);

/* device.c */
int identity_load(const paths_t *p, identity_t *id, char *err, size_t errlen);
int identity_validate(const paths_t *p, identity_t *id, char *err, size_t errlen);
int device_discover(const paths_t *p, identity_t *id, device_t *dev, char *err, size_t errlen);
int mounts_scan(const paths_t *p, device_t *dev, mounts_t *mounts, char *err, size_t errlen);
void mounts_free(mounts_t *mounts);
int mounts_check_for_plan(const paths_t *p, device_t *dev, mounts_t *mounts, char *err, size_t errlen);
int mounts_check_unmounted(const paths_t *p, device_t *dev, mounts_t *mounts, char *err, size_t errlen);
int mounts_unmount_data(const paths_t *p, device_t *dev, mounts_t *mounts, char *err, size_t errlen);
int open_usage_scan(const paths_t *p, device_t *dev, char *detail, size_t detail_len);
int caps_check(const paths_t *p, const identity_t *id, device_t *dev, f2fs_info_t *fs,
               caps_t *caps, char *err, size_t errlen);
int dm_topology_check(const paths_t *p, const char *raw_node, const char *mapped_node,
                      char *err, size_t errlen);
int metadata_content_hash(const paths_t *p, device_t *dev, uint8_t out[32], char *err, size_t errlen);
int device_partition_geometry(const paths_t *p, const char *name, uint32_t sector,
                              int *index, uint64_t *start_lba, uint64_t *end_lba,
                              uint64_t *bytes, char *err, size_t errlen);

typedef struct {
    char size[64];
    char plan_dir[PATH_MAX];
    char mode[32];
    char plan[PATH_MAX];
    char confirm[512];
    int acknowledge_data_loss;
} request_t;

typedef struct {
    char plan_id[65];
    char tool[64];
    char version[32];
    char created[64];
    uint64_t size_bytes;
    char serial[192];
    char disk[PATH_MAX];
    uint64_t disk_bytes;
    uint32_t sector;
    int userdata_index;
    uint64_t userdata_start_lba;
    uint64_t userdata_end_lba;
    uint64_t userdata_bytes;
    uint64_t userdata_new_end_lba;
    uint64_t userdata_new_bytes;
    int metadata_index;
    uint64_t metadata_start_lba;
    uint64_t metadata_end_lba;
    uint64_t metadata_bytes;
    char metadata_sha256[65];
    char gpt_primary_sha256[65];
    char gpt_backup_sha256[65];
    uint64_t gpt_primary_len;
    uint64_t gpt_backup_len;
    uint64_t gpt_table_lba;
    uint64_t gpt_backup_lba;
    char gpt_disk_guid[37];
    uint64_t fs_blocks;
    char fs_uuid[40];
    char fs_sb_sha256[65];
    uint32_t fs_feature;
    uint64_t limit_blocks;
    uint64_t expected_blocks;
    int unixnu_index;
    uint64_t unixnu_start_lba;
    uint64_t unixnu_end_lba;
    uint64_t unixnu_bytes;
    char unixnu_type_guid[37];
    char unixnu_unique_guid[37];
    char userdata_type_guid[37];
    char userdata_unique_guid[37];
    uint64_t userdata_attrs;
    uint64_t tail_gap_bytes;
    char confirm[512];
} plan_info_t;

/* state.c */
int state_collect(const paths_t *p, state_t *s, int strict, char *err, size_t errlen);
void state_free(state_t *s);
int inspect_run(const paths_t *p, const char *out_path, char *err, size_t errlen);

/* plan.c */
int read_request_file(const char *path, request_t *req, char *err, size_t errlen);
int plan_run(const paths_t *p, const char *request_path, char *err, size_t errlen);
int plan_load(const char *plan_dir, plan_info_t *plan, char *err, size_t errlen);
int plan_check_stage(const char *plan_dir, const char *expect, char *stage, size_t stage_cap,
                     char *err, size_t errlen);
int plan_write_stage(const char *plan_dir, const char *stage, char *err, size_t errlen);
int plan_bundle_verify(const char *plan_dir, char *err, size_t errlen);
int plan_match_state(const plan_info_t *plan, const state_t *s, char *err, size_t errlen);
int plan_identity_match(const plan_info_t *plan, const state_t *s, char *err, size_t errlen);
int plan_read_file(const char *plan_dir, const char *name, uint8_t **data, size_t *len,
                   char *err, size_t errlen);

/* apply.c */
int apply_run(const paths_t *p, const char *plan_dir, const char *confirm,
              int acknowledge_data_loss, char *err, size_t errlen);
int verify_run(const paths_t *p, const char *plan_dir, char *err, size_t errlen);
int recover_run(const paths_t *p, const char *plan_dir, char *err, size_t errlen);

/* util helper for plan/apply */
int sha256_region(int fd, uint64_t length, uint8_t out[32]);
int write_atomic_text(const char *path, const char *text, mode_t mode);

#endif
