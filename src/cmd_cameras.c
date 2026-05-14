#include "orionctl.h"
#include "cache.h"
#include "cmd_cameras_internal.h"
#include "conn.h"
#include "json_out.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "OrionPublicPacket.h"
#include "OrionPublicPacketShim.h"

static const char *type_name(OrionCameraType_t t)
{
    switch (t) {
    case CAMERA_TYPE_NONE:    return "none";
    case CAMERA_TYPE_VISIBLE: return "visible";
    case CAMERA_TYPE_SWIR:    return "swir";
    case CAMERA_TYPE_MWIR:    return "mwir";
    case CAMERA_TYPE_LWIR:    return "lwir";
    case CAMERA_TYPE_SPOTTER: return "spotter";
    case CAMERA_TYPE_NIR:     return "nir";
    case CAMERA_TYPE_1064:    return "mono_1064";
    default:                  return "unknown";
    }
}

static const char *proto_name(OrionCameraProtocol_t p)
{
    switch (p) {
    case CAMERA_PROTO_UNKNOWN: return "unknown";
    case CAMERA_PROTO_FLIR:    return "flir";
    case CAMERA_PROTO_APTINA:  return "aptina";
    case CAMERA_PROTO_ZAFIRO:  return "zafiro";
    case CAMERA_PROTO_HITACHI: return "hitachi";
    case CAMERA_PROTO_BAE:     return "bae";
    case CAMERA_PROTO_SONY:    return "sony";
    case CAMERA_PROTO_KTNC:    return "ktnc";
    case CAMERA_PROTO_ATTOLLO: return "attollo";
    case CAMERA_PROTO_MIRA:    return "mira";
    case CAMERA_PROTO_ALVIUM:  return "alvium";
    case CAMERA_PROTO_OMNIVIS: return "omnivis";
    case CAMERA_PROTO_SIERRA:  return "sierra";
    case CAMERA_PROTO_SIONYX:  return "sionyx";
    default:                   return "unknown";
    }
}

void cameras_emit(jout_t *j, const char *ip, const OrionCameras_t *c, int active_idx)
{
    jout_obj_open(j);
    if (ip) jout_kv_str(j, "ip", ip);
    jout_kv_int(j, "num_cameras", c->NumCameras);
    if (active_idx >= 0) jout_kv_int(j, "active_index", active_idx);
    else                 jout_kv_null(j, "active_index");

    jout_key(j, "slots");
    jout_arr_open(j);
    for (int i = 0; i < NUM_CAMERAS; i++) {
        const OrionCamSettings_t *s = &c->OrionCamSettings[i];
        int populated = (s->Type != CAMERA_TYPE_NONE);

        jout_obj_open(j);
        jout_kv_int (j, "index", i);
        jout_kv_bool(j, "populated", populated);
        jout_kv_str (j, "type", type_name(s->Type));
        jout_kv_int (j, "type_id", s->Type);
        jout_kv_str (j, "proto", proto_name(s->Proto));
        jout_kv_int (j, "proto_id", s->Proto);
        jout_kv_bool(j, "is_active", populated && i == active_idx);

        jout_key(j, "optics");
        jout_obj_open(j);
        jout_kv_dbl(j, "pixel_pitch_mm",     s->PixelPitch);
        jout_kv_int(j, "array_width_px",     s->ArrayWidth);
        jout_kv_int(j, "array_height_px",    s->ArrayHeight);
        jout_kv_dbl(j, "min_focal_length_mm", s->MinFocalLength);
        jout_kv_dbl(j, "max_focal_length_mm", s->MaxFocalLength);
        double maxzoom = 0.0;
        if (s->MinFocalLength > 0 && s->MaxFocalLength > 0)
            maxzoom = (double)s->MaxFocalLength / (double)s->MinFocalLength;
        if (maxzoom > 0.0) jout_kv_dbl(j, "max_zoom", maxzoom);
        else               jout_kv_null(j, "max_zoom");
        jout_kv_dbl(j, "frame_rate_hz", s->FrameRate);
        jout_obj_close(j);

        jout_kv_int(j, "gpio",        s->Gpio);
        jout_kv_int(j, "gpio_active", s->GpioActiveState);

        jout_key(j, "alignment");
        jout_obj_open(j);
        jout_key(j, "min_rad"); jout_arr_open(j);
        jout_dbl(j, s->AlignMin[0]);
        jout_dbl(j, s->AlignMin[1]);
        jout_arr_close(j);
        jout_key(j, "max_rad"); jout_arr_open(j);
        jout_dbl(j, s->AlignMax[0]);
        jout_dbl(j, s->AlignMax[1]);
        jout_arr_close(j);
        jout_key(j, "min_deg"); jout_arr_open(j);
        jout_dbl(j, s->AlignMin[0] * (180.0 / M_PI));
        jout_dbl(j, s->AlignMin[1] * (180.0 / M_PI));
        jout_arr_close(j);
        jout_key(j, "max_deg"); jout_arr_open(j);
        jout_dbl(j, s->AlignMax[0] * (180.0 / M_PI));
        jout_dbl(j, s->AlignMax[1] * (180.0 / M_PI));
        jout_arr_close(j);
        jout_obj_close(j);
        jout_obj_close(j);
    }
    jout_arr_close(j);
    jout_obj_close(j);
}

#include "GeolocateTelemetry.h"

int telem_active_index(int timeout_ms)
{
    OrionPkt_t pkt;
    if (conn_wait_for(ORION_PKT_GEOLOCATE_TELEMETRY, &pkt, timeout_ms) != 0)
        return -1;
    GeolocateTelemetry_t geo;
    if (!DecodeGeolocateTelemetry(&pkt, &geo)) return -1;
    return geo.base.cameraIndex;
}

int cmd_cameras_fetch(octl_ctx_t *ctx, OrionCameras_t *out, int *active_idx_out)
{
    OrionPkt_t req, resp;
    MakeOrionPacket(&req, getOrionCamerasPacketID(), 0);
    if (conn_send(&req) != 0) return -1;

    if (conn_wait_for(ORION_PKT_CAMERAS, &resp, ctx->timeout_ms) != 0) return -2;
    if (!decodeOrionCamerasPacketStructure(&resp, out)) return -3;

    cache_cameras_bin_write(ctx->ip, &resp, sizeof(resp));

    *active_idx_out = telem_active_index(ctx->timeout_ms);
    return 0;
}

int cameras_load_or_fetch(octl_ctx_t *ctx, OrionCameras_t *out, int *from_cache)
{
    OrionPkt_t pkt;
    long n = cache_cameras_bin_read(ctx->ip, &pkt, sizeof(pkt));
    if (n == (long)sizeof(pkt) && pkt.ID == ORION_PKT_CAMERAS &&
        decodeOrionCamerasPacketStructure(&pkt, out)) {
        if (from_cache) *from_cache = 1;
        return 0;
    }
    OrionPkt_t req, resp;
    MakeOrionPacket(&req, getOrionCamerasPacketID(), 0);
    if (conn_send(&req) != 0) return -1;
    if (conn_wait_for(ORION_PKT_CAMERAS, &resp, ctx->timeout_ms) != 0) return -2;
    if (!decodeOrionCamerasPacketStructure(&resp, out)) return -3;
    cache_cameras_bin_write(ctx->ip, &resp, sizeof(resp));
    if (from_cache) *from_cache = 0;
    return 0;
}

int cmd_cameras(octl_ctx_t *ctx)
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

    OrionCameras_t cams;
    int active = -1;
    int rc = cmd_cameras_fetch(ctx, &cams, &active);
    conn_close();

    if (rc == -2) {
        jout_err(stderr, OCTL_TIMEOUT, "cameras_timeout",
                 "no Cameras response within %d ms", ctx->timeout_ms);
        return OCTL_TIMEOUT;
    }
    if (rc < 0) {
        jout_err(stderr, OCTL_INTERNAL, "cameras_failed",
                 "Cameras request failed (rc=%d)", rc);
        return OCTL_INTERNAL;
    }

    jout_t j;
    jout_init(&j, stdout);
    cameras_emit(&j, ctx->ip, &cams, active);
    jout_done(&j);

    FILE *cf = cache_cameras_open_write(ctx->ip);
    if (cf) {
        jout_t jc;
        jout_init(&jc, cf);
        cameras_emit(&jc, ctx->ip, &cams, active);
        jout_done(&jc);
        cache_cameras_commit(cf, ctx->ip);
    }

    return OCTL_OK;
}
