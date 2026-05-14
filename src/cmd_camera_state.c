#include "orionctl.h"
#include "cmd_cameras_internal.h"
#include "conn.h"
#include "fov_math.h"
#include "json_out.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "OrionPublicPacket.h"
#include "OrionPublicPacketShim.h"
#include "GeolocateTelemetry.h"

int cmd_camera_switch(octl_ctx_t *ctx);

static long long now_ms_local(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static void emit_camera_state(jout_t *j, const char *ip, int idx, int is_active,
                              const OrionCamSettings_t *meta,
                              const OrionCameraState_t *state,
                              const char *warning)
{
    jout_obj_open(j);
    jout_kv_str (j, "ip", ip);
    jout_kv_int (j, "index", idx);
    jout_kv_bool(j, "is_active", is_active);
    jout_kv_bool(j, "populated", meta && meta->Type != CAMERA_TYPE_NONE);
    if (warning) jout_kv_str(j, "warning", warning);
    else         jout_kv_null(j, "warning");

    if (state) {
        jout_key(j, "state");
        jout_obj_open(j);
        jout_kv_dbl (j, "zoom",  state->Zoom);
        jout_kv_dbl (j, "focus", state->Focus);
        jout_kv_int (j, "keep_active_camera", state->KeepActiveCamera);
        jout_obj_close(j);
    } else {
        jout_kv_null(j, "state");
    }

    if (meta) {
        jout_key(j, "optics");
        jout_obj_open(j);
        jout_kv_dbl(j, "pixel_pitch_mm",      meta->PixelPitch);
        jout_kv_int(j, "array_width_px",      meta->ArrayWidth);
        jout_kv_int(j, "array_height_px",     meta->ArrayHeight);
        jout_kv_dbl(j, "min_focal_length_mm", meta->MinFocalLength);
        jout_kv_dbl(j, "max_focal_length_mm", meta->MaxFocalLength);
        double maxzoom = 0.0;
        if (meta->MinFocalLength > 0 && meta->MaxFocalLength > 0)
            maxzoom = (double)meta->MaxFocalLength / (double)meta->MinFocalLength;
        if (maxzoom > 0.0) jout_kv_dbl (j, "max_zoom", maxzoom);
        else               jout_kv_null(j, "max_zoom");
        jout_obj_close(j);
    }
    jout_obj_close(j);
}

static int cmd_camera_get(octl_ctx_t *ctx)
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
    if (rc != 0) {
        conn_close();
        jout_err(stderr, OCTL_TIMEOUT, "cameras_timeout",
                 "could not retrieve Cameras packet (rc=%d)", rc);
        return OCTL_TIMEOUT;
    }

    int idx = ctx->idx_set ? ctx->idx : active;
    if (idx < 0 || idx >= NUM_CAMERAS) {
        conn_close();
        jout_err(stderr, OCTL_CAMERA_IDX, "bad_idx",
                 "camera index %d out of range [0..%d]", idx, NUM_CAMERAS - 1);
        return OCTL_CAMERA_IDX;
    }
    const OrionCamSettings_t *meta = &cams.OrionCamSettings[idx];
    if (meta->Type == CAMERA_TYPE_NONE) {
        conn_close();
        jout_err(stderr, OCTL_CAMERA_IDX, "empty_slot",
                 "camera slot %d is not populated", idx);
        return OCTL_CAMERA_IDX;
    }

    int is_active = (idx == active);
    OrionCameraState_t state = {0};
    OrionCameraState_t *pstate = NULL;
    const char *warning = NULL;

    if (is_active) {
        OrionPkt_t req, resp;
        MakeOrionPacket(&req, getOrionCameraStatePacketID(), 0);
        if (conn_send(&req) == 0 &&
            conn_wait_for(ORION_PKT_CAMERA_STATE, &resp, ctx->timeout_ms) == 0 &&
            decodeOrionCameraStatePacketStructure(&resp, &state)) {
            pstate = &state;
        } else {
            warning = "camera_state_unavailable";
        }
    } else {
        warning = "inactive_slot_state_not_live";
    }
    conn_close();

    jout_t j;
    jout_init(&j, stdout);
    emit_camera_state(&j, ctx->ip, idx, is_active, meta, pstate, warning);
    jout_done(&j);
    return OCTL_OK;
}

static int resolve_target_slot(octl_ctx_t *ctx, OrionCameras_t *cams_out,
                               int *active_out, int *idx_out,
                               OrionCamSettings_t **meta_out)
{
    int active = telem_active_index(ctx->timeout_ms);
    int from_cache = 0;
    if (cameras_load_or_fetch(ctx, cams_out, &from_cache) != 0) return -1;
    int idx = ctx->idx_set ? ctx->idx : active;
    if (idx < 0 || idx >= NUM_CAMERAS) {
        jout_err(stderr, OCTL_CAMERA_IDX, "bad_idx",
                 "camera index %d out of range [0..%d]", idx, NUM_CAMERAS - 1);
        return -2;
    }
    OrionCamSettings_t *meta = &cams_out->OrionCamSettings[idx];
    if (meta->Type == CAMERA_TYPE_NONE) {
        jout_err(stderr, OCTL_CAMERA_IDX, "empty_slot",
                 "camera slot %d is not populated", idx);
        return -3;
    }
    *active_out = active;
    *idx_out    = idx;
    *meta_out   = meta;
    return 0;
}

static int verify_hfov(int timeout_ms, float target_hfov_rad, float *observed_out, int *verify_ms_out)
{
    long long start = now_ms_local();
    long long deadline = start + timeout_ms;
    float observed = NAN;
    while (now_ms_local() < deadline) {
        OrionPkt_t pkt;
        int remaining = (int)(deadline - now_ms_local());
        if (remaining <= 0) break;
        if (conn_wait_for(ORION_PKT_GEOLOCATE_TELEMETRY, &pkt, remaining) != 0) break;
        GeolocateTelemetry_t geo;
        if (!DecodeGeolocateTelemetry(&pkt, &geo)) continue;
        observed = geo.base.hfov;
        if (target_hfov_rad > 0.0f &&
            fabsf(observed - target_hfov_rad) <= 0.05f * target_hfov_rad) {
            *observed_out  = observed;
            *verify_ms_out = (int)(now_ms_local() - start);
            return 0;
        }
    }
    *observed_out  = observed;
    *verify_ms_out = (int)(now_ms_local() - start);
    return -1;
}

static int send_camera_state(uint8_t idx, float zoom, float focus, int timeout_ms)
{
    OrionCameraState_t st = {0};
    st.Index = idx;
    st.Zoom  = zoom;
    st.Focus = focus;
    st.KeepActiveCamera = 1;
    OrionPkt_t pkt, echo;
    encodeOrionCameraStatePacketStructure(&pkt, &st);
    if (conn_send(&pkt) != 0) return -1;
    if (conn_wait_for(ORION_PKT_CAMERA_STATE, &echo, timeout_ms) != 0) return -2;
    return 0;
}

static int common_fov_zoom_send(octl_ctx_t *ctx, int has_fov, double fov_rad_in,
                                int has_zoom_direct, double zoom_in)
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
    int active = -1, idx = -1;
    OrionCamSettings_t *meta = NULL;
    int rsrc = resolve_target_slot(ctx, &cams, &active, &idx, &meta);
    if (rsrc < 0) {
        conn_close();
        if (rsrc == -1) return OCTL_TIMEOUT;
        return OCTL_CAMERA_IDX;
    }

    float pitch_m   = meta->PixelPitch     * 1.0e-3f;
    float fmin_m    = meta->MinFocalLength * 1.0e-3f;
    float fmax_m    = meta->MaxFocalLength * 1.0e-3f;
    float max_zoom  = effective_max_zoom(fmin_m, fmax_m);

    float zoom_cmd = 1.0f;
    float fov_cmd  = 0.0f;

    if (has_fov) {
        fov_cmd  = (float)fov_rad_in;
        zoom_cmd = zoom_from_fov(pitch_m, (float)meta->ArrayWidth, fmin_m, fov_cmd);
        if (zoom_cmd <= 0.0f) {
            conn_close();
            jout_err(stderr, OCTL_REJECTED, "fov_compute_failed",
                     "fov_from_zoom failed for slot %d", idx);
            return OCTL_REJECTED;
        }
    } else if (has_zoom_direct) {
        zoom_cmd = (float)zoom_in;
        fov_cmd  = fov_from_zoom(pitch_m, (float)meta->ArrayWidth, fmin_m, zoom_cmd);
    }

    if (zoom_cmd < 1.0f - 1e-6f || zoom_cmd > max_zoom + 1e-6f) {
        float min_fov = fov_from_zoom(pitch_m, (float)meta->ArrayWidth, fmin_m, max_zoom);
        float max_fov = fov_from_zoom(pitch_m, (float)meta->ArrayWidth, fmin_m, 1.0f);
        conn_close();
        FILE *err = stderr;
        jout_t j; jout_init(&j, err);
        jout_obj_open(&j);
        jout_kv_str (&j, "error", has_fov ? "fov_out_of_range" : "zoom_out_of_range");
        jout_kv_int (&j, "index", idx);
        jout_kv_dbl (&j, "requested_zoom", zoom_cmd);
        jout_kv_dbl (&j, "max_zoom",       max_zoom);
        jout_kv_dbl (&j, "min_deg",        min_fov * (180.0 / M_PI));
        jout_kv_dbl (&j, "max_deg",        max_fov * (180.0 / M_PI));
        jout_kv_int (&j, "exit_code",      OCTL_REJECTED);
        jout_obj_close(&j);
        jout_done(&j);
        return OCTL_REJECTED;
    }

    if (send_camera_state((uint8_t)idx, zoom_cmd, -1.0f, ctx->timeout_ms) != 0) {
        conn_close();
        jout_err(stderr, OCTL_TIMEOUT, "send_or_echo_failed",
                 "CameraState send/echo failed");
        return OCTL_TIMEOUT;
    }

    int is_active = (idx == active);
    jout_t j;
    jout_init(&j, stdout);
    jout_obj_open(&j);
    jout_kv_str (&j, "ip", ctx->ip);
    jout_kv_int (&j, "index", idx);
    jout_kv_bool(&j, "is_active", is_active);
    jout_kv_dbl (&j, "request_zoom",    zoom_cmd);
    jout_kv_dbl (&j, "request_fov_deg", fov_cmd * (180.0 / M_PI));

    if (is_active) {
        float observed = NAN;
        int   verify_ms = 0;
        int   ok = verify_hfov(ctx->timeout_ms, fov_cmd, &observed, &verify_ms);
        jout_kv_dbl(&j, "observed_hfov_deg", isnan(observed) ? 0.0 : observed * (180.0 / M_PI));
        jout_kv_int(&j, "verify_ms", verify_ms);
        jout_kv_str(&j, "status", ok == 0 ? "ok" : "in_progress");
        if (ok != 0) jout_kv_str(&j, "note", "continuous-zoom lenses may take >2s");
    } else {
        jout_kv_str(&j, "status", "sent");
        jout_kv_str(&j, "note",   "target slot inactive, cannot verify HFOV");
    }
    jout_obj_close(&j);
    jout_done(&j);
    conn_close();
    return OCTL_OK;
}

static int cmd_camera_fov(octl_ctx_t *ctx)
{
    double fov_rad = 0.0;
    if (ctx->deg_set) {
        fov_rad = ctx->deg_value * (M_PI / 180.0);
    } else if (ctx->npos >= 3) {
        char *end;
        double v = strtod(ctx->pos[2], &end);
        if (*end != '\0' || end == ctx->pos[2]) {
            jout_err(stderr, OCTL_USAGE, "bad_fov",
                     "fov requires <radians> or --deg <degrees>");
            return OCTL_USAGE;
        }
        fov_rad = v;
    } else {
        jout_err(stderr, OCTL_USAGE, "missing_fov",
                 "fov requires <radians> or --deg <degrees>");
        return OCTL_USAGE;
    }
    if (fov_rad <= 0.0 || fov_rad >= M_PI) {
        jout_err(stderr, OCTL_USAGE, "bad_fov",
                 "fov must be in (0, pi) radians");
        return OCTL_USAGE;
    }
    return common_fov_zoom_send(ctx, 1, fov_rad, 0, 0.0);
}

static int cmd_camera_zoom(octl_ctx_t *ctx)
{
    if (ctx->npos < 3) {
        jout_err(stderr, OCTL_USAGE, "missing_zoom", "zoom requires <multiplier>");
        return OCTL_USAGE;
    }
    char *end;
    double v = strtod(ctx->pos[2], &end);
    if (*end != '\0' || end == ctx->pos[2] || v <= 0.0) {
        jout_err(stderr, OCTL_USAGE, "bad_zoom",
                 "zoom requires positive multiplier");
        return OCTL_USAGE;
    }
    return common_fov_zoom_send(ctx, 0, 0.0, 1, v);
}

static int cmd_camera_focus(octl_ctx_t *ctx)
{
    if (ctx->npos < 3) {
        jout_err(stderr, OCTL_USAGE, "missing_focus",
                 "focus requires <0..1|auto|-1>");
        return OCTL_USAGE;
    }
    const char *arg = ctx->pos[2];
    float focus_val;
    if (strcmp(arg, "auto") == 0) {
        focus_val = 0.0f;
    } else {
        char *end;
        double v = strtod(arg, &end);
        if (*end != '\0' || end == arg) {
            jout_err(stderr, OCTL_USAGE, "bad_focus",
                     "focus must be number or 'auto'");
            return OCTL_USAGE;
        }
        if (v < -1.0 || v > 1.0) {
            jout_err(stderr, OCTL_USAGE, "focus_out_of_range",
                     "focus must be in [-1, 1] (-1=no change, 0=auto, 0..1=manual)");
            return OCTL_USAGE;
        }
        focus_val = (float)v;
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
    int active = -1, idx = -1;
    OrionCamSettings_t *meta = NULL;
    int rsrc = resolve_target_slot(ctx, &cams, &active, &idx, &meta);
    if (rsrc < 0) {
        conn_close();
        return rsrc == -1 ? OCTL_TIMEOUT : OCTL_CAMERA_IDX;
    }

    if (send_camera_state((uint8_t)idx, 0.0f, focus_val, ctx->timeout_ms) != 0) {
        conn_close();
        jout_err(stderr, OCTL_TIMEOUT, "send_or_echo_failed",
                 "CameraState send/echo failed");
        return OCTL_TIMEOUT;
    }
    conn_close();

    jout_t j;
    jout_init(&j, stdout);
    jout_obj_open(&j);
    jout_kv_str (&j, "ip", ctx->ip);
    jout_kv_int (&j, "index", idx);
    jout_kv_bool(&j, "is_active", idx == active);
    jout_kv_dbl (&j, "request_focus", focus_val);
    jout_kv_str (&j, "status", "sent");
    jout_obj_close(&j);
    jout_done(&j);
    return OCTL_OK;
}

int cmd_camera(octl_ctx_t *ctx)
{
    if (ctx->npos < 2) {
        jout_err(stderr, OCTL_USAGE, "missing_subverb",
                 "camera requires subverb: get | switch | fov | zoom | focus");
        return OCTL_USAGE;
    }
    const char *sub = ctx->pos[1];
    if (strcmp(sub, "get")    == 0) return cmd_camera_get(ctx);
    if (strcmp(sub, "switch") == 0) return cmd_camera_switch(ctx);
    if (strcmp(sub, "fov")    == 0) return cmd_camera_fov(ctx);
    if (strcmp(sub, "zoom")   == 0) return cmd_camera_zoom(ctx);
    if (strcmp(sub, "focus")  == 0) return cmd_camera_focus(ctx);
    if (strcmp(sub, "set")    == 0) return cmd_camera_set(ctx);

    jout_err(stderr, OCTL_USAGE, "unknown_subverb",
             "unknown camera subverb: %s", sub);
    return OCTL_USAGE;
}
