#include "orionctl.h"
#include "conn.h"
#include "json_out.h"

#include <stdio.h>
#include <string.h>

#include "OrionPublicPacket.h"
#include "OrionPublicPacketShim.h"

static const char *laser_type_name(int t)
{
    switch (t) {
    case LASER_TYPE_NONE:           return "none";
    case LASER_TYPE_POINTER:        return "pointer";
    case LASER_TYPE_10MJ_MARKER:    return "marker_10mj";
    case LASER_TYPE_LIGHTWARE:      return "lightware";
    case LASER_TYPE_JENOPTIK_DLEM:  return "jenoptik_dlem";
    case LASER_TYPE_VECTRONIX:      return "vectronix";
    case LASER_TYPE_DESIGNATOR:     return "designator";
    case LASER_TYPE_JENOPTIK_DLEM_TEST: return "jenoptik_dlem_test";
    case LASER_TYPE_DESIGNATOR_6x:  return "designator_6x";
    case LASER_TYPE_DESIGNATOR_DUMMY:return "designator_dummy";
    case LASER_TYPE_ARETE_LD:       return "arete_ld";
    case LASER_TYPE_VECTRONIX_3013: return "vectronix_3013";
    default:                        return "unknown";
    }
}

static int read_states(octl_ctx_t *ctx, OrionLaserStates_t *out)
{
    OrionPkt_t req, resp;
    MakeOrionPacket(&req, getOrionLaserStatesPacketID(), 0);
    if (conn_send(&req) != 0) return -1;
    if (conn_wait_for(ORION_PKT_LASER_STATES, &resp, ctx->timeout_ms) != 0) return -2;
    if (!decodeOrionLaserStatesPacketStructure(&resp, out)) return -3;
    return 0;
}

static void emit_one(jout_t *j, int idx, const OrionLaserState_t *s)
{
    jout_obj_open(j);
    jout_kv_int (j, "index",             idx);
    jout_kv_str (j, "type",              laser_type_name(s->Type));
    jout_kv_int (j, "type_id",           s->Type);
    jout_kv_bool(j, "enabled",           s->Enabled);
    jout_kv_bool(j, "armed",             s->Armed);
    jout_kv_bool(j, "active",            s->Active);
    jout_kv_bool(j, "ground_speed_lock", s->GroundSpeedLock);
    jout_kv_bool(j, "altitude_lock",     s->AltitudeLock);
    jout_kv_bool(j, "password_lock",     s->PasswordLock);
    jout_kv_bool(j, "ap_comm_lock",      s->ApCommLock);
    jout_kv_bool(j, "ap_flying_lock",    s->ApFlyingLock);
    jout_kv_bool(j, "bypass_enabled",    s->BypassEnabled);
    jout_kv_bool(j, "pitch_angle_lock",  s->PitchAngleLock);
    jout_kv_bool(j, "watchdog_lock",     s->WatchdogLock);
    jout_kv_bool(j, "aux_output_status", s->AuxOutputStatus);
    jout_kv_bool(j, "aux_operating_mode",s->AuxOperatingMode);
    jout_kv_bool(j, "vcc_undervoltage",  s->VccUndervoltage);
    jout_kv_dbl (j, "laser_temp_c",      s->LaserTemp);
    jout_kv_uint(j, "wait_timer_ms",     s->WaitTimer);
    jout_obj_close(j);
}

static int laser_status(octl_ctx_t *ctx)
{
    if (octl_resolve_ip(ctx) != 0) {
        jout_err(stderr, OCTL_CONN_FAILED, "no_ip", "no gimbal IP");
        return OCTL_CONN_FAILED;
    }
    if (conn_open(ctx->ip) != 0) {
        jout_err(stderr, OCTL_CONN_FAILED, "connect_failed",
                 "could not open TCP to %s", ctx->ip);
        return OCTL_CONN_FAILED;
    }
    conn_drain(OCTL_DRAIN_MS);
    OrionLaserStates_t st;
    int rc = read_states(ctx, &st);
    conn_close();
    if (rc == -2) {
        jout_err(stderr, OCTL_TIMEOUT, "laser_states_timeout",
                 "no LaserStates within %d ms", ctx->timeout_ms);
        return OCTL_TIMEOUT;
    }
    if (rc != 0) {
        jout_err(stderr, OCTL_INTERNAL, "laser_states_failed",
                 "LaserStates read failed (rc=%d)", rc);
        return OCTL_INTERNAL;
    }
    jout_t j; jout_init(&j, stdout);
    jout_obj_open(&j);
    jout_kv_int(&j, "num_lasers", st.NumLasers);
    jout_key(&j, "lasers"); jout_arr_open(&j);
    int n = st.NumLasers;
    if (n < 0) n = 0;
    if (n > MAX_LASERS) n = MAX_LASERS;
    for (int i = 0; i < n; i++) emit_one(&j, i, &st.State[i]);
    jout_arr_close(&j);
    jout_obj_close(&j);
    jout_done(&j);
    return OCTL_OK;
}

/* Map (type, variant) to OrionLaserType_t enum */
static int laser_type_resolve(const char *type, const char *variant, OrionLaserType_t *out)
{
    if (!type) return -1;
    if (!strcmp(type, "pointer")) { *out = LASER_TYPE_POINTER;     return 0; }
    if (!strcmp(type, "marker"))  { *out = LASER_TYPE_10MJ_MARKER; return 0; }
    if (!strcmp(type, "designator")) {
        const char *v = variant ? variant : "default";
        if (!strcmp(v, "default") || !strcmp(v, "designator")) { *out = LASER_TYPE_DESIGNATOR; return 0; }
        if (!strcmp(v, "arete-6x") || !strcmp(v, "arete_ld"))   { *out = LASER_TYPE_ARETE_LD; return 0; }
        if (!strcmp(v, "dummy"))                                { *out = LASER_TYPE_DESIGNATOR_DUMMY; return 0; }
        if (!strcmp(v, "6x") || !strcmp(v, "designator_6x"))    { *out = LASER_TYPE_DESIGNATOR_6x; return 0; }
        return -1;
    }
    if (!strcmp(type, "lrf")) {
        const char *v = variant ? variant : "lightware";
        if (!strcmp(v, "lightware"))         { *out = LASER_TYPE_LIGHTWARE;          return 0; }
        if (!strcmp(v, "dlem"))              { *out = LASER_TYPE_JENOPTIK_DLEM;      return 0; }
        if (!strcmp(v, "dlem-test"))         { *out = LASER_TYPE_JENOPTIK_DLEM_TEST; return 0; }
        if (!strcmp(v, "vectronix"))         { *out = LASER_TYPE_VECTRONIX;          return 0; }
        if (!strcmp(v, "vectronix-3013"))    { *out = LASER_TYPE_VECTRONIX_3013;     return 0; }
        return -1;
    }
    return -1;
}

static int find_index_by_type(const OrionLaserStates_t *st, OrionLaserType_t want)
{
    int n = st->NumLasers;
    if (n > MAX_LASERS) n = MAX_LASERS;
    for (int i = 0; i < n; i++) {
        if (st->State[i].Type == want) return i;
    }
    return -1;
}

static int laser_fire_or_stop(octl_ctx_t *ctx, int fire)
{
    if (!gate_laser_allowed(ctx)) {
        jout_err(stderr, OCTL_LASER_GATE, "laser_gated",
                 "laser %s requires --allow-laser", fire ? "fire" : "stop");
        return OCTL_LASER_GATE;
    }

    if (octl_resolve_ip(ctx) != 0) {
        jout_err(stderr, OCTL_CONN_FAILED, "no_ip", "no gimbal IP");
        return OCTL_CONN_FAILED;
    }
    if (conn_open(ctx->ip) != 0) {
        jout_err(stderr, OCTL_CONN_FAILED, "connect_failed",
                 "could not open TCP to %s", ctx->ip);
        return OCTL_CONN_FAILED;
    }
    conn_drain(OCTL_DRAIN_MS);

    OrionLaserStates_t st;
    int rc = read_states(ctx, &st);
    if (rc != 0) {
        jout_err(stderr, rc == -2 ? OCTL_TIMEOUT : OCTL_INTERNAL,
                 rc == -2 ? "laser_states_timeout" : "laser_states_failed",
                 "LaserStates pre-read failed (rc=%d)", rc);
        conn_close();
        return rc == -2 ? OCTL_TIMEOUT : OCTL_INTERNAL;
    }

    /* For "fire": find index by type+variant from pos[2] and ctx->filter (re-using --filter as variant? no) */
    int idx = -1;
    int issued = 0;
    jout_t j; jout_init(&j, stdout);
    jout_obj_open(&j);
    jout_kv_str (&j, "verb", fire ? "fire" : "stop");

    if (fire) {
        if (ctx->npos < 3) {
            jout_err(stderr, OCTL_USAGE, "missing_type",
                     "laser fire requires <pointer|marker|designator|lrf>");
            conn_close();
            return OCTL_USAGE;
        }
        OrionLaserType_t want;
        /* Reuse --target as the variant flag (already exists) */
        if (laser_type_resolve(ctx->pos[2], ctx->target, &want) != 0) {
            jout_err(stderr, OCTL_USAGE, "bad_type_variant",
                     "unknown laser type/variant: %s/%s",
                     ctx->pos[2], ctx->target ? ctx->target : "(default)");
            conn_close();
            return OCTL_USAGE;
        }
        idx = find_index_by_type(&st, want);
        if (idx < 0) {
            jout_err(stderr, OCTL_UNSUPPORTED, "laser_not_installed",
                     "no installed laser of type %s on this gimbal", laser_type_name(want));
            conn_close();
            return OCTL_UNSUPPORTED;
        }
        OrionLaserCommand_t c;
        memset(&c, 0, sizeof(c));
        c.Index = (uint8_t)idx;
        c.Enable = 1;
        c.Arm    = 1;
        c.Fire   = 1;
        OrionPkt_t pkt;
        encodeOrionLaserCommandPacketStructure(&pkt, &c);
        if (conn_send(&pkt) != 0) {
            jout_err(stderr, OCTL_CONN_FAILED, "send_failed", "could not send LaserCommand");
            conn_close();
            return OCTL_CONN_FAILED;
        }
        issued = 1;
        jout_kv_int(&j, "index",   idx);
        jout_kv_str(&j, "type",    laser_type_name(want));
        jout_kv_int(&j, "type_id", want);
    } else {
        /* stop: send Fire=0 to every installed laser */
        int n = st.NumLasers;
        if (n > MAX_LASERS) n = MAX_LASERS;
        jout_key(&j, "stopped_indices"); jout_arr_open(&j);
        for (int i = 0; i < n; i++) {
            if (st.State[i].Type == LASER_TYPE_NONE) continue;
            OrionLaserCommand_t c;
            memset(&c, 0, sizeof(c));
            c.Index = (uint8_t)i;
            c.Enable = 0;
            c.Arm    = 0;
            c.Fire   = 0;
            OrionPkt_t pkt;
            encodeOrionLaserCommandPacketStructure(&pkt, &c);
            if (conn_send(&pkt) == 0) {
                jout_int(&j, i);
                issued++;
            }
        }
        jout_arr_close(&j);
    }

    OrionPkt_t echo;
    int got = (conn_wait_for(ORION_PKT_LASER_CMD, &echo, ctx->timeout_ms) == 0);
    conn_close();
    jout_kv_int (&j, "commands_issued", issued);
    jout_kv_bool(&j, "echo_seen", got);
    jout_obj_close(&j);
    jout_done(&j);
    return OCTL_OK;
}

int cmd_laser(octl_ctx_t *ctx)
{
    if (ctx->npos < 2) {
        jout_err(stderr, OCTL_USAGE, "missing_subverb",
                 "laser requires subverb: status | fire | stop");
        return OCTL_USAGE;
    }
    const char *sub = ctx->pos[1];
    if (strcmp(sub, "status") == 0) return laser_status(ctx);
    if (strcmp(sub, "fire")   == 0) return laser_fire_or_stop(ctx, 1);
    if (strcmp(sub, "stop")   == 0) return laser_fire_or_stop(ctx, 0);
    jout_err(stderr, OCTL_USAGE, "unknown_subverb",
             "unknown laser subverb: %s", sub);
    return OCTL_USAGE;
}
