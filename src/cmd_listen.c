#include "orionctl.h"
#include "conn.h"
#include "json_out.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "OrionPublicPacket.h"
#include "OrionPublicPacketShim.h"

static long long ms_now(void) { struct timeval tv; gettimeofday(&tv, NULL); return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000; }

static void emit_hex(jout_t *j, const char *key, const uint8_t *v, int len)
{
    char *buf = (char *)malloc((size_t)len * 2 + 1);
    if (!buf) { jout_kv_str(j, key, ""); return; }
    static const char H[] = "0123456789abcdef";
    int o = 0;
    for (int i = 0; i < len; i++) { buf[o++] = H[(v[i]>>4)&0xF]; buf[o++] = H[v[i]&0xF]; }
    buf[o] = '\0';
    jout_kv_str(j, key, buf);
    free(buf);
}

int cmd_listen(octl_ctx_t *ctx)
{
    if (!ctx->id_arg) {
        jout_err(stderr, OCTL_USAGE, "missing_id", "listen requires --id <hex>");
        return OCTL_USAGE;
    }
    char *end;
    unsigned long id = strtoul(ctx->id_arg, &end, 16);
    if (*end != '\0' || end == ctx->id_arg || id > 0xFF) {
        jout_err(stderr, OCTL_USAGE, "bad_id", "--id must be hex byte");
        return OCTL_USAGE;
    }
    int max_n = ctx->watch_n > 0 ? ctx->watch_n : 5;
    int since_s = ctx->since_set ? ctx->since_s : -1;

    if (octl_resolve_ip(ctx) != 0) { jout_err(stderr, OCTL_CONN_FAILED, "no_ip", "no gimbal IP"); return OCTL_CONN_FAILED; }
    if (conn_open(ctx->ip) != 0)   { jout_err(stderr, OCTL_CONN_FAILED, "connect_failed", "could not open TCP to %s", ctx->ip); return OCTL_CONN_FAILED; }
    conn_drain(OCTL_DRAIN_MS);

    long long deadline = since_s >= 0 ? (ms_now() + (long long)since_s * 1000) : (ms_now() + 24LL * 3600 * 1000);
    int count = 0;
    jout_t j; jout_init(&j, stdout);
    jout_obj_open(&j);
    jout_kv_uint(&j, "id", (unsigned)id);
    if (since_s >= 0) jout_kv_int(&j, "since_s", since_s);
    jout_kv_int(&j, "max_n",  max_n);
    jout_key(&j, "packets"); jout_arr_open(&j);

    while (count < max_n && ms_now() < deadline) {
        OrionPkt_t pkt;
        long long remaining = deadline - ms_now();
        if (remaining <= 0) break;
        if (conn_wait_for((uint8_t)id, &pkt, (int)remaining) != 0) break;
        int len = pkt.Length;
        if (len < 0) len = 0;
        jout_obj_open(&j);
        jout_kv_uint(&j, "id",     pkt.ID);
        jout_kv_int (&j, "length", len);
        emit_hex(&j, "data_hex", getOrionPublicPacketData(&pkt), len);
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
