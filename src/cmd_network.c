#include "orionctl.h"
#include "conn.h"
#include "json_out.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "OrionPublicPacket.h"
#include "OrionPublicPacketShim.h"

static void ip_str(uint32_t ip, char *out)
{
    snprintf(out, 32, "%u.%u.%u.%u",
             (ip >> 24) & 0xFFu, (ip >> 16) & 0xFFu,
             (ip >> 8)  & 0xFFu,  ip        & 0xFFu);
}

static int parse_ip(const char *s, uint32_t *out)
{
    unsigned a, b, c, d;
    if (sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return -1;
    if (a > 255 || b > 255 || c > 255 || d > 255) return -1;
    *out = (a << 24) | (b << 16) | (c << 8) | d;
    return 0;
}

static int read_net(octl_ctx_t *ctx, OrionNetworkSettings_t *out)
{
    OrionPkt_t req, resp;
    MakeOrionPacket(&req, getOrionNetworkSettingsPacketID(), 0);
    if (conn_send(&req) != 0) return -1;
    if (conn_wait_for(ORION_PKT_NETWORK_SETTINGS, &resp, ctx->timeout_ms) != 0) return -2;
    if (!decodeOrionNetworkSettingsPacketStructure(&resp, out)) return -3;
    return 0;
}

static void emit(jout_t *j, const OrionNetworkSettings_t *n)
{
    char ipa[32], ipm[32], ipg[32];
    ip_str(n->Ip, ipa); ip_str(n->Mask, ipm); ip_str(n->Gateway, ipg);
    jout_obj_open(j);
    jout_kv_str (j, "ip",                 ipa);
    jout_kv_str (j, "netmask",            ipm);
    jout_kv_str (j, "gateway",            ipg);
    jout_kv_uint(j, "ip_u32",             n->Ip);
    jout_kv_uint(j, "netmask_u32",        n->Mask);
    jout_kv_uint(j, "gateway_u32",        n->Gateway);
    jout_kv_uint(j, "low_delay",          n->LowDelay);
    jout_kv_uint(j, "mtu",                n->Mtu);
    jout_kv_uint(j, "secondary_tcp_port", n->SecondaryTcpPort);
    jout_kv_uint(j, "low_bandwidth",      n->LowBandwidth);
    jout_kv_uint(j, "max_tcp_clients",    n->MaxTcpClients);
    jout_obj_close(j);
}

static int net_get(octl_ctx_t *ctx)
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
    OrionNetworkSettings_t n;
    int rc = read_net(ctx, &n);
    conn_close();
    if (rc == -2) { jout_err(stderr, OCTL_TIMEOUT, "network_timeout", "no NetworkSettings in %d ms", ctx->timeout_ms); return OCTL_TIMEOUT; }
    if (rc != 0)  { jout_err(stderr, OCTL_INTERNAL, "network_failed", "NetworkSettings rc=%d", rc); return OCTL_INTERNAL; }
    jout_t j; jout_init(&j, stdout);
    emit(&j, &n); jout_done(&j);
    return OCTL_OK;
}

static int net_set(octl_ctx_t *ctx)
{
    if (!ctx->iknow) {
        jout_err(stderr, OCTL_REJECTED, "missing_iknow",
                 "network set may orphan the control session; pass --i-know");
        return OCTL_REJECTED;
    }
    /* pos[0]=network, pos[1]=set, pos[2..] Field=Value (e.g. Ip=A.B.C.D, Mask=...) */
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

    OrionNetworkSettings_t n;
    int rc = read_net(ctx, &n);
    if (rc != 0) {
        jout_err(stderr, rc == -2 ? OCTL_TIMEOUT : OCTL_INTERNAL,
                 rc == -2 ? "network_timeout" : "network_failed",
                 "NetworkSettings pre-read failed (rc=%d)", rc);
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
        const char *v = eq + 1;
        char *end;
        if      (!strcmp(name, "Ip"))                { uint32_t x; if (parse_ip(v, &x) != 0) { jout_err(stderr, OCTL_USAGE, "bad_ip", "bad Ip: %s", v); conn_close(); return OCTL_USAGE; } n.Ip = x; mutations++; }
        else if (!strcmp(name, "Mask"))              { uint32_t x; if (parse_ip(v, &x) != 0) { jout_err(stderr, OCTL_USAGE, "bad_ip", "bad Mask: %s", v); conn_close(); return OCTL_USAGE; } n.Mask = x; mutations++; }
        else if (!strcmp(name, "Gateway"))           { uint32_t x; if (parse_ip(v, &x) != 0) { jout_err(stderr, OCTL_USAGE, "bad_ip", "bad Gateway: %s", v); conn_close(); return OCTL_USAGE; } n.Gateway = x; mutations++; }
        else if (!strcmp(name, "LowDelay"))          { n.LowDelay = (uint8_t)strtoul(v, &end, 0); mutations++; }
        else if (!strcmp(name, "Mtu"))               { n.Mtu = (uint16_t)strtoul(v, &end, 0); mutations++; }
        else if (!strcmp(name, "SecondaryTcpPort"))  { n.SecondaryTcpPort = (uint16_t)strtoul(v, &end, 0); mutations++; }
        else if (!strcmp(name, "LowBandwidth"))      { n.LowBandwidth = (uint8_t)strtoul(v, &end, 0); mutations++; }
        else if (!strcmp(name, "MaxTcpClients"))     { n.MaxTcpClients = (uint8_t)strtoul(v, &end, 0); mutations++; }
        else { jout_err(stderr, OCTL_USAGE, "unknown_field", "unknown field: %s", name); conn_close(); return OCTL_USAGE; }
    }
    if (mutations == 0) {
        jout_err(stderr, OCTL_USAGE, "no_fields",
                 "network set requires at least one Field=Value");
        conn_close();
        return OCTL_USAGE;
    }
    OrionPkt_t out;
    encodeOrionNetworkSettingsPacketStructure(&out, &n);
    int sent = (conn_send(&out) == 0);
    /* Echo may or may not arrive depending on whether session survives. */
    OrionPkt_t echo;
    int got = sent && (conn_wait_for(ORION_PKT_NETWORK_SETTINGS, &echo, ctx->timeout_ms) == 0);
    conn_close();

    jout_t j; jout_init(&j, stdout);
    jout_obj_open(&j);
    jout_kv_int (&j, "mutations", mutations);
    jout_kv_bool(&j, "sent",      sent);
    jout_kv_bool(&j, "echo_seen", got);
    jout_key(&j, "intended");
    emit(&j, &n);
    jout_obj_close(&j);
    jout_done(&j);
    return OCTL_OK;
}

int cmd_network(octl_ctx_t *ctx)
{
    if (ctx->npos < 2) {
        jout_err(stderr, OCTL_USAGE, "missing_subverb",
                 "network requires subverb: get | set");
        return OCTL_USAGE;
    }
    const char *sub = ctx->pos[1];
    if (strcmp(sub, "get") == 0) return net_get(ctx);
    if (strcmp(sub, "set") == 0) return net_set(ctx);
    jout_err(stderr, OCTL_USAGE, "unknown_subverb",
             "unknown network subverb: %s", sub);
    return OCTL_USAGE;
}
