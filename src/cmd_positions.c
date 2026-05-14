#include "orionctl.h"
#include "conn.h"
#include "json_out.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "OrionPublicPacket.h"
#include "OrionPublicPacketShim.h"
#include "Constants.h"

static const char *slot_name(int s)
{
    switch (s) {
    case POSITION_HOME:    return "home";
    case POSITION_STOW:    return "stow";
    case POSITION_RETRACT: return "retract";
    case POSITION_FFC:     return "ffc";
    case POSITION_USER_0:  return "user_0";
    case POSITION_USER_1:  return "user_1";
    default:               return "unknown";
    }
}

static int parse_slot_arg(const char *s, int *out)
{
    if      (!strcmp(s, "home"))    *out = POSITION_HOME;
    else if (!strcmp(s, "stow"))    *out = POSITION_STOW;
    else if (!strcmp(s, "retract")) *out = POSITION_RETRACT;
    else if (!strcmp(s, "ffc"))     *out = POSITION_FFC;
    else if (!strcmp(s, "user_0"))  *out = POSITION_USER_0;
    else if (!strcmp(s, "user_1"))  *out = POSITION_USER_1;
    else {
        char *end;
        long v = strtol(s, &end, 0);
        if (*end != '\0' || end == s) return -1;
        if (v < 0 || v >= NUM_POSITIONS) return -1;
        *out = (int)v;
    }
    return 0;
}

static int read_positions(octl_ctx_t *ctx, OrionPositions_t *out)
{
    OrionPkt_t req, resp;
    MakeOrionPacket(&req, getOrionPositionsPacketID(), 0);
    if (conn_send(&req) != 0) return -1;
    if (conn_wait_for(ORION_PKT_POSITIONS, &resp, ctx->timeout_ms) != 0) return -2;
    if (!decodeOrionPositionsPacketStructure(&resp, out)) return -3;
    return 0;
}

static void emit_positions(jout_t *j, const OrionPositions_t *p)
{
    jout_obj_open(j);
    jout_kv_int(j, "num_positions", p->NumPositions);
    jout_key(j, "slots");
    jout_arr_open(j);
    for (int i = 0; i < NUM_POSITIONS; i++) {
        jout_obj_open(j);
        jout_kv_int (j, "index",   i);
        jout_kv_str (j, "name",    slot_name(i));
        jout_kv_bool(j, "enabled", p->PosPreset[i].Enabled);
        jout_kv_dbl (j, "pan_rad", p->PosPreset[i].Pan);
        jout_kv_dbl (j, "tilt_rad",p->PosPreset[i].Tilt);
        jout_kv_dbl (j, "pan_deg", rad2deg(p->PosPreset[i].Pan));
        jout_kv_dbl (j, "tilt_deg",rad2deg(p->PosPreset[i].Tilt));
        jout_obj_close(j);
    }
    jout_arr_close(j);
    jout_obj_close(j);
}

static int positions_get(octl_ctx_t *ctx)
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

    OrionPositions_t p;
    int rc = read_positions(ctx, &p);
    conn_close();
    if (rc == -2) {
        jout_err(stderr, OCTL_TIMEOUT, "positions_timeout",
                 "no Positions within %d ms", ctx->timeout_ms);
        return OCTL_TIMEOUT;
    }
    if (rc != 0) {
        jout_err(stderr, OCTL_INTERNAL, "positions_failed",
                 "Positions read failed (rc=%d)", rc);
        return OCTL_INTERNAL;
    }
    jout_t j; jout_init(&j, stdout);
    emit_positions(&j, &p);
    jout_done(&j);
    return OCTL_OK;
}

static int positions_set(octl_ctx_t *ctx)
{
    if (!gate_motion_allowed(ctx)) {
        jout_err(stderr, OCTL_MOTION_GATE, "motion_gated",
                 "positions set requires --allow-motion");
        return OCTL_MOTION_GATE;
    }
    if (!ctx->iknow) {
        jout_err(stderr, OCTL_REJECTED, "missing_iknow",
                 "positions set affects motion targets; pass --i-know");
        return OCTL_REJECTED;
    }
    if (ctx->npos < 3) {
        jout_err(stderr, OCTL_USAGE, "missing_slot", "positions set requires <slot>");
        return OCTL_USAGE;
    }
    int slot = 0;
    if (parse_slot_arg(ctx->pos[2], &slot) != 0) {
        jout_err(stderr, OCTL_USAGE, "bad_slot",
                 "slot must be home|stow|retract|ffc|user_0|user_1 or 0..%d", NUM_POSITIONS - 1);
        return OCTL_USAGE;
    }
    int pan_ok  = ctx->pan_set  || ctx->pan_deg_set;
    int tilt_ok = ctx->tilt_set || ctx->tilt_deg_set;
    if (!pan_ok || !tilt_ok) {
        jout_err(stderr, OCTL_USAGE, "missing_pantilt",
                 "positions set requires --pan|--pan-deg AND --tilt|--tilt-deg");
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

    OrionPositions_t pos;
    int rc = read_positions(ctx, &pos);
    if (rc != 0) {
        jout_err(stderr, rc == -2 ? OCTL_TIMEOUT : OCTL_INTERNAL,
                 rc == -2 ? "positions_timeout" : "positions_failed",
                 "Positions pre-read failed (rc=%d)", rc);
        conn_close();
        return rc == -2 ? OCTL_TIMEOUT : OCTL_INTERNAL;
    }
    /* Ensure slot index covered */
    if (slot >= NUM_POSITIONS) {
        jout_err(stderr, OCTL_USAGE, "bad_slot", "slot out of range");
        conn_close();
        return OCTL_USAGE;
    }
    if (pos.NumPositions < (uint8_t)(slot + 1)) pos.NumPositions = (uint8_t)(slot + 1);
    double pan_v  = ctx->pan_deg_set  ? ctx->pan_deg  * (M_PI / 180.0) : ctx->pan_rad;
    double tilt_v = ctx->tilt_deg_set ? ctx->tilt_deg * (M_PI / 180.0) : ctx->tilt_rad;
    pos.PosPreset[slot].Pan = (float)pan_v;
    pos.PosPreset[slot].Tilt = (float)tilt_v;
    pos.PosPreset[slot].Enabled = 1;

    OrionPkt_t out;
    encodeOrionPositionsPacketStructure(&out, &pos);
    if (conn_send(&out) != 0) {
        jout_err(stderr, OCTL_CONN_FAILED, "send_failed", "could not send Positions");
        conn_close();
        return OCTL_CONN_FAILED;
    }
    OrionPkt_t echo;
    int got = (conn_wait_for(ORION_PKT_POSITIONS, &echo, ctx->timeout_ms) == 0);
    OrionPositions_t echo_p;
    int echo_ok = got && decodeOrionPositionsPacketStructure(&echo, &echo_p);
    conn_close();

    jout_t j; jout_init(&j, stdout);
    jout_obj_open(&j);
    jout_kv_int (&j, "slot",        slot);
    jout_kv_str (&j, "slot_name",   slot_name(slot));
    jout_kv_dbl (&j, "pan_rad",     pan_v);
    jout_kv_dbl (&j, "tilt_rad",    tilt_v);
    jout_kv_bool(&j, "echo_seen",   got);
    if (echo_ok) {
        jout_key(&j, "echo");
        emit_positions(&j, &echo_p);
    }
    jout_obj_close(&j);
    jout_done(&j);
    return OCTL_OK;
}

int cmd_positions(octl_ctx_t *ctx)
{
    if (ctx->npos < 2) {
        jout_err(stderr, OCTL_USAGE, "missing_subverb",
                 "positions requires subverb: get | set");
        return OCTL_USAGE;
    }
    const char *sub = ctx->pos[1];
    if (strcmp(sub, "get") == 0) return positions_get(ctx);
    if (strcmp(sub, "set") == 0) return positions_set(ctx);
    jout_err(stderr, OCTL_USAGE, "unknown_subverb",
             "unknown positions subverb: %s", sub);
    return OCTL_USAGE;
}
