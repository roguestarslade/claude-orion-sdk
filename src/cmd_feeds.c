#include "orionctl.h"
#include "conn.h"
#include "json_out.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "OrionPublicPacket.h"
#include "OrionPublicPacketShim.h"

static int open_conn(octl_ctx_t *ctx)
{
    if (octl_resolve_ip(ctx) != 0) { jout_err(stderr, OCTL_CONN_FAILED, "no_ip", "no gimbal IP"); return -1; }
    if (conn_open(ctx->ip) != 0)   { jout_err(stderr, OCTL_CONN_FAILED, "connect_failed", "could not open TCP to %s", ctx->ip); return -1; }
    conn_drain(OCTL_DRAIN_MS);
    return 0;
}

static int parse_gps_source(const char *s, gpsSource_t *out)
{
    if (!s) { *out = externalSource; return 0; }
    if (!strcmp(s, "external"))  { *out = externalSource;  return 0; }
    if (!strcmp(s, "ublox"))     { *out = ubloxSource;     return 0; }
    if (!strcmp(s, "mavlink"))   { *out = mavlinkSource;   return 0; }
    if (!strcmp(s, "nmea"))      { *out = nmeaSource;      return 0; }
    if (!strcmp(s, "novatel"))   { *out = novatelSource;   return 0; }
    if (!strcmp(s, "autopilot")) { *out = autopilotSource; return 0; }
    if (!strcmp(s, "piksi"))     { *out = piksiSource;     return 0; }
    if (!strcmp(s, "apnt"))      { *out = apntSource;      return 0; }
    return -1;
}

int cmd_gps_feed(octl_ctx_t *ctx)
{
    if (ctx->lla_set != 7) {
        jout_err(stderr, OCTL_USAGE, "missing_lla",
                 "gps-feed requires --lat <rad> --lon <rad> --alt <m>");
        return OCTL_USAGE;
    }
    gpsSource_t src;
    if (parse_gps_source(ctx->gps_source, &src) != 0) {
        jout_err(stderr, OCTL_USAGE, "bad_source",
                 "--source must be external|ublox|mavlink|nmea|novatel|autopilot|piksi|apnt");
        return OCTL_USAGE;
    }
    if (open_conn(ctx) != 0) return OCTL_CONN_FAILED;

    GpsData_t g; memset(&g, 0, sizeof(g));
    g.Latitude  = ctx->lat;
    g.Longitude = ctx->lon;
    g.Altitude  = ctx->alt;
    if (ctx->vel_n_set) g.VelNED[0] = (float)ctx->vel_n;
    if (ctx->vel_e_set) g.VelNED[1] = (float)ctx->vel_e;
    if (ctx->vel_d_set) g.VelNED[2] = (float)ctx->vel_d;
    g.source = src;
    g.FixType = 3; /* gnssFix3D */
    g.FixState = 1;
    g.TrackedSats = 8;

    int sent_count = 0;
    if (ctx->rate_hz_set && ctx->rate_hz > 0.0) {
        long usec = (long)(1.0e6 / ctx->rate_hz);
        jout_t j; jout_init(&j, stdout);
        jout_obj_open(&j);
        jout_kv_dbl(&j, "rate_hz", ctx->rate_hz);
        jout_key(&j, "note"); jout_str(&j, "streaming; ctrl-c to stop");
        jout_obj_close(&j);
        jout_done(&j);
        while (1) {
            OrionPkt_t pkt;
            encodeGpsDataPacketStructure(&pkt, &g);
            if (conn_send(&pkt) != 0) break;
            sent_count++;
            usleep(usec);
        }
        conn_close();
        return OCTL_OK;
    } else {
        OrionPkt_t pkt;
        encodeGpsDataPacketStructure(&pkt, &g);
        int sent = (conn_send(&pkt) == 0);
        if (sent) sent_count++;
        conn_close();
        jout_t j; jout_init(&j, stdout);
        jout_obj_open(&j);
        jout_kv_int(&j, "sent_count", sent_count);
        jout_kv_dbl(&j, "lat_rad", g.Latitude);
        jout_kv_dbl(&j, "lon_rad", g.Longitude);
        jout_kv_dbl(&j, "alt_m",   g.Altitude);
        jout_kv_int(&j, "source_id", g.source);
        jout_obj_close(&j);
        jout_done(&j);
        return sent ? OCTL_OK : OCTL_CONN_FAILED;
    }
}

int cmd_heading_feed(octl_ctx_t *ctx)
{
    if (!ctx->heading_set) {
        jout_err(stderr, OCTL_USAGE, "missing_heading", "heading-feed requires --heading <rad>");
        return OCTL_USAGE;
    }
    if (open_conn(ctx) != 0) return OCTL_CONN_FAILED;
    float noise = ctx->heading_acc_set ? (float)ctx->heading_acc : 0.0f;
    OrionPkt_t pkt;
    encodeOrionExtHeadingDataPacket(&pkt, (float)ctx->heading, noise, 0u, 0u, 0.0f);
    int sent = (conn_send(&pkt) == 0);
    conn_close();
    jout_t j; jout_init(&j, stdout);
    jout_obj_open(&j);
    jout_kv_dbl (&j, "heading_rad", ctx->heading);
    jout_kv_dbl (&j, "noise_rad",   noise);
    jout_kv_bool(&j, "sent",        sent);
    jout_obj_close(&j);
    jout_done(&j);
    return sent ? OCTL_OK : OCTL_CONN_FAILED;
}

int cmd_autopilot_feed(octl_ctx_t *ctx)
{
    if (ctx->npos < 2) {
        jout_err(stderr, OCTL_USAGE, "missing_args", "autopilot-feed requires Field=Value pairs");
        return OCTL_USAGE;
    }
    OrionAutopilotData_t a; memset(&a, 0, sizeof(a));
    int mutations = 0;
    for (int i = 1; i < ctx->npos; i++) {
        const char *kv = ctx->pos[i];
        const char *eq = strchr(kv, '=');
        if (!eq || eq == kv) {
            jout_err(stderr, OCTL_USAGE, "bad_kv", "expected Field=Value: %s", kv);
            return OCTL_USAGE;
        }
        char name[32]; size_t L = (size_t)(eq - kv);
        if (L >= sizeof(name)) { jout_err(stderr, OCTL_USAGE, "field_too_long", "field too long"); return OCTL_USAGE; }
        memcpy(name, kv, L); name[L] = '\0';
        const char *v = eq + 1; char *end;
        if      (!strcmp(name, "HasIAS"))   { a.HasIAS   = (unsigned)strtoul(v, &end, 0); }
        else if (!strcmp(name, "HasTAS"))   { a.HasTAS   = (unsigned)strtoul(v, &end, 0); }
        else if (!strcmp(name, "IsFlying")) { a.IsFlying = (unsigned)strtoul(v, &end, 0); }
        else if (!strcmp(name, "CommGood")) { a.CommGood = (uint8_t)strtoul(v, &end, 0); }
        else if (!strcmp(name, "Agl"))      { a.Agl      = (int16_t)strtol(v, &end, 0); }
        else if (!strcmp(name, "IAS"))      { a.IAS      = (float)strtod(v, &end); }
        else if (!strcmp(name, "TAS"))      { a.TAS      = (float)strtod(v, &end); }
        else { jout_err(stderr, OCTL_USAGE, "unknown_field", "unknown field: %s", name); return OCTL_USAGE; }
        if (*end != '\0' || end == v) { jout_err(stderr, OCTL_USAGE, "bad_value", "bad value for %s: %s", name, v); return OCTL_USAGE; }
        mutations++;
    }
    if (open_conn(ctx) != 0) return OCTL_CONN_FAILED;
    OrionPkt_t pkt;
    encodeOrionAutopilotDataPacketStructure(&pkt, &a);
    int sent = (conn_send(&pkt) == 0);
    conn_close();

    jout_t j; jout_init(&j, stdout);
    jout_obj_open(&j);
    jout_kv_int (&j, "mutations", mutations);
    jout_kv_bool(&j, "sent",      sent);
    jout_kv_bool(&j, "has_ias",   a.HasIAS);
    jout_kv_bool(&j, "has_tas",   a.HasTAS);
    jout_kv_bool(&j, "is_flying", a.IsFlying);
    jout_kv_int (&j, "comm_good", a.CommGood);
    jout_kv_int (&j, "agl_m",     a.Agl);
    jout_kv_dbl (&j, "ias_mps",   a.IAS);
    jout_kv_dbl (&j, "tas_mps",   a.TAS);
    jout_obj_close(&j);
    jout_done(&j);
    return sent ? OCTL_OK : OCTL_CONN_FAILED;
}

int cmd_geoid_feed(octl_ctx_t *ctx)
{
    if (!ctx->undulation_set) {
        jout_err(stderr, OCTL_USAGE, "missing_undulation", "geoid-feed requires --undulation <m>");
        return OCTL_USAGE;
    }
    if (open_conn(ctx) != 0) return OCTL_CONN_FAILED;
    OrionPkt_t pkt;
    encodeGeoidUndulationPacket(&pkt, (float)ctx->undulation);
    int sent = (conn_send(&pkt) == 0);
    conn_close();
    jout_t j; jout_init(&j, stdout);
    jout_obj_open(&j);
    jout_kv_dbl (&j, "undulation_m", ctx->undulation);
    jout_kv_bool(&j, "sent",         sent);
    jout_obj_close(&j);
    jout_done(&j);
    return sent ? OCTL_OK : OCTL_CONN_FAILED;
}
