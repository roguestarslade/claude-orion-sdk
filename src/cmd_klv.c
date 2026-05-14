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

static long long ms_now(void)
{
    struct timeval tv; gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static int hex_nibble(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c |= 0x20;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static int parse_hexstr(const char *s, uint8_t *out, size_t cap)
{
    size_t n = 0;
    while (*s) {
        if (isspace((unsigned char)*s) || *s == ':' || *s == '-') { s++; continue; }
        int hi = hex_nibble(*s++);
        if (hi < 0 || *s == '\0') return -1;
        int lo = hex_nibble(*s++);
        if (lo < 0) return -1;
        if (n >= cap) return -1;
        out[n++] = (uint8_t)((hi << 4) | lo);
    }
    return (int)n;
}

static void emit_value_hex(jout_t *j, const uint8_t *v, int len)
{
    char buf[256];
    int o = 0;
    int n = len > 127 ? 127 : len;
    for (int i = 0; i < n && o < (int)sizeof(buf) - 3; i++) {
        static const char H[] = "0123456789abcdef";
        buf[o++] = H[(v[i] >> 4) & 0xF];
        buf[o++] = H[v[i] & 0xF];
    }
    buf[o] = '\0';
    jout_kv_str(j, "value_hex", buf);
}

static int is_printable_ascii(const uint8_t *v, int len)
{
    for (int i = 0; i < len; i++) {
        if (v[i] < 0x20 || v[i] > 0x7E) return 0;
    }
    return 1;
}

static void emit_klv(jout_t *j, uint8_t key, uint8_t subkey, uint8_t len, const uint8_t value[127])
{
    jout_obj_open(j);
    jout_kv_uint(j, "key",     key);
    jout_kv_uint(j, "subkey",  subkey);
    jout_kv_uint(j, "length",  len);
    emit_value_hex(j, value, len);
    if (len > 0 && is_printable_ascii(value, len)) {
        char buf[128];
        int n = len > 127 ? 127 : len;
        memcpy(buf, value, n);
        buf[n] = '\0';
        jout_kv_str(j, "value_ascii", buf);
    }
    jout_obj_close(j);
}

static int klv_open(octl_ctx_t *ctx)
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
    return OCTL_OK;
}

static int klv_get(octl_ctx_t *ctx)
{
    int rc = klv_open(ctx);
    if (rc != OCTL_OK) return rc;

    OrionPkt_t req, resp;
    MakeOrionPacket(&req, getKlvUserDataPacketID(), 0);
    if (conn_send(&req) != 0) {
        jout_err(stderr, OCTL_CONN_FAILED, "send_failed", "could not send KlvUserData probe");
        conn_close();
        return OCTL_CONN_FAILED;
    }
    if (conn_wait_for(ORION_PKT_KLV_USER_DATA, &resp, ctx->timeout_ms) != 0) {
        jout_err(stderr, OCTL_TIMEOUT, "klv_timeout",
                 "no KlvUserData within %d ms", ctx->timeout_ms);
        conn_close();
        return OCTL_TIMEOUT;
    }
    uint8_t key = 0, subkey = 0, len = 0;
    uint8_t value[127] = {0};
    if (!decodeKlvUserDataPacket(&resp, &key, &subkey, &len, value)) {
        jout_err(stderr, OCTL_INTERNAL, "decode_failed", "KlvUserData decode failed");
        conn_close();
        return OCTL_INTERNAL;
    }
    conn_close();
    jout_t j; jout_init(&j, stdout);
    emit_klv(&j, key, subkey, len, value);
    jout_done(&j);
    return OCTL_OK;
}

static int klv_list(octl_ctx_t *ctx)
{
    int rc = klv_open(ctx);
    if (rc != OCTL_OK) return rc;

    int since_s = ctx->since_set ? ctx->since_s : 5;
    if (since_s < 0) since_s = 0;
    long long deadline = ms_now() + (long long)since_s * 1000;
    int count = 0;

    /* Send one probe to kick the gimbal */
    OrionPkt_t probe;
    MakeOrionPacket(&probe, getKlvUserDataPacketID(), 0);
    conn_send(&probe);

    jout_t j; jout_init(&j, stdout);
    jout_obj_open(&j);
    jout_kv_int(&j, "since_s", since_s);
    jout_key(&j, "klv_tags");
    jout_arr_open(&j);

    long long remaining;
    while ((remaining = deadline - ms_now()) > 0) {
        OrionPkt_t pkt;
        if (conn_wait_for(ORION_PKT_KLV_USER_DATA, &pkt, (int)remaining) != 0) break;
        uint8_t key = 0, subkey = 0, len = 0;
        uint8_t value[127] = {0};
        if (!decodeKlvUserDataPacket(&pkt, &key, &subkey, &len, value)) continue;
        emit_klv(&j, key, subkey, len, value);
        count++;
    }
    jout_arr_close(&j);
    jout_kv_int(&j, "count", count);
    jout_obj_close(&j);
    jout_done(&j);
    conn_close();
    return OCTL_OK;
}

static int klv_query(octl_ctx_t *ctx)
{
    if (!ctx->key_set || !ctx->subkey_set) {
        jout_err(stderr, OCTL_USAGE, "missing_key",
                 "klv query requires --key N --subkey N");
        return OCTL_USAGE;
    }
    int rc = klv_open(ctx);
    if (rc != OCTL_OK) return rc;

    /* Send a zero-length request for the specific key/subkey */
    OrionPkt_t req;
    uint8_t empty[127] = {0};
    encodeKlvUserDataPacket(&req, (uint8_t)ctx->key, (uint8_t)ctx->subkey, 0, empty);
    if (conn_send(&req) != 0) {
        jout_err(stderr, OCTL_CONN_FAILED, "send_failed", "could not send KlvUserData query");
        conn_close();
        return OCTL_CONN_FAILED;
    }
    /* Wait for matching key/subkey echo within timeout */
    long long deadline = ms_now() + ctx->timeout_ms;
    while (ms_now() < deadline) {
        OrionPkt_t resp;
        int remaining = (int)(deadline - ms_now());
        if (remaining <= 0) break;
        if (conn_wait_for(ORION_PKT_KLV_USER_DATA, &resp, remaining) != 0) break;
        uint8_t k = 0, sk = 0, len = 0;
        uint8_t value[127] = {0};
        if (!decodeKlvUserDataPacket(&resp, &k, &sk, &len, value)) continue;
        if (k == (uint8_t)ctx->key && sk == (uint8_t)ctx->subkey) {
            conn_close();
            jout_t j; jout_init(&j, stdout);
            emit_klv(&j, k, sk, len, value);
            jout_done(&j);
            return OCTL_OK;
        }
    }
    conn_close();
    jout_err(stderr, OCTL_TIMEOUT, "klv_query_timeout",
             "no KlvUserData for key=%d subkey=%d within %d ms",
             ctx->key, ctx->subkey, ctx->timeout_ms);
    return OCTL_TIMEOUT;
}

static int klv_set(octl_ctx_t *ctx)
{
    if (!ctx->key_set || !ctx->subkey_set) {
        jout_err(stderr, OCTL_USAGE, "missing_key",
                 "klv set requires --key N --subkey N");
        return OCTL_USAGE;
    }
    if (!ctx->value && !ctx->value_hex) {
        jout_err(stderr, OCTL_USAGE, "missing_value",
                 "klv set requires --value <str> or --value-hex <H>");
        return OCTL_USAGE;
    }
    uint8_t value[127] = {0};
    int len = 0;
    if (ctx->value_hex) {
        len = parse_hexstr(ctx->value_hex, value, sizeof(value));
        if (len < 0) {
            jout_err(stderr, OCTL_USAGE, "bad_hex", "could not parse --value-hex");
            return OCTL_USAGE;
        }
    } else {
        size_t L = strlen(ctx->value);
        if (L > 127) L = 127;
        memcpy(value, ctx->value, L);
        len = (int)L;
    }

    int rc = klv_open(ctx);
    if (rc != OCTL_OK) return rc;

    OrionPkt_t req;
    encodeKlvUserDataPacket(&req, (uint8_t)ctx->key, (uint8_t)ctx->subkey, (uint8_t)len, value);
    if (conn_send(&req) != 0) {
        jout_err(stderr, OCTL_CONN_FAILED, "send_failed", "could not send KlvUserData write");
        conn_close();
        return OCTL_CONN_FAILED;
    }
    OrionPkt_t echo;
    int got_echo = (conn_wait_for(ORION_PKT_KLV_USER_DATA, &echo, ctx->timeout_ms) == 0);
    conn_close();

    jout_t j; jout_init(&j, stdout);
    jout_obj_open(&j);
    jout_kv_uint(&j, "key",       ctx->key);
    jout_kv_uint(&j, "subkey",    ctx->subkey);
    jout_kv_uint(&j, "length",    len);
    emit_value_hex(&j, value, len);
    jout_kv_bool(&j, "echo_seen", got_echo);
    jout_obj_close(&j);
    jout_done(&j);
    return OCTL_OK;
}

static int klv_delete(octl_ctx_t *ctx)
{
    if (!ctx->key_set || !ctx->subkey_set) {
        jout_err(stderr, OCTL_USAGE, "missing_key",
                 "klv delete requires --key N --subkey N");
        return OCTL_USAGE;
    }
    int rc = klv_open(ctx);
    if (rc != OCTL_OK) return rc;

    uint8_t value[127] = {0};
    OrionPkt_t req;
    encodeKlvUserDataPacket(&req, (uint8_t)ctx->key, (uint8_t)ctx->subkey, 0, value);
    if (conn_send(&req) != 0) {
        jout_err(stderr, OCTL_CONN_FAILED, "send_failed", "could not send KlvUserData delete");
        conn_close();
        return OCTL_CONN_FAILED;
    }
    OrionPkt_t echo;
    int got_echo = (conn_wait_for(ORION_PKT_KLV_USER_DATA, &echo, ctx->timeout_ms) == 0);
    conn_close();

    jout_t j; jout_init(&j, stdout);
    jout_obj_open(&j);
    jout_kv_str (&j, "action",    "delete");
    jout_kv_uint(&j, "key",       ctx->key);
    jout_kv_uint(&j, "subkey",    ctx->subkey);
    jout_kv_bool(&j, "echo_seen", got_echo);
    jout_obj_close(&j);
    jout_done(&j);
    return OCTL_OK;
}

int cmd_klv(octl_ctx_t *ctx)
{
    if (ctx->npos < 2) {
        jout_err(stderr, OCTL_USAGE, "missing_subverb",
                 "klv requires subverb: get | list | query | set | delete");
        return OCTL_USAGE;
    }
    const char *sub = ctx->pos[1];
    if (strcmp(sub, "get")    == 0) return klv_get(ctx);
    if (strcmp(sub, "list")   == 0) return klv_list(ctx);
    if (strcmp(sub, "query")  == 0) return klv_query(ctx);
    if (strcmp(sub, "set")    == 0) return klv_set(ctx);
    if (strcmp(sub, "delete") == 0) return klv_delete(ctx);
    jout_err(stderr, OCTL_USAGE, "unknown_subverb",
             "unknown klv subverb: %s", sub);
    return OCTL_USAGE;
}
