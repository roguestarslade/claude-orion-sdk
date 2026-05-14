#include "orionctl.h"
#include "conn.h"
#include "json_out.h"

#include <stdio.h>

#include "OrionPublicPacket.h"
#include "OrionPublicPacketShim.h"

static const char *ins_mode_name(int m)
{
    switch (m) {
    case insModeInit1:    return "init1";
    case insModeInit2:    return "init2";
    case insModeAHRS:     return "ahrs";
    case insModeRunHard:  return "run_hard";
    case insModeRun:      return "run";
    case insModeRunTight: return "run_tight";
    default:              return "unknown";
    }
}

static const char *imu_type_name(int t)
{
    switch (t) {
    case imuTypeInternal:  return "internal";
    case imuTypeSensonor:  return "sensonor";
    case imuTypeDmu11:     return "dmu11";
    case imuTypeExternal:  return "external";
    case imuTypeEpson:     return "epson";
    case imuTypeSBGPulse:  return "sbg_pulse";
    case imuTypeVectorNav: return "vectornav";
    default:               return "unknown";
    }
}

static const char *gps_src_name(int s)
{
    switch (s) {
    case externalSource:  return "external";
    case ubloxSource:     return "ublox";
    case mavlinkSource:   return "mavlink";
    case nmeaSource:      return "nmea";
    case novatelSource:   return "novatel";
    case autopilotSource: return "autopilot";
    case piksiSource:     return "piksi";
    case apntSource:      return "apnt";
    default:              return "unknown";
    }
}

int cmd_ins(octl_ctx_t *ctx)
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
    MakeOrionPacket(&req, getInsQualityPacketID(), 0);
    if (conn_send(&req) != 0) {
        jout_err(stderr, OCTL_CONN_FAILED, "send_failed", "could not send InsQuality request");
        conn_close();
        return OCTL_CONN_FAILED;
    }
    if (conn_wait_for(ORION_PKT_INS_QUALITY, &resp, ctx->timeout_ms) != 0) {
        jout_err(stderr, OCTL_TIMEOUT, "ins_timeout",
                 "no InsQuality within %d ms", ctx->timeout_ms);
        conn_close();
        return OCTL_TIMEOUT;
    }
    InsQuality_t q;
    if (!decodeInsQualityPacketStructure(&resp, &q)) {
        jout_err(stderr, OCTL_INTERNAL, "decode_failed", "InsQuality decode failed");
        conn_close();
        return OCTL_INTERNAL;
    }
    conn_close();

    jout_t j;
    jout_init(&j, stdout);
    jout_obj_open(&j);
    jout_kv_uint(&j, "system_ms", q.systemTime);
    jout_kv_str (&j, "ins_mode",    ins_mode_name(q.insMode));
    jout_kv_int (&j, "ins_mode_id", q.insMode);
    jout_kv_str (&j, "imu_type",    imu_type_name(q.imuType));
    jout_kv_int (&j, "imu_type_id", q.imuType);
    jout_kv_str (&j, "gps_source",    gps_src_name(q.gpsSource));
    jout_kv_int (&j, "gps_source_id", q.gpsSource);

    jout_key(&j, "flags");
    jout_obj_open(&j);
    jout_kv_bool(&j, "has_gyro_bias",     q.hasGyroBias);
    jout_kv_bool(&j, "has_gravity_bias",  q.hasGravityBias);
    jout_kv_bool(&j, "has_accel_bias",    q.hasAccelBias);
    jout_kv_bool(&j, "has_clock_bias",    q.hasClockBias);
    jout_kv_bool(&j, "has_pan_tilt_bias", q.hasPanTiltBias);
    jout_kv_bool(&j, "pos_rejected",      q.posRejected);
    jout_kv_bool(&j, "vel_rejected",      q.velRejected);
    jout_kv_bool(&j, "hdg_rejected",      q.hdgRejected);
    jout_obj_close(&j);

    jout_key(&j, "periods_s");
    jout_obj_open(&j);
    jout_kv_dbl(&j, "gps", q.gpsPeriod);
    jout_kv_dbl(&j, "hdg", q.hdgPeriod);
    jout_obj_close(&j);

    jout_key(&j, "chi_square");
    jout_obj_open(&j);
    jout_kv_dbl(&j, "pos", q.posChiSquare);
    jout_kv_dbl(&j, "vel", q.velChiSquare);
    jout_kv_dbl(&j, "hdg", q.hdgChiSquare);
    jout_kv_dbl(&j, "pan_tilt", q.panTiltChiSquare);
    jout_obj_close(&j);

    jout_key(&j, "att_confidence_rad"); jout_arr_open(&j);
    jout_dbl(&j, q.attConfidence[0]);
    jout_dbl(&j, q.attConfidence[1]);
    jout_dbl(&j, q.attConfidence[2]);
    jout_arr_close(&j);

    jout_key(&j, "vel_confidence_mps"); jout_arr_open(&j);
    jout_dbl(&j, q.velConfidence[0]);
    jout_dbl(&j, q.velConfidence[1]);
    jout_dbl(&j, q.velConfidence[2]);
    jout_arr_close(&j);

    jout_key(&j, "pos_confidence_m"); jout_arr_open(&j);
    jout_dbl(&j, q.posConfidence[0]);
    jout_dbl(&j, q.posConfidence[1]);
    jout_dbl(&j, q.posConfidence[2]);
    jout_arr_close(&j);

    jout_key(&j, "gyro_confidence_radps"); jout_arr_open(&j);
    jout_dbl(&j, q.gyroConfidence[0]);
    jout_dbl(&j, q.gyroConfidence[1]);
    jout_dbl(&j, q.gyroConfidence[2]);
    jout_arr_close(&j);

    jout_key(&j, "accel_confidence_mps2"); jout_arr_open(&j);
    jout_dbl(&j, q.accelConfidence[0]);
    jout_dbl(&j, q.accelConfidence[1]);
    jout_dbl(&j, q.accelConfidence[2]);
    jout_arr_close(&j);

    jout_kv_dbl(&j, "gravity_confidence_mps2", q.gravityConfidence);
    jout_kv_dbl(&j, "clock_bias_confidence_m", q.clockBiasConfidence);
    jout_kv_dbl(&j, "clock_drift_confidence_mps", q.clockDriftConfidence);

    jout_key(&j, "gyro_bias_radps"); jout_arr_open(&j);
    jout_dbl(&j, q.gyroBias[0]);
    jout_dbl(&j, q.gyroBias[1]);
    jout_dbl(&j, q.gyroBias[2]);
    jout_arr_close(&j);

    jout_key(&j, "accel_bias_mps2"); jout_arr_open(&j);
    jout_dbl(&j, q.accelBias[0]);
    jout_dbl(&j, q.accelBias[1]);
    jout_dbl(&j, q.accelBias[2]);
    jout_arr_close(&j);

    jout_kv_dbl(&j, "gravity_bias_mps2", q.gravityBias);
    jout_kv_dbl(&j, "clock_bias_m",      q.clockBias);
    jout_kv_dbl(&j, "clock_drift_mps",   q.clockDrift);

    jout_key(&j, "tight_coupled");
    jout_obj_open(&j);
    jout_kv_int(&j, "num_sat_pos_updates", q.numTightSatPosUpdates);
    jout_kv_int(&j, "num_sat_vel_updates", q.numTightSatVelUpdates);
    jout_kv_int(&j, "num_pos_updates",     q.numTightPosUpdates);
    jout_kv_int(&j, "num_vel_updates",     q.numTightVelUpdates);
    jout_obj_close(&j);

    jout_key(&j, "pan_tilt_bias_rad"); jout_arr_open(&j);
    jout_dbl(&j, q.panTiltBias[0]);
    jout_dbl(&j, q.panTiltBias[1]);
    jout_arr_close(&j);

    jout_obj_close(&j);
    jout_done(&j);
    return OCTL_OK;
}
