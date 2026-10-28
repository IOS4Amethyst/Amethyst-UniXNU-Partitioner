/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "common.h"

void rp_open(report_t *r, const char *path, int console)
{
    memset(r, 0, sizeof(*r));
    r->console = console;
    if (path) {
        r->f = fopen(path, "w");
        if (!r->f)
            die("cannot create %s: %s", path, strerror(errno));
    }
}

void rp_close(report_t *r)
{
    if (r->f) {
        fflush(r->f);
        fclose(r->f);
    }
    r->f = NULL;
}

void rp(report_t *r, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (r->f) {
        va_list cp;
        va_copy(cp, ap);
        vfprintf(r->f, fmt, cp);
        va_end(cp);
    }
    if (r->console) {
        vfprintf(stdout, fmt, ap);
        fflush(stdout);
    }
    va_end(ap);
}

void rp_section(report_t *r, const char *name)
{
    rp(r, "\n== %s ==\n", name);
}

void rp_kv(report_t *r, const char *key, const char *fmt, ...)
{
    rp(r, "%-28s: ", key);
    va_list ap;
    va_start(ap, fmt);
    if (r->f) {
        va_list cp;
        va_copy(cp, ap);
        vfprintf(r->f, fmt, cp);
        va_end(cp);
        fputc('\n', r->f);
    }
    if (r->console) {
        vfprintf(stdout, fmt, ap);
        fputc('\n', stdout);
        fflush(stdout);
    }
    va_end(ap);
}

static void add_bounded(char (*list)[320], int *count, const char *text)
{
    if (*count >= 64)
        return;
    snprintf(list[*count], 320, "%s", text);
    (*count)++;
}

void rp_warn(state_t *s, report_t *r, const char *fmt, ...)
{
    char msg[320];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    add_bounded(s->warnings, &s->n_warnings, msg);
    rp(r, "WARNING: %s\n", msg);
}

void rp_unsupported(state_t *s, report_t *r, const char *fmt, ...)
{
    char msg[320];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    add_bounded(s->unsupported, &s->n_unsupported, msg);
    rp(r, "UNSUPPORTED: %s\n", msg);
}

const char *state_verdict(const state_t *s)
{
    if (s->n_unsupported)
        return "UNSUPPORTED";
    if (s->n_warnings)
        return "SUPPORTED WITH WARNINGS";
    return "SUPPORTED";
}
