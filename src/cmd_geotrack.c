#include "orionctl.h"
#include "conn.h"
#include "json_out.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "OrionPublicPacket.h"
#include "OrionPublicPacketShim.h"
#include "Constants.h"

static const char *gt_filter_name(int f)
{
    switch (f) {
    case GEO_TRACKER_FILTER_STATIC:    return "static";
    case GEO_TRACKER_FILTER_CONST_VEL: return "const-vel";
    case GEO_TRACKER_FILTER_CONST_ACC: return "const-acc";
    default:                           return "unknown";
    }
}

static const char *gt_target_name(int t)
{
    switch (t) {
    case GEO_TRACKER_TARGET_STATIC:      return "static";
    case GEO_TRACKER_TARGET_PERSON:      return "person";
    case GEO_TRACKER_TARGET_CAR:         return "car";
    case GEO_TRACKER_TARGET_CAR_CITY:    return "car-city";
    case GEO_TRACKER_TARGET_CAR_HIGHWAY: return "car-highway";
    case GEO_TRACKER_TARGET_BOAT:        return "boat";
    case GEO_TRACKER_TARGET_BOAT_FAST:   return "boat-fast";
    default:                             return "unknown";
    }
}

static const char *gt_cmd_name(int c)
{
    switch (c) {
    case GEO_TRACKER_CMD_STOP:    return "stop";
    case GEO_TRACKER_CMD_RUN:     return "run";
    case GEO_TRACKER_CMD_RESTART: return "restart";
    default:                      return "unknown";
    }
}

static const char *gt_state_name(int s)
{
    switch (s) {
    case GEO_TRACKER_STATE_OFF:     return "off";
    case GEO_TRACKER_STATE_RUNNING: return "running";
    default:                            return "unknown";
    }
}

static int parse_filter(const char *s, GeoTrackerFilterType_t *out)
{
    if (!s)                            { *out = GEO_TRACKER_FILTER_CONST_VEL; return 0; }
    if (!strcmp(s, "static"))          { *out = GEO_TRACKER_FILTER_STATIC;    return 0; }
    if (!strcmp(s, "const-vel"))       { *out = GEO_TRACKER_FILTER_CONST_VEL; return 0; }
    if (!strcmp(s, "const-acc"))       { *out = GEO_TRACKER_FILTER_CONST_ACC; return 0; }
    return -1;
}

static int parse_target(const char *s, GeoTrackerTargetType_t *out)
{
    if (!s)                            { *out = GEO_TRACKER_TARGET_STATIC; return 0; }
    if (!strcmp(s, "static"))          { *out = GEO_TRACKER_TARGET_STATIC;       return 0; }
    if (!strcmp(s, "person"))          { *out = GEO_TRACKER_TARGET_PERSON;       return 0; }
    if (!strcmp(s, "car"))             { *out = GEO_TRACKER_TARGET_CAR;          return 0; }
    if (!strcmp(s, "car-city"))        { *out = GEO_TRACKER_TARGET_CAR_CITY;     return 0; }
    if (!strcmp(s, "car-highway"))     { *out = GEO_TRACKER_TARGET_CAR_HIGHWAY;  return 0; }
    if (!strcmp(s, "boat"))            { *out = GEO_TRACKER_TARGET_BOAT;         return 0; }
    if (!strcmp(s, "boat-fast"))       { *out = GEO_TRACKER_TARGET_BOAT_FAST;    return 0; }
    return -1;
}

static int send_geotrack_cmd(octl_ctx_t *ctx, const GeoTrackerCommand_t *cmd)
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

    OrionPkt_t pkt;
    encodeGeoTrackerCommandPacketStructure(&pkt, cmd);
    if (conn_send(&pkt) != 0) {
        jout_err(stderr, OCTL_CONN_FAILED, "send_failed", "could not send GeoTrackerCommand");
        conn_close();
        return OCTL_CONN_FAILED;
    }

    OrionPkt_t echo;
    int got_echo = (conn_wait_for(ORION_PKT_GEO_TRACK_COMMAND, &echo, ctx->timeout_ms) == 0);

    jout_t j;
    jout_init(&j, stdout);
    jout_obj_open(&j);
    jout_kv_str (&j, "cmd",         gt_cmd_name(cmd->cmd));
    jout_kv_int (&j, "cmd_id",      cmd->cmd);
    jout_kv_str (&j, "filter",      gt_filter_name(cmd->FilterType));
    jout_kv_int (&j, "filter_id",   cmd->FilterType);
    jout_kv_str (&j, "target",      gt_target_name(cmd->TargetType));
    jout_kv_int (&j, "target_id",   cmd->TargetType);
    jout_kv_bool(&j, "echo_seen",   got_echo);
    jout_obj_close(&j);
    jout_done(&j);

    conn_close();
    return OCTL_OK;
}

static int gt_make_cmd(octl_ctx_t *ctx, GeoTrackerCmd_t which)
{
    GeoTrackerCommand_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    if (parse_filter(ctx->filter, &cmd.FilterType) != 0) {
        jout_err(stderr, OCTL_USAGE, "bad_filter",
                 "--filter must be static|const-vel|const-acc, got: %s", ctx->filter);
        return OCTL_USAGE;
    }
    if (parse_target(ctx->target, &cmd.TargetType) != 0) {
        jout_err(stderr, OCTL_USAGE, "bad_target",
                 "--target must be static|person|car|car-city|car-highway|boat|boat-fast, got: %s",
                 ctx->target);
        return OCTL_USAGE;
    }
    cmd.cmd = which;
    return send_geotrack_cmd(ctx, &cmd);
}

static void emit_status(jout_t *j, const GeoTrackStatus_t *s)
{
    jout_obj_open(j);
    jout_kv_int (j, "index",  s->Index);
    jout_kv_uint(j, "id",     s->ID);
    jout_kv_int (j, "status", s->Status);
    jout_key(j, "position");
    jout_obj_open(j);
    jout_kv_dbl(j, "lat_rad", s->Latitude);
    jout_kv_dbl(j, "lon_rad", s->Longitude);
    jout_kv_dbl(j, "lat_deg", rad2deg(s->Latitude));
    jout_kv_dbl(j, "lon_deg", rad2deg(s->Longitude));
    jout_kv_dbl(j, "alt_m",   s->Altitude);
    jout_obj_close(j);
    jout_key(j, "sigma");
    jout_obj_open(j);
    jout_kv_dbl(j, "lat_rad", s->SigmaLatitude);
    jout_kv_dbl(j, "lon_rad", s->SigmaLongitude);
    jout_kv_dbl(j, "alt_m",   s->SigmaAltitude);
    jout_obj_close(j);
    jout_key(j, "ellipse");
    jout_obj_open(j);
    jout_kv_dbl(j, "major_m",  s->EllipseMajor);
    jout_kv_dbl(j, "minor_m",  s->EllipseMinor);
    jout_kv_dbl(j, "angle_rad",s->EllipseAngle);
    jout_obj_close(j);
    jout_key(j, "vel_ned_mps"); jout_arr_open(j);
    jout_dbl(j, s->VelNED[0]); jout_dbl(j, s->VelNED[1]); jout_dbl(j, s->VelNED[2]);
    jout_arr_close(j);
    jout_key(j, "sigma_vel_ned_mps"); jout_arr_open(j);
    jout_dbl(j, s->SigmaVelNED[0]); jout_dbl(j, s->SigmaVelNED[1]); jout_dbl(j, s->SigmaVelNED[2]);
    jout_arr_close(j);
    jout_kv_str(j, "state",     gt_state_name(s->State));
    jout_kv_int(j, "state_id",  s->State);
    jout_kv_dbl(j, "course_deg",s->Course);
    jout_obj_close(j);
}

static int gt_status(octl_ctx_t *ctx)
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

    int n = ctx->watch ? (ctx->watch_n > 0 ? ctx->watch_n : 0) : 1;
    int got_any = 0;
    int i = 0;
    while (n == 0 || i < n) {
        OrionPkt_t pkt;
        if (conn_wait_for(ORION_PKT_GEO_TRACK_STATUS, &pkt, ctx->timeout_ms) != 0) {
            if (!got_any) {
                jout_err(stderr, OCTL_TIMEOUT, "geotrack_status_timeout",
                         "no GeoTrackStatus within %d ms", ctx->timeout_ms);
                conn_close();
                return OCTL_TIMEOUT;
            }
            break;
        }
        GeoTrackStatus_t s;
        if (!decodeGeoTrackStatusPacketStructure(&pkt, &s)) {
            jout_err(stderr, OCTL_INTERNAL, "decode_failed", "GeoTrackStatus decode failed");
            conn_close();
            return OCTL_INTERNAL;
        }
        jout_t j;
        jout_init(&j, stdout);
        emit_status(&j, &s);
        jout_done(&j);
        got_any = 1;
        i++;
        if (!ctx->watch) break;
    }

    conn_close();
    return OCTL_OK;
}

int cmd_geotrack(octl_ctx_t *ctx)
{
    if (ctx->npos < 2) {
        jout_err(stderr, OCTL_USAGE, "missing_subverb",
                 "geotrack requires subverb: run | stop | restart | status");
        return OCTL_USAGE;
    }
    const char *sub = ctx->pos[1];
    if (strcmp(sub, "run")     == 0) return gt_make_cmd(ctx, GEO_TRACKER_CMD_RUN);
    if (strcmp(sub, "stop")    == 0) return gt_make_cmd(ctx, GEO_TRACKER_CMD_STOP);
    if (strcmp(sub, "restart") == 0) return gt_make_cmd(ctx, GEO_TRACKER_CMD_RESTART);
    if (strcmp(sub, "status")  == 0) return gt_status(ctx);

    jout_err(stderr, OCTL_USAGE, "unknown_subverb",
             "unknown geotrack subverb: %s", sub);
    return OCTL_USAGE;
}
