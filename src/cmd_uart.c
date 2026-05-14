#include "orionctl.h"
#include "conn.h"
#include "json_out.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "OrionPublicPacket.h"
#include "OrionPublicPacketShim.h"

static struct { const char *name; OrionProtocols_t v; } PROTOS[] = {
    {"no_change",         PROTOCOL_NO_CHANGE},
    {"none",              PROTOCOL_NONE},
    {"default",           PROTOCOL_DEFAULT},
    {"orion",             PROTOCOL_ORION},
    {"clevis",            PROTOCOL_CLEVIS},
    {"clevis_raw",        PROTOCOL_CLEVIS_RAW},
    {"nmea_gps",          PROTOCOL_NMEA_GPS},
    {"ublox_gps",         PROTOCOL_UBLOX_GPS},
    {"novatel_gps",       PROTOCOL_NOVATEL_GPS},
    {"mavlink_gps",       PROTOCOL_MAVLINK_GPS},
    {"um7_mag",           PROTOCOL_UM7_MAG},
    {"sightline_video",   PROTOCOL_SIGHTLINE_VIDEO},
    {"sensonor_imu",      PROTOCOL_SENSONOR_IMU},
    {"dmu11_imu",         PROTOCOL_DMU11_IMU},
    {"discovery",         PROTOCOL_DISCOVERY},
    {"airrobot",          PROTOCOL_AIRROBOT},
    {"flir",              PROTOCOL_FLIR},
    {"epson_imu",         PROTOCOL_EPSON_IMU},
    {"piksi",             PROTOCOL_PIKSI},
    {"wepilot",           PROTOCOL_WEPILOT},
    {"ethernet",          PROTOCOL_ETHERNET},
    {"vectornav",         PROTOCOL_VECTORNAV},
    {"ublox_hdg",         PROTOCOL_UBLOX_HDG},
    {"sbg_pulse_imu",     PROTOCOL_SBG_PULSE_IMU},
    {"headway_ins",       PROTOCOL_HEADWAY_INS},
};
#define NP ((int)(sizeof(PROTOS)/sizeof(PROTOS[0])))

static const char *proto_name(int v) { for (int i = 0; i < NP; i++) if ((int)PROTOS[i].v == v) return PROTOS[i].name; return "unknown"; }
static int parse_proto(const char *s, OrionProtocols_t *out) {
    for (int i = 0; i < NP; i++) if (!strcmp(PROTOS[i].name, s)) { *out = PROTOS[i].v; return 0; }
    return -1;
}

static const char *port_name(int p) {
    switch (p) {
    case USER_DATA_PORT_ETHERNET: return "ethernet";
    case USER_DATA_PORT_PRIMARY:  return "primary";
    case USER_DATA_PORT_FP2:      return "fp2";
    case USER_DATA_PORT_FP1:      return "fp1";
    case USER_DATA_PORT_FP3:      return "fp3";
    case USER_DATA_PORT_FP4:      return "fp4";
    case USER_DATA_PORT_CURRENT:  return "current";
    default:                      return "unknown";
    }
}

static int read_uart(octl_ctx_t *ctx, OrionUartConfig_t *out)
{
    OrionPkt_t req, resp;
    /* If --port set, request specific port */
    if (ctx->port_set) {
        OrionUartConfig_t q; memset(&q, 0, sizeof(q));
        q.port = (UserDataPort_t)ctx->port;
        q.protocol = PROTOCOL_NO_CHANGE;
        /* zero-len request OR a probe with port - prefer probe */
        encodeOrionUartConfigPacketStructure(&req, &q);
    } else {
        MakeOrionPacket(&req, getOrionUartConfigPacketID(), 0);
    }
    if (conn_send(&req) != 0) return -1;
    if (conn_wait_for(ORION_PKT_UART_CONFIG, &resp, ctx->timeout_ms) != 0) return -2;
    if (!decodeOrionUartConfigPacketStructure(&resp, out)) return -3;
    return 0;
}

static void emit(jout_t *j, const OrionUartConfig_t *c)
{
    jout_obj_open(j);
    jout_kv_str (j, "port",      port_name(c->port));
    jout_kv_int (j, "port_id",   c->port);
    jout_kv_uint(j, "baud",      c->baud);
    jout_kv_str (j, "protocol",  proto_name(c->protocol));
    jout_kv_int (j, "protocol_id",c->protocol);
    jout_kv_bool(j, "temporary", c->temporary);
    jout_kv_uint(j, "param",     c->param);
    jout_obj_close(j);
}

static int uart_get(octl_ctx_t *ctx)
{
    if (octl_resolve_ip(ctx) != 0) { jout_err(stderr, OCTL_CONN_FAILED, "no_ip", "no gimbal IP"); return OCTL_CONN_FAILED; }
    if (conn_open(ctx->ip) != 0)   { jout_err(stderr, OCTL_CONN_FAILED, "connect_failed", "could not open TCP to %s", ctx->ip); return OCTL_CONN_FAILED; }
    conn_drain(OCTL_DRAIN_MS);
    OrionUartConfig_t c;
    int rc = read_uart(ctx, &c);
    conn_close();
    if (rc == -2) { jout_err(stderr, OCTL_TIMEOUT, "uart_timeout", "no UartConfig in %d ms", ctx->timeout_ms); return OCTL_TIMEOUT; }
    if (rc != 0)  { jout_err(stderr, OCTL_INTERNAL, "uart_failed", "UartConfig rc=%d", rc); return OCTL_INTERNAL; }
    jout_t j; jout_init(&j, stdout);
    emit(&j, &c); jout_done(&j);
    return OCTL_OK;
}

static int uart_set(octl_ctx_t *ctx)
{
    if (!ctx->iknow) {
        jout_err(stderr, OCTL_REJECTED, "missing_iknow", "uart set may lose serial debug access; pass --i-know");
        return OCTL_REJECTED;
    }
    if (!ctx->port_set) { jout_err(stderr, OCTL_USAGE, "missing_port", "uart set requires --port"); return OCTL_USAGE; }
    if (!ctx->protocol) { jout_err(stderr, OCTL_USAGE, "missing_protocol", "uart set requires --protocol <name>"); return OCTL_USAGE; }

    OrionProtocols_t proto;
    if (parse_proto(ctx->protocol, &proto) != 0) {
        jout_err(stderr, OCTL_USAGE, "bad_protocol", "unknown protocol: %s", ctx->protocol);
        return OCTL_USAGE;
    }

    if (octl_resolve_ip(ctx) != 0) { jout_err(stderr, OCTL_CONN_FAILED, "no_ip", "no gimbal IP"); return OCTL_CONN_FAILED; }
    if (conn_open(ctx->ip) != 0)   { jout_err(stderr, OCTL_CONN_FAILED, "connect_failed", "could not open TCP to %s", ctx->ip); return OCTL_CONN_FAILED; }
    conn_drain(OCTL_DRAIN_MS);

    OrionUartConfig_t c; memset(&c, 0, sizeof(c));
    c.port = (UserDataPort_t)ctx->port;
    c.baud = ctx->baud_set ? (uint32_t)ctx->baud : 0;
    c.protocol = proto;
    c.temporary = 0;
    c.param = 0;

    OrionPkt_t pkt;
    encodeOrionUartConfigPacketStructure(&pkt, &c);
    int sent = (conn_send(&pkt) == 0);
    OrionPkt_t echo;
    int got = sent && (conn_wait_for(ORION_PKT_UART_CONFIG, &echo, ctx->timeout_ms) == 0);
    OrionUartConfig_t e; int e_ok = got && decodeOrionUartConfigPacketStructure(&echo, &e);
    conn_close();

    jout_t j; jout_init(&j, stdout);
    jout_obj_open(&j);
    jout_kv_bool(&j, "sent", sent);
    jout_kv_bool(&j, "echo_seen", got);
    if (e_ok) { jout_key(&j, "echo"); emit(&j, &e); }
    else      { jout_key(&j, "written"); emit(&j, &c); }
    jout_obj_close(&j);
    jout_done(&j);
    return OCTL_OK;
}

int cmd_uart(octl_ctx_t *ctx)
{
    if (ctx->npos < 2) {
        jout_err(stderr, OCTL_USAGE, "missing_subverb", "uart requires subverb: get | set");
        return OCTL_USAGE;
    }
    const char *sub = ctx->pos[1];
    if (strcmp(sub, "get") == 0) return uart_get(ctx);
    if (strcmp(sub, "set") == 0) return uart_set(ctx);
    jout_err(stderr, OCTL_USAGE, "unknown_subverb", "unknown uart subverb: %s", sub);
    return OCTL_USAGE;
}
