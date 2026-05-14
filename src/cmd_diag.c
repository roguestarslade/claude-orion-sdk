#include "orionctl.h"
#include "conn.h"
#include "json_out.h"

#include <stdio.h>

#include "OrionPublicPacket.h"
#include "OrionPublicPacketShim.h"

int cmd_diag(octl_ctx_t *ctx)
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
    MakeOrionPacket(&req, getOrionDiagnosticsPacketID(), 0);
    if (conn_send(&req) != 0) {
        jout_err(stderr, OCTL_CONN_FAILED, "send_failed", "could not send Diagnostics request");
        conn_close();
        return OCTL_CONN_FAILED;
    }
    if (conn_wait_for(ORION_PKT_DIAGNOSTICS, &resp, ctx->timeout_ms) != 0) {
        jout_err(stderr, OCTL_TIMEOUT, "diag_timeout",
                 "no Diagnostics within %d ms", ctx->timeout_ms);
        conn_close();
        return OCTL_TIMEOUT;
    }
    OrionDiagnostics_t d;
    if (!decodeOrionDiagnosticsPacketStructure(&resp, &d)) {
        jout_err(stderr, OCTL_INTERNAL, "decode_failed", "Diagnostics decode failed");
        conn_close();
        return OCTL_INTERNAL;
    }
    conn_close();

    jout_t j;
    jout_init(&j, stdout);
    jout_obj_open(&j);

    jout_key(&j, "rails");
    jout_obj_open(&j);
    jout_key(&j, "v24");
    jout_obj_open(&j);
    jout_kv_dbl(&j, "voltage", d.Voltage24);
    jout_kv_dbl(&j, "current_a", d.Current24);
    jout_kv_dbl(&j, "voltage_var", d.Voltage24Var);
    jout_kv_dbl(&j, "current_var_a", d.Current24Var);
    jout_obj_close(&j);
    jout_key(&j, "v12");
    jout_obj_open(&j);
    jout_kv_dbl(&j, "voltage", d.Voltage12);
    jout_kv_dbl(&j, "current_a", d.Current12);
    jout_kv_dbl(&j, "voltage_var", d.Voltage12Var);
    jout_kv_dbl(&j, "current_var_a", d.Current12Var);
    jout_obj_close(&j);
    jout_key(&j, "v3v3");
    jout_obj_open(&j);
    jout_kv_dbl(&j, "voltage", d.Voltage3v3);
    jout_kv_dbl(&j, "current_a", d.Current3v3);
    jout_kv_dbl(&j, "voltage_var", d.Voltage3v3Var);
    jout_kv_dbl(&j, "current_var_a", d.Current3v3Var);
    jout_obj_close(&j);
    jout_obj_close(&j);

    jout_key(&j, "temps_c");
    jout_obj_open(&j);
    jout_kv_dbl(&j, "crown",   d.CrownTemp);
    jout_kv_dbl(&j, "sla",     d.SlaTemp);
    jout_kv_dbl(&j, "gyro",    d.GyroTemp);
    jout_kv_dbl(&j, "payload", d.PayloadTemp);
    jout_obj_close(&j);

    jout_kv_dbl(&j, "payload_humidity", d.PayloadHumidity);
    jout_kv_dbl(&j, "laser_current_a",  d.CurrentLaser);

    jout_obj_close(&j);
    jout_done(&j);
    return OCTL_OK;
}
