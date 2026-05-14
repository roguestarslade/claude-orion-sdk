#include "orionctl.h"
#include "conn.h"
#include "json_out.h"

#include <stdio.h>

#include "OrionPublicPacket.h"
#include "OrionPublicPacketShim.h"

int cmd_reset(octl_ctx_t *ctx)
{
    if (!gate_reset_allowed(ctx)) {
        jout_err(stderr, OCTL_MOTION_GATE, "reset_gated",
                 "reset requires --allow-motion --i-know");
        return OCTL_MOTION_GATE;
    }
    if (octl_resolve_ip(ctx) != 0) {
        jout_err(stderr, OCTL_CONN_FAILED, "no_ip", "no gimbal IP");
        return OCTL_CONN_FAILED;
    }
    if (conn_open(ctx->ip) != 0) {
        jout_err(stderr, OCTL_CONN_FAILED, "connect_failed",
                 "could not open TCP to %s", ctx->ip);
        return OCTL_CONN_FAILED;
    }
    OrionPkt_t pkt;
    MakeOrionPacket(&pkt, ORION_PKT_RESET, 0);
    int sent = (conn_send(&pkt) == 0);
    conn_close();

    jout_t j; jout_init(&j, stdout);
    jout_obj_open(&j);
    jout_kv_str (&j, "verb",         "reset");
    jout_kv_str (&j, "policy",       "fire_and_exit");
    jout_kv_bool(&j, "sent",         sent);
    jout_obj_close(&j);
    jout_done(&j);
    return sent ? OCTL_OK : OCTL_CONN_FAILED;
}
