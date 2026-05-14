#include "orionctl.h"
#include "conn.h"
#include "json_out.h"

#include <stdio.h>

#include "OrionPublicPacket.h"
#include "OrionPublicPacketShim.h"

static const char *range_src_name(int r)
{
    switch (r) {
    case RANGE_SRC_NONE:          return "none";
    case RANGE_SRC_SKYLINK:       return "skylink";
    case RANGE_SRC_LASER:         return "laser";
    case RANGE_SRC_OTHER:         return "other";
    case RANGE_SRC_INTERNAL:      return "internal";
    case RANGE_SRC_INTERNAL_DTED: return "internal_dted";
    default:                      return "unknown";
    }
}

int cmd_range(octl_ctx_t *ctx)
{
    if (octl_resolve_ip(ctx) != 0) {
        jout_err(stderr, OCTL_CONN_FAILED, "no_ip",
                 "no gimbal IP (set --ip, ORION_GIMBAL_IP, or use --discover)");
        return OCTL_CONN_FAILED;
    }
    if (conn_open(ctx->ip) != 0) {
        jout_err(stderr, OCTL_CONN_FAILED, "connect_failed",
                 "could not open TCP to %s", ctx->ip);
        return OCTL_CONN_FAILED;
    }
    conn_drain(OCTL_DRAIN_MS);

    OrionPkt_t req, resp;
    MakeOrionPacket(&req, getOrionRangeDataPacketID(), 0);
    if (conn_send(&req) != 0) {
        jout_err(stderr, OCTL_CONN_FAILED, "send_failed", "could not send RangeData request");
        conn_close();
        return OCTL_CONN_FAILED;
    }
    if (conn_wait_for(ORION_PKT_RANGE_DATA, &resp, ctx->timeout_ms) != 0) {
        jout_err(stderr, OCTL_TIMEOUT, "range_timeout",
                 "no RangeData within %d ms", ctx->timeout_ms);
        conn_close();
        return OCTL_TIMEOUT;
    }
    float range = 0;
    uint16_t maxAgeMs = 0;
    RangeDataSrc_t src = RANGE_SRC_NONE;
    if (!decodeOrionRangeDataPacket(&resp, &range, &maxAgeMs, &src)) {
        jout_err(stderr, OCTL_INTERNAL, "decode_failed", "RangeData decode failed");
        conn_close();
        return OCTL_INTERNAL;
    }
    conn_close();

    jout_t j;
    jout_init(&j, stdout);
    jout_obj_open(&j);
    jout_kv_dbl (&j, "range_m",    range);
    jout_kv_uint(&j, "max_age_ms", maxAgeMs);
    jout_kv_str (&j, "source",     range_src_name(src));
    jout_kv_int (&j, "source_id",  src);
    jout_obj_close(&j);
    jout_done(&j);
    return OCTL_OK;
}
