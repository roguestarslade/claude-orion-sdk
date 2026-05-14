#include "orionctl.h"
#include "conn.h"
#include "json_out.h"

#include <stdio.h>
#include <string.h>

#include "OrionPublicPacket.h"
#include "OrionPublicPacketShim.h"

static const char *cm_name(int m)
{
    switch (m) {
    case crownModeNormal:    return "normal";
    case crownModeLogINS:    return "log_ins";
    case crownModePrintIMU:  return "print_imu";
    case crownModePrintINS:  return "print_ins";
    case crownModeTempCal:   return "temp_cal";
    case crownModeIMUCal:    return "imu_cal";
    default:                 return "unknown";
    }
}

static int parse_cm(const char *s, crownModes *out)
{
    if (!s) return -1;
    if (!strcmp(s, "normal"))    { *out = crownModeNormal;   return 0; }
    if (!strcmp(s, "log-ins"))   { *out = crownModeLogINS;   return 0; }
    if (!strcmp(s, "print-imu")) { *out = crownModePrintIMU; return 0; }
    if (!strcmp(s, "print-ins")) { *out = crownModePrintINS; return 0; }
    if (!strcmp(s, "temp-cal"))  { *out = crownModeTempCal;  return 0; }
    if (!strcmp(s, "imu-cal"))   { *out = crownModeIMUCal;   return 0; }
    return -1;
}

static int crown_get(octl_ctx_t *ctx)
{
    if (octl_resolve_ip(ctx) != 0) { jout_err(stderr, OCTL_CONN_FAILED, "no_ip", "no gimbal IP"); return OCTL_CONN_FAILED; }
    if (conn_open(ctx->ip) != 0)   { jout_err(stderr, OCTL_CONN_FAILED, "connect_failed", "could not open TCP to %s", ctx->ip); return OCTL_CONN_FAILED; }
    conn_drain(OCTL_DRAIN_MS);
    OrionPkt_t req, resp;
    MakeOrionPacket(&req, getCrownModePacketID(), 0);
    if (conn_send(&req) != 0) { jout_err(stderr, OCTL_CONN_FAILED, "send_failed", "could not send CrownMode probe"); conn_close(); return OCTL_CONN_FAILED; }
    if (conn_wait_for(ORION_PKT_CROWN_MODE, &resp, ctx->timeout_ms) != 0) {
        jout_err(stderr, OCTL_TIMEOUT, "crown_mode_timeout", "no CrownMode in %d ms", ctx->timeout_ms);
        conn_close();
        return OCTL_TIMEOUT;
    }
    crownModes mode = crownModeNormal;
    if (!decodeCrownModePacket(&resp, &mode)) {
        jout_err(stderr, OCTL_INTERNAL, "decode_failed", "CrownMode decode failed");
        conn_close();
        return OCTL_INTERNAL;
    }
    conn_close();
    jout_t j; jout_init(&j, stdout);
    jout_obj_open(&j);
    jout_kv_str(&j, "mode",    cm_name(mode));
    jout_kv_int(&j, "mode_id", mode);
    jout_obj_close(&j);
    jout_done(&j);
    return OCTL_OK;
}

static int crown_set(octl_ctx_t *ctx)
{
    /* pos[0]=crown-mode, pos[1]=set, pos[2]=<mode-name> */
    if (ctx->npos < 3) { jout_err(stderr, OCTL_USAGE, "missing_mode", "crown-mode set <mode-name>"); return OCTL_USAGE; }
    crownModes m;
    if (parse_cm(ctx->pos[2], &m) != 0) {
        jout_err(stderr, OCTL_USAGE, "bad_mode",
                 "mode must be normal|log-ins|print-imu|print-ins|temp-cal|imu-cal");
        return OCTL_USAGE;
    }
    if (octl_resolve_ip(ctx) != 0) { jout_err(stderr, OCTL_CONN_FAILED, "no_ip", "no gimbal IP"); return OCTL_CONN_FAILED; }
    if (conn_open(ctx->ip) != 0)   { jout_err(stderr, OCTL_CONN_FAILED, "connect_failed", "could not open TCP to %s", ctx->ip); return OCTL_CONN_FAILED; }
    conn_drain(OCTL_DRAIN_MS);
    OrionPkt_t pkt;
    encodeCrownModePacket(&pkt, m);
    int sent = (conn_send(&pkt) == 0);
    OrionPkt_t echo;
    int got = sent && (conn_wait_for(ORION_PKT_CROWN_MODE, &echo, ctx->timeout_ms) == 0);
    crownModes echoed_mode = crownModeNormal;
    int echo_ok = got && decodeCrownModePacket(&echo, &echoed_mode);
    conn_close();
    jout_t j; jout_init(&j, stdout);
    jout_obj_open(&j);
    jout_kv_str (&j, "intended",  cm_name(m));
    jout_kv_int (&j, "intended_id", m);
    jout_kv_bool(&j, "sent",      sent);
    jout_kv_bool(&j, "echo_seen", got);
    if (echo_ok) {
        jout_kv_str(&j, "echo_mode",    cm_name(echoed_mode));
        jout_kv_int(&j, "echo_mode_id", echoed_mode);
    }
    jout_obj_close(&j);
    jout_done(&j);
    return OCTL_OK;
}

int cmd_crown_mode(octl_ctx_t *ctx)
{
    if (ctx->npos < 2) { jout_err(stderr, OCTL_USAGE, "missing_subverb", "crown-mode get | set"); return OCTL_USAGE; }
    const char *sub = ctx->pos[1];
    if (strcmp(sub, "get") == 0) return crown_get(ctx);
    if (strcmp(sub, "set") == 0) return crown_set(ctx);
    jout_err(stderr, OCTL_USAGE, "unknown_subverb", "unknown crown-mode subverb: %s", sub);
    return OCTL_USAGE;
}
