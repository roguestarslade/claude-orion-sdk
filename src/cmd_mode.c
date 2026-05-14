#include "orionctl.h"
#include "conn.h"
#include "json_out.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "OrionPublicPacket.h"
#include "OrionPublicPacketShim.h"
#include "Constants.h"

static const char *mode_name(int m)
{
    switch (m) {
    case ORION_MODE_DISABLED:           return "disabled";
    case ORION_MODE_FAULT:              return "fault";
    case ORION_MODE_RATE:               return "rate";
    case ORION_MODE_GEO_RATE:           return "geo_rate";
    case ORION_MODE_FFC_AUTO:           return "ffc_auto";
    case ORION_MODE_FFC_MANUAL:         return "ffc_manual";
    case ORION_MODE_SCENE:              return "scene";
    case ORION_MODE_TRACK:              return "track";
    case ORION_MODE_CALIBRATION:        return "calibration";
    case ORION_MODE_NULL_GYROS:         return "null_gyros";
    case ORION_MODE_POSITION:           return "position";
    case ORION_MODE_POSITION_NO_LIMITS: return "position_no_limits";
    case ORION_MODE_GEOPOINT:           return "geopoint";
    case ORION_MODE_PATH:               return "path";
    case ORION_MODE_DOWN:               return "down";
    default:                            return "unknown";
    }
}

static int require_motion(octl_ctx_t *ctx, const char *what)
{
    if (gate_motion_allowed(ctx)) return 0;
    jout_err(stderr, OCTL_MOTION_GATE, "motion_gated",
             "%s requires --allow-motion or ORION_ALLOW_MOTION=1", what);
    return -1;
}

static double pan_value(octl_ctx_t *ctx)
{
    if (ctx->pan_deg_set) return ctx->pan_deg * (M_PI / 180.0);
    return ctx->pan_rad;
}

static double tilt_value(octl_ctx_t *ctx)
{
    if (ctx->tilt_deg_set) return ctx->tilt_deg * (M_PI / 180.0);
    return ctx->tilt_rad;
}

static int send_cmd(octl_ctx_t *ctx, const OrionCmd_t *c, const char *verb)
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

    OrionPkt_t pkt;
    encodeOrionCmdPacket(&pkt, c);
    if (conn_send(&pkt) != 0) {
        jout_err(stderr, OCTL_CONN_FAILED, "send_failed", "could not send OrionCmd");
        conn_close();
        return OCTL_CONN_FAILED;
    }
    OrionPkt_t echo;
    int got_echo = (conn_wait_for(ORION_PKT_CMD, &echo, ctx->timeout_ms) == 0);
    conn_close();

    jout_t j; jout_init(&j, stdout);
    jout_obj_open(&j);
    jout_kv_str(&j, "verb",      verb);
    jout_kv_str(&j, "mode",      mode_name(c->Mode));
    jout_kv_int(&j, "mode_id",   c->Mode);
    jout_key(&j, "target_rad"); jout_arr_open(&j);
    jout_dbl(&j, c->Target[0]); jout_dbl(&j, c->Target[1]);
    jout_arr_close(&j);
    jout_key(&j, "target_deg"); jout_arr_open(&j);
    jout_dbl(&j, rad2deg(c->Target[0])); jout_dbl(&j, rad2deg(c->Target[1]));
    jout_arr_close(&j);
    jout_kv_int (&j, "stabilized",   c->Stabilized);
    jout_kv_dbl (&j, "impulse_time_s",c->ImpulseTime);
    jout_kv_bool(&j, "echo_seen",    got_echo);
    jout_obj_close(&j);
    jout_done(&j);
    return OCTL_OK;
}

static int m_disable(octl_ctx_t *ctx)
{
    if (require_motion(ctx, "mode disable") != 0) return OCTL_MOTION_GATE;
    OrionCmd_t c; memset(&c, 0, sizeof(c));
    c.Mode = ORION_MODE_DISABLED;
    return send_cmd(ctx, &c, "disable");
}

static int m_rate(octl_ctx_t *ctx)
{
    if (require_motion(ctx, "mode rate") != 0) return OCTL_MOTION_GATE;
    if (!ctx->pan_set || !ctx->tilt_set) {
        jout_err(stderr, OCTL_USAGE, "missing_args", "mode rate requires --pan <r/s> --tilt <r/s>");
        return OCTL_USAGE;
    }
    OrionCmd_t c; memset(&c, 0, sizeof(c));
    c.Mode = ORION_MODE_RATE;
    c.Target[0] = (float)ctx->pan_rad;
    c.Target[1] = (float)ctx->tilt_rad;
    c.Stabilized = ctx->stab_set ? (uint8_t)ctx->stab : 1;
    return send_cmd(ctx, &c, "rate");
}

static int m_georate(octl_ctx_t *ctx)
{
    if (require_motion(ctx, "mode georate") != 0) return OCTL_MOTION_GATE;
    if (!ctx->pan_set || !ctx->tilt_set) {
        jout_err(stderr, OCTL_USAGE, "missing_args", "mode georate requires --pan --tilt");
        return OCTL_USAGE;
    }
    OrionCmd_t c; memset(&c, 0, sizeof(c));
    c.Mode = ORION_MODE_GEO_RATE;
    c.Target[0] = (float)ctx->pan_rad;
    c.Target[1] = (float)ctx->tilt_rad;
    c.Stabilized = 1;
    return send_cmd(ctx, &c, "georate");
}

static int m_position(octl_ctx_t *ctx)
{
    if (require_motion(ctx, "mode position") != 0) return OCTL_MOTION_GATE;
    int pan_ok  = ctx->pan_set  || ctx->pan_deg_set;
    int tilt_ok = ctx->tilt_set || ctx->tilt_deg_set;
    if (!pan_ok || !tilt_ok) {
        jout_err(stderr, OCTL_USAGE, "missing_args",
                 "mode position requires --pan|--pan-deg AND --tilt|--tilt-deg");
        return OCTL_USAGE;
    }
    OrionCmd_t c; memset(&c, 0, sizeof(c));
    c.Mode = ORION_MODE_POSITION;
    c.Target[0] = (float)pan_value(ctx);
    c.Target[1] = (float)tilt_value(ctx);
    c.Stabilized = 1;
    return send_cmd(ctx, &c, "position");
}

static int m_scene(octl_ctx_t *ctx)
{
    if (require_motion(ctx, "mode scene") != 0) return OCTL_MOTION_GATE;
    if (!ctx->rate_x_set || !ctx->rate_y_set) {
        jout_err(stderr, OCTL_USAGE, "missing_args", "mode scene requires --rate-x --rate-y");
        return OCTL_USAGE;
    }
    OrionCmd_t c; memset(&c, 0, sizeof(c));
    c.Mode = ORION_MODE_SCENE;
    c.Target[0] = (float)ctx->rate_x;
    c.Target[1] = (float)ctx->rate_y;
    c.Stabilized = 1;
    return send_cmd(ctx, &c, "scene");
}

static int m_track(octl_ctx_t *ctx)
{
    if (require_motion(ctx, "mode track") != 0) return OCTL_MOTION_GATE;
    if (!ctx->box_x_set || !ctx->box_y_set) {
        jout_err(stderr, OCTL_USAGE, "missing_args", "mode track requires --box-x --box-y");
        return OCTL_USAGE;
    }
    OrionCmd_t c; memset(&c, 0, sizeof(c));
    c.Mode = ORION_MODE_TRACK;
    c.Target[0] = (float)ctx->box_x;
    c.Target[1] = (float)ctx->box_y;
    c.Stabilized = 1;
    return send_cmd(ctx, &c, "track");
}

static int m_ffc(octl_ctx_t *ctx)
{
    if (require_motion(ctx, "mode ffc") != 0) return OCTL_MOTION_GATE;
    if (ctx->npos < 3) {
        jout_err(stderr, OCTL_USAGE, "missing_args", "mode ffc requires auto|manual");
        return OCTL_USAGE;
    }
    OrionCmd_t c; memset(&c, 0, sizeof(c));
    if      (strcmp(ctx->pos[2], "auto")   == 0) c.Mode = ORION_MODE_FFC_AUTO;
    else if (strcmp(ctx->pos[2], "manual") == 0) c.Mode = ORION_MODE_FFC_MANUAL;
    else {
        jout_err(stderr, OCTL_USAGE, "bad_ffc", "mode ffc <auto|manual>");
        return OCTL_USAGE;
    }
    if (c.Mode == ORION_MODE_FFC_MANUAL) {
        if (ctx->pan_set  || ctx->pan_deg_set)  c.Target[0] = (float)pan_value(ctx);
        if (ctx->tilt_set || ctx->tilt_deg_set) c.Target[1] = (float)tilt_value(ctx);
    }
    c.Stabilized = 1;
    return send_cmd(ctx, &c, "ffc");
}

static int m_calibration(octl_ctx_t *ctx)
{
    if (require_motion(ctx, "mode calibration") != 0) return OCTL_MOTION_GATE;
    OrionCmd_t c; memset(&c, 0, sizeof(c));
    c.Mode = ORION_MODE_CALIBRATION;
    return send_cmd(ctx, &c, "calibration");
}

static int m_null_gyros(octl_ctx_t *ctx)
{
    if (require_motion(ctx, "mode null-gyros") != 0) return OCTL_MOTION_GATE;
    OrionCmd_t c; memset(&c, 0, sizeof(c));
    c.Mode = ORION_MODE_NULL_GYROS;
    return send_cmd(ctx, &c, "null_gyros");
}

int cmd_mode(octl_ctx_t *ctx)
{
    if (ctx->npos < 2) {
        jout_err(stderr, OCTL_USAGE, "missing_subverb",
                 "mode requires subverb: disable|rate|georate|position|scene|track|ffc|calibration|null-gyros");
        return OCTL_USAGE;
    }
    const char *sub = ctx->pos[1];
    if (strcmp(sub, "disable")     == 0) return m_disable(ctx);
    if (strcmp(sub, "rate")        == 0) return m_rate(ctx);
    if (strcmp(sub, "georate")     == 0) return m_georate(ctx);
    if (strcmp(sub, "position")    == 0) return m_position(ctx);
    if (strcmp(sub, "scene")       == 0) return m_scene(ctx);
    if (strcmp(sub, "track")       == 0) return m_track(ctx);
    if (strcmp(sub, "ffc")         == 0) return m_ffc(ctx);
    if (strcmp(sub, "calibration") == 0) return m_calibration(ctx);
    if (strcmp(sub, "null-gyros")  == 0) return m_null_gyros(ctx);
    jout_err(stderr, OCTL_USAGE, "unknown_subverb",
             "unknown mode subverb: %s", sub);
    return OCTL_USAGE;
}
