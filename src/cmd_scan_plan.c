#include "orionctl.h"
#include "conn.h"
#include "json_out.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "OrionPublicPacket.h"
#include "OrionPublicPacketShim.h"

static const char *state_name(int s) {
    switch (s) {
    case SCAN_STOPPED:            return "stopped";
    case SCAN_RUNNING:            return "running";
    case SCAN_RUNNING_TARGET_RATE:return "running_target_rate";
    default:                      return "unknown";
    }
}
static const char *frame_name(int f) {
    switch (f) {
    case SCAN_WORLD:  return "world";
    case SCAN_GIMBAL: return "gimbal";
    default:          return "unknown";
    }
}

static int read_sp(octl_ctx_t *ctx, OrionScanPlan_t *out)
{
    OrionPkt_t req, resp;
    MakeOrionPacket(&req, getOrionScanPlanPacketID(), 0);
    if (conn_send(&req) != 0) return -1;
    if (conn_wait_for(ORION_PKT_SCAN_PLAN, &resp, ctx->timeout_ms) != 0) return -2;
    if (!decodeOrionScanPlanPacketStructure(&resp, out)) return -3;
    return 0;
}

static void emit(jout_t *j, const OrionScanPlan_t *p)
{
    jout_obj_open(j);
    jout_kv_uint(j, "enabled",                p->enabled);
    jout_kv_str (j, "state",                  state_name(p->state));
    jout_kv_int (j, "state_id",               p->state);
    jout_kv_uint(j, "horizontal_scan",        p->horizontalScan);
    jout_kv_uint(j, "use_percent_horizon",    p->usePercentHorizon);
    jout_kv_uint(j, "use_target_hfov",        p->useTargetHfov);
    jout_kv_str (j, "reference_frame",        frame_name(p->referenceFrame));
    jout_kv_int (j, "reference_frame_id",     p->referenceFrame);
    jout_kv_uint(j, "enable_autonomous_rate", p->enableAutonomousRate);
    jout_kv_dbl (j, "sweep_angle_deg",        p->sweepAngleDeg);
    jout_kv_dbl (j, "hfov_rate_deg_per_s",    p->hfovRateDeg);
    jout_kv_dbl (j, "hfov_deg",               p->hfovDeg);
    jout_kv_dbl (j, "max_target_distance_km", p->maxTargetDistanceKm);
    jout_kv_dbl (j, "min_target_distance_km", p->minTargetDistanceKm);
    jout_kv_dbl (j, "horizon_percent",        p->horizonPercent);
    jout_kv_dbl (j, "yaw_offset_deg",         p->yawOffsetDeg);
    jout_obj_close(j);
}

static int sp_get(octl_ctx_t *ctx)
{
    if (octl_resolve_ip(ctx) != 0) { jout_err(stderr, OCTL_CONN_FAILED, "no_ip", "no gimbal IP"); return OCTL_CONN_FAILED; }
    if (conn_open(ctx->ip) != 0)   { jout_err(stderr, OCTL_CONN_FAILED, "connect_failed", "could not open TCP to %s", ctx->ip); return OCTL_CONN_FAILED; }
    conn_drain(OCTL_DRAIN_MS);
    OrionScanPlan_t p;
    int rc = read_sp(ctx, &p);
    conn_close();
    if (rc == -2) { jout_err(stderr, OCTL_TIMEOUT, "scan_plan_timeout", "no ScanPlan in %d ms", ctx->timeout_ms); return OCTL_TIMEOUT; }
    if (rc != 0)  { jout_err(stderr, OCTL_INTERNAL, "scan_plan_failed", "ScanPlan rc=%d", rc); return OCTL_INTERNAL; }
    jout_t j; jout_init(&j, stdout); emit(&j, &p); jout_done(&j);
    return OCTL_OK;
}

static int set_field(OrionScanPlan_t *p, const char *name, const char *v)
{
    char *end;
    if      (!strcmp(name, "enabled"))              p->enabled              = (uint8_t)strtoul(v, &end, 0);
    else if (!strcmp(name, "state"))                p->state                = (ScanPlanState_t)strtoul(v, &end, 0);
    else if (!strcmp(name, "horizontalScan"))       p->horizontalScan       = (uint8_t)strtoul(v, &end, 0);
    else if (!strcmp(name, "usePercentHorizon"))    p->usePercentHorizon    = (uint8_t)strtoul(v, &end, 0);
    else if (!strcmp(name, "useTargetHfov"))        p->useTargetHfov        = (uint8_t)strtoul(v, &end, 0);
    else if (!strcmp(name, "referenceFrame"))       p->referenceFrame       = (ScanPlanReferenceFrame_t)strtoul(v, &end, 0);
    else if (!strcmp(name, "enableAutonomousRate")) p->enableAutonomousRate = (uint8_t)strtoul(v, &end, 0);
    else if (!strcmp(name, "sweepAngleDeg"))        p->sweepAngleDeg        = (float)strtod(v, &end);
    else if (!strcmp(name, "hfovRateDeg"))          p->hfovRateDeg          = (float)strtod(v, &end);
    else if (!strcmp(name, "hfovDeg"))              p->hfovDeg              = (float)strtod(v, &end);
    else if (!strcmp(name, "maxTargetDistanceKm"))  p->maxTargetDistanceKm  = (float)strtod(v, &end);
    else if (!strcmp(name, "minTargetDistanceKm"))  p->minTargetDistanceKm  = (float)strtod(v, &end);
    else if (!strcmp(name, "horizonPercent"))       p->horizonPercent       = (float)strtod(v, &end);
    else if (!strcmp(name, "yawOffsetDeg"))         p->yawOffsetDeg         = (float)strtod(v, &end);
    else return -1;
    if (*end != '\0' || end == v) return -1;
    return 0;
}

static int sp_set(octl_ctx_t *ctx)
{
    if (ctx->npos < 3) { jout_err(stderr, OCTL_USAGE, "missing_args", "scan-plan set requires Field=Value"); return OCTL_USAGE; }
    if (octl_resolve_ip(ctx) != 0) { jout_err(stderr, OCTL_CONN_FAILED, "no_ip", "no gimbal IP"); return OCTL_CONN_FAILED; }
    if (conn_open(ctx->ip) != 0)   { jout_err(stderr, OCTL_CONN_FAILED, "connect_failed", "could not open TCP to %s", ctx->ip); return OCTL_CONN_FAILED; }
    conn_drain(OCTL_DRAIN_MS);

    OrionScanPlan_t p;
    int rc = read_sp(ctx, &p);
    if (rc != 0) {
        jout_err(stderr, rc == -2 ? OCTL_TIMEOUT : OCTL_INTERNAL,
                 rc == -2 ? "scan_plan_timeout" : "scan_plan_failed",
                 "ScanPlan pre-read failed (rc=%d)", rc);
        conn_close();
        return rc == -2 ? OCTL_TIMEOUT : OCTL_INTERNAL;
    }
    int mutations = 0;
    for (int i = 2; i < ctx->npos; i++) {
        const char *kv = ctx->pos[i];
        const char *eq = strchr(kv, '=');
        if (!eq || eq == kv) { jout_err(stderr, OCTL_USAGE, "bad_kv", "expected Field=Value: %s", kv); conn_close(); return OCTL_USAGE; }
        char name[40]; size_t L = (size_t)(eq - kv);
        if (L >= sizeof(name)) { jout_err(stderr, OCTL_USAGE, "field_too_long", "field name too long"); conn_close(); return OCTL_USAGE; }
        memcpy(name, kv, L); name[L] = '\0';
        if (set_field(&p, name, eq + 1) != 0) {
            jout_err(stderr, OCTL_USAGE, "bad_field", "unknown/invalid field: %s", name);
            conn_close();
            return OCTL_USAGE;
        }
        mutations++;
    }
    OrionPkt_t out;
    encodeOrionScanPlanPacketStructure(&out, &p);
    int sent = (conn_send(&out) == 0);
    OrionPkt_t echo;
    int got = sent && (conn_wait_for(ORION_PKT_SCAN_PLAN, &echo, ctx->timeout_ms) == 0);
    OrionScanPlan_t e; int e_ok = got && decodeOrionScanPlanPacketStructure(&echo, &e);
    conn_close();
    jout_t j; jout_init(&j, stdout);
    jout_obj_open(&j);
    jout_kv_int (&j, "mutations", mutations);
    jout_kv_bool(&j, "echo_seen", got);
    if (e_ok) { jout_key(&j, "echo"); emit(&j, &e); } else { jout_key(&j, "written"); emit(&j, &p); }
    jout_obj_close(&j);
    jout_done(&j);
    return OCTL_OK;
}

int cmd_scan_plan(octl_ctx_t *ctx)
{
    if (ctx->npos < 2) { jout_err(stderr, OCTL_USAGE, "missing_subverb", "scan-plan get | set"); return OCTL_USAGE; }
    const char *sub = ctx->pos[1];
    if (strcmp(sub, "get") == 0) return sp_get(ctx);
    if (strcmp(sub, "set") == 0) return sp_set(ctx);
    jout_err(stderr, OCTL_USAGE, "unknown_subverb", "unknown scan-plan subverb: %s", sub);
    return OCTL_USAGE;
}
