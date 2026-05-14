#include "orionctl.h"
#include "conn.h"
#include "json_out.h"

#include <stdio.h>

#include "OrionPublicPacket.h"
#include "OrionPublicPacketShim.h"

int cmd_vibration(octl_ctx_t *ctx)
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

    OrionPkt_t req, resp;
    MakeOrionPacket(&req, getOrionVibrationPacketID(), 0);
    if (conn_send(&req) != 0) {
        jout_err(stderr, OCTL_CONN_FAILED, "send_failed", "could not send Vibration request");
        conn_close();
        return OCTL_CONN_FAILED;
    }
    if (conn_wait_for(ORION_PKT_VIBRATION, &resp, ctx->timeout_ms) != 0) {
        jout_err(stderr, OCTL_TIMEOUT, "vibration_timeout",
                 "no Vibration within %d ms", ctx->timeout_ms);
        conn_close();
        return OCTL_TIMEOUT;
    }
    OrionVibration_t v;
    if (!decodeOrionVibrationPacketStructure(&resp, &v)) {
        jout_err(stderr, OCTL_INTERNAL, "decode_failed", "Vibration decode failed");
        conn_close();
        return OCTL_INTERNAL;
    }
    conn_close();

    jout_t j;
    jout_init(&j, stdout);
    jout_obj_open(&j);

    jout_key(&j, "max_accel_mps2"); jout_arr_open(&j);
    jout_dbl(&j, v.MaxAccel[0]);
    jout_dbl(&j, v.MaxAccel[1]);
    jout_dbl(&j, v.MaxAccel[2]);
    jout_arr_close(&j);

    jout_key(&j, "max_gyro_radps"); jout_arr_open(&j);
    jout_dbl(&j, v.MaxGyro[0]);
    jout_dbl(&j, v.MaxGyro[1]);
    jout_dbl(&j, v.MaxGyro[2]);
    jout_arr_close(&j);

    jout_key(&j, "fft_bins");
    jout_arr_open(&j);
    for (int i = 0; i < 16; i++) {
        const FftData_t *b = &v.FftData[i];
        jout_obj_open(&j);
        jout_kv_dbl(&j, "frequency_hz", b->Frequency);
        jout_key(&j, "accel_pct"); jout_arr_open(&j);
        jout_dbl(&j, b->Accel[0]);
        jout_dbl(&j, b->Accel[1]);
        jout_dbl(&j, b->Accel[2]);
        jout_arr_close(&j);
        jout_key(&j, "gyro_pct"); jout_arr_open(&j);
        jout_dbl(&j, b->Gyro[0]);
        jout_dbl(&j, b->Gyro[1]);
        jout_dbl(&j, b->Gyro[2]);
        jout_arr_close(&j);
        jout_obj_close(&j);
    }
    jout_arr_close(&j);

    jout_obj_close(&j);
    jout_done(&j);
    return OCTL_OK;
}
