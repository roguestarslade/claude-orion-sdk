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

typedef enum { F_U32, F_F32, F_U8, F_U16 } field_type_t;

typedef struct {
    const char  *cli_name;   /* CamelCase, exact match */
    const char  *json_name;  /* snake_case for JSON */
    size_t       offset;
    field_type_t type;
} td_t;

static const td_t TRACK_FIELDS[] = {
    { "DebugEnable",           "debug_enable",            offsetof(TrackOptions_t, DebugEnable),           F_U32 },
    { "KpScene",               "kp_scene",                offsetof(TrackOptions_t, KpScene),               F_F32 },
    { "KiScene",               "ki_scene",                offsetof(TrackOptions_t, KiScene),               F_F32 },
    { "KpTrack",               "kp_track",                offsetof(TrackOptions_t, KpTrack),               F_F32 },
    { "KiTrack",               "ki_track",                offsetof(TrackOptions_t, KiTrack),               F_F32 },
    { "MaxRate",               "max_rate",                offsetof(TrackOptions_t, MaxRate),               F_F32 },
    { "MaxAccel",              "max_accel",               offsetof(TrackOptions_t, MaxAccel),              F_F32 },
    { "TrackSize",             "track_size",              offsetof(TrackOptions_t, TrackSize),             F_F32 },
    { "TrackMode",             "track_mode",              offsetof(TrackOptions_t, TrackMode),             F_U8  },
    { "TrackAlpha",            "track_alpha",             offsetof(TrackOptions_t, TrackAlpha),            F_F32 },
    { "RateAlpha",             "rate_alpha",              offsetof(TrackOptions_t, RateAlpha),             F_F32 },
    { "MinSceneConf",          "min_scene_conf",          offsetof(TrackOptions_t, MinSceneConf),          F_F32 },
    { "MinTrackConf",          "min_track_conf",          offsetof(TrackOptions_t, MinTrackConf),          F_F32 },
    { "MaxCoastTime",          "max_coast_time",          offsetof(TrackOptions_t, MaxCoastTime),          F_F32 },
    { "TrackDelay",            "track_delay",             offsetof(TrackOptions_t, TrackDelay),            F_U8  },
    { "DisableMti",            "disable_mti",             offsetof(TrackOptions_t, DisableMti),            F_U8  },
    { "DetectionMode",         "detection_mode",          offsetof(TrackOptions_t, DetectionMode),         F_U16 },
    { "DetectionThreshold",    "detection_threshold",     offsetof(TrackOptions_t, DetectionThreshold),    F_U8  },
    { "DetectionMaxTelemTrks", "detection_max_telem_trks",offsetof(TrackOptions_t, DetectionMaxTelemTrks), F_U8  },
    { "TrackOffsetPan",        "track_offset_pan",        offsetof(TrackOptions_t, TrackOffsetPan),        F_F32 },
    { "TrackOffsetTilt",       "track_offset_tilt",       offsetof(TrackOptions_t, TrackOffsetTilt),       F_F32 },
};
#define NUM_TRACK_FIELDS ((int)(sizeof(TRACK_FIELDS) / sizeof(TRACK_FIELDS[0])))

static void emit_field(jout_t *j, const td_t *f, const TrackOptions_t *opt)
{
    const unsigned char *base = (const unsigned char *)opt;
    switch (f->type) {
    case F_U32: {
        uint32_t v;
        memcpy(&v, base + f->offset, sizeof(v));
        jout_kv_uint(j, f->json_name, v);
        break;
    }
    case F_F32: {
        float v;
        memcpy(&v, base + f->offset, sizeof(v));
        jout_kv_dbl(j, f->json_name, v);
        break;
    }
    case F_U8: {
        uint8_t v;
        memcpy(&v, base + f->offset, sizeof(v));
        jout_kv_uint(j, f->json_name, v);
        break;
    }
    case F_U16: {
        uint16_t v;
        memcpy(&v, base + f->offset, sizeof(v));
        jout_kv_uint(j, f->json_name, v);
        break;
    }
    }
}

static void emit_track_options(jout_t *j, const TrackOptions_t *opt)
{
    jout_obj_open(j);
    for (int i = 0; i < NUM_TRACK_FIELDS; i++) {
        emit_field(j, &TRACK_FIELDS[i], opt);
    }
    jout_obj_close(j);
}

static int set_field(TrackOptions_t *opt, const td_t *f, const char *value_str)
{
    unsigned char *base = (unsigned char *)opt;
    char *end;
    switch (f->type) {
    case F_U32: {
        unsigned long v = strtoul(value_str, &end, 0);
        if (*end != '\0' || end == value_str) return -1;
        uint32_t out = (uint32_t)v;
        memcpy(base + f->offset, &out, sizeof(out));
        return 0;
    }
    case F_F32: {
        double d = strtod(value_str, &end);
        if (*end != '\0' || end == value_str) return -1;
        float out = (float)d;
        memcpy(base + f->offset, &out, sizeof(out));
        return 0;
    }
    case F_U8: {
        unsigned long v = strtoul(value_str, &end, 0);
        if (*end != '\0' || end == value_str || v > 255) return -1;
        uint8_t out = (uint8_t)v;
        memcpy(base + f->offset, &out, sizeof(out));
        return 0;
    }
    case F_U16: {
        unsigned long v = strtoul(value_str, &end, 0);
        if (*end != '\0' || end == value_str || v > 0xFFFF) return -1;
        uint16_t out = (uint16_t)v;
        memcpy(base + f->offset, &out, sizeof(out));
        return 0;
    }
    }
    return -1;
}

static const td_t *find_field(const char *cli_name)
{
    for (int i = 0; i < NUM_TRACK_FIELDS; i++) {
        if (strcmp(cli_name, TRACK_FIELDS[i].cli_name) == 0) return &TRACK_FIELDS[i];
    }
    return NULL;
}

static int read_options(octl_ctx_t *ctx, TrackOptions_t *out)
{
    OrionPkt_t req, resp;
    MakeOrionPacket(&req, getTrackOptionsPacketID(), 0);
    if (conn_send(&req) != 0) return -1;
    if (conn_wait_for(ORION_PKT_TRACK_OPTIONS, &resp, ctx->timeout_ms) != 0) return -2;
    if (!decodeTrackOptionsPacketStructure(&resp, out)) return -3;
    return 0;
}

static int do_get(octl_ctx_t *ctx)
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

    TrackOptions_t opt;
    int rc = read_options(ctx, &opt);
    if (rc != 0) {
        if (rc == -2) {
            jout_err(stderr, OCTL_TIMEOUT, "track_options_timeout",
                     "no TrackOptions within %d ms", ctx->timeout_ms);
            conn_close();
            return OCTL_TIMEOUT;
        }
        jout_err(stderr, OCTL_INTERNAL, "track_options_failed",
                 "TrackOptions read failed (rc=%d)", rc);
        conn_close();
        return OCTL_INTERNAL;
    }
    conn_close();

    jout_t j;
    jout_init(&j, stdout);
    emit_track_options(&j, &opt);
    jout_done(&j);
    return OCTL_OK;
}

static int do_set(octl_ctx_t *ctx)
{
    /* pos[0]=track, pos[1]=options, pos[2]=set, pos[3..]=Field=Value pairs */
    if (ctx->npos < 4) {
        jout_err(stderr, OCTL_USAGE, "missing_args",
                 "track options set requires at least one Field=Value pair");
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

    TrackOptions_t opt;
    int rc = read_options(ctx, &opt);
    if (rc != 0) {
        if (rc == -2) {
            jout_err(stderr, OCTL_TIMEOUT, "track_options_timeout",
                     "no TrackOptions within %d ms", ctx->timeout_ms);
            conn_close();
            return OCTL_TIMEOUT;
        }
        jout_err(stderr, OCTL_INTERNAL, "track_options_failed",
                 "TrackOptions pre-read failed (rc=%d)", rc);
        conn_close();
        return OCTL_INTERNAL;
    }

    /* Mutate from Field=Value pairs */
    int mutations = 0;
    for (int i = 3; i < ctx->npos; i++) {
        const char *kv = ctx->pos[i];
        const char *eq = strchr(kv, '=');
        if (!eq || eq == kv) {
            jout_err(stderr, OCTL_USAGE, "bad_kv",
                     "expected Field=Value, got: %s", kv);
            conn_close();
            return OCTL_USAGE;
        }
        char name[64];
        size_t nlen = (size_t)(eq - kv);
        if (nlen >= sizeof(name)) {
            jout_err(stderr, OCTL_USAGE, "field_too_long",
                     "field name too long: %s", kv);
            conn_close();
            return OCTL_USAGE;
        }
        memcpy(name, kv, nlen);
        name[nlen] = '\0';
        const td_t *f = find_field(name);
        if (!f) {
            jout_err(stderr, OCTL_USAGE, "unknown_field",
                     "unknown TrackOptions field: %s", name);
            conn_close();
            return OCTL_USAGE;
        }
        if (set_field(&opt, f, eq + 1) != 0) {
            jout_err(stderr, OCTL_USAGE, "bad_value",
                     "could not parse value for %s: %s", name, eq + 1);
            conn_close();
            return OCTL_USAGE;
        }
        mutations++;
    }

    /* Write back */
    OrionPkt_t out_pkt;
    encodeTrackOptionsPacketStructure(&out_pkt, &opt);
    if (conn_send(&out_pkt) != 0) {
        jout_err(stderr, OCTL_CONN_FAILED, "send_failed", "could not send TrackOptions write");
        conn_close();
        return OCTL_CONN_FAILED;
    }

    /* Wait for echo: gimbal echoes the new TrackOptions back */
    OrionPkt_t echo;
    int echoed = (conn_wait_for(ORION_PKT_TRACK_OPTIONS, &echo, ctx->timeout_ms) == 0);
    TrackOptions_t echo_opt;
    int echo_decoded = echoed && decodeTrackOptionsPacketStructure(&echo, &echo_opt);
    conn_close();

    jout_t j;
    jout_init(&j, stdout);
    jout_obj_open(&j);
    jout_kv_int (&j, "mutations", mutations);
    jout_kv_bool(&j, "echo_seen", echoed);
    if (echo_decoded) {
        jout_key(&j, "echo");
        emit_track_options(&j, &echo_opt);
    } else {
        jout_key(&j, "written");
        emit_track_options(&j, &opt);
    }
    jout_obj_close(&j);
    jout_done(&j);
    return OCTL_OK;
}

int cmd_track_options(octl_ctx_t *ctx)
{
    /* pos[0]=track, pos[1]=options, pos[2]=get|set */
    if (ctx->npos < 3) {
        jout_err(stderr, OCTL_USAGE, "missing_subverb",
                 "track options requires get | set");
        return OCTL_USAGE;
    }
    const char *sub = ctx->pos[2];
    if (strcmp(sub, "get") == 0) return do_get(ctx);
    if (strcmp(sub, "set") == 0) return do_set(ctx);
    jout_err(stderr, OCTL_USAGE, "unknown_subverb",
             "unknown track options subverb: %s", sub);
    return OCTL_USAGE;
}
