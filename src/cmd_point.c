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

static int require_motion(octl_ctx_t *ctx, const char *what)
{
    if (gate_motion_allowed(ctx)) return 0;
    jout_err(stderr, OCTL_MOTION_GATE, "motion_gated",
             "%s requires --allow-motion or ORION_ALLOW_MOTION=1", what);
    return -1;
}

static int open_for_write(octl_ctx_t *ctx)
{
    if (octl_resolve_ip(ctx) != 0) {
        jout_err(stderr, OCTL_CONN_FAILED, "no_ip", "no gimbal IP");
        return -1;
    }
    if (conn_open(ctx->ip) != 0) {
        jout_err(stderr, OCTL_CONN_FAILED, "connect_failed",
                 "could not open TCP to %s", ctx->ip);
        return -1;
    }
    conn_drain(OCTL_DRAIN_MS);
    return 0;
}

static int parse_triple(const char *s, double *a, double *b, double *c)
{
    return sscanf(s, "%lf,%lf,%lf", a, b, c) == 3 ? 0 : -1;
}

static int p_geopoint(octl_ctx_t *ctx)
{
    if (require_motion(ctx, "point geopoint") != 0) return OCTL_MOTION_GATE;
    if (ctx->npos < 5) {
        jout_err(stderr, OCTL_USAGE, "missing_args",
                 "point geopoint requires <lat> <lon> <alt>");
        return OCTL_USAGE;
    }
    char *end;
    double lat = strtod(ctx->pos[2], &end); if (*end) goto bad_coords;
    double lon = strtod(ctx->pos[3], &end); if (*end) goto bad_coords;
    double alt = strtod(ctx->pos[4], &end); if (*end) goto bad_coords;

    float vel[3] = {0};
    if (ctx->vel_n_set) vel[0] = (float)ctx->vel_n;
    if (ctx->vel_e_set) vel[1] = (float)ctx->vel_e;
    if (ctx->vel_d_set) vel[2] = (float)ctx->vel_d;

    int opts = geopointNone;
    if (ctx->stare_flag)   opts |= geopointStare;
    if (ctx->closure_flag) opts |= geopointClosure;

    if (open_for_write(ctx) != 0) return OCTL_CONN_FAILED;
    OrionPkt_t pkt;
    encodeGeopointCmdPacket(&pkt, lat, lon, alt, vel, 0.0f, (geopointOptions)opts);
    if (conn_send(&pkt) != 0) {
        jout_err(stderr, OCTL_CONN_FAILED, "send_failed", "could not send GeopointCmd");
        conn_close();
        return OCTL_CONN_FAILED;
    }
    OrionPkt_t echo;
    int got = (conn_wait_for(ORION_PKT_GEOPOINT_CMD, &echo, ctx->timeout_ms) == 0);
    conn_close();

    jout_t j; jout_init(&j, stdout);
    jout_obj_open(&j);
    jout_kv_str(&j, "verb", "geopoint");
    jout_kv_dbl(&j, "lat_rad", lat); jout_kv_dbl(&j, "lon_rad", lon); jout_kv_dbl(&j, "alt_m", alt);
    jout_kv_dbl(&j, "lat_deg", rad2deg(lat)); jout_kv_dbl(&j, "lon_deg", rad2deg(lon));
    jout_key(&j, "vel_ned_mps"); jout_arr_open(&j);
    jout_dbl(&j, vel[0]); jout_dbl(&j, vel[1]); jout_dbl(&j, vel[2]);
    jout_arr_close(&j);
    jout_kv_uint(&j, "options",   opts);
    jout_kv_bool(&j, "stare",     (opts & geopointStare) != 0);
    jout_kv_bool(&j, "closure",   (opts & geopointClosure) != 0);
    jout_kv_bool(&j, "echo_seen", got);
    jout_obj_close(&j);
    jout_done(&j);
    return OCTL_OK;

bad_coords:
    jout_err(stderr, OCTL_USAGE, "bad_coords", "lat/lon/alt must be doubles (radians/m)");
    return OCTL_USAGE;
}

static int send_path(octl_ctx_t *ctx, const OrionPath_t *p, const char *verb)
{
    if (open_for_write(ctx) != 0) return OCTL_CONN_FAILED;
    OrionPkt_t pkt;
    encodeOrionPathPacketStructure(&pkt, p);
    if (conn_send(&pkt) != 0) {
        jout_err(stderr, OCTL_CONN_FAILED, "send_failed", "could not send Path");
        conn_close();
        return OCTL_CONN_FAILED;
    }
    OrionPkt_t echo;
    int got = (conn_wait_for(ORION_PKT_PATH, &echo, ctx->timeout_ms) == 0);
    conn_close();

    jout_t j; jout_init(&j, stdout);
    jout_obj_open(&j);
    jout_kv_str(&j, "verb",                 verb);
    jout_kv_int(&j, "num_points",           p->numPoints);
    jout_kv_bool(&j,"point_down",           p->pointDown);
    jout_kv_int(&j, "num_cross_track_steps",p->numCrossTrackSteps);
    jout_kv_dbl(&j, "along_track_step_rad", p->alongTrackStepAngle);
    jout_kv_dbl(&j, "cross_track_step_ratio",p->crossTrackStepRatio);
    jout_kv_bool(&j,"echo_seen",            got);
    jout_obj_close(&j);
    jout_done(&j);
    return OCTL_OK;
}

static void wgs84_lla_to_ecef(double lat, double lon, double alt, double out[3])
{
    /* WGS-84 ellipsoid */
    const double a = 6378137.0;
    const double f = 1.0 / 298.257223563;
    const double e2 = f * (2.0 - f);
    double s = sin(lat), c = cos(lat);
    double N = a / sqrt(1.0 - e2 * s * s);
    out[0] = (N + alt) * c * cos(lon);
    out[1] = (N + alt) * c * sin(lon);
    out[2] = (N * (1.0 - e2) + alt) * s;
}

static int p_path(octl_ctx_t *ctx)
{
    if (require_motion(ctx, "point path") != 0) return OCTL_MOTION_GATE;
    /* pos[0]=point, pos[1]=path, pos[2..]=lat,lon,alt triples */
    int nptr = ctx->npos - 2;
    if (nptr < 1) {
        jout_err(stderr, OCTL_USAGE, "missing_args",
                 "point path requires at least one <lat,lon,alt> triple");
        return OCTL_USAGE;
    }
    if (nptr > MAX_PATH_POINTS) {
        jout_err(stderr, OCTL_USAGE, "too_many_points",
                 "point path supports up to %d waypoints, got %d", MAX_PATH_POINTS, nptr);
        return OCTL_USAGE;
    }
    OrionPath_t p; memset(&p, 0, sizeof(p));
    p.numPoints = (uint8_t)nptr;
    p.pointDown = 0;
    p.numCrossTrackSteps = ctx->step_count_set && ctx->step_count >= 0 ? (unsigned)ctx->step_count : 0;
    for (int i = 0; i < nptr; i++) {
        double lat, lon, alt;
        if (parse_triple(ctx->pos[2 + i], &lat, &lon, &alt) != 0) {
            jout_err(stderr, OCTL_USAGE, "bad_triple",
                     "waypoint %d must be lat,lon,alt", i);
            return OCTL_USAGE;
        }
        double e[3];
        wgs84_lla_to_ecef(lat, lon, alt, e);
        p.Point[i].posEcef[0] = (float)e[0];
        p.Point[i].posEcef[1] = (float)e[1];
        p.Point[i].posEcef[2] = (float)e[2];
    }
    return send_path(ctx, &p, "path");
}

static int p_nadir(octl_ctx_t *ctx)
{
    if (require_motion(ctx, "point nadir") != 0) return OCTL_MOTION_GATE;
    OrionPath_t p; memset(&p, 0, sizeof(p));
    p.numPoints = 0;
    p.pointDown = 1;
    return send_path(ctx, &p, "nadir");
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

static int p_home_or_stow(octl_ctx_t *ctx, int home)
{
    if (require_motion(ctx, home ? "point home" : "point stow") != 0) return OCTL_MOTION_GATE;
    if (open_for_write(ctx) != 0) return OCTL_CONN_FAILED;

    OrionPositions_t pos;
    int rc = read_positions(ctx, &pos);
    if (rc != 0) {
        jout_err(stderr, rc == -2 ? OCTL_TIMEOUT : OCTL_INTERNAL,
                 rc == -2 ? "positions_timeout" : "positions_failed",
                 "Positions pre-read failed (rc=%d)", rc);
        conn_close();
        return rc == -2 ? OCTL_TIMEOUT : OCTL_INTERNAL;
    }
    int slot = home ? POSITION_HOME : POSITION_STOW;
    if (slot >= pos.NumPositions || !pos.PosPreset[slot].Enabled) {
        jout_err(stderr, OCTL_UNSUPPORTED, "position_disabled",
                 "%s position is not configured/enabled", home ? "home" : "stow");
        conn_close();
        return OCTL_UNSUPPORTED;
    }
    OrionCmd_t c; memset(&c, 0, sizeof(c));
    c.Mode = ORION_MODE_POSITION;
    c.Target[0] = pos.PosPreset[slot].Pan;
    c.Target[1] = pos.PosPreset[slot].Tilt;
    c.Stabilized = 1;

    OrionPkt_t pkt;
    encodeOrionCmdPacket(&pkt, &c);
    if (conn_send(&pkt) != 0) {
        jout_err(stderr, OCTL_CONN_FAILED, "send_failed", "could not send OrionCmd");
        conn_close();
        return OCTL_CONN_FAILED;
    }
    OrionPkt_t echo;
    int got = (conn_wait_for(ORION_PKT_CMD, &echo, ctx->timeout_ms) == 0);
    conn_close();

    jout_t j; jout_init(&j, stdout);
    jout_obj_open(&j);
    jout_kv_str(&j, "verb", home ? "home" : "stow");
    jout_kv_dbl(&j, "pan_rad",  c.Target[0]);
    jout_kv_dbl(&j, "tilt_rad", c.Target[1]);
    jout_kv_dbl(&j, "pan_deg",  rad2deg(c.Target[0]));
    jout_kv_dbl(&j, "tilt_deg", rad2deg(c.Target[1]));
    jout_kv_bool(&j,"echo_seen", got);
    jout_obj_close(&j);
    jout_done(&j);
    return OCTL_OK;
}

int cmd_point(octl_ctx_t *ctx)
{
    if (ctx->npos < 2) {
        jout_err(stderr, OCTL_USAGE, "missing_subverb",
                 "point requires subverb: geopoint|path|nadir|home|stow");
        return OCTL_USAGE;
    }
    const char *sub = ctx->pos[1];
    if (strcmp(sub, "geopoint") == 0) return p_geopoint(ctx);
    if (strcmp(sub, "path")     == 0) return p_path(ctx);
    if (strcmp(sub, "nadir")    == 0) return p_nadir(ctx);
    if (strcmp(sub, "home")     == 0) return p_home_or_stow(ctx, 1);
    if (strcmp(sub, "stow")     == 0) return p_home_or_stow(ctx, 0);
    jout_err(stderr, OCTL_USAGE, "unknown_subverb",
             "unknown point subverb: %s", sub);
    return OCTL_USAGE;
}
