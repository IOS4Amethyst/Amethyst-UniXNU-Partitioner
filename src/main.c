/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "common.h"

static void usage(FILE *f)
{
    fprintf(f,
        TOOL_NAME " " TOOL_VERSION "\n"
        "Recovery-side GPT/F2FS partition planner for Amethyst (SM7635).\n"
        "\n"
        "Usage:\n"
        "  %s inspect  [--out PATH]\n"
        "  %s plan     --request FILE\n"
        "  %s apply    --request FILE\n"
        "  %s apply    --plan DIR --confirm PHRASE --i-understand-data-loss\n"
        "  %s verify   --plan DIR\n"
        "  %s recover  --plan DIR\n"
        "  %s selftest\n"
        "\n"
        "Global options:\n"
        "  --root DIR        Testing only: treat DIR as the filesystem root.\n"
        "  --base DIR        Report/plan base (default /sdcard/Amethyst-UniXNU-Partitioner).\n"
        "  --identity FILE   Key=value identity file written by update-binary.\n"
        "\n"
        "inspect is read-only. plan writes files only. apply performs the destructive\n"
        "resize only with a device-matched bundle, the exact confirmation phrase and\n"
        "--i-understand-data-loss. There is no retry or rollback.\n",
        TOOL_NAME, TOOL_NAME, TOOL_NAME, TOOL_NAME, TOOL_NAME, TOOL_NAME, TOOL_NAME);
}

int selftest_run(void);

static int path_os(const paths_t *p, const char *logical, char *out, size_t cap, char *err,
                   size_t errlen)
{
    if (root_join(p, out, cap, logical) < 0)
        return failf(err, errlen, "path is too long: %s", logical);
    return 0;
}

int main(int argc, char **argv)
{
    paths_t p;
    memset(&p, 0, sizeof(p));
#ifdef UNIXNU_TESTING
    p.testing = 1;
#endif
    snprintf(p.base, sizeof(p.base), "/sdcard/Amethyst-UniXNU-Partitioner");
    const char *root = NULL, *identity = NULL, *request = NULL, *plan = NULL;
    const char *confirm = NULL, *out = NULL;
    int ack = 0;
    const char *command = NULL;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            usage(stdout);
            return 0;
        } else if (!strcmp(a, "--version")) {
            printf("%s %s\n", TOOL_NAME, TOOL_VERSION);
            return 0;
        } else if (!strcmp(a, "--root")) {
            if (++i >= argc) {
                fprintf(stderr, "STOP: --root needs a directory\n");
                return 2;
            }
            root = argv[i];
        } else if (!strcmp(a, "--base")) {
            if (++i >= argc) {
                fprintf(stderr, "STOP: --base needs a directory\n");
                return 2;
            }
            snprintf(p.base, sizeof(p.base), "%s", argv[i]);
        } else if (!strcmp(a, "--identity")) {
            if (++i >= argc) {
                fprintf(stderr, "STOP: --identity needs a file\n");
                return 2;
            }
            identity = argv[i];
        } else if (!strcmp(a, "--request")) {
            if (++i >= argc) {
                fprintf(stderr, "STOP: --request needs a file\n");
                return 2;
            }
            request = argv[i];
        } else if (!strcmp(a, "--plan")) {
            if (++i >= argc) {
                fprintf(stderr, "STOP: --plan needs a directory\n");
                return 2;
            }
            plan = argv[i];
        } else if (!strcmp(a, "--confirm")) {
            if (++i >= argc) {
                fprintf(stderr, "STOP: --confirm needs a phrase\n");
                return 2;
            }
            confirm = argv[i];
        } else if (!strcmp(a, "--out")) {
            if (++i >= argc) {
                fprintf(stderr, "STOP: --out needs a file\n");
                return 2;
            }
            out = argv[i];
        } else if (!strcmp(a, "--i-understand-data-loss")) {
            ack = 1;
        } else if (a[0] == '-') {
            fprintf(stderr, "STOP: unknown option %s\n", a);
            return 2;
        } else if (!command) {
            command = a;
        } else {
            fprintf(stderr, "STOP: unexpected argument %s\n", a);
            return 2;
        }
    }
    if (!command) {
        usage(stderr);
        return 2;
    }
    if (root)
        snprintf(p.root, sizeof(p.root), "%s", root);
    if (identity) {
        if (root_join(&p, p.identity, sizeof(p.identity), identity) < 0) {
            fprintf(stderr, "STOP: identity path is too long\n");
            return 2;
        }
    }

    char err[768] = "";
    if (!strcmp(command, "selftest"))
        return selftest_run();

    if (!strcmp(command, "inspect")) {
        char out_os[PATH_MAX], logical[PATH_MAX];
        if (out) {
            if (path_os(&p, out, out_os, sizeof(out_os), err, sizeof(err)) < 0) {
                fprintf(stderr, "STOP: %s\n", err);
                return 1;
            }
        } else {
            char stamp[64], name[96];
            now_stamp(stamp, sizeof(stamp));
            snprintf(name, sizeof(name), "inspect-%s.txt", stamp);
            if (path_join(logical, sizeof(logical), p.base, name) < 0) {
                fprintf(stderr, "STOP: report path is too long\n");
                return 1;
            }
            if (path_os(&p, logical, out_os, sizeof(out_os), err, sizeof(err)) < 0) {
                fprintf(stderr, "STOP: %s\n", err);
                return 1;
            }
        }
        if (inspect_run(&p, out_os, err, sizeof(err)) < 0) {
            fprintf(stderr, "STOP: %s\n", err);
            return 1;
        }
        return 0;
    }

    if (!strcmp(command, "plan")) {
        char req_os[PATH_MAX], logical[PATH_MAX];
        if (request) {
            if (path_os(&p, request, req_os, sizeof(req_os), err, sizeof(err)) < 0) {
                fprintf(stderr, "STOP: %s\n", err);
                return 1;
            }
        } else {
            if (path_join(logical, sizeof(logical), p.base, "request.conf") < 0) {
                fprintf(stderr, "STOP: request path is too long\n");
                return 1;
            }
            if (path_os(&p, logical, req_os, sizeof(req_os), err, sizeof(err)) < 0) {
                fprintf(stderr, "STOP: %s\n", err);
                return 1;
            }
        }
        if (plan_run(&p, req_os, err, sizeof(err)) < 0) {
            fprintf(stderr, "\nSTOP: %s\n", err);
            return 1;
        }
        return 0;
    }

    if (!strcmp(command, "apply")) {
        request_t req;
        memset(&req, 0, sizeof(req));
        if (request) {
            char req_os[PATH_MAX];
            if (path_os(&p, request, req_os, sizeof(req_os), err, sizeof(err)) < 0) {
                fprintf(stderr, "STOP: %s\n", err);
                return 1;
            }
            if (read_request_file(req_os, &req, err, sizeof(err)) < 0) {
                fprintf(stderr, "STOP: %s\n", err);
                return 1;
            }
            if (req.mode[0] && strcmp(req.mode, "apply")) {
                fprintf(stderr, "STOP: request mode is '%s'; the Apply ZIP requires mode = apply\n",
                        req.mode);
                return 1;
            }
        }
        const char *plan_arg = plan ? plan : (req.plan[0] ? req.plan : NULL);
        const char *confirm_arg = confirm ? confirm : (req.confirm[0] ? req.confirm : NULL);
        int ack_arg = ack || req.acknowledge_data_loss;
        if (!plan_arg) {
            fprintf(stderr, "STOP: apply requires --plan DIR (or plan = ... in the request file)\n");
            return 1;
        }
        char plan_os[PATH_MAX];
        if (path_os(&p, plan_arg, plan_os, sizeof(plan_os), err, sizeof(err)) < 0) {
            fprintf(stderr, "STOP: %s\n", err);
            return 1;
        }
        if (apply_run(&p, plan_os, confirm_arg, ack_arg, err, sizeof(err)) < 0) {
            fprintf(stderr, "\nSTOP: %s\n", err);
            return 1;
        }
        return 0;
    }

    if (!strcmp(command, "verify")) {
        if (!plan) {
            fprintf(stderr, "STOP: verify requires --plan DIR\n");
            return 1;
        }
        char plan_os[PATH_MAX];
        if (path_os(&p, plan, plan_os, sizeof(plan_os), err, sizeof(err)) < 0) {
            fprintf(stderr, "STOP: %s\n", err);
            return 1;
        }
        if (verify_run(&p, plan_os, err, sizeof(err)) < 0) {
            fprintf(stderr, "\nSTOP: %s\n", err);
            return 1;
        }
        return 0;
    }

    if (!strcmp(command, "recover")) {
        if (!plan) {
            fprintf(stderr, "STOP: recover requires --plan DIR\n");
            return 1;
        }
        char plan_os[PATH_MAX];
        if (path_os(&p, plan, plan_os, sizeof(plan_os), err, sizeof(err)) < 0) {
            fprintf(stderr, "STOP: %s\n", err);
            return 1;
        }
        int rc = recover_run(&p, plan_os, err, sizeof(err));
        if (rc < 0)
            fprintf(stderr, "STOP: %s\n", err);
        return rc < 0 ? 1 : rc;
    }

    fprintf(stderr, "STOP: unknown command %s\n", command);
    usage(stderr);
    return 2;
}
