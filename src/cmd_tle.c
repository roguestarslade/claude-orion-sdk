#include "orionctl.h"
#include "conn.h"
#include "json_out.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "OrionPublicPacket.h"
#include "OrionPublicPacketShim.h"
#include "Constants.h"

static const char *tle_type_name(int t)
{
    switch (t) {
    case TLE_TYPE_NONE:      return "none";
    case TLE_TYPE_NO_CHANGE: return "no_change";
    case TLE_TYPE_GEOLOCATE: return "geolocate";
    case TLE_TYPE_ALIGN:     return "align";
    default:                 return "unknown";
    }
}

static const char *tle_state_name(int s)
{
    switch (s) {
    case TLE_STATE_OFF:     return "off";
    case TLE_STATE_RUNNING: return "running";
    default:                return "unknown";
    }
}

static const char *tle_source_name(int s)
{
    switch (s) {
    case TLE_SOURCE_GIMBAL_GEO:  return "gimbal_geo";
    case TLE_SOURCE_SKYLINK_GEO: return "skylink_geo";
    case TLE_SOURCE_GIMBAL_DTED: return "gimbal_dted";
    case TLE_SOURCE_OTHER:       return "other";
    default:                     return "unknown";
    }
}

static int parse_tle_filter(const char *s, TleType_t *out)
{
    if (!s) return -1;
    if (!strcmp(s, "geolocate")) { *out = TLE_TYPE_GEOLOCATE; return 0; }
    if (!strcmp(s, "align"))     { *out = TLE_TYPE_ALIGN;     return 0; }
    return -1;
}

static int send_tle_cmd(octl_ctx_t *ctx, const TleCommand_t *cmd)
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
    encodeTleCommandPacketStructure(&pkt, cmd);
    if (conn_send(&pkt) != 0) {
        jout_err(stderr, OCTL_CONN_FAILED, "send_failed", "could not send TleCommand");
        conn_close();
        return OCTL_CONN_FAILED;
    }

    OrionPkt_t echo;
    int got_echo = (conn_wait_for(ORION_PKT_TLE_COMMAND, &echo, ctx->timeout_ms) == 0);

    jout_t j;
    jout_init(&j, stdout);
    jout_obj_open(&j);
    jout_kv_str (&j, "filter",      tle_type_name(cmd->FilterType));
    jout_kv_int (&j, "filter_id",   cmd->FilterType);
    jout_kv_int (&j, "run",         cmd->Run);
    jout_kv_uint(&j, "options_bits",cmd->Options);
    jout_key(&j, "options");
    jout_obj_open(&j);
    jout_kv_bool(&j, "ins",      (cmd->Options & tleIns)      != 0);
    jout_kv_bool(&j, "dted",     (cmd->Options & tleDted)     != 0);
    jout_kv_bool(&j, "lrf",      (cmd->Options & tleLrf)      != 0);
    jout_kv_bool(&j, "offboard", (cmd->Options & tleOffBoard) != 0);
    jout_obj_close(&j);
    jout_kv_bool(&j, "echo_seen",   got_echo);
    jout_obj_close(&j);
    jout_done(&j);

    conn_close();
    return OCTL_OK;
}

static int tle_start(octl_ctx_t *ctx)
{
    /* pos[0]=tle, pos[1]=start, pos[2]=<geolocate|align> */
    if (ctx->npos < 3) {
        jout_err(stderr, OCTL_USAGE, "missing_filter",
                 "tle start requires <geolocate|align>");
        return OCTL_USAGE;
    }
    TleCommand_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    if (parse_tle_filter(ctx->pos[2], &cmd.FilterType) != 0) {
        jout_err(stderr, OCTL_USAGE, "bad_filter",
                 "tle filter must be geolocate or align, got: %s", ctx->pos[2]);
        return OCTL_USAGE;
    }
    cmd.Run = 1;
    uint8_t opts = 0;
    if (ctx->tle_ins)      opts |= tleIns;
    if (ctx->tle_dted)     opts |= tleDted;
    if (ctx->tle_lrf)      opts |= tleLrf;
    if (ctx->tle_offboard) opts |= tleOffBoard;
    cmd.Options = opts;
    return send_tle_cmd(ctx, &cmd);
}

static int tle_stop(octl_ctx_t *ctx)
{
    TleCommand_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.FilterType = TLE_TYPE_NO_CHANGE;
    cmd.Run = 0;
    cmd.Options = 0;
    return send_tle_cmd(ctx, &cmd);
}

static void emit_status(jout_t *j, const TleStatus_t *s)
{
    jout_obj_open(j);
    jout_kv_uint(j, "system_ms",     s->SystemTime);
    jout_kv_str (j, "filter_type",   tle_type_name(s->FilterType));
    jout_kv_int (j, "filter_type_id",s->FilterType);
    jout_kv_str (j, "state",         tle_state_name(s->State));
    jout_kv_int (j, "state_id",      s->State);
    jout_kv_uint(j, "sample_count",  s->SampleCount);

    jout_key(j, "position");
    jout_obj_open(j);
    jout_kv_dbl(j, "lat_rad", s->Latitude);
    jout_kv_dbl(j, "lon_rad", s->Longitude);
    jout_kv_dbl(j, "lat_deg", rad2deg(s->Latitude));
    jout_kv_dbl(j, "lon_deg", rad2deg(s->Longitude));
    jout_kv_dbl(j, "alt_m",   s->Altitude);
    jout_obj_close(j);

    jout_key(j, "error_90pct");
    jout_obj_open(j);
    jout_kv_dbl(j, "ce90_m", s->CE90);
    jout_kv_dbl(j, "ve90_m", s->VE90);
    jout_kv_dbl(j, "se90_m", s->SE90);
    jout_obj_close(j);

    jout_key(j, "ellipse");
    jout_obj_open(j);
    jout_kv_dbl(j, "major_m",  s->EllipseMajor);
    jout_kv_dbl(j, "minor_m",  s->EllipseMinor);
    jout_kv_dbl(j, "angle_rad",s->EllipseAngle);
    jout_obj_close(j);

    jout_key(j, "error");
    jout_obj_open(j);
    jout_kv_dbl(j, "pan_rad",  s->PanError);
    jout_kv_dbl(j, "tilt_rad", s->TiltError);
    jout_kv_dbl(j, "alt_m",    s->AltError);
    jout_obj_close(j);

    jout_key(j, "uncertainty");
    jout_obj_open(j);
    jout_kv_dbl(j, "pan_rad",  s->PanUncertainty);
    jout_kv_dbl(j, "tilt_rad", s->TiltUncertainty);
    jout_kv_dbl(j, "alt_m",    s->AltUncertainty);
    jout_obj_close(j);

    jout_kv_dbl(j, "percent_orbit", s->PercentOrbit);
    jout_kv_str(j, "source",        tle_source_name(s->Source));
    jout_kv_int(j, "source_id",     s->Source);

    jout_key(j, "detections");
    jout_obj_open(j);
    jout_kv_int(j, "id",             s->TleDetects.ID);
    jout_kv_dbl(j, "sigma_north_m",  s->TleDetects.SigmaNorth);
    jout_kv_dbl(j, "sigma_east_m",   s->TleDetects.SigmaEast);
    jout_kv_dbl(j, "sigma_down_m",   s->TleDetects.SigmaDown);
    jout_kv_dbl(j, "rho_north_east", s->TleDetects.RhoNorthEast);
    jout_kv_dbl(j, "rho_north_down", s->TleDetects.RhoNorthDown);
    jout_kv_dbl(j, "rho_east_down",  s->TleDetects.RhoEastDown);
    jout_obj_close(j);

    jout_obj_close(j);
}

static int tle_status(octl_ctx_t *ctx)
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
        if (conn_wait_for(ORION_PKT_TLE_STATUS, &pkt, ctx->timeout_ms) != 0) {
            if (!got_any) {
                jout_err(stderr, OCTL_TIMEOUT, "tle_status_timeout",
                         "no TleStatus within %d ms", ctx->timeout_ms);
                conn_close();
                return OCTL_TIMEOUT;
            }
            break;
        }
        TleStatus_t s;
        if (!decodeTleStatusPacketStructure(&pkt, &s)) {
            jout_err(stderr, OCTL_INTERNAL, "decode_failed", "TleStatus decode failed");
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

int cmd_tle(octl_ctx_t *ctx)
{
    if (ctx->npos < 2) {
        jout_err(stderr, OCTL_USAGE, "missing_subverb",
                 "tle requires subverb: start | stop | status");
        return OCTL_USAGE;
    }
    const char *sub = ctx->pos[1];
    if (strcmp(sub, "start")  == 0) return tle_start(ctx);
    if (strcmp(sub, "stop")   == 0) return tle_stop(ctx);
    if (strcmp(sub, "status") == 0) return tle_status(ctx);

    jout_err(stderr, OCTL_USAGE, "unknown_subverb",
             "unknown tle subverb: %s", sub);
    return OCTL_USAGE;
}
