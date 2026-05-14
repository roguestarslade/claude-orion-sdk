#include "orionctl.h"
#include "conn.h"
#include "json_out.h"

#include <stdio.h>

#include "OrionPublicPacket.h"
#include "OrionPublicPacketShim.h"

int cmd_perf(octl_ctx_t *ctx)
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
    MakeOrionPacket(&req, getOrionPerformancePacketID(), 0);
    if (conn_send(&req) != 0) {
        jout_err(stderr, OCTL_CONN_FAILED, "send_failed", "could not send Performance request");
        conn_close();
        return OCTL_CONN_FAILED;
    }
    if (conn_wait_for(ORION_PKT_PERFORMANCE, &resp, ctx->timeout_ms) != 0) {
        jout_err(stderr, OCTL_TIMEOUT, "perf_timeout",
                 "no Performance within %d ms", ctx->timeout_ms);
        conn_close();
        return OCTL_TIMEOUT;
    }
    OrionPerformance_t p;
    if (!decodeOrionPerformancePacketStructure(&resp, &p)) {
        jout_err(stderr, OCTL_INTERNAL, "decode_failed", "Performance decode failed");
        conn_close();
        return OCTL_INTERNAL;
    }
    conn_close();

    jout_t j;
    jout_init(&j, stdout);
    jout_obj_open(&j);

    jout_key(&j, "rms_quad"); jout_arr_open(&j);
    jout_dbl(&j, p.RmsQuad[0]); jout_dbl(&j, p.RmsQuad[1]); jout_arr_close(&j);

    jout_key(&j, "rms_dir"); jout_arr_open(&j);
    jout_dbl(&j, p.RmsDir[0]);  jout_dbl(&j, p.RmsDir[1]);  jout_arr_close(&j);

    jout_key(&j, "rms_vel"); jout_arr_open(&j);
    jout_dbl(&j, p.RmsVel[0]);  jout_dbl(&j, p.RmsVel[1]);  jout_arr_close(&j);

    jout_key(&j, "rms_pos"); jout_arr_open(&j);
    jout_dbl(&j, p.RmsPos[0]);  jout_dbl(&j, p.RmsPos[1]);  jout_arr_close(&j);

    jout_key(&j, "iout_a"); jout_arr_open(&j);
    jout_dbl(&j, p.Iout[0]);    jout_dbl(&j, p.Iout[1]);    jout_arr_close(&j);

    jout_obj_close(&j);
    jout_done(&j);
    return OCTL_OK;
}
