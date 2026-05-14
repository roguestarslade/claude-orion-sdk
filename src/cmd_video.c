#include "orionctl.h"
#include "conn.h"
#include "json_out.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "OrionPublicPacket.h"
#include "OrionPublicPacketShim.h"

int cmd_video_net(octl_ctx_t *ctx);

typedef enum { V_U32, V_F32, V_U8, V_U16, V_STR40 } vtype_t;

typedef struct {
    const char *cli_name;
    const char *json_section;   /* NULL = top-level; else nested key */
    const char *json_key;
    size_t      offset;
    vtype_t     type;
} vd_t;

static const vd_t VOPT[] = {
    { "Stabilized",                    NULL,                "stabilized",            offsetof(VideoOptions_t, Stabilized),               V_U32 },
    { "ShowTrackBox",                  NULL,                "show_track_box",        offsetof(VideoOptions_t, ShowTrackBox),             V_U32 },
    { "ShowReticle",                   NULL,                "show_reticle",          offsetof(VideoOptions_t, ShowReticle),              V_U32 },
    { "RotateVideo",                   NULL,                "rotate_video",          offsetof(VideoOptions_t, RotateVideo),              V_U32 },
    { "DisableFovMatch",               NULL,                "disable_fov_match",     offsetof(VideoOptions_t, DisableFovMatch),          V_U32 },
    { "ShowTelemetry",                 NULL,                "show_telemetry",        offsetof(VideoOptions_t, ShowTelemetry),            V_U32 },
    { "HasTelemetryOptions",           NULL,                "has_telemetry_options", offsetof(VideoOptions_t, HasTelemetryOptions),      V_U32 },
    { "ShowDetections",                NULL,                "show_detections",       offsetof(VideoOptions_t, ShowDetections),           V_U32 },
    { "DisableNtsc",                   NULL,                "disable_ntsc",          offsetof(VideoOptions_t, DisableNtsc),              V_U32 },
    { "EnablePip",                     NULL,                "enable_pip",            offsetof(VideoOptions_t, EnablePip),                V_U32 },
    { "ZoomTrackBox",                  NULL,                "zoom_track_box",        offsetof(VideoOptions_t, ZoomTrackBox),             V_U32 },
    { "ShowTrackId",                   NULL,                "show_track_id",         offsetof(VideoOptions_t, ShowTrackId),              V_U32 },
    { "MaxShift",                      NULL,                "max_shift",             offsetof(VideoOptions_t, MaxShift),                 V_F32 },
    { "TelemetryOptions.CameraPos",    "telemetry_options", "camera_pos",            offsetof(VideoOptions_t, TelemetryOptions.CameraPos),     V_U32 },
    { "TelemetryOptions.TargetPos",    "telemetry_options", "target_pos",            offsetof(VideoOptions_t, TelemetryOptions.TargetPos),     V_U32 },
    { "TelemetryOptions.Fov",          "telemetry_options", "fov",                   offsetof(VideoOptions_t, TelemetryOptions.Fov),           V_U32 },
    { "TelemetryOptions.Range",        "telemetry_options", "range",                 offsetof(VideoOptions_t, TelemetryOptions.Range),         V_U32 },
    { "TelemetryOptions.MetricAlt",    "telemetry_options", "metric_alt",            offsetof(VideoOptions_t, TelemetryOptions.MetricAlt),     V_U32 },
    { "TelemetryOptions.MetricDist",   "telemetry_options", "metric_dist",           offsetof(VideoOptions_t, TelemetryOptions.MetricDist),    V_U32 },
    { "TelemetryOptions.CoordFormat",  "telemetry_options", "coord_format",          offsetof(VideoOptions_t, TelemetryOptions.CoordFormat),   V_U32 },
    { "TelemetryOptions.Date",         "telemetry_options", "date",                  offsetof(VideoOptions_t, TelemetryOptions.Date),          V_U32 },
    { "TelemetryOptions.Time",         "telemetry_options", "time",                  offsetof(VideoOptions_t, TelemetryOptions.Time),          V_U32 },
    { "TelemetryOptions.NorthIndicator","telemetry_options","north_indicator",       offsetof(VideoOptions_t, TelemetryOptions.NorthIndicator),V_U32 },
    { "TelemetryOptions.UserString",   "telemetry_options", "user_string",           offsetof(VideoOptions_t, TelemetryOptions.UserString),    V_STR40 },
};
#define NUM_VOPT ((int)(sizeof(VOPT) / sizeof(VOPT[0])))

static void emit_field(jout_t *j, const vd_t *f, const VideoOptions_t *o)
{
    const unsigned char *base = (const unsigned char *)o;
    switch (f->type) {
    case V_U32: { uint32_t v; memcpy(&v, base + f->offset, sizeof(v)); jout_kv_uint(j, f->json_key, v); break; }
    case V_F32: { float v;    memcpy(&v, base + f->offset, sizeof(v)); jout_kv_dbl (j, f->json_key, v); break; }
    case V_U8:  { uint8_t v;  memcpy(&v, base + f->offset, sizeof(v)); jout_kv_uint(j, f->json_key, v); break; }
    case V_U16: { uint16_t v; memcpy(&v, base + f->offset, sizeof(v)); jout_kv_uint(j, f->json_key, v); break; }
    case V_STR40: {
        char buf[41];
        memcpy(buf, base + f->offset, 40);
        buf[40] = '\0';
        jout_kv_str(j, f->json_key, buf);
        break;
    }
    }
}

static void emit_video_options(jout_t *j, const VideoOptions_t *o)
{
    jout_obj_open(j);
    /* First pass: top-level (json_section == NULL) */
    for (int i = 0; i < NUM_VOPT; i++) {
        if (VOPT[i].json_section == NULL) emit_field(j, &VOPT[i], o);
    }
    /* Open telemetry_options subobject and dump nested */
    int any_nested = 0;
    for (int i = 0; i < NUM_VOPT; i++) {
        if (VOPT[i].json_section != NULL) { any_nested = 1; break; }
    }
    if (any_nested) {
        jout_key(j, "telemetry_options");
        jout_obj_open(j);
        for (int i = 0; i < NUM_VOPT; i++) {
            if (VOPT[i].json_section != NULL) emit_field(j, &VOPT[i], o);
        }
        jout_obj_close(j);
    }
    jout_obj_close(j);
}

static int set_field_value(VideoOptions_t *o, const vd_t *f, const char *vstr)
{
    unsigned char *base = (unsigned char *)o;
    char *end;
    switch (f->type) {
    case V_U32: {
        unsigned long v = strtoul(vstr, &end, 0);
        if (*end != '\0' || end == vstr) return -1;
        uint32_t out = (uint32_t)v; memcpy(base + f->offset, &out, sizeof(out));
        return 0;
    }
    case V_F32: {
        double d = strtod(vstr, &end);
        if (*end != '\0' || end == vstr) return -1;
        float out = (float)d; memcpy(base + f->offset, &out, sizeof(out));
        return 0;
    }
    case V_U8: {
        unsigned long v = strtoul(vstr, &end, 0);
        if (*end != '\0' || end == vstr || v > 255) return -1;
        uint8_t out = (uint8_t)v; memcpy(base + f->offset, &out, sizeof(out));
        return 0;
    }
    case V_U16: {
        unsigned long v = strtoul(vstr, &end, 0);
        if (*end != '\0' || end == vstr || v > 0xFFFF) return -1;
        uint16_t out = (uint16_t)v; memcpy(base + f->offset, &out, sizeof(out));
        return 0;
    }
    case V_STR40: {
        size_t L = strlen(vstr);
        if (L > 40) L = 40;
        memcpy(base + f->offset, vstr, L);
        if (L < 40) memset(base + f->offset + L, 0, 40 - L);
        return 0;
    }
    }
    return -1;
}

static const vd_t *find_field(const char *cli)
{
    for (int i = 0; i < NUM_VOPT; i++) {
        if (strcmp(cli, VOPT[i].cli_name) == 0) return &VOPT[i];
    }
    return NULL;
}

static int read_voptions(octl_ctx_t *ctx, VideoOptions_t *out)
{
    OrionPkt_t req, resp;
    MakeOrionPacket(&req, getVideoOptionsPacketID(), 0);
    if (conn_send(&req) != 0) return -1;
    if (conn_wait_for(ORION_PKT_VIDEO_OPTIONS, &resp, ctx->timeout_ms) != 0) return -2;
    if (!decodeVideoOptionsPacketStructure(&resp, out)) return -3;
    return 0;
}

static int video_get(octl_ctx_t *ctx)
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

    VideoOptions_t o;
    int rc = read_voptions(ctx, &o);
    conn_close();
    if (rc == -2) {
        jout_err(stderr, OCTL_TIMEOUT, "video_options_timeout",
                 "no VideoOptions within %d ms", ctx->timeout_ms);
        return OCTL_TIMEOUT;
    }
    if (rc != 0) {
        jout_err(stderr, OCTL_INTERNAL, "video_options_failed",
                 "VideoOptions read failed (rc=%d)", rc);
        return OCTL_INTERNAL;
    }

    jout_t j;
    jout_init(&j, stdout);
    emit_video_options(&j, &o);
    jout_done(&j);
    return OCTL_OK;
}

static int video_set(octl_ctx_t *ctx)
{
    /* pos[0]=video, pos[1]=set, pos[2..]=Field=Value pairs */
    if (ctx->npos < 3) {
        jout_err(stderr, OCTL_USAGE, "missing_args",
                 "video set requires at least one Field=Value pair");
        return OCTL_USAGE;
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

    VideoOptions_t o;
    int rc = read_voptions(ctx, &o);
    if (rc != 0) {
        jout_err(stderr, rc == -2 ? OCTL_TIMEOUT : OCTL_INTERNAL,
                 rc == -2 ? "video_options_timeout" : "video_options_failed",
                 "VideoOptions pre-read failed (rc=%d)", rc);
        conn_close();
        return rc == -2 ? OCTL_TIMEOUT : OCTL_INTERNAL;
    }

    int mutations = 0;
    for (int i = 2; i < ctx->npos; i++) {
        const char *kv = ctx->pos[i];
        const char *eq = strchr(kv, '=');
        if (!eq || eq == kv) {
            jout_err(stderr, OCTL_USAGE, "bad_kv",
                     "expected Field=Value, got: %s", kv);
            conn_close();
            return OCTL_USAGE;
        }
        char name[80];
        size_t nlen = (size_t)(eq - kv);
        if (nlen >= sizeof(name)) {
            jout_err(stderr, OCTL_USAGE, "field_too_long", "field name too long");
            conn_close();
            return OCTL_USAGE;
        }
        memcpy(name, kv, nlen);
        name[nlen] = '\0';
        const vd_t *f = find_field(name);
        if (!f) {
            jout_err(stderr, OCTL_USAGE, "unknown_field",
                     "unknown VideoOptions field: %s", name);
            conn_close();
            return OCTL_USAGE;
        }
        if (set_field_value(&o, f, eq + 1) != 0) {
            jout_err(stderr, OCTL_USAGE, "bad_value",
                     "could not parse value for %s: %s", name, eq + 1);
            conn_close();
            return OCTL_USAGE;
        }
        mutations++;
    }

    OrionPkt_t out_pkt;
    encodeVideoOptionsPacketStructure(&out_pkt, &o);
    if (conn_send(&out_pkt) != 0) {
        jout_err(stderr, OCTL_CONN_FAILED, "send_failed", "could not send VideoOptions");
        conn_close();
        return OCTL_CONN_FAILED;
    }
    OrionPkt_t echo;
    int echoed = (conn_wait_for(ORION_PKT_VIDEO_OPTIONS, &echo, ctx->timeout_ms) == 0);
    VideoOptions_t echo_opt;
    int echo_decoded = echoed && decodeVideoOptionsPacketStructure(&echo, &echo_opt);
    conn_close();

    jout_t j;
    jout_init(&j, stdout);
    jout_obj_open(&j);
    jout_kv_int (&j, "mutations", mutations);
    jout_kv_bool(&j, "echo_seen", echoed);
    if (echo_decoded) {
        jout_key(&j, "echo");
        emit_video_options(&j, &echo_opt);
    } else {
        jout_key(&j, "written");
        emit_video_options(&j, &o);
    }
    jout_obj_close(&j);
    jout_done(&j);
    return OCTL_OK;
}

int cmd_video(octl_ctx_t *ctx)
{
    if (ctx->npos < 2) {
        jout_err(stderr, OCTL_USAGE, "missing_subverb",
                 "video requires subverb: get | set | net get | net set");
        return OCTL_USAGE;
    }
    const char *sub = ctx->pos[1];
    if (strcmp(sub, "get") == 0) return video_get(ctx);
    if (strcmp(sub, "set") == 0) return video_set(ctx);
    if (strcmp(sub, "net") == 0) return cmd_video_net(ctx);
    jout_err(stderr, OCTL_USAGE, "unknown_subverb",
             "unknown video subverb: %s", sub);
    return OCTL_USAGE;
}
