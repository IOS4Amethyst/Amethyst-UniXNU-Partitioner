/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "common.h"
#include <ctype.h>
#include <dirent.h>
#include <sys/random.h>

static uint32_t crc_table[256];
static int crc_ready;

static void crc_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++)
            c = (c & 1) ? (0xedb88320U ^ (c >> 1)) : (c >> 1);
        crc_table[i] = c;
    }
    crc_ready = 1;
}

void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p)
        die("out of memory");
    return p;
}

void *xcalloc(size_t n, size_t m)
{
    void *p = calloc(n ? n : 1, m ? m : 1);
    if (!p)
        die("out of memory");
    return p;
}

void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q)
        die("out of memory");
    return q;
}

char *xstrdup(const char *s)
{
    char *p = strdup(s ? s : "");
    if (!p)
        die("out of memory");
    return p;
}

void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "FATAL: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(2);
}

void warn_msg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "WARNING: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

int failf(char *err, size_t errlen, const char *fmt, ...)
{
    va_list ap;
    if (err && errlen) {
        va_start(ap, fmt);
        vsnprintf(err, errlen, fmt, ap);
        va_end(ap);
    }
    return -1;
}

void trim(char *s)
{
    if (!s)
        return;
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1]))
        s[--n] = '\0';
    size_t i = 0;
    while (s[i] && isspace((unsigned char)s[i]))
        i++;
    if (i)
        memmove(s, s + i, strlen(s + i) + 1);
}

int read_file(const char *path, void *buf, size_t cap, size_t *out_len)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    size_t n = 0;
    while (n < cap) {
        ssize_t got = read(fd, (uint8_t *)buf + n, cap - n);
        if (got < 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            return -1;
        }
        if (got == 0)
            break;
        n += (size_t)got;
    }
    close(fd);
    if (out_len)
        *out_len = n;
    return 0;
}

int read_text(const char *path, char *buf, size_t cap)
{
    if (!cap)
        return -1;
    size_t n = 0;
    if (read_file(path, buf, cap - 1, &n) < 0)
        return -1;
    buf[n] = '\0';
    return 0;
}

int write_file(const char *path, const void *buf, size_t len, mode_t mode)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (fd < 0)
        return -1;
    const uint8_t *p = buf;
    size_t done = 0;
    while (done < len) {
        ssize_t n = write(fd, p + done, len - done);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            return -1;
        }
        done += (size_t)n;
    }
    if (fsync(fd) < 0) {
        close(fd);
        return -1;
    }
    return close(fd);
}

int write_text(const char *path, const char *text, mode_t mode)
{
    return write_file(path, text, strlen(text), mode);
}

int mkdir_p(const char *path, mode_t mode)
{
    if (!path || !*path)
        return -1;
    char tmp[PATH_MAX];
    if (snprintf(tmp, sizeof(tmp), "%s", path) >= (int)sizeof(tmp))
        return failf(NULL, 0, "path too long: %s", path);
    size_t len = strlen(tmp);
    while (len > 1 && tmp[len - 1] == '/')
        tmp[--len] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(tmp, mode) < 0 && errno != EEXIST)
            return -1;
        *p = '/';
    }
    if (mkdir(tmp, mode) < 0 && errno != EEXIST)
        return -1;
    struct stat st;
    if (stat(tmp, &st) < 0 || !S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
        return -1;
    }
    return 0;
}

int copy_file(const char *src, const char *dst, mode_t mode)
{
    int in = open(src, O_RDONLY | O_CLOEXEC);
    if (in < 0)
        return -1;
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (out < 0) {
        close(in);
        return -1;
    }
    uint8_t buf[65536];
    int rc = 0;
    for (;;) {
        ssize_t n = read(in, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            rc = -1;
            break;
        }
        if (!n)
            break;
        ssize_t done = 0;
        while (done < n) {
            ssize_t w = write(out, buf + done, (size_t)(n - done));
            if (w < 0) {
                if (errno == EINTR)
                    continue;
                rc = -1;
                break;
            }
            done += w;
        }
        if (rc)
            break;
    }
    if (!rc && fsync(out) < 0)
        rc = -1;
    close(in);
    if (close(out) < 0)
        rc = -1;
    return rc;
}

/* Callers legitimately pass the same buffer as dst and a (or b), so the
 * concatenation must never make snprintf read and write overlapping memory.
 * Build the result separately and copy it back. */
static int join_copy(char *dst, size_t cap, const char *a, const char *b, const char *sep)
{
    size_t alen = strlen(a), blen = strlen(b), slen = strlen(sep);
    size_t need = alen + slen + blen + 1;
    if (need > cap)
        return -1;
    char *tmp = xmalloc(need);
    memcpy(tmp, a, alen);
    memcpy(tmp + alen, sep, slen);
    memcpy(tmp + alen + slen, b, blen);
    tmp[need - 1] = '\0';
    memcpy(dst, tmp, need);
    free(tmp);
    return 0;
}

int path_join(char *dst, size_t cap, const char *a, const char *b)
{
    if (!a)
        a = "";
    if (!b)
        b = "";
    const char *sep = (*a && a[strlen(a) - 1] != '/') ? "/" : "";
    return join_copy(dst, cap, a, b, sep);
}

int root_join(const paths_t *p, char *dst, size_t cap, const char *path)
{
    if (path[0] != '/')
        return failf(NULL, 0, "internal path is not absolute: %s", path);
    if (!p || !p->root[0])
        return join_copy(dst, cap, path, "", "");
    return join_copy(dst, cap, p->root, path, "");
}

const char *path_basename(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

int path_dirname(const char *path, char *out, size_t cap)
{
    const char *s = strrchr(path, '/');
    if (!s) {
        if (cap < 2)
            return -1;
        strcpy(out, ".");
        return 0;
    }
    size_t n = s == path ? 1 : (size_t)(s - path);
    if (n >= cap)
        return -1;
    memcpy(out, path, n);
    out[n] = '\0';
    return 0;
}

/* Best-effort durability for a newly renamed/created file. Some recovery
 * storage (FAT/exFAT) rejects directory fsync; the file itself is already
 * fsynced by the caller, so ignore an unsupported directory sync. */
void fsync_parent_dir(const char *path)
{
    char dir[PATH_MAX];
    if (path_dirname(path, dir, sizeof(dir)) < 0)
        return;
    int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd >= 0) {
        (void)fsync(fd);
        close(fd);
    }
}

int read_u64_file(const char *path, uint64_t *value)
{
    char buf[128];
    if (read_text(path, buf, sizeof(buf)) < 0)
        return -1;
    trim(buf);
    return parse_u64(buf, value);
}

int read_u32_file(const char *path, uint32_t *value)
{
    uint64_t v;
    if (read_u64_file(path, &v) < 0 || v > UINT32_MAX)
        return -1;
    *value = (uint32_t)v;
    return 0;
}

int parse_u64(const char *s, uint64_t *value)
{
    if (!s || !*s)
        return -1;
    errno = 0;
    char *end = NULL;
    unsigned long long v = strtoull(s, &end, 10);
    if (errno || !end || *end || end == s)
        return -1;
    *value = (uint64_t)v;
    return 0;
}

static int parse_frac_mib(const char *frac, uint64_t *out)
{
    size_t n = strlen(frac);
    if (!n || n > 12)
        return -1;
    uint64_t v = 0;
    for (size_t i = 0; i < n; i++) {
        if (!isdigit((unsigned char)frac[i]))
            return -1;
        v = v * 10 + (uint64_t)(frac[i] - '0');
    }
    uint64_t den = 1;
    for (size_t i = 0; i < n; i++)
        den *= 10;
    uint64_t scaled = v * 1024;
    if (scaled % den)
        return -1;
    *out = scaled / den;
    return 0;
}

int parse_gib_to_mib(const char *s, uint64_t *mib, char *err, size_t errlen)
{
    if (!s || !*s)
        return failf(err, errlen, "size is empty");
    const char *dot = strchr(s, '.');
    char whole[64];
    size_t wlen = dot ? (size_t)(dot - s) : strlen(s);
    if (!wlen || wlen >= sizeof(whole))
        return failf(err, errlen, "invalid GiB size");
    for (size_t i = 0; i < wlen; i++) {
        if (!isdigit((unsigned char)s[i]))
            return failf(err, errlen, "invalid GiB size");
        whole[i] = s[i];
    }
    whole[wlen] = '\0';
    uint64_t gib;
    if (parse_u64(whole, &gib) < 0 || gib > (UINT64_MAX / 1024) - 1)
        return failf(err, errlen, "invalid GiB size");
    uint64_t frac_mib = 0;
    if (dot && parse_frac_mib(dot + 1, &frac_mib) < 0)
        return failf(err, errlen, "GiB size must be a positive value in whole MiB increments");
    uint64_t total = gib * 1024 + frac_mib;
    if (!total || total > (UINT64_MAX / MIB))
        return failf(err, errlen, "GiB size is out of range");
    *mib = total;
    return 0;
}

int parse_mib(const char *s, uint64_t *mib, char *err, size_t errlen)
{
    uint64_t v;
    if (parse_u64(s, &v) < 0 || !v || v > (UINT64_MAX / MIB))
        return failf(err, errlen, "MiB size must be a positive integer");
    *mib = v;
    return 0;
}

void fmt_bytes(uint64_t bytes, char *out, size_t cap)
{
    double gib = (double)bytes / (double)GIB;
    double mib = (double)bytes / (double)MIB;
    if (bytes >= GIB)
        snprintf(out, cap, "%.6f GiB (%" PRIu64 " bytes)", gib, bytes);
    else
        snprintf(out, cap, "%.3f MiB (%" PRIu64 " bytes)", mib, bytes);
}

void fmt_size(uint64_t bytes, char *out, size_t cap)
{
    if (bytes >= GIB)
        snprintf(out, cap, "%.3f GiB", (double)bytes / (double)GIB);
    else
        snprintf(out, cap, "%.3f MiB", (double)bytes / (double)MIB);
}

void now_utc(char *out, size_t cap)
{
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(out, cap, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

void now_stamp(char *out, size_t cap)
{
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(out, cap, "%Y%m%dT%H%M%SZ", &tm);
}

int random_bytes(void *buf, size_t len)
{
    uint8_t *p = buf;
    size_t done = 0;
    while (done < len) {
        ssize_t n = getrandom(p + done, len - done, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        done += (size_t)n;
    }
    if (done == len)
        return 0;
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    done = 0;
    while (done < len) {
        ssize_t n = read(fd, p + done, len - done);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            return -1;
        }
        if (!n) {
            close(fd);
            return -1;
        }
        done += (size_t)n;
    }
    close(fd);
    return 0;
}

int uuid4(uint8_t out[16])
{
    if (random_bytes(out, 16) < 0)
        return -1;
    out[6] = (uint8_t)((out[6] & 0x0f) | 0x40);
    out[8] = (uint8_t)((out[8] & 0x3f) | 0x80);
    return 0;
}

void uuid_format_le(const uint8_t g[16], char out[37])
{
    snprintf(out, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             g[3], g[2], g[1], g[0], g[5], g[4], g[7], g[6], g[8], g[9],
             g[10], g[11], g[12], g[13], g[14], g[15]);
}

void uuid_format_be(const uint8_t g[16], char out[37])
{
    snprintf(out, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             g[0], g[1], g[2], g[3], g[4], g[5], g[6], g[7], g[8], g[9],
             g[10], g[11], g[12], g[13], g[14], g[15]);
}

void hex_encode(const uint8_t *buf, size_t len, char *out)
{
    static const char h[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i * 2] = h[buf[i] >> 4];
        out[i * 2 + 1] = h[buf[i] & 15];
    }
    out[len * 2] = '\0';
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

int hex_decode(const char *hex, uint8_t *out, size_t cap, size_t *len)
{
    size_t n = strlen(hex);
    if (n % 2 || n / 2 > cap)
        return -1;
    for (size_t i = 0; i < n / 2; i++) {
        int a = hexval(hex[i * 2]), b = hexval(hex[i * 2 + 1]);
        if (a < 0 || b < 0)
            return -1;
        out[i] = (uint8_t)((a << 4) | b);
    }
    if (len)
        *len = n / 2;
    return 0;
}

uint32_t crc32_buf(const uint8_t *data, size_t len)
{
    if (!crc_ready)
        crc_init();
    uint32_t c = 0xffffffffU;
    for (size_t i = 0; i < len; i++)
        c = crc_table[(c ^ data[i]) & 0xff] ^ (c >> 8);
    return c ^ 0xffffffffU;
}

/* SHA-256, compact public-domain style implementation. */
typedef struct {
    uint32_t h[8];
    uint64_t len;
    uint8_t buf[64];
    size_t used;
} sha256_ctx;

static uint32_t rotr(uint32_t x, unsigned n)
{
    return (x >> n) | (x << (32 - n));
}

static void sha256_block(sha256_ctx *c, const uint8_t *p)
{
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
    };
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3];
    uint32_t e = c->h[4], f = c->h[5], g = c->h[6], h = c->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + s1 + ch + k[i] + w[i];
        uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = s0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
    c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

static void sha256_init(sha256_ctx *c)
{
    static const uint32_t init[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };
    memcpy(c->h, init, sizeof(init));
    c->len = 0;
    c->used = 0;
}

static void sha256_update(sha256_ctx *c, const void *data, size_t len)
{
    const uint8_t *p = data;
    c->len += len;
    while (len) {
        size_t n = MIN(len, sizeof(c->buf) - c->used);
        memcpy(c->buf + c->used, p, n);
        c->used += n;
        p += n;
        len -= n;
        if (c->used == sizeof(c->buf)) {
            sha256_block(c, c->buf);
            c->used = 0;
        }
    }
}

static void sha256_final(sha256_ctx *c, uint8_t out[32])
{
    uint64_t bits = c->len * 8;
    uint8_t one = 0x80;
    sha256_update(c, &one, 1);
    uint8_t zero = 0;
    while (c->used != 56)
        sha256_update(c, &zero, 1);
    uint8_t lenbuf[8];
    for (int i = 0; i < 8; i++)
        lenbuf[i] = (uint8_t)(bits >> (56 - i * 8));
    sha256_update(c, lenbuf, 8);
    for (int i = 0; i < 8; i++) {
        out[i * 4] = (uint8_t)(c->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(c->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(c->h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)c->h[i];
    }
}

void sha256_buf(const void *data, size_t len, uint8_t out[32])
{
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, data, len);
    sha256_final(&c, out);
}

int sha256_file(const char *path, uint8_t out[32])
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    sha256_ctx c;
    sha256_init(&c);
    uint8_t buf[65536];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            return -1;
        }
        if (!n)
            break;
        sha256_update(&c, buf, (size_t)n);
    }
    close(fd);
    sha256_final(&c, out);
    return 0;
}

int sha256_region(int fd, uint64_t length, uint8_t out[32])
{
    sha256_ctx c;
    sha256_init(&c);
    uint8_t buf[65536];
    uint64_t done = 0;
    while (done < length) {
        size_t want = (size_t)MIN((uint64_t)sizeof(buf), length - done);
        ssize_t n = pread(fd, buf, want, (off_t)done);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (!n)
            return -1;
        sha256_update(&c, buf, (size_t)n);
        done += (uint64_t)n;
    }
    sha256_final(&c, out);
    return 0;
}

int write_atomic_text(const char *path, const char *text, mode_t mode)
{
    char tmp[PATH_MAX];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp))
        return failf(NULL, 0, "path too long");
    if (write_text(tmp, text, mode) < 0)
        return -1;
    if (rename(tmp, path) < 0) {
        unlink(tmp);
        return -1;
    }
    fsync_parent_dir(path);
    return 0;
}

int parse_kv_file(const char *path, int (*cb)(void *, const char *, const char *), void *arg)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    char line[4096];
    int rc = 0;
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p && isspace((unsigned char)*p))
            p++;
        if (!*p || *p == '#')
            continue;
        char *eq = strchr(p, '=');
        if (!eq) {
            rc = -1;
            break;
        }
        *eq = '\0';
        char *key = p;
        char *val = eq + 1;
        trim(key);
        trim(val);
        size_t n = strlen(val);
        if (n >= 2 && ((val[0] == '"' && val[n - 1] == '"') || (val[0] == '\'' && val[n - 1] == '\''))) {
            val[n - 1] = '\0';
            val++;
        }
        if (cb(arg, key, val) < 0) {
            rc = -1;
            break;
        }
    }
    fclose(f);
    return rc;
}

int shell_copy_value(char *dst, size_t cap, const char *src)
{
    if (!cap)
        return -1;
    size_t n = 0;
    for (const char *p = src ? src : ""; *p && n + 1 < cap; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '\n' || c == '\r' || c == '\0')
            continue;
        dst[n++] = (char)c;
    }
    dst[n] = '\0';
    trim(dst);
    return 0;
}
