#include "orionctl.h"
#include "conn.h"
#include "json_out.h"

#include <math.h>
#include <stdio.h>

#include "OrionPublicPacket.h"
#include "OrionPublicPacketShim.h"
#include "GeolocateTelemetry.h"

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
    case ORION_MODE_UNKNOWN:            return "unknown";
    default:                            return "unknown";
    }
}

static void emit_telem(jout_t *j, const GeolocateTelemetry_t *geo)
{
    jout_obj_open(j);
    jout_kv_str(j, "mode", mode_name(geo->base.mode));
    jout_kv_int(j, "mode_id", geo->base.mode);
    jout_kv_int(j, "camera_index", geo->base.cameraIndex);

    jout_key(j, "los");
    jout_obj_open(j);
    jout_kv_dbl(j, "pan_rad",  geo->base.pan);
    jout_kv_dbl(j, "tilt_rad", geo->base.tilt);
    jout_kv_dbl(j, "pan_deg",  rad2deg(geo->base.pan));
    jout_kv_dbl(j, "tilt_deg", rad2deg(geo->base.tilt));
    jout_kv_dbl(j, "hfov_rad", geo->base.hfov);
    jout_kv_dbl(j, "vfov_rad", geo->base.vfov);
    jout_kv_dbl(j, "hfov_deg", rad2deg(geo->base.hfov));
    jout_kv_dbl(j, "vfov_deg", rad2deg(geo->base.vfov));
    jout_obj_close(j);

    jout_key(j, "position");
    jout_obj_open(j);
    jout_kv_dbl(j, "lat_rad", geo->base.posLat);
    jout_kv_dbl(j, "lon_rad", geo->base.posLon);
    jout_kv_dbl(j, "lat_deg", rad2deg(geo->base.posLat));
    jout_kv_dbl(j, "lon_deg", rad2deg(geo->base.posLon));
    jout_kv_dbl(j, "alt_m",   geo->base.posAlt);
    jout_obj_close(j);

    jout_key(j, "vel_ned");
    jout_arr_open(j);
    jout_dbl(j, geo->base.velNED[0]);
    jout_dbl(j, geo->base.velNED[1]);
    jout_dbl(j, geo->base.velNED[2]);
    jout_arr_close(j);

    jout_key(j, "gimbal_quat");
    jout_arr_open(j);
    jout_dbl(j, geo->base.gimbalQuat[0]);
    jout_dbl(j, geo->base.gimbalQuat[1]);
    jout_dbl(j, geo->base.gimbalQuat[2]);
    jout_dbl(j, geo->base.gimbalQuat[3]);
    jout_arr_close(j);

    jout_key(j, "los_ecef");
    jout_arr_open(j);
    jout_dbl(j, geo->base.losECEF[0]);
    jout_dbl(j, geo->base.losECEF[1]);
    jout_dbl(j, geo->base.losECEF[2]);
    jout_arr_close(j);

    jout_kv_int(j, "pixel_w", geo->base.pixelWidth);
    jout_kv_int(j, "pixel_h", geo->base.pixelHeight);

    jout_key(j, "time");
    jout_obj_open(j);
    jout_kv_uint(j, "system_ms", geo->base.systemTime);
    jout_kv_uint(j, "gps_itow_ms", geo->base.gpsITOW);
    jout_kv_uint(j, "gps_week",   geo->base.gpsWeek);
    jout_obj_close(j);

    jout_obj_close(j);
}

int cmd_telem(octl_ctx_t *ctx)
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

    int n = ctx->watch ? (ctx->watch_n > 0 ? ctx->watch_n : 0) : 1;
    int i = 0;
    while (n == 0 || i < n) {
        OrionPkt_t pkt;
        int rc = conn_wait_for(ORION_PKT_GEOLOCATE_TELEMETRY, &pkt, ctx->timeout_ms);
        if (rc != 0) {
            jout_err(stderr, OCTL_TIMEOUT, "telem_timeout",
                     "no GeolocateTelemetry within %d ms", ctx->timeout_ms);
            conn_close();
            return OCTL_TIMEOUT;
        }
        GeolocateTelemetry_t geo;
        if (!DecodeGeolocateTelemetry(&pkt, &geo)) {
            jout_err(stderr, OCTL_INTERNAL, "decode_failed",
                     "GeolocateTelemetry decode failed");
            conn_close();
            return OCTL_INTERNAL;
        }
        jout_t j;
        jout_init(&j, stdout);
        emit_telem(&j, &geo);
        jout_done(&j);
        i++;
        if (!ctx->watch) break;
    }

    conn_close();
    return OCTL_OK;
}
