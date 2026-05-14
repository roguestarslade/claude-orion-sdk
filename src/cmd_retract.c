#include "orionctl.h"
#include "conn.h"
#include "json_out.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "OrionPublicPacket.h"
#include "OrionPublicPacketShim.h"
#include "Constants.h"

static const char *rcmd_name(int c) {
    switch (c) {
    case RETRACT_CMD_DISABLE: return "disable";
    case RETRACT_CMD_DEPLOY:  return "deploy";
    case RETRACT_CMD_RETRACT: return "retract";
    default:                  return "unknown";
    }
}
static const char *rstate_name(int s) {
    switch (s) {
    case RETRACT_STATE_DISABLED:  return "disabled";
    case RETRACT_STATE_RETRACTED: return "retracted";
    case RETRACT_STATE_RETRACTING:return "retracting";
    case RETRACT_STATE_DEPLOYING: return "deploying";
    case RETRACT_STATE_DEPLOYED:  return "deployed";
    case RETRACT_STATE_FAULT:     return "fault";
    default:                      return "unknown";
    }
}

static int retract_status(octl_ctx_t *ctx)
{
    if (octl_resolve_ip(ctx) != 0) { jout_err(stderr, OCTL_CONN_FAILED, "no_ip", "no gimbal IP"); return OCTL_CONN_FAILED; }
    if (conn_open(ctx->ip) != 0)   { jout_err(stderr, OCTL_CONN_FAILED, "connect_failed", "could not open TCP to %s", ctx->ip); return OCTL_CONN_FAILED; }
    conn_drain(OCTL_DRAIN_MS);
    OrionPkt_t req, resp;
    MakeOrionPacket(&req, getOrionRetractStatusPacketID(), 0);
    if (conn_send(&req) != 0) { jout_err(stderr, OCTL_CONN_FAILED, "send_failed", "could not send"); conn_close(); return OCTL_CONN_FAILED; }
    if (conn_wait_for(ORION_PKT_RETRACT_STATUS, &resp, ctx->timeout_ms) != 0) {
        jout_err(stderr, OCTL_TIMEOUT, "retract_status_timeout", "no RetractStatus in %d ms", ctx->timeout_ms);
        conn_close();
        return OCTL_TIMEOUT;
    }
    OrionRetractCmd_t c; OrionRetractState_t s; float pos = 0; uint16_t flags = 0;
    if (!decodeOrionRetractStatusPacket(&resp, &c, &s, &pos, &flags)) {
        jout_err(stderr, OCTL_INTERNAL, "decode_failed", "RetractStatus decode failed");
        conn_close();
        return OCTL_INTERNAL;
    }
    conn_close();
    jout_t j; jout_init(&j, stdout);
    jout_obj_open(&j);
    jout_kv_str (&j, "cmd",      rcmd_name(c));
    jout_kv_int (&j, "cmd_id",   c);
    jout_kv_str (&j, "state",    rstate_name(s));
    jout_kv_int (&j, "state_id", s);
    jout_kv_dbl (&j, "pos_rad",  pos);
    jout_kv_dbl (&j, "pos_deg",  rad2deg(pos));
    jout_kv_uint(&j, "flags",    flags);
    jout_obj_close(&j);
    jout_done(&j);
    return OCTL_OK;
}

static int retract_send(octl_ctx_t *ctx, OrionRetractCmd_t cmd, const char *verb)
{
    if (!gate_motion_allowed(ctx)) {
        jout_err(stderr, OCTL_MOTION_GATE, "motion_gated", "retract %s requires --allow-motion", verb);
        return OCTL_MOTION_GATE;
    }
    if (!ctx->iknow) {
        jout_err(stderr, OCTL_REJECTED, "missing_iknow",
                 "retract %s can damage payload if obstructed; pass --i-know", verb);
        return OCTL_REJECTED;
    }
    if (octl_resolve_ip(ctx) != 0) { jout_err(stderr, OCTL_CONN_FAILED, "no_ip", "no gimbal IP"); return OCTL_CONN_FAILED; }
    if (conn_open(ctx->ip) != 0)   { jout_err(stderr, OCTL_CONN_FAILED, "connect_failed", "could not open TCP to %s", ctx->ip); return OCTL_CONN_FAILED; }
    conn_drain(OCTL_DRAIN_MS);
    OrionPkt_t pkt;
    encodeOrionRetractCommandPacket(&pkt, cmd);
    int sent = (conn_send(&pkt) == 0);
    OrionPkt_t echo;
    int got = sent && (conn_wait_for(ORION_PKT_RETRACT_STATUS, &echo, ctx->timeout_ms) == 0);
    conn_close();
    jout_t j; jout_init(&j, stdout);
    jout_obj_open(&j);
    jout_kv_str (&j, "verb",      verb);
    jout_kv_str (&j, "cmd",       rcmd_name(cmd));
    jout_kv_int (&j, "cmd_id",    cmd);
    jout_kv_bool(&j, "sent",      sent);
    jout_kv_bool(&j, "echo_seen", got);
    jout_obj_close(&j);
    jout_done(&j);
    return sent ? OCTL_OK : OCTL_CONN_FAILED;
}

int cmd_retract(octl_ctx_t *ctx)
{
    if (ctx->npos < 2) { jout_err(stderr, OCTL_USAGE, "missing_subverb", "retract status|deploy|stow"); return OCTL_USAGE; }
    const char *sub = ctx->pos[1];
    if (strcmp(sub, "status") == 0) return retract_status(ctx);
    if (strcmp(sub, "deploy") == 0) return retract_send(ctx, RETRACT_CMD_DEPLOY,  "deploy");
    if (strcmp(sub, "stow")   == 0) return retract_send(ctx, RETRACT_CMD_RETRACT, "stow");
    jout_err(stderr, OCTL_USAGE, "unknown_subverb", "unknown retract subverb: %s", sub);
    return OCTL_USAGE;
}
