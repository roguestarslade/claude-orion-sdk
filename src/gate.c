#include "orionctl.h"

#include <stdlib.h>
#include <string.h>

int gate_motion_allowed(const octl_ctx_t *ctx)
{
    if (ctx->allow_motion) return 1;
    const char *env = getenv("ORION_ALLOW_MOTION");
    return env && strcmp(env, "1") == 0;
}

int gate_laser_allowed(const octl_ctx_t *ctx)
{
    return ctx->allow_laser ? 1 : 0;
}

int gate_reset_allowed(const octl_ctx_t *ctx)
{
    return gate_motion_allowed(ctx) && ctx->iknow;
}
