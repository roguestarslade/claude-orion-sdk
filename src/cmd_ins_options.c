#include "orionctl.h"
#include "conn.h"
#include "json_out.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "OrionPublicPacket.h"
#include "OrionPublicPacketShim.h"

static int read_io(octl_ctx_t *ctx, InsOptions_t *out)
{
    OrionPkt_t req, resp;
    MakeOrionPacket(&req, getInsOptionsPacketID(), 0);
    if (conn_send(&req) != 0) return -1;
    if (conn_wait_for(ORION_PKT_INS_OPTIONS, &resp, ctx->timeout_ms) != 0) return -2;
    if (!decodeInsOptionsPacketStructure(&resp, out)) return -3;
    return 0;
}

static void emit(jout_t *j, const InsOptions_t *o)
{
    jout_obj_open(j);
    jout_kv_bool(j, "enable_platform_rotation", o->enablePlatformRotation);
    jout_kv_bool(j, "enable_course_is_heading", o->enableCourseIsHeading);
    jout_kv_bool(j, "disable_magnetometer",     o->disableMagnetometer);
    jout_kv_bool(j, "disable_gps",              o->disableGPS);
    jout_kv_bool(j, "reset_ins",                o->resetINS);
    jout_kv_bool(j, "disable_gps_clock_error",  o->disableGpsClockError);
    jout_kv_bool(j, "enable_accel_bias",        o->enableAccelBias);
    jout_kv_bool(j, "enable_tightly_coupled",   o->enableTightlyCoupled);
    jout_kv_bool(j, "use_phase_for_velocity",   o->usePhaseForVelocity);
    jout_kv_bool(j, "disable_l1",               o->disableL1);
    jout_kv_bool(j, "disable_l2",               o->disableL2);
    jout_kv_bool(j, "disable_l5",               o->disableL5);
    jout_kv_dbl (j, "elevation_mask_rad",       o->elevationMask);
    jout_kv_bool(j, "enable_payload_ins",       o->enablePayloadIns);
    jout_kv_bool(j, "enable_non_linear_heading",o->enableNonLinearHeading);
    jout_kv_bool(j, "enable_sensonor_mv",       o->enableSensonorMV);
    jout_kv_bool(j, "enable_hd25_payload_ins",  o->enableHD25PayloadINS);
    jout_kv_uint(j, "optical_flow_setting",     o->opticalFlowSetting);
    jout_kv_bool(j, "enable_custom_imu_rotation",o->enableCustomIMURotation);
    jout_key(j, "gimbal_to_platform_euler_rad"); jout_arr_open(j);
    jout_dbl(j, o->gimbalToPlatformEuler[0]); jout_dbl(j, o->gimbalToPlatformEuler[1]); jout_dbl(j, o->gimbalToPlatformEuler[2]);
    jout_arr_close(j);
    jout_kv_dbl(j, "initial_heading_rad", o->initialHeading);
    jout_key(j, "gps_lever_arm_m"); jout_arr_open(j);
    jout_dbl(j, o->gpsLeverArm[0]); jout_dbl(j, o->gpsLeverArm[1]); jout_dbl(j, o->gpsLeverArm[2]);
    jout_arr_close(j);
    jout_kv_dbl(j, "heading_observation_bias_angle_rad", o->headingObservationBiasAngle);
    jout_key(j, "second_gps_lever_arm_m"); jout_arr_open(j);
    jout_dbl(j, o->secondGPSLeverArm[0]); jout_dbl(j, o->secondGPSLeverArm[1]); jout_dbl(j, o->secondGPSLeverArm[2]);
    jout_arr_close(j);
    jout_kv_dbl(j, "align_heading_noise_rad", o->alignHeadingNoise);
    jout_key(j, "imu_to_ins_euler_rad"); jout_arr_open(j);
    jout_dbl(j, o->imuToInsEuler[0]); jout_dbl(j, o->imuToInsEuler[1]); jout_dbl(j, o->imuToInsEuler[2]);
    jout_arr_close(j);
    jout_obj_close(j);
}

static int set_field(InsOptions_t *o, const char *name, const char *v)
{
    char *end;
    unsigned long uv = 0;
    double dv = 0;
    int as_uint = 0, as_dbl = 0;
    /* parse numeric */
    uv = strtoul(v, &end, 0);
    if (*end == '\0' && end != v) as_uint = 1;
    end = NULL;
    dv = strtod(v, &end);
    if (*end == '\0' && end != v) as_dbl = 1;
    if (!as_uint && !as_dbl) return -1;

    if      (!strcmp(name, "enablePlatformRotation")) o->enablePlatformRotation = (unsigned)uv;
    else if (!strcmp(name, "enableCourseIsHeading"))  o->enableCourseIsHeading  = (unsigned)uv;
    else if (!strcmp(name, "disableMagnetometer"))    o->disableMagnetometer    = (unsigned)uv;
    else if (!strcmp(name, "disableGPS"))             o->disableGPS             = (unsigned)uv;
    else if (!strcmp(name, "resetINS"))               o->resetINS               = (unsigned)uv;
    else if (!strcmp(name, "disableGpsClockError"))   o->disableGpsClockError   = (unsigned)uv;
    else if (!strcmp(name, "enableAccelBias"))        o->enableAccelBias        = (unsigned)uv;
    else if (!strcmp(name, "enableTightlyCoupled"))   o->enableTightlyCoupled   = (unsigned)uv;
    else if (!strcmp(name, "usePhaseForVelocity"))    o->usePhaseForVelocity    = (unsigned)uv;
    else if (!strcmp(name, "disableL1"))              o->disableL1              = (unsigned)uv;
    else if (!strcmp(name, "disableL2"))              o->disableL2              = (unsigned)uv;
    else if (!strcmp(name, "disableL5"))              o->disableL5              = (unsigned)uv;
    else if (!strcmp(name, "elevationMask"))          o->elevationMask          = (float)dv;
    else if (!strcmp(name, "enablePayloadIns"))       o->enablePayloadIns       = (unsigned)uv;
    else if (!strcmp(name, "enableNonLinearHeading")) o->enableNonLinearHeading = (unsigned)uv;
    else if (!strcmp(name, "enableSensonorMV"))       o->enableSensonorMV       = (unsigned)uv;
    else if (!strcmp(name, "enableHD25PayloadINS"))   o->enableHD25PayloadINS   = (unsigned)uv;
    else if (!strcmp(name, "opticalFlowSetting"))     o->opticalFlowSetting     = (unsigned)(uv & 0xF);
    else if (!strcmp(name, "enableCustomIMURotation"))o->enableCustomIMURotation= (unsigned)uv;
    else if (!strcmp(name, "initialHeading"))         o->initialHeading         = (float)dv;
    else if (!strcmp(name, "headingObservationBiasAngle")) o->headingObservationBiasAngle = (float)dv;
    else if (!strcmp(name, "alignHeadingNoise"))      o->alignHeadingNoise      = (float)dv;
    else return -1;
    return 0;
}

static int io_get(octl_ctx_t *ctx)
{
    if (octl_resolve_ip(ctx) != 0) { jout_err(stderr, OCTL_CONN_FAILED, "no_ip", "no gimbal IP"); return OCTL_CONN_FAILED; }
    if (conn_open(ctx->ip) != 0)   { jout_err(stderr, OCTL_CONN_FAILED, "connect_failed", "could not open TCP to %s", ctx->ip); return OCTL_CONN_FAILED; }
    conn_drain(OCTL_DRAIN_MS);
    InsOptions_t o;
    int rc = read_io(ctx, &o);
    conn_close();
    if (rc == -2) { jout_err(stderr, OCTL_TIMEOUT, "ins_options_timeout", "no InsOptions in %d ms", ctx->timeout_ms); return OCTL_TIMEOUT; }
    if (rc != 0)  { jout_err(stderr, OCTL_INTERNAL, "ins_options_failed", "InsOptions rc=%d", rc); return OCTL_INTERNAL; }
    jout_t j; jout_init(&j, stdout); emit(&j, &o); jout_done(&j);
    return OCTL_OK;
}

static int io_set(octl_ctx_t *ctx)
{
    if (ctx->npos < 3) { jout_err(stderr, OCTL_USAGE, "missing_args", "ins-options set requires Field=Value"); return OCTL_USAGE; }
    if (octl_resolve_ip(ctx) != 0) { jout_err(stderr, OCTL_CONN_FAILED, "no_ip", "no gimbal IP"); return OCTL_CONN_FAILED; }
    if (conn_open(ctx->ip) != 0)   { jout_err(stderr, OCTL_CONN_FAILED, "connect_failed", "could not open TCP to %s", ctx->ip); return OCTL_CONN_FAILED; }
    conn_drain(OCTL_DRAIN_MS);

    InsOptions_t o;
    int rc = read_io(ctx, &o);
    if (rc != 0) {
        jout_err(stderr, rc == -2 ? OCTL_TIMEOUT : OCTL_INTERNAL,
                 rc == -2 ? "ins_options_timeout" : "ins_options_failed",
                 "InsOptions pre-read failed (rc=%d)", rc);
        conn_close();
        return rc == -2 ? OCTL_TIMEOUT : OCTL_INTERNAL;
    }
    int mutations = 0;
    for (int i = 2; i < ctx->npos; i++) {
        const char *kv = ctx->pos[i];
        const char *eq = strchr(kv, '=');
        if (!eq || eq == kv) { jout_err(stderr, OCTL_USAGE, "bad_kv", "expected Field=Value: %s", kv); conn_close(); return OCTL_USAGE; }
        char name[40]; size_t L = (size_t)(eq - kv);
        if (L >= sizeof(name)) { jout_err(stderr, OCTL_USAGE, "field_too_long", "field too long"); conn_close(); return OCTL_USAGE; }
        memcpy(name, kv, L); name[L] = '\0';
        if (set_field(&o, name, eq + 1) != 0) {
            jout_err(stderr, OCTL_USAGE, "bad_field", "unknown/invalid field: %s", name);
            conn_close();
            return OCTL_USAGE;
        }
        mutations++;
    }
    OrionPkt_t out;
    encodeInsOptionsPacketStructure(&out, &o);
    int sent = (conn_send(&out) == 0);
    OrionPkt_t echo;
    int got = sent && (conn_wait_for(ORION_PKT_INS_OPTIONS, &echo, ctx->timeout_ms) == 0);
    InsOptions_t e; int e_ok = got && decodeInsOptionsPacketStructure(&echo, &e);
    conn_close();
    jout_t j; jout_init(&j, stdout);
    jout_obj_open(&j);
    jout_kv_int (&j, "mutations", mutations);
    jout_kv_bool(&j, "echo_seen", got);
    if (e_ok) { jout_key(&j, "echo"); emit(&j, &e); } else { jout_key(&j, "written"); emit(&j, &o); }
    jout_obj_close(&j);
    jout_done(&j);
    return OCTL_OK;
}

int cmd_ins_options(octl_ctx_t *ctx)
{
    if (ctx->npos < 2) { jout_err(stderr, OCTL_USAGE, "missing_subverb", "ins-options get | set"); return OCTL_USAGE; }
    const char *sub = ctx->pos[1];
    if (strcmp(sub, "get") == 0) return io_get(ctx);
    if (strcmp(sub, "set") == 0) return io_set(ctx);
    jout_err(stderr, OCTL_USAGE, "unknown_subverb", "unknown ins-options subverb: %s", sub);
    return OCTL_USAGE;
}
