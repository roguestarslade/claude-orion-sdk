#include "orionctl.h"
#include "cache.h"
#include "json_out.h"

#include <stdio.h>
#include <string.h>

int cmd_cache(octl_ctx_t *ctx)
{
    if (ctx->npos < 2) {
        jout_err(stderr, OCTL_USAGE, "missing_subverb", "cache requires subverb: clear");
        return OCTL_USAGE;
    }
    const char *sub = ctx->pos[1];
    if (strcmp(sub, "clear") != 0) {
        jout_err(stderr, OCTL_USAGE, "unknown_subverb", "unknown cache subverb: %s", sub);
        return OCTL_USAGE;
    }
    int rc = cache_clear(ctx->ip);
    jout_t j; jout_init(&j, stdout);
    jout_obj_open(&j);
    jout_kv_str (&j, "action",      "clear");
    jout_kv_str (&j, "scope",       ctx->ip ? "single_ip" : "all_ips");
    if (ctx->ip) jout_kv_str(&j, "ip", ctx->ip);
    jout_kv_int (&j, "files_removed", rc);
    jout_obj_close(&j);
    jout_done(&j);
    return OCTL_OK;
}
