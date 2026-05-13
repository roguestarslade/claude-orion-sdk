#include "orionctl.h"
#include "json_out.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int (*verb_fn)(octl_ctx_t *);

typedef struct {
    const char *name;
    verb_fn     fn;
    const char *help;
} verb_t;

static int cmd_help(octl_ctx_t *ctx);

static const verb_t VERBS[] = {
    { "status",  cmd_status,  "one-shot health snapshot"                  },
    { "telem",   cmd_telem,   "telemetry [--watch --n N]"                 },
    { "cameras", cmd_cameras, "list all 4 camera slots"                   },
    { "camera",  cmd_camera,  "camera get|switch|fov|zoom|focus [--idx N]" },
    { "help",    cmd_help,    "print this help"                           },
    { NULL, NULL, NULL }
};

int octl_resolve_ip(octl_ctx_t *ctx)
{
    if (ctx->ip && *ctx->ip) return 0;
    const char *env = getenv("ORION_GIMBAL_IP");
    if (env && *env) { ctx->ip = env; return 0; }
    if (ctx->discover) { ctx->ip = "255.255.255.255"; return 0; }
    return -1;
}

static int parse_int(const char *s, int *out)
{
    if (!s) return -1;
    char *end;
    long v = strtol(s, &end, 10);
    if (*end != '\0' || end == s) return -1;
    *out = (int)v;
    return 0;
}

static int parse_dbl(const char *s, double *out)
{
    if (!s) return -1;
    char *end;
    double v = strtod(s, &end);
    if (*end != '\0' || end == s) return -1;
    *out = v;
    return 0;
}

int octl_parse_global_flags(int argc, char **argv, octl_ctx_t *ctx)
{
    int i = 1;
    while (i < argc) {
        const char *a = argv[i];
        if (strcmp(a, "--ip") == 0 && i + 1 < argc) {
            ctx->ip = argv[++i];
        } else if (strcmp(a, "--timeout") == 0 && i + 1 < argc) {
            if (parse_int(argv[++i], &ctx->timeout_ms) != 0) return -1;
        } else if (strcmp(a, "--discover") == 0) {
            ctx->discover = 1;
        } else if (strcmp(a, "--allow-motion") == 0) {
            ctx->allow_motion = 1;
        } else if (strcmp(a, "--allow-laser") == 0) {
            ctx->allow_laser = 1;
        } else if (strcmp(a, "--i-know") == 0) {
            ctx->iknow = 1;
        } else if (strcmp(a, "--idx") == 0 && i + 1 < argc) {
            if (parse_int(argv[++i], &ctx->idx) != 0) return -1;
            ctx->idx_set = 1;
        } else if (strcmp(a, "--zoom") == 0 && i + 1 < argc) {
            if (parse_dbl(argv[++i], &ctx->zoom) != 0) return -1;
            ctx->zoom_set = 1;
        } else if (strcmp(a, "--watch") == 0) {
            ctx->watch = 1;
        } else if (strcmp(a, "--n") == 0 && i + 1 < argc) {
            if (parse_int(argv[++i], &ctx->watch_n) != 0) return -1;
        } else if (strcmp(a, "--deg") == 0 && i + 1 < argc) {
            if (parse_dbl(argv[++i], &ctx->deg_value) != 0) return -1;
            ctx->deg_set = 1;
        } else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            if (ctx->npos < OCTL_MAX_POS) ctx->pos[ctx->npos++] = "help";
        } else if (strncmp(a, "--", 2) == 0) {
            return -1;
        } else {
            if (ctx->npos < OCTL_MAX_POS) ctx->pos[ctx->npos++] = a;
        }
        i++;
    }
    return 0;
}

static int cmd_help(octl_ctx_t *ctx)
{
    (void)ctx;
    jout_t j;
    jout_init(&j, stdout);
    jout_obj_open(&j);
    jout_kv_str(&j, "tool", "orionctl");
    jout_kv_str(&j, "version", "0.2.0-phase2");

    jout_key(&j, "global_flags");
    jout_arr_open(&j);
    jout_str(&j, "--ip <addr>");
    jout_str(&j, "--timeout <ms>");
    jout_str(&j, "--discover");
    jout_str(&j, "--allow-motion");
    jout_str(&j, "--allow-laser");
    jout_str(&j, "--i-know");
    jout_str(&j, "--idx <N>");
    jout_str(&j, "--zoom <N>");
    jout_str(&j, "--watch");
    jout_str(&j, "--n <N>");
    jout_str(&j, "--deg <N>");
    jout_arr_close(&j);

    jout_key(&j, "verbs");
    jout_arr_open(&j);
    for (const verb_t *v = VERBS; v->name; v++) {
        jout_obj_open(&j);
        jout_kv_str(&j, "name", v->name);
        jout_kv_str(&j, "help", v->help);
        jout_obj_close(&j);
    }
    jout_arr_close(&j);

    jout_obj_close(&j);
    jout_done(&j);
    return OCTL_OK;
}

int main(int argc, char **argv)
{
    octl_ctx_t ctx = {0};
    ctx.timeout_ms = OCTL_DEFAULT_TIMEOUT_MS;
    ctx.idx = -1;
    ctx.argc = argc;
    ctx.argv = argv;

    if (octl_parse_global_flags(argc, argv, &ctx) != 0) {
        jout_err(stderr, OCTL_USAGE, "bad_flag", "unknown or malformed flag");
        return OCTL_USAGE;
    }
    if (ctx.npos == 0) {
        return cmd_help(&ctx);
    }

    const char *verb = ctx.pos[0];
    for (const verb_t *v = VERBS; v->name; v++) {
        if (strcmp(verb, v->name) == 0) {
            return v->fn(&ctx);
        }
    }

    jout_err(stderr, OCTL_USAGE, "unknown_verb", "unknown verb: %s", verb);
    return OCTL_USAGE;
}
