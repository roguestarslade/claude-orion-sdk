#include "orionctl.h"
#include "conn.h"
#include "json_out.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "OrionPublicPacket.h"
#include "OrionPublicPacketShim.h"
#include "Constants.h"

static int read_limits(octl_ctx_t *ctx, OrionLimitsData_t *out)
{
    OrionPkt_t req, resp;
    MakeOrionPacket(&req, getOrionLimitsDataPacketID(), 0);
    if (conn_send(&req) != 0) return -1;
    if (conn_wait_for(ORION_PKT_LIMITS, &resp, ctx->timeout_ms) != 0) return -2;
    if (!decodeOrionLimitsDataPacketStructure(&resp, out)) return -3;
    return 0;
}

static void emit_per_axis(jout_t *j, const char *name, const float v[NUM_GIMBAL_AXES])
{
    jout_key(j, name);
    jout_arr_open(j);
    jout_dbl(j, v[0]);
    jout_dbl(j, v[1]);
    jout_arr_close(j);
}

static void emit_limits(jout_t *j, const OrionLimitsData_t *l)
{
    jout_obj_open(j);
    emit_per_axis(j, "min_pos_rad",      l->MinPos);
    emit_per_axis(j, "max_pos_rad",      l->MaxPos);
    emit_per_axis(j, "max_vel_radps",    l->MaxVel);
    emit_per_axis(j, "max_accel_radps2", l->MaxAccel);
    emit_per_axis(j, "cont_current_a",   l->ContCur);
    emit_per_axis(j, "peak_current_a",   l->PeakCur);
    emit_per_axis(j, "peak_current_time_s", l->PeakCurTime);
    emit_per_axis(j, "init_current_a",   l->InitCur);
    emit_per_axis(j, "max_power_w",      l->MaxPower);
    jout_obj_close(j);
}

static int limits_get(octl_ctx_t *ctx)
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
    OrionLimitsData_t l;
    int rc = read_limits(ctx, &l);
    conn_close();
    if (rc == -2) {
        jout_err(stderr, OCTL_TIMEOUT, "limits_timeout",
                 "no Limits within %d ms", ctx->timeout_ms);
        return OCTL_TIMEOUT;
    }
    if (rc != 0) {
        jout_err(stderr, OCTL_INTERNAL, "limits_failed",
                 "Limits read failed (rc=%d)", rc);
        return OCTL_INTERNAL;
    }
    jout_t j; jout_init(&j, stdout);
    emit_limits(&j, &l);
    jout_done(&j);
    return OCTL_OK;
}

static int limits_set(octl_ctx_t *ctx)
{
    if (!gate_motion_allowed(ctx)) {
        jout_err(stderr, OCTL_MOTION_GATE, "motion_gated",
                 "limits set requires --allow-motion");
        return OCTL_MOTION_GATE;
    }
    if (!ctx->iknow) {
        jout_err(stderr, OCTL_REJECTED, "missing_iknow",
                 "limits set affects motor safety bounds; pass --i-know");
        return OCTL_REJECTED;
    }
    if (!ctx->axis) {
        jout_err(stderr, OCTL_USAGE, "missing_axis",
                 "limits set requires --axis pan|tilt");
        return OCTL_USAGE;
    }
    int ax;
    if      (!strcmp(ctx->axis, "pan"))  ax = 0;
    else if (!strcmp(ctx->axis, "tilt")) ax = 1;
    else {
        jout_err(stderr, OCTL_USAGE, "bad_axis", "axis must be pan or tilt");
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

    OrionLimitsData_t l;
    int rc = read_limits(ctx, &l);
    if (rc != 0) {
        jout_err(stderr, rc == -2 ? OCTL_TIMEOUT : OCTL_INTERNAL,
                 rc == -2 ? "limits_timeout" : "limits_failed",
                 "Limits pre-read failed (rc=%d)", rc);
        conn_close();
        return rc == -2 ? OCTL_TIMEOUT : OCTL_INTERNAL;
    }

    int mutations = 0;
    if (ctx->max_current_set)  { l.ContCur[ax]  = (float)ctx->max_current;  mutations++; }
    if (ctx->max_accel_set)    { l.MaxAccel[ax] = (float)ctx->max_accel;    mutations++; }
    if (ctx->max_velocity_set) { l.MaxVel[ax]   = (float)ctx->max_velocity; mutations++; }
    if (ctx->min_pos_set)      { l.MinPos[ax]   = (float)ctx->min_pos;      mutations++; }
    if (ctx->max_pos_set)      { l.MaxPos[ax]   = (float)ctx->max_pos;      mutations++; }
    if (mutations == 0) {
        jout_err(stderr, OCTL_USAGE, "no_fields",
                 "limits set requires at least one of --max-current --max-accel --max-velocity --min-pos --max-pos");
        conn_close();
        return OCTL_USAGE;
    }

    OrionPkt_t out;
    encodeOrionLimitsDataPacketStructure(&out, &l);
    if (conn_send(&out) != 0) {
        jout_err(stderr, OCTL_CONN_FAILED, "send_failed", "could not send Limits");
        conn_close();
        return OCTL_CONN_FAILED;
    }
    OrionPkt_t echo;
    int got = (conn_wait_for(ORION_PKT_LIMITS, &echo, ctx->timeout_ms) == 0);
    OrionLimitsData_t echo_l;
    int echo_ok = got && decodeOrionLimitsDataPacketStructure(&echo, &echo_l);
    conn_close();

    jout_t j; jout_init(&j, stdout);
    jout_obj_open(&j);
    jout_kv_str (&j, "axis", ctx->axis);
    jout_kv_int (&j, "mutations", mutations);
    jout_kv_bool(&j, "echo_seen", got);
    if (echo_ok) {
        jout_key(&j, "echo");
        emit_limits(&j, &echo_l);
    }
    jout_obj_close(&j);
    jout_done(&j);
    return OCTL_OK;
}

int cmd_limits(octl_ctx_t *ctx)
{
    if (ctx->npos < 2) {
        jout_err(stderr, OCTL_USAGE, "missing_subverb",
                 "limits requires subverb: get | set");
        return OCTL_USAGE;
    }
    const char *sub = ctx->pos[1];
    if (strcmp(sub, "get") == 0) return limits_get(ctx);
    if (strcmp(sub, "set") == 0) return limits_set(ctx);
    jout_err(stderr, OCTL_USAGE, "unknown_subverb",
             "unknown limits subverb: %s", sub);
    return OCTL_USAGE;
}
