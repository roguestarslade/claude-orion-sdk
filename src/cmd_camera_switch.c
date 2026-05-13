#include "orionctl.h"
#include "cmd_cameras_internal.h"
#include "conn.h"
#include "json_out.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "OrionPublicPacket.h"
#include "OrionPublicPacketShim.h"

int cmd_camera_switch(octl_ctx_t *ctx)
{
    int idx = -1;
    if (ctx->npos >= 3) {
        char *end;
        long v = strtol(ctx->pos[2], &end, 10);
        if (*end == '\0' && end != ctx->pos[2]) idx = (int)v;
    }
    if (idx < 0 && ctx->idx_set) idx = ctx->idx;

    if (idx < 0 || idx >= NUM_CAMERAS) {
        jout_err(stderr, OCTL_USAGE, "bad_idx",
                 "camera switch requires <idx> in [0..%d]", NUM_CAMERAS - 1);
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

    OrionCameras_t cams;
    int active = -1;
    int rc = cmd_cameras_fetch(ctx, &cams, &active);
    if (rc != 0) {
        conn_close();
        jout_err(stderr, OCTL_TIMEOUT, "cameras_timeout",
                 "could not retrieve Cameras packet (rc=%d)", rc);
        return OCTL_TIMEOUT;
    }
    if (cams.OrionCamSettings[idx].Type == CAMERA_TYPE_NONE) {
        conn_close();
        jout_err(stderr, OCTL_CAMERA_IDX, "empty_slot",
                 "camera slot %d is not populated", idx);
        return OCTL_CAMERA_IDX;
    }

    OrionPkt_t swp, echo;
    encodeOrionCameraSwitchPacket(&swp, (uint8_t)idx);
    if (conn_send(&swp) != 0) {
        conn_close();
        jout_err(stderr, OCTL_INTERNAL, "send_failed", "send CameraSwitch failed");
        return OCTL_INTERNAL;
    }
    int switch_ok = (conn_wait_for(ORION_PKT_CAMERA_SWITCH, &echo, ctx->timeout_ms) == 0);

    int zoom_ok = -1;
    if (ctx->zoom_set) {
        OrionCameraState_t st = {0};
        st.Index = (uint8_t)idx;
        st.Zoom  = (float)ctx->zoom;
        st.Focus = -1.0f;
        st.KeepActiveCamera = 1;
        OrionPkt_t sp;
        encodeOrionCameraStatePacketStructure(&sp, &st);
        if (conn_send(&sp) == 0 &&
            conn_wait_for(ORION_PKT_CAMERA_STATE, &echo, ctx->timeout_ms) == 0) {
            zoom_ok = 1;
        } else {
            zoom_ok = 0;
        }
    }
    conn_close();

    jout_t j;
    jout_init(&j, stdout);
    jout_obj_open(&j);
    jout_kv_str (&j, "ip", ctx->ip);
    jout_kv_int (&j, "target_index", idx);
    jout_kv_int (&j, "previous_active_index", active);
    jout_kv_bool(&j, "switch_echo_ok", switch_ok);
    if (ctx->zoom_set) {
        jout_kv_dbl (&j, "zoom_target",  ctx->zoom);
        jout_kv_bool(&j, "zoom_echo_ok", zoom_ok == 1);
    }
    if (!switch_ok) jout_kv_str(&j, "warning", "switch_echo_timeout");
    jout_obj_close(&j);
    jout_done(&j);

    if (!switch_ok) return OCTL_TIMEOUT;
    if (ctx->zoom_set && zoom_ok != 1) return OCTL_TIMEOUT;
    return OCTL_OK;
}
