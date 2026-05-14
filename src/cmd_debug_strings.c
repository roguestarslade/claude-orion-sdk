#include "orionctl.h"
#include "conn.h"
#include "json_out.h"

#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "OrionPublicPacket.h"
#include "OrionPublicPacketShim.h"

static long long ms_now(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static const char *board_name(int b)
{
    switch (b) {
    case BOARD_NONE:    return "none";
    case BOARD_CLEVIS:  return "clevis";
    case BOARD_CROWN:   return "crown";
    case BOARD_PAYLOAD: return "payload";
    case BOARD_LENSCTRL:return "lensctrl";
    case BOARD_MISSCOMP:return "misscomp";
    default:            return "unknown";
    }
}

static const char *priority_name(int p)
{
    switch (p) {
    case debugLevelLog:   return "log";
    case debugLevelInfo:  return "info";
    case debugLevelWarn:  return "warn";
    case debugLevelError: return "error";
    case debugLevelFatal: return "fatal";
    default:              return "unknown";
    }
}

int cmd_debug_strings(octl_ctx_t *ctx)
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
    jout_key(&j, "debug_strings");
    jout_arr_open(&j);

    long long remaining;
    while ((remaining = deadline - ms_now()) > 0) {
        OrionPkt_t pkt;
        if (conn_wait_for(ORION_PKT_DEBUG_STRING, &pkt, (int)remaining) != 0) break;
        DebugString_t s;
        if (!decodeDebugStringPacketStructure(&pkt, &s)) continue;
        /* Description is fixed-size char[128]; ensure NUL-termination for JSON. */
        char buf[129];
        memcpy(buf, s.description, 128);
        buf[128] = '\0';
        jout_obj_open(&j);
        jout_kv_str(&j, "source",      board_name(s.source));
        jout_kv_int(&j, "source_id",   s.source);
        jout_kv_str(&j, "priority",    priority_name(s.priority));
        jout_kv_int(&j, "priority_id", s.priority);
        jout_kv_str(&j, "description", buf);
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
