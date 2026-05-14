#include "orionctl.h"
#include "conn.h"
#include "json_out.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "OrionPublicPacket.h"
#include "OrionPublicPacketShim.h"
#include "Constants.h"

static int read_prf(octl_ctx_t *ctx, OrionPRFDetects_t *out)
{
    OrionPkt_t req, resp;
    MakeOrionPacket(&req, getOrionPRFDetectsPacketID(), 0);
    if (conn_send(&req) != 0) return -1;
    if (conn_wait_for(ORION_PKT_PRF_DETECT, &resp, ctx->timeout_ms) != 0) return -2;
    if (!decodeOrionPRFDetectsPacketStructure(&resp, out)) return -3;
    return 0;
}

static void emit(jout_t *j, const OrionPRFDetects_t *p)
{
    jout_obj_open(j);
    jout_kv_uint(j, "detect_state",  p->detectState);
    jout_kv_uint(j, "faux_sequence", p->fauxSequence);
    jout_kv_uint(j, "track_state",   p->trackState);
    jout_key(j, "detects"); jout_arr_open(j);
    for (int i = 0; i < 5; i++) {
        const PRFDetect_t *d = &p->detects[i];
        jout_obj_open(j);
        jout_kv_uint(j, "prf",       d->PRF);
        jout_kv_dbl (j, "pan_rad",   d->panAngle);
        jout_kv_dbl (j, "tilt_rad",  d->tiltAngle);
        jout_kv_dbl (j, "pan_deg",   rad2deg(d->panAngle));
        jout_kv_dbl (j, "tilt_deg",  rad2deg(d->tiltAngle));
        jout_obj_close(j);
    }
    jout_arr_close(j);
    jout_obj_close(j);
}

static int prf_get(octl_ctx_t *ctx)
{
    if (octl_resolve_ip(ctx) != 0) { jout_err(stderr, OCTL_CONN_FAILED, "no_ip", "no gimbal IP"); return OCTL_CONN_FAILED; }
    if (conn_open(ctx->ip) != 0)   { jout_err(stderr, OCTL_CONN_FAILED, "connect_failed", "could not open TCP to %s", ctx->ip); return OCTL_CONN_FAILED; }
    conn_drain(OCTL_DRAIN_MS);
    OrionPRFDetects_t p;
    int rc = read_prf(ctx, &p);
    conn_close();
    if (rc == -2) { jout_err(stderr, OCTL_TIMEOUT, "prf_timeout", "no PRFDetects in %d ms", ctx->timeout_ms); return OCTL_TIMEOUT; }
    if (rc != 0)  { jout_err(stderr, OCTL_INTERNAL, "prf_failed", "PRFDetects rc=%d", rc); return OCTL_INTERNAL; }
    jout_t j; jout_init(&j, stdout); emit(&j, &p); jout_done(&j);
    return OCTL_OK;
}

static int set_field(OrionPRFDetects_t *p, const char *name, const char *v)
{
    char *end;
    if      (!strcmp(name, "detectState"))  p->detectState  = (uint8_t)strtoul(v, &end, 0);
    else if (!strcmp(name, "fauxSequence")) p->fauxSequence = strtoull(v, &end, 0);
    else if (!strcmp(name, "trackState"))   p->trackState   = (uint8_t)strtoul(v, &end, 0);
    else return -1;
    if (*end != '\0' || end == v) return -1;
    return 0;
}

static int prf_set(octl_ctx_t *ctx)
{
    if (ctx->npos < 3) { jout_err(stderr, OCTL_USAGE, "missing_args", "prf set requires Field=Value"); return OCTL_USAGE; }
    if (octl_resolve_ip(ctx) != 0) { jout_err(stderr, OCTL_CONN_FAILED, "no_ip", "no gimbal IP"); return OCTL_CONN_FAILED; }
    if (conn_open(ctx->ip) != 0)   { jout_err(stderr, OCTL_CONN_FAILED, "connect_failed", "could not open TCP to %s", ctx->ip); return OCTL_CONN_FAILED; }
    conn_drain(OCTL_DRAIN_MS);

    OrionPRFDetects_t p;
    int rc = read_prf(ctx, &p);
    if (rc != 0) {
        jout_err(stderr, rc == -2 ? OCTL_TIMEOUT : OCTL_INTERNAL,
                 rc == -2 ? "prf_timeout" : "prf_failed",
                 "PRFDetects pre-read failed (rc=%d)", rc);
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
        if (set_field(&p, name, eq + 1) != 0) {
            jout_err(stderr, OCTL_USAGE, "bad_field", "unknown field: %s", name);
            conn_close();
            return OCTL_USAGE;
        }
        mutations++;
    }
    OrionPkt_t out;
    encodeOrionPRFDetectsPacketStructure(&out, &p);
    int sent = (conn_send(&out) == 0);
    OrionPkt_t echo;
    int got = sent && (conn_wait_for(ORION_PKT_PRF_DETECT, &echo, ctx->timeout_ms) == 0);
    OrionPRFDetects_t e; int e_ok = got && decodeOrionPRFDetectsPacketStructure(&echo, &e);
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

int cmd_prf(octl_ctx_t *ctx)
{
    if (ctx->npos < 2) { jout_err(stderr, OCTL_USAGE, "missing_subverb", "prf get | set"); return OCTL_USAGE; }
    const char *sub = ctx->pos[1];
    if (strcmp(sub, "get") == 0) return prf_get(ctx);
    if (strcmp(sub, "set") == 0) return prf_set(ctx);
    jout_err(stderr, OCTL_USAGE, "unknown_subverb", "unknown prf subverb: %s", sub);
    return OCTL_USAGE;
}
