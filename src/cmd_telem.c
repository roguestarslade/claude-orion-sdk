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
    case ORION_MODE_NUDGE_TRACK:        return "nudge_track";
    case ORION_MODE_SECONDARY_TRACK:    return "secondary_track";
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

static const char *range_src_name(int r)
{
    switch (r) {
    case RANGE_SRC_NONE:          return "none";
    case RANGE_SRC_SKYLINK:       return "skylink";
    case RANGE_SRC_LASER:         return "laser";
    case RANGE_SRC_OTHER:         return "other";
    case RANGE_SRC_INTERNAL:      return "internal";
    case RANGE_SRC_INTERNAL_DTED: return "internal_dted";
    default:                      return "unknown";
    }
}

static const char *ins_rot_name(int r)
{
    switch (r) {
    case insInGimbalNative: return "gimbal_native";
    case insInPlatform:     return "platform";
    case insInPayloadBall:  return "payload_ball";
    default:                return "unknown";
    }
}

static void emit_telem(jout_t *j, const GeolocateTelemetry_t *geo)
{
    const GeolocateTelemetryCore_t *b = &geo->base;

    jout_obj_open(j);
    jout_kv_str(j, "mode", mode_name(b->mode));
    jout_kv_int(j, "mode_id", b->mode);
    jout_kv_int(j, "camera_index", b->cameraIndex);

    jout_key(j, "los");
    jout_obj_open(j);
    jout_kv_dbl(j, "pan_rad",  b->pan);
    jout_kv_dbl(j, "tilt_rad", b->tilt);
    jout_kv_dbl(j, "pan_deg",  rad2deg(b->pan));
    jout_kv_dbl(j, "tilt_deg", rad2deg(b->tilt));
    jout_kv_dbl(j, "hfov_rad", b->hfov);
    jout_kv_dbl(j, "vfov_rad", b->vfov);
    jout_kv_dbl(j, "hfov_deg", rad2deg(b->hfov));
    jout_kv_dbl(j, "vfov_deg", rad2deg(b->vfov));
    jout_obj_close(j);

    jout_key(j, "alignment");
    jout_obj_open(j);
    jout_kv_dbl(j, "pan_rad",  b->panAlignment);
    jout_kv_dbl(j, "tilt_rad", b->tiltAlignment);
    jout_kv_dbl(j, "pan_deg",  rad2deg(b->panAlignment));
    jout_kv_dbl(j, "tilt_deg", rad2deg(b->tiltAlignment));
    jout_obj_close(j);

    jout_key(j, "position");
    jout_obj_open(j);
    jout_kv_dbl(j, "lat_rad", b->posLat);
    jout_kv_dbl(j, "lon_rad", b->posLon);
    jout_kv_dbl(j, "lat_deg", rad2deg(b->posLat));
    jout_kv_dbl(j, "lon_deg", rad2deg(b->posLon));
    jout_kv_dbl(j, "alt_m",   b->posAlt);
    jout_kv_dbl(j, "geoid_undulation_m", b->geoidUndulation);
    jout_obj_close(j);

    jout_key(j, "vel_ned");
    jout_arr_open(j);
    jout_dbl(j, b->velNED[0]);
    jout_dbl(j, b->velNED[1]);
    jout_dbl(j, b->velNED[2]);
    jout_arr_close(j);

    jout_key(j, "gimbal_quat");
    jout_arr_open(j);
    jout_dbl(j, b->gimbalQuat[0]);
    jout_dbl(j, b->gimbalQuat[1]);
    jout_dbl(j, b->gimbalQuat[2]);
    jout_dbl(j, b->gimbalQuat[3]);
    jout_arr_close(j);

    jout_key(j, "los_ecef");
    jout_arr_open(j);
    jout_dbl(j, b->losECEF[0]);
    jout_dbl(j, b->losECEF[1]);
    jout_dbl(j, b->losECEF[2]);
    jout_arr_close(j);

    jout_kv_int(j, "pixel_w", b->pixelWidth);
    jout_kv_int(j, "pixel_h", b->pixelHeight);

    jout_key(j, "stab");
    jout_obj_open(j);
    jout_key(j, "image_shifts_rad"); jout_arr_open(j);
    jout_dbl(j, b->imageShifts[0]);
    jout_dbl(j, b->imageShifts[1]);
    jout_arr_close(j);
    jout_kv_dbl(j, "image_shift_delta_time_s", b->imageShiftDeltaTime);
    jout_kv_dbl(j, "image_shift_confidence",   b->imageShiftConfidence);
    jout_key(j, "output_shifts_rad"); jout_arr_open(j);
    jout_dbl(j, b->outputShifts[0]);
    jout_dbl(j, b->outputShifts[1]);
    jout_arr_close(j);
    jout_kv_dbl(j, "image_rotation_rad", b->imageRotation);
    jout_kv_dbl(j, "image_rotation_deg", rad2deg(b->imageRotation));
    jout_obj_close(j);

    jout_key(j, "range");
    jout_obj_open(j);
    jout_kv_str(j, "source",    range_src_name(b->rangeSource));
    jout_kv_int(j, "source_id", b->rangeSource);
    jout_obj_close(j);

    jout_key(j, "path");
    jout_obj_open(j);
    jout_kv_dbl (j, "progress",   b->pathProgress);
    jout_kv_dbl (j, "stare_time_s", b->stareTime);
    jout_kv_int (j, "from_index", b->pathFrom);
    jout_kv_int (j, "to_index",   b->pathTo);
    jout_obj_close(j);

    jout_key(j, "track");
    jout_obj_open(j);
    jout_kv_bool(j, "has_track_data", b->hasTrackData);
    jout_key(j, "primary_track_data");
    jout_obj_open(j);
    jout_key(j, "position"); jout_arr_open(j);
    jout_dbl(j, b->primaryTrackData.Pos[0]);
    jout_dbl(j, b->primaryTrackData.Pos[1]);
    jout_arr_close(j);
    jout_kv_dbl (j, "size",       b->primaryTrackData.Size);
    jout_kv_dbl (j, "confidence", b->primaryTrackData.Confidence);
    jout_kv_bool(j, "coasting",   b->primaryTrackData.Coasting);
    jout_kv_bool(j, "active",     b->primaryTrackData.Active);
    jout_obj_close(j);
    jout_obj_close(j);

    jout_key(j, "ins");
    jout_obj_open(j);
    jout_kv_str(j, "rotation_option",    ins_rot_name(b->insRotationOption));
    jout_kv_int(j, "rotation_option_id", b->insRotationOption);
    jout_key(j, "quat"); jout_arr_open(j);
    jout_dbl(j, b->insQuat[0]);
    jout_dbl(j, b->insQuat[1]);
    jout_dbl(j, b->insQuat[2]);
    jout_dbl(j, b->insQuat[3]);
    jout_arr_close(j);
    jout_obj_close(j);

    jout_key(j, "time");
    jout_obj_open(j);
    jout_kv_uint(j, "system_ms",   b->systemTime);
    jout_kv_uint(j, "gps_itow_ms", b->gpsITOW);
    jout_kv_uint(j, "gps_week",    b->gpsWeek);
    jout_kv_int (j, "leap_seconds", b->leapSeconds);
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
