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

static int parse_type(const char *s, OrionCameraType_t *out)
{
    if (!strcmp(s, "none"))      { *out = CAMERA_TYPE_NONE;    return 0; }
    if (!strcmp(s, "visible"))   { *out = CAMERA_TYPE_VISIBLE; return 0; }
    if (!strcmp(s, "swir"))      { *out = CAMERA_TYPE_SWIR;    return 0; }
    if (!strcmp(s, "mwir"))      { *out = CAMERA_TYPE_MWIR;    return 0; }
    if (!strcmp(s, "lwir"))      { *out = CAMERA_TYPE_LWIR;    return 0; }
    if (!strcmp(s, "spotter"))   { *out = CAMERA_TYPE_SPOTTER; return 0; }
    if (!strcmp(s, "nir"))       { *out = CAMERA_TYPE_NIR;     return 0; }
    if (!strcmp(s, "mono_1064")) { *out = CAMERA_TYPE_1064;    return 0; }
    return -1;
}

static int parse_proto(const char *s, OrionCameraProtocol_t *out)
{
    if (!strcmp(s, "unknown")) { *out = CAMERA_PROTO_UNKNOWN; return 0; }
    if (!strcmp(s, "flir"))    { *out = CAMERA_PROTO_FLIR;    return 0; }
    if (!strcmp(s, "aptina"))  { *out = CAMERA_PROTO_APTINA;  return 0; }
    if (!strcmp(s, "zafiro"))  { *out = CAMERA_PROTO_ZAFIRO;  return 0; }
    if (!strcmp(s, "hitachi")) { *out = CAMERA_PROTO_HITACHI; return 0; }
    if (!strcmp(s, "bae"))     { *out = CAMERA_PROTO_BAE;     return 0; }
    if (!strcmp(s, "sony"))    { *out = CAMERA_PROTO_SONY;    return 0; }
    if (!strcmp(s, "ktnc"))    { *out = CAMERA_PROTO_KTNC;    return 0; }
    if (!strcmp(s, "attollo")) { *out = CAMERA_PROTO_ATTOLLO; return 0; }
    if (!strcmp(s, "mira"))    { *out = CAMERA_PROTO_MIRA;    return 0; }
    if (!strcmp(s, "alvium"))  { *out = CAMERA_PROTO_ALVIUM;  return 0; }
    if (!strcmp(s, "omnivis")) { *out = CAMERA_PROTO_OMNIVIS; return 0; }
    if (!strcmp(s, "sierra"))  { *out = CAMERA_PROTO_SIERRA;  return 0; }
    if (!strcmp(s, "sionyx"))  { *out = CAMERA_PROTO_SIONYX;  return 0; }
    return -1;
}

static void emit_slot(jout_t *j, const char *key, const OrionCamSettings_t *s)
{
    jout_key(j, key);
    jout_obj_open(j);
    jout_kv_int(j, "type_id",       s->Type);
    jout_kv_int(j, "proto_id",      s->Proto);
    jout_kv_int(j, "gpio",          s->Gpio);
    jout_kv_int(j, "gpio_active",   s->GpioActiveState);
    jout_kv_dbl(j, "pixel_pitch_mm", s->PixelPitch);
    jout_kv_int(j, "array_w",       s->ArrayWidth);
    jout_kv_int(j, "array_h",       s->ArrayHeight);
    jout_kv_dbl(j, "min_focal_mm",  s->MinFocalLength);
    jout_kv_dbl(j, "max_focal_mm",  s->MaxFocalLength);
    jout_key(j, "align_min_rad"); jout_arr_open(j);
    jout_dbl(j, s->AlignMin[0]); jout_dbl(j, s->AlignMin[1]);
    jout_arr_close(j);
    jout_key(j, "align_max_rad"); jout_arr_open(j);
    jout_dbl(j, s->AlignMax[0]); jout_dbl(j, s->AlignMax[1]);
    jout_arr_close(j);
    jout_obj_close(j);
}

int cmd_camera_set(octl_ctx_t *ctx)
{
    if (!ctx->iknow) {
        jout_err(stderr, OCTL_REJECTED, "missing_iknow",
                 "camera set rewrites slot config; pass --i-know to confirm");
        return OCTL_REJECTED;
    }
    if (!ctx->idx_set) {
        jout_err(stderr, OCTL_USAGE, "missing_idx",
                 "camera set requires explicit --idx <0..%d>", NUM_CAMERAS - 1);
        return OCTL_USAGE;
    }
    if (ctx->idx < 0 || ctx->idx >= NUM_CAMERAS) {
        jout_err(stderr, OCTL_CAMERA_IDX, "bad_idx",
                 "camera index %d out of range [0..%d]",
                 ctx->idx, NUM_CAMERAS - 1);
        return OCTL_CAMERA_IDX;
    }

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
    if (rc != 0) {
        conn_close();
        jout_err(stderr, OCTL_TIMEOUT, "cameras_timeout",
                 "could not retrieve current Cameras packet (rc=%d)", rc);
        return OCTL_TIMEOUT;
    }

    OrionCamSettings_t before = cams.OrionCamSettings[ctx->idx];
    OrionCamSettings_t *s = &cams.OrionCamSettings[ctx->idx];
    int changes = 0;

    if (ctx->set_type) {
        OrionCameraType_t t;
        if (parse_type(ctx->set_type, &t) != 0) {
            conn_close();
            jout_err(stderr, OCTL_USAGE, "bad_type",
                     "unknown --type: %s", ctx->set_type);
            return OCTL_USAGE;
        }
        s->Type = t; changes++;
    }
    if (ctx->set_proto) {
        OrionCameraProtocol_t p;
        if (parse_proto(ctx->set_proto, &p) != 0) {
            conn_close();
            jout_err(stderr, OCTL_USAGE, "bad_proto",
                     "unknown --proto: %s", ctx->set_proto);
            return OCTL_USAGE;
        }
        s->Proto = p; changes++;
    }
    if (ctx->set_gpio_set)         { s->Gpio            = (uint8_t)ctx->set_gpio;        changes++; }
    if (ctx->set_gpio_active_set)  { s->GpioActiveState = (uint8_t)ctx->set_gpio_active; changes++; }
    if (ctx->set_min_focal_set)    { s->MinFocalLength  = (float)ctx->set_min_focal;     changes++; }
    if (ctx->set_max_focal_set)    { s->MaxFocalLength  = (float)ctx->set_max_focal;     changes++; }
    if (ctx->set_pixel_pitch_set)  { s->PixelPitch      = (float)ctx->set_pixel_pitch;   changes++; }
    if (ctx->set_array_w_set)      { s->ArrayWidth      = (uint16_t)ctx->set_array_w;    changes++; }
    if (ctx->set_array_h_set)      { s->ArrayHeight     = (uint16_t)ctx->set_array_h;    changes++; }
    if (ctx->set_align_min_set) {
        s->AlignMin[0] = (float)ctx->set_align_min[0];
        s->AlignMin[1] = (float)ctx->set_align_min[1];
        changes++;
    }
    if (ctx->set_align_max_set) {
        s->AlignMax[0] = (float)ctx->set_align_max[0];
        s->AlignMax[1] = (float)ctx->set_align_max[1];
        changes++;
    }

    if (changes == 0) {
        conn_close();
        jout_err(stderr, OCTL_USAGE, "no_changes",
                 "no --<field> args given; nothing to set");
        return OCTL_USAGE;
    }

    OrionPkt_t wpkt, echo;
    encodeOrionCamerasPacketStructure(&wpkt, &cams);
    if (conn_send(&wpkt) != 0) {
        conn_close();
        jout_err(stderr, OCTL_INTERNAL, "send_failed",
                 "send OrionCameras failed");
        return OCTL_INTERNAL;
    }
    int echo_ok = (conn_wait_for(ORION_PKT_CAMERAS, &echo, ctx->timeout_ms) == 0);

    int persist_ok = -1;
    if (ctx->persist) {
        OrionPkt_t pp, pe;
        MakeOrionPacket(&pp, ORION_PKT_PRIVATE_20, 0);
        if (conn_send(&pp) == 0 &&
            conn_wait_for(ORION_PKT_PRIVATE_20, &pe, ctx->timeout_ms) == 0) {
            persist_ok = 1;
        } else {
            persist_ok = 0;
        }
    }

    OrionCameras_t after_cams;
    int after_active = -1;
    OrionCamSettings_t after = before;
    int verify_ok = 0;
    if (cmd_cameras_fetch(ctx, &after_cams, &after_active) == 0) {
        after = after_cams.OrionCamSettings[ctx->idx];
        verify_ok = 1;
    }

    conn_close();
    cache_clear(ctx->ip);

    jout_t j;
    jout_init(&j, stdout);
    jout_obj_open(&j);
    jout_kv_str (&j, "ip",            ctx->ip);
    jout_kv_int (&j, "index",         ctx->idx);
    jout_kv_int (&j, "changes",       changes);
    jout_kv_bool(&j, "write_echo_ok", echo_ok);
    if (ctx->persist) jout_kv_bool(&j, "persist_echo_ok", persist_ok == 1);
    jout_kv_bool(&j, "verify_read_ok", verify_ok);
    emit_slot(&j, "before", &before);
    emit_slot(&j, "after",  &after);
    jout_obj_close(&j);
    jout_done(&j);

    if (!echo_ok) return OCTL_TIMEOUT;
    if (ctx->persist && persist_ok != 1) return OCTL_TIMEOUT;
    return OCTL_OK;
}
