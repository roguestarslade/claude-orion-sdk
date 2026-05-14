#include "orionctl.h"
#include "conn.h"
#include "json_out.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "OrionPublicPacket.h"
#include "OrionPublicPacketShim.h"
#include "Constants.h"

static const char *mname(int m)
{
    switch (m) {
    case ORION_MODE_DISABLED:   return "disabled";
    case ORION_MODE_RATE:       return "rate";
    case ORION_MODE_POSITION:   return "position";
    case ORION_MODE_GEOPOINT:   return "geopoint";
    case ORION_MODE_PATH:       return "path";
    case ORION_MODE_DOWN:       return "down";
    case ORION_MODE_GEO_RATE:   return "geo_rate";
    default:                    return "other";
    }
}

static int parse_mode(const char *s, OrionMode_t *out)
{
    if (!s) return -1;
    if (!strcmp(s, "disabled")) { *out = ORION_MODE_DISABLED; return 0; }
    if (!strcmp(s, "position")) { *out = ORION_MODE_POSITION; return 0; }
    if (!strcmp(s, "rate"))     { *out = ORION_MODE_RATE;     return 0; }
    if (!strcmp(s, "geopoint")) { *out = ORION_MODE_GEOPOINT; return 0; }
    if (!strcmp(s, "geo_rate")) { *out = ORION_MODE_GEO_RATE; return 0; }
    if (!strcmp(s, "down"))     { *out = ORION_MODE_DOWN;     return 0; }
    return -1;
}

static int read_su(octl_ctx_t *ctx, OrionStartupCmd_t *out)
{
    OrionPkt_t req, resp;
    MakeOrionPacket(&req, getOrionStartupCmdPacketID(), 0);
    if (conn_send(&req) != 0) return -1;
    if (conn_wait_for(ORION_PKT_STARTUP_CMD, &resp, ctx->timeout_ms) != 0) return -2;
    if (!decodeOrionStartupCmdPacketStructure(&resp, out)) return -3;
    return 0;
}

static void emit_su(jout_t *j, const OrionStartupCmd_t *s)
{
    jout_obj_open(j);
    jout_kv_str(j, "mode",       mname(s->Cmd.Mode));
    jout_kv_int(j, "mode_id",    s->Cmd.Mode);
    jout_kv_dbl(j, "pan_rad",    s->Cmd.Target[0]);
    jout_kv_dbl(j, "tilt_rad",   s->Cmd.Target[1]);
    jout_kv_dbl(j, "pan_deg",    rad2deg(s->Cmd.Target[0]));
    jout_kv_dbl(j, "tilt_deg",   rad2deg(s->Cmd.Target[1]));
    jout_kv_int(j, "stabilized", s->Cmd.Stabilized);
    jout_kv_dbl(j, "impulse_time_s", s->Cmd.ImpulseTime);
    jout_obj_close(j);
}

static int su_get(octl_ctx_t *ctx)
{
    if (octl_resolve_ip(ctx) != 0) {
        jout_err(stderr, OCTL_CONN_FAILED, "no_ip", "no gimbal IP");
        return OCTL_CONN_FAILED;
    }
    if (conn_open(ctx->ip) != 0) {
        jout_err(stderr, OCTL_CONN_FAILED, "connect_failed",
                 "could not open TCP to %s", ctx->ip);
        return OCTL_CONN_FAILED;
    }
    conn_drain(OCTL_DRAIN_MS);
    OrionStartupCmd_t s;
    int rc = read_su(ctx, &s);
    conn_close();
    if (rc == -2) {
        jout_err(stderr, OCTL_TIMEOUT, "startup_cmd_timeout",
                 "no StartupCmd within %d ms", ctx->timeout_ms);
        return OCTL_TIMEOUT;
    }
    if (rc != 0) {
        jout_err(stderr, OCTL_INTERNAL, "startup_cmd_failed",
                 "StartupCmd read failed (rc=%d)", rc);
        return OCTL_INTERNAL;
    }
    jout_t j; jout_init(&j, stdout);
    emit_su(&j, &s);
    jout_done(&j);
    return OCTL_OK;
}

static int su_set(octl_ctx_t *ctx)
{
    if (!gate_motion_allowed(ctx)) {
        jout_err(stderr, OCTL_MOTION_GATE, "motion_gated",
                 "startup-cmd set requires --allow-motion");
        return OCTL_MOTION_GATE;
    }
    if (!ctx->iknow) {
        jout_err(stderr, OCTL_REJECTED, "missing_iknow",
                 "startup-cmd set affects next-boot behavior; pass --i-know");
        return OCTL_REJECTED;
    }
    int pan_ok  = ctx->pan_set  || ctx->pan_deg_set;
    int tilt_ok = ctx->tilt_set || ctx->tilt_deg_set;
    if (!pan_ok || !tilt_ok || !ctx->mode_name) {
        jout_err(stderr, OCTL_USAGE, "missing_args",
                 "startup-cmd set requires --pan|--pan-deg --tilt|--tilt-deg --mode <name>");
        return OCTL_USAGE;
    }
    OrionMode_t m;
    if (parse_mode(ctx->mode_name, &m) != 0) {
        jout_err(stderr, OCTL_USAGE, "bad_mode",
                 "--mode must be disabled|rate|position|geopoint|geo_rate|down");
        return OCTL_USAGE;
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
    conn_drain(OCTL_DRAIN_MS);

    OrionStartupCmd_t s;
    memset(&s, 0, sizeof(s));
    s.Cmd.Mode = m;
    s.Cmd.Target[0] = (float)(ctx->pan_deg_set  ? ctx->pan_deg  * (M_PI / 180.0) : ctx->pan_rad);
    s.Cmd.Target[1] = (float)(ctx->tilt_deg_set ? ctx->tilt_deg * (M_PI / 180.0) : ctx->tilt_rad);
    s.Cmd.Stabilized = 1;

    OrionPkt_t out;
    encodeOrionStartupCmdPacketStructure(&out, &s);
    if (conn_send(&out) != 0) {
        jout_err(stderr, OCTL_CONN_FAILED, "send_failed", "could not send StartupCmd");
        conn_close();
        return OCTL_CONN_FAILED;
    }
    OrionPkt_t echo;
    int got = (conn_wait_for(ORION_PKT_STARTUP_CMD, &echo, ctx->timeout_ms) == 0);
    OrionStartupCmd_t echo_s;
    int echo_ok = got && decodeOrionStartupCmdPacketStructure(&echo, &echo_s);
    conn_close();

    jout_t j; jout_init(&j, stdout);
    jout_obj_open(&j);
    jout_kv_bool(&j, "echo_seen", got);
    if (echo_ok) {
        jout_key(&j, "echo");
        emit_su(&j, &echo_s);
    } else {
        jout_key(&j, "written");
        emit_su(&j, &s);
    }
    jout_obj_close(&j);
    jout_done(&j);
    return OCTL_OK;
}

int cmd_startup_cmd(octl_ctx_t *ctx)
{
    if (ctx->npos < 2) {
        jout_err(stderr, OCTL_USAGE, "missing_subverb",
                 "startup-cmd requires subverb: get | set");
        return OCTL_USAGE;
    }
    const char *sub = ctx->pos[1];
    if (strcmp(sub, "get") == 0) return su_get(ctx);
    if (strcmp(sub, "set") == 0) return su_set(ctx);
    jout_err(stderr, OCTL_USAGE, "unknown_subverb",
             "unknown startup-cmd subverb: %s", sub);
    return OCTL_USAGE;
}
