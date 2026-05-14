#include "orionctl.h"
#include "conn.h"
#include "json_out.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "OrionPublicPacket.h"
#include "OrionPublicPacketShim.h"

int cmd_stare_ack(octl_ctx_t *ctx)
{
    if (octl_resolve_ip(ctx) != 0) { jout_err(stderr, OCTL_CONN_FAILED, "no_ip", "no gimbal IP"); return OCTL_CONN_FAILED; }
    if (conn_open(ctx->ip) != 0)   { jout_err(stderr, OCTL_CONN_FAILED, "connect_failed", "could not open TCP to %s", ctx->ip); return OCTL_CONN_FAILED; }
    conn_drain(OCTL_DRAIN_MS);

    int loop = ctx->watch;
    int got_any = 0;
    int acked = 0;

    jout_t j; jout_init(&j, stdout);
    jout_obj_open(&j);
    jout_kv_bool(&j, "watch", loop);
    jout_key(&j, "events"); jout_arr_open(&j);

    do {
        OrionPkt_t pkt;
        if (conn_wait_for(ORION_PKT_STARE_START, &pkt, ctx->timeout_ms) != 0) break;
        StareStart_t s;
        if (!decodeStareStartPacketStructure(&pkt, &s)) continue;
        OrionPkt_t ack;
        encodeStareAckPacket(&ack, s.systemTime);
        int sent = (conn_send(&ack) == 0);
        if (sent) acked++;
        jout_obj_open(&j);
        jout_kv_uint(&j, "system_ms",        s.systemTime);
        jout_kv_dbl (&j, "max_stare_time_s", s.maxStareTime);
        jout_kv_dbl (&j, "lat_rad",          s.posLat);
        jout_kv_dbl (&j, "lon_rad",          s.posLon);
        jout_kv_dbl (&j, "alt_m",            s.posAlt);
        jout_kv_bool(&j, "ack_sent",         sent);
        jout_obj_close(&j);
        got_any = 1;
    } while (loop);

    jout_arr_close(&j);
    jout_kv_int(&j, "acks_sent", acked);
    jout_obj_close(&j);
    jout_done(&j);
    conn_close();
    if (!got_any && !loop) {
        /* never saw a stare event in one-shot; not an error */
    }
    return OCTL_OK;
}
