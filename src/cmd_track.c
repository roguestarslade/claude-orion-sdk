#include "orionctl.h"
#include "conn.h"
#include "json_out.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "OrionPublicPacket.h"
#include "OrionPublicPacketShim.h"
#include "GeolocateTelemetry.h"

int cmd_track_options(octl_ctx_t *ctx);

static const char *track_cmd_name(int c)
{
    switch (c) {
    case TRACK_START_PRIMARY:        return "start_primary";
    case TRACK_START_SECONDARY:      return "start_secondary";
    case TRACK_STOP_ALL:             return "stop_all";
    case TRACK_STOP_ALL_BUT_PRIMARY: return "stop_all_but_primary";
    case TRACK_NUDGE_PRIMARY:        return "nudge_primary";
    case TRACK_RESIZE_PRIMARY:       return "resize_primary";
    case TRACK_NUDGE_BY_INDEX:       return "nudge_by_index";
    case TRACK_RESIZE_BY_INDEX:      return "resize_by_index";
    case TRACK_REMOVE_BY_INDEX:      return "remove_by_index";
    case TRACK_REMOVE_BY_PIXEL:      return "remove_by_pixel";
    default:                         return "unknown";
    }
}

static int parse_dbl_arg(const char *s, double *out)
{
    if (!s) return -1;
    char *end;
    double v = strtod(s, &end);
    if (*end != '\0' || end == s) return -1;
    *out = v;
    return 0;
}

static const char *target_unit_for(int c)
{
    switch (c) {
    case TRACK_START_PRIMARY:
    case TRACK_START_SECONDARY:
    case TRACK_REMOVE_BY_PIXEL:
        return "image_fraction";
    case TRACK_NUDGE_PRIMARY:
    case TRACK_NUDGE_BY_INDEX:
        return "percent_of_fov";
    default:
        return NULL;
    }
}

static const char *resize_unit_for(int c)
{
    switch (c) {
    case TRACK_RESIZE_PRIMARY:
    case TRACK_RESIZE_BY_INDEX:
        return "percent_of_frame";
    default:
        return NULL;
    }
}

static int send_track_cmd_echo(octl_ctx_t *ctx, const TrackCmd_t *cmd)
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
    encodeTrackCmdPacketStructure(&pkt, cmd);
    if (conn_send(&pkt) != 0) {
        jout_err(stderr, OCTL_CONN_FAILED, "send_failed", "could not send TrackCmd");
        conn_close();
        return OCTL_CONN_FAILED;
    }

    OrionPkt_t echo;
    int got_echo = (conn_wait_for(ORION_PKT_TRACK_CMD, &echo, ctx->timeout_ms) == 0);

    jout_t j;
    jout_init(&j, stdout);
    jout_obj_open(&j);
    jout_kv_str (&j, "cmd",       track_cmd_name(cmd->Cmd));
    jout_kv_int (&j, "cmd_id",    cmd->Cmd);
    jout_key(&j, "target"); jout_arr_open(&j);
    jout_dbl(&j, cmd->Target[0]);
    jout_dbl(&j, cmd->Target[1]);
    jout_arr_close(&j);
    const char *t_unit = target_unit_for(cmd->Cmd);
    if (t_unit) jout_kv_str(&j, "target_unit", t_unit);
    jout_kv_int (&j, "track_index", cmd->TrackIndex);
    jout_kv_dbl (&j, "resize",      cmd->Resize);
    const char *r_unit = resize_unit_for(cmd->Cmd);
    if (r_unit) jout_kv_str(&j, "resize_unit", r_unit);
    jout_kv_bool(&j, "echo_seen",   got_echo);
    jout_obj_close(&j);
    jout_done(&j);

    conn_close();
    return OCTL_OK;
}

static int track_create(octl_ctx_t *ctx)
{
    /* track create <x> <y> -- positional args at pos[2], pos[3] */
    if (ctx->npos < 4) {
        jout_err(stderr, OCTL_USAGE, "missing_args",
                 "track create requires <x> <y> image coords (-0.5..0.5)");
        return OCTL_USAGE;
    }
    double x, y;
    if (parse_dbl_arg(ctx->pos[2], &x) != 0 ||
        parse_dbl_arg(ctx->pos[3], &y) != 0) {
        jout_err(stderr, OCTL_USAGE, "bad_coords",
                 "x and y must be doubles in [-0.5, +0.5]");
        return OCTL_USAGE;
    }
    if (x < -0.5 || x > 0.5 || y < -0.5 || y > 0.5) {
        jout_err(stderr, OCTL_USAGE, "out_of_range",
                 "x and y must be in [-0.5, +0.5] image-fraction coords");
        return OCTL_USAGE;
    }
    TrackCmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.Cmd = TRACK_START_PRIMARY;
    cmd.Target[0] = (float)x;
    cmd.Target[1] = (float)y;
    return send_track_cmd_echo(ctx, &cmd);
}

static int track_resize(octl_ctx_t *ctx)
{
    if (!ctx->delta_set) {
        jout_err(stderr, OCTL_USAGE, "missing_delta",
                 "track resize requires --delta <pct> (percent of frame)");
        return OCTL_USAGE;
    }
    TrackCmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.Resize = (float)ctx->delta;
    if (ctx->idx_set) {
        cmd.Cmd = TRACK_RESIZE_BY_INDEX;
        cmd.TrackIndex = ctx->idx;
    } else {
        cmd.Cmd = TRACK_RESIZE_PRIMARY;
    }
    return send_track_cmd_echo(ctx, &cmd);
}

static int track_nudge(octl_ctx_t *ctx)
{
    if (!ctx->dx_set || !ctx->dy_set) {
        jout_err(stderr, OCTL_USAGE, "missing_dxdy",
                 "track nudge requires --dx <pct> --dy <pct> (percent of FOV)");
        return OCTL_USAGE;
    }
    TrackCmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.Target[0] = (float)ctx->dx;
    cmd.Target[1] = (float)ctx->dy;
    if (ctx->idx_set) {
        cmd.Cmd = TRACK_NUDGE_BY_INDEX;
        cmd.TrackIndex = ctx->idx;
    } else {
        cmd.Cmd = TRACK_NUDGE_PRIMARY;
    }
    return send_track_cmd_echo(ctx, &cmd);
}

static int track_destroy(octl_ctx_t *ctx)
{
    if (!ctx->idx_set && !ctx->all) {
        jout_err(stderr, OCTL_USAGE, "missing_target",
                 "track destroy requires --id N or --all");
        return OCTL_USAGE;
    }
    TrackCmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    if (ctx->all) {
        cmd.Cmd = TRACK_STOP_ALL;
    } else {
        cmd.Cmd = TRACK_REMOVE_BY_INDEX;
        cmd.TrackIndex = ctx->idx;
    }
    return send_track_cmd_echo(ctx, &cmd);
}

static void emit_track_block(jout_t *j, const GeolocateTelemetry_t *geo)
{
    const GeolocateTelemetryCore_t *b = &geo->base;
    jout_obj_open(j);
    jout_kv_uint(j, "system_ms",      b->systemTime);
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
}

static int track_status_or_watch(octl_ctx_t *ctx, int watch)
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

    int n = watch ? (ctx->watch_n > 0 ? ctx->watch_n : 0) : 1;
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
        emit_track_block(&j, &geo);
        jout_done(&j);
        i++;
        if (!watch) break;
    }

    conn_close();
    return OCTL_OK;
}

int cmd_track(octl_ctx_t *ctx)
{
    if (ctx->npos < 2) {
        jout_err(stderr, OCTL_USAGE, "missing_subverb",
                 "track requires subverb: options|create|resize|nudge|destroy|status|watch");
        return OCTL_USAGE;
    }
    const char *sub = ctx->pos[1];
    if (strcmp(sub, "options") == 0) return cmd_track_options(ctx);
    if (strcmp(sub, "create")  == 0) return track_create(ctx);
    if (strcmp(sub, "resize")  == 0) return track_resize(ctx);
    if (strcmp(sub, "nudge")   == 0) return track_nudge(ctx);
    if (strcmp(sub, "destroy") == 0) return track_destroy(ctx);
    if (strcmp(sub, "status")  == 0) return track_status_or_watch(ctx, 0);
    if (strcmp(sub, "watch")   == 0) return track_status_or_watch(ctx, 1);

    jout_err(stderr, OCTL_USAGE, "unknown_subverb",
             "unknown track subverb: %s", sub);
    return OCTL_USAGE;
}
