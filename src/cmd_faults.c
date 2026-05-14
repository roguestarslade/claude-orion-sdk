#include "orionctl.h"
#include "conn.h"
#include "json_out.h"

#include <stdio.h>
#include <sys/time.h>

#include "OrionPublicPacket.h"
#include "OrionPublicPacketShim.h"

static long long ms_now(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static const char *fault_type_name(int t)
{
    switch (t) {
    case FAULT_TYPE_NONE:                       return "none";
    case FAULT_TYPE_CLEVIS_RESET_COMMANDED:     return "clevis_reset_commanded";
    case FAULT_TYPE_INVALID_GYRO_CALIBRATION:   return "invalid_gyro_calibration";
    case FAULT_TYPE_VELOCITY_LIMIT_EXCEEDED:    return "velocity_limit_exceeded";
    case FAULT_TYPE_ACCELERATION_LIMIT_EXCEEDED:return "acceleration_limit_exceeded";
    default:                                    return "unknown";
    }
}

static const char *fault_level_name(int l)
{
    switch (l) {
    case FAULT_LEVEL_LOG:     return "log";
    case FAULT_LEVEL_INFO:    return "info";
    case FAULT_LEVEL_WARNING: return "warning";
    case FAULT_LEVEL_ERROR:   return "error";
    case FAULT_LEVEL_FATAL:   return "fatal";
    default:                  return "unknown";
    }
}

static const char *fault_component_name(int c)
{
    switch (c) {
    case FAULT_COMPONENT_PAN_AXIS:      return "pan_axis";
    case FAULT_COMPONENT_TILT_AXIS:     return "tilt_axis";
    case FAULT_COMPONENT_PAYLOAD_GYROS: return "payload_gyros";
    default:                            return "unknown";
    }
}

int cmd_faults(octl_ctx_t *ctx)
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

    int since_s = ctx->since_set ? ctx->since_s : 5;
    if (since_s < 0) since_s = 0;
    long long deadline = ms_now() + (long long)since_s * 1000;
    int count = 0;

    jout_t j;
    jout_init(&j, stdout);
    jout_obj_open(&j);
    jout_kv_int(&j, "since_s", since_s);
    jout_key(&j, "faults");
    jout_arr_open(&j);

    long long remaining;
    while ((remaining = deadline - ms_now()) > 0) {
        OrionPkt_t pkt;
        if (conn_wait_for(ORION_PKT_FAULTS, &pkt, (int)remaining) != 0) break;
        OrionFault_t f;
        if (!decodeOrionFaultPacketStructure(&pkt, &f)) continue;
        jout_obj_open(&j);
        jout_kv_str(&j, "type",         fault_type_name(f.Type));
        jout_kv_int(&j, "type_id",      f.Type);
        jout_kv_str(&j, "level",        fault_level_name(f.Level));
        jout_kv_int(&j, "level_id",     f.Level);
        jout_kv_str(&j, "component",    fault_component_name(f.Component));
        jout_kv_int(&j, "component_id", f.Component);
        jout_kv_uint(&j, "code",        f.Code);
        jout_kv_dbl(&j, "value",        f.Value);
        jout_obj_close(&j);
        count++;
    }

    jout_arr_close(&j);
    jout_kv_int(&j, "count", count);
    jout_obj_close(&j);
    jout_done(&j);
    conn_close();
    return OCTL_OK;
}
