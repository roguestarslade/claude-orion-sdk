#include "orionctl.h"
#include "conn.h"
#include "json_out.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "OrionPublicPacket.h"
#include "OrionPublicPacketShim.h"
#include "TrilliumPacket.h"

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

static void emit_hex(jout_t *j, const char *key, const uint8_t *v, int len, int cap)
{
    if (len > cap) len = cap;
    int o = 0;
    int buf_sz = len * 2 + 1;
    char *buf = (char *)malloc((size_t)buf_sz);
    if (!buf) { jout_kv_str(j, key, ""); return; }
    static const char H[] = "0123456789abcdef";
    for (int i = 0; i < len; i++) { buf[o++] = H[(v[i]>>4)&0xF]; buf[o++] = H[v[i]&0xF]; }
    buf[o] = '\0';
    jout_kv_str(j, key, buf);
    free(buf);
}

int cmd_raw(octl_ctx_t *ctx)
{
    if (ctx->npos < 2) {
        jout_err(stderr, OCTL_USAGE, "missing_id",
                 "raw <pkt_id_hex> [hex_payload]");
        return OCTL_USAGE;
    }
    char *end;
    unsigned long id = strtoul(ctx->pos[1], &end, 16);
    if (*end != '\0' || end == ctx->pos[1] || id > 0xFF) {
        jout_err(stderr, OCTL_USAGE, "bad_id", "pkt_id_hex must be 0..0xFF hex");
        return OCTL_USAGE;
    }
    uint8_t payload[1024];
    int plen = 0;
    if (ctx->npos >= 3) {
        plen = parse_hexstr(ctx->pos[2], payload, sizeof(payload));
        if (plen < 0) {
            jout_err(stderr, OCTL_USAGE, "bad_payload_hex", "could not parse hex payload");
            return OCTL_USAGE;
        }
    }
    if (octl_resolve_ip(ctx) != 0) { jout_err(stderr, OCTL_CONN_FAILED, "no_ip", "no gimbal IP"); return OCTL_CONN_FAILED; }
    if (conn_open(ctx->ip) != 0)   { jout_err(stderr, OCTL_CONN_FAILED, "connect_failed", "could not open TCP to %s", ctx->ip); return OCTL_CONN_FAILED; }
    conn_drain(OCTL_DRAIN_MS);

    OrionPkt_t pkt;
    MakeOrionPacket(&pkt, (uint8_t)id, (uint16_t)plen);
    if (plen > 0) {
        memcpy(getOrionPublicPacketData(&pkt), payload, plen);
        /* TrilliumPacket library finalizes length when sending? Re-make with the proper length */
        MakeOrionPacket(&pkt, (uint8_t)id, (uint16_t)plen);
        memcpy(getOrionPublicPacketData(&pkt), payload, plen);
    }
    int sent = (conn_send(&pkt) == 0);

    OrionPkt_t echo;
    int got = sent && (conn_wait_for((uint8_t)id, &echo, ctx->timeout_ms) == 0);
    conn_close();

    jout_t j; jout_init(&j, stdout);
    jout_obj_open(&j);
    jout_kv_uint(&j, "sent_id",      (unsigned)id);
    jout_kv_int (&j, "payload_len",  plen);
    if (plen > 0) emit_hex(&j, "payload_hex", payload, plen, plen);
    jout_kv_bool(&j, "sent",         sent);
    jout_kv_bool(&j, "echo_seen",    got);
    if (got) {
        int elen = echo.Length;
        if (elen < 0) elen = 0;
        if (elen > (int)sizeof(echo.Data)) elen = (int)sizeof(echo.Data);
        jout_kv_int(&j, "echo_id",  echo.ID);
        jout_kv_int(&j, "echo_len", elen);
        emit_hex(&j, "echo_hex", getOrionPublicPacketData(&echo), elen, elen);
    }
    jout_obj_close(&j);
    jout_done(&j);
    return OCTL_OK;
}
