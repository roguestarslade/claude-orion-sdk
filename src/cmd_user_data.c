#include "orionctl.h"
#include "conn.h"
#include "json_out.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "OrionPublicPacket.h"
#include "OrionPublicPacketShim.h"

static long long ms_now(void) { struct timeval tv; gettimeofday(&tv, NULL); return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000; }

static int hex_nibble(int c) { if (c>='0'&&c<='9') return c-'0'; c|=0x20; if (c>='a'&&c<='f') return c-'a'+10; return -1; }
static int parse_hexstr(const char *s, uint8_t *out, size_t cap)
{
    size_t n = 0;
    while (*s) {
        if (isspace((unsigned char)*s) || *s == ':' || *s == '-') { s++; continue; }
        int hi = hex_nibble(*s++); if (hi < 0 || *s == '\0') return -1;
        int lo = hex_nibble(*s++); if (lo < 0) return -1;
        if (n >= cap) return -1;
        out[n++] = (uint8_t)((hi << 4) | lo);
    }
    return (int)n;
}

static void emit_hex(jout_t *j, const char *key, const uint8_t *v, int len)
{
    char buf[260]; int o = 0;
    int n = len > 128 ? 128 : len;
    static const char H[] = "0123456789abcdef";
    for (int i = 0; i < n && o < (int)sizeof(buf)-3; i++) { buf[o++] = H[(v[i]>>4)&0xF]; buf[o++] = H[v[i]&0xF]; }
    buf[o] = '\0';
    jout_kv_str(j, key, buf);
}

static int ud_send(octl_ctx_t *ctx)
{
    if (!ctx->port_set) { jout_err(stderr, OCTL_USAGE, "missing_port", "user-data send requires --port <N>"); return OCTL_USAGE; }
    if (!ctx->hex_arg && !ctx->str_arg) {
        jout_err(stderr, OCTL_USAGE, "missing_payload", "user-data send requires --hex or --str");
        return OCTL_USAGE;
    }
    uint8_t data[128] = {0};
    int len = 0;
    if (ctx->hex_arg) {
        len = parse_hexstr(ctx->hex_arg, data, sizeof(data));
        if (len < 0) { jout_err(stderr, OCTL_USAGE, "bad_hex", "could not parse --hex"); return OCTL_USAGE; }
    } else {
        size_t L = strlen(ctx->str_arg);
        if (L > 128) L = 128;
        memcpy(data, ctx->str_arg, L);
        len = (int)L;
    }
    if (octl_resolve_ip(ctx) != 0) { jout_err(stderr, OCTL_CONN_FAILED, "no_ip", "no gimbal IP"); return OCTL_CONN_FAILED; }
    if (conn_open(ctx->ip) != 0)   { jout_err(stderr, OCTL_CONN_FAILED, "connect_failed", "could not open TCP to %s", ctx->ip); return OCTL_CONN_FAILED; }
    conn_drain(OCTL_DRAIN_MS);
    OrionUserData_t u; memset(&u, 0, sizeof(u));
    u.port = (UserDataPort_t)ctx->port;
    u.size = (uint8_t)len;
    u.id   = 0;
    memcpy(u.data, data, len);
    OrionPkt_t pkt;
    encodeOrionUserDataPacketStructure(&pkt, &u);
    int sent = (conn_send(&pkt) == 0);
    conn_close();

    jout_t j; jout_init(&j, stdout);
    jout_obj_open(&j);
    jout_kv_int (&j, "port",   u.port);
    jout_kv_int (&j, "length", len);
    emit_hex(&j, "payload_hex", data, len);
    jout_kv_bool(&j, "sent",   sent);
    jout_obj_close(&j);
    jout_done(&j);
    return sent ? OCTL_OK : OCTL_CONN_FAILED;
}

static int ud_listen(octl_ctx_t *ctx)
{
    if (octl_resolve_ip(ctx) != 0) { jout_err(stderr, OCTL_CONN_FAILED, "no_ip", "no gimbal IP"); return OCTL_CONN_FAILED; }
    if (conn_open(ctx->ip) != 0)   { jout_err(stderr, OCTL_CONN_FAILED, "connect_failed", "could not open TCP to %s", ctx->ip); return OCTL_CONN_FAILED; }
    conn_drain(OCTL_DRAIN_MS);
    int since_s = ctx->since_set ? ctx->since_s : 5;
    if (since_s < 0) since_s = 0;
    long long deadline = ms_now() + (long long)since_s * 1000;
    int count = 0;

    jout_t j; jout_init(&j, stdout);
    jout_obj_open(&j);
    jout_kv_int(&j, "since_s", since_s);
    if (ctx->port_set) jout_kv_int(&j, "filter_port", ctx->port);
    jout_key(&j, "messages"); jout_arr_open(&j);

    long long remaining;
    while ((remaining = deadline - ms_now()) > 0) {
        OrionPkt_t pkt;
        if (conn_wait_for(ORION_PKT_USER_DATA, &pkt, (int)remaining) != 0) break;
        OrionUserData_t u;
        if (!decodeOrionUserDataPacketStructure(&pkt, &u)) continue;
        if (ctx->port_set && (int)u.port != ctx->port) continue;
        jout_obj_open(&j);
        jout_kv_int (&j, "port", u.port);
        jout_kv_uint(&j, "id",   u.id);
        jout_kv_int (&j, "size", u.size);
        emit_hex(&j, "payload_hex", u.data, u.size);
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

int cmd_user_data(octl_ctx_t *ctx)
{
    if (ctx->npos < 2) { jout_err(stderr, OCTL_USAGE, "missing_subverb", "user-data send | listen"); return OCTL_USAGE; }
    const char *sub = ctx->pos[1];
    if (strcmp(sub, "send")   == 0) return ud_send(ctx);
    if (strcmp(sub, "listen") == 0) return ud_listen(ctx);
    jout_err(stderr, OCTL_USAGE, "unknown_subverb", "unknown user-data subverb: %s", sub);
    return OCTL_USAGE;
}
