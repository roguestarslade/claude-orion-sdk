#include "orionctl.h"
#include "conn.h"
#include "json_out.h"

#include <arpa/inet.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "OrionPublicPacket.h"
#include "OrionPublicPacketShim.h"

static const char *stream_type_name(int s)
{
    switch (s) {
    case STREAM_TYPE_H264:  return "h264";
    case STREAM_TYPE_MJPEG: return "mjpeg";
    case STREAM_TYPE_RAW:   return "raw";
    case STREAM_TYPE_YUV:   return "yuv";
    case STREAM_TYPE_H265:  return "h265";
    case STREAM_TYPE_NONE:  return "none";
    default:                  return "unknown";
    }
}

static const char *klv_mode_name(int m)
{
    switch (m) {
    case KlvModeLegacy:       return "legacy";
    case KlvModeSynchronous:  return "synchronous";
    case KlvModeAsynchronous: return "asynchronous";
    case KlvModeDoNotChange:  return "do_not_change";
    default:                  return "unknown";
    }
}

typedef enum { N_U32_IP, N_U16, N_U32, N_I8, N_U8, N_BITS1, N_BITS4, N_BITS3, N_ENUM_U8 } ntype_t;

typedef struct {
    const char *cli;
    const char *json;
    size_t      offset;
    ntype_t     type;
    /* for bit fields we use a setter helper instead */
} nd_t;

/* NB: bit-fields (SaveSettings:1, TsPacketCount:4, FramePeriod:3) are inside
 * a uint32 in C ABI and we cannot use offsetof on individual bit-fields.
 * We handle them explicitly. The descriptor table covers byte-addressable fields
 * (DestIp, Port, Bitrate, Ttl, MjpegQuality, StreamType, KlvMode) only. */

static const nd_t NVD[] = {
    { "DestIp",       "dest_ip",       offsetof(OrionNetworkVideo_t, DestIp),       N_U32_IP  },
    { "Port",         "port",          offsetof(OrionNetworkVideo_t, Port),         N_U16     },
    { "Bitrate",      "bitrate_bps",   offsetof(OrionNetworkVideo_t, Bitrate),      N_U32     },
    { "Ttl",          "ttl",           offsetof(OrionNetworkVideo_t, Ttl),          N_I8      },
    { "StreamType",   "stream_type",   offsetof(OrionNetworkVideo_t, StreamType),   N_ENUM_U8 },
    { "MjpegQuality", "mjpeg_quality", offsetof(OrionNetworkVideo_t, MjpegQuality), N_U8      },
    { "KlvMode",      "klv_mode",      offsetof(OrionNetworkVideo_t, KlvMode),      N_ENUM_U8 },
};
#define NUM_NVD ((int)(sizeof(NVD) / sizeof(NVD[0])))

static int parse_ipv4_to_u32(const char *s, uint32_t *out)
{
    /* SDK stores DestIp as host-order u32 (e.g. 127.0.0.1 = 0x7F000001).
     * inet_pton would give network-byte-order s_addr — convert manually. */
    unsigned a, b, c, d;
    if (sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return -1;
    if (a > 255 || b > 255 || c > 255 || d > 255) return -1;
    *out = (a << 24) | (b << 16) | (c << 8) | d;
    return 0;
}

static void ip_u32_to_str(uint32_t ip, char out[INET_ADDRSTRLEN])
{
    snprintf(out, INET_ADDRSTRLEN, "%u.%u.%u.%u",
             (ip >> 24) & 0xFFu, (ip >> 16) & 0xFFu,
             (ip >> 8)  & 0xFFu,  ip        & 0xFFu);
}

static void emit_one(jout_t *j, const nd_t *f, const OrionNetworkVideo_t *o)
{
    const unsigned char *base = (const unsigned char *)o;
    switch (f->type) {
    case N_U32_IP: {
        uint32_t v; memcpy(&v, base + f->offset, sizeof(v));
        char ipstr[INET_ADDRSTRLEN];
        ip_u32_to_str(v, ipstr);
        jout_kv_str (j, "dest_ip",     ipstr);
        jout_kv_uint(j, "dest_ip_u32", v);
        break;
    }
    case N_U16: { uint16_t v; memcpy(&v, base + f->offset, sizeof(v)); jout_kv_uint(j, f->json, v); break; }
    case N_U32: { uint32_t v; memcpy(&v, base + f->offset, sizeof(v)); jout_kv_uint(j, f->json, v); break; }
    case N_I8:  { int8_t v;   memcpy(&v, base + f->offset, sizeof(v)); jout_kv_int (j, f->json, v); break; }
    case N_U8:  { uint8_t v;  memcpy(&v, base + f->offset, sizeof(v)); jout_kv_uint(j, f->json, v); break; }
    case N_ENUM_U8: {
        uint8_t v; memcpy(&v, base + f->offset, sizeof(v));
        if (f->offset == offsetof(OrionNetworkVideo_t, StreamType)) {
            jout_kv_str (j, "stream_type",      stream_type_name(v));
            jout_kv_uint(j, "stream_type_id",   v);
        } else {
            jout_kv_str (j, "klv_mode",         klv_mode_name(v));
            jout_kv_uint(j, "klv_mode_id",      v);
        }
        break;
    }
    default: break;
    }
}

static void emit_video_net(jout_t *j, const OrionNetworkVideo_t *o)
{
    jout_obj_open(j);
    for (int i = 0; i < NUM_NVD; i++) emit_one(j, &NVD[i], o);
    jout_kv_uint(j, "save_settings",   o->SaveSettings);
    jout_kv_uint(j, "ts_packet_count", o->TsPacketCount);
    jout_kv_uint(j, "frame_period",    o->FramePeriod);
    jout_obj_close(j);
}

static int set_one(OrionNetworkVideo_t *o, const nd_t *f, const char *vstr)
{
    unsigned char *base = (unsigned char *)o;
    char *end;
    switch (f->type) {
    case N_U32_IP: {
        uint32_t ip = 0;
        if (parse_ipv4_to_u32(vstr, &ip) != 0) {
            unsigned long v = strtoul(vstr, &end, 0);
            if (*end != '\0' || end == vstr) return -1;
            ip = (uint32_t)v;
        }
        memcpy(base + f->offset, &ip, sizeof(ip));
        (void)end;
        return 0;
    }
    case N_U16: {
        unsigned long v = strtoul(vstr, &end, 0);
        if (*end != '\0' || end == vstr || v > 0xFFFF) return -1;
        uint16_t out = (uint16_t)v; memcpy(base + f->offset, &out, sizeof(out));
        return 0;
    }
    case N_U32: {
        unsigned long v = strtoul(vstr, &end, 0);
        if (*end != '\0' || end == vstr) return -1;
        uint32_t out = (uint32_t)v; memcpy(base + f->offset, &out, sizeof(out));
        return 0;
    }
    case N_I8: {
        long v = strtol(vstr, &end, 0);
        if (*end != '\0' || end == vstr || v < -128 || v > 127) return -1;
        int8_t out = (int8_t)v; memcpy(base + f->offset, &out, sizeof(out));
        return 0;
    }
    case N_U8: case N_ENUM_U8: {
        unsigned long v = strtoul(vstr, &end, 0);
        if (*end != '\0' || end == vstr || v > 255) return -1;
        uint8_t out = (uint8_t)v; memcpy(base + f->offset, &out, sizeof(out));
        return 0;
    }
    default: return -1;
    }
}

static const nd_t *find_nvd(const char *cli)
{
    for (int i = 0; i < NUM_NVD; i++) {
        if (strcmp(cli, NVD[i].cli) == 0) return &NVD[i];
    }
    return NULL;
}

static int read_nvideo(octl_ctx_t *ctx, OrionNetworkVideo_t *out)
{
    OrionPkt_t req, resp;
    MakeOrionPacket(&req, getOrionNetworkVideoPacketID(), 0);
    if (conn_send(&req) != 0) return -1;
    if (conn_wait_for(ORION_PKT_NETWORK_VIDEO, &resp, ctx->timeout_ms) != 0) return -2;
    if (!decodeOrionNetworkVideoPacketStructure(&resp, out)) return -3;
    return 0;
}

static int nv_get(octl_ctx_t *ctx)
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

    OrionNetworkVideo_t o;
    int rc = read_nvideo(ctx, &o);
    conn_close();
    if (rc == -2) {
        jout_err(stderr, OCTL_TIMEOUT, "network_video_timeout",
                 "no NetworkVideo within %d ms", ctx->timeout_ms);
        return OCTL_TIMEOUT;
    }
    if (rc != 0) {
        jout_err(stderr, OCTL_INTERNAL, "network_video_failed",
                 "NetworkVideo read failed (rc=%d)", rc);
        return OCTL_INTERNAL;
    }
    jout_t j;
    jout_init(&j, stdout);
    emit_video_net(&j, &o);
    jout_done(&j);
    return OCTL_OK;
}

static int nv_set(octl_ctx_t *ctx)
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

    OrionNetworkVideo_t o;
    int rc = read_nvideo(ctx, &o);
    if (rc != 0) {
        jout_err(stderr, rc == -2 ? OCTL_TIMEOUT : OCTL_INTERNAL,
                 rc == -2 ? "network_video_timeout" : "network_video_failed",
                 "NetworkVideo pre-read failed (rc=%d)", rc);
        conn_close();
        return rc == -2 ? OCTL_TIMEOUT : OCTL_INTERNAL;
    }

    int mutations = 0;

    /* convenience flags */
    if (ctx->ip && ctx->npos >= 3 /* nothing - placeholder */) { /* no-op so we can keep style */ }
    /* Actually use ctx->ip is *gimbal* IP, not video dest. We need a separate flag.
       Re-use --port for dest port (it's still --port). For dest ip, accept positional
       or via DestIp=A.B.C.D in descriptor. */

    if (ctx->port_set) {
        OrionNetworkVideo_t before = o;
        uint16_t p = (uint16_t)ctx->port;
        o.Port = p;
        if (memcmp(&before, &o, sizeof(o)) != 0) mutations++;
    }

    /* pos[0]=video, pos[1]=net, pos[2]=set, pos[3..]=Field=Value */
    for (int i = 3; i < ctx->npos; i++) {
        const char *kv = ctx->pos[i];
        const char *eq = strchr(kv, '=');
        if (!eq || eq == kv) {
            jout_err(stderr, OCTL_USAGE, "bad_kv",
                     "expected Field=Value, got: %s", kv);
            conn_close();
            return OCTL_USAGE;
        }
        char name[64];
        size_t nlen = (size_t)(eq - kv);
        if (nlen >= sizeof(name)) {
            jout_err(stderr, OCTL_USAGE, "field_too_long", "field name too long");
            conn_close();
            return OCTL_USAGE;
        }
        memcpy(name, kv, nlen);
        name[nlen] = '\0';

        /* bit fields not in descriptor: SaveSettings, TsPacketCount, FramePeriod */
        if (strcmp(name, "SaveSettings") == 0) {
            char *end; unsigned long v = strtoul(eq + 1, &end, 0);
            if (*end != '\0' || end == eq + 1 || v > 1) {
                jout_err(stderr, OCTL_USAGE, "bad_value", "SaveSettings must be 0 or 1");
                conn_close(); return OCTL_USAGE;
            }
            o.SaveSettings = (unsigned)v;
            mutations++;
            continue;
        }
        if (strcmp(name, "TsPacketCount") == 0) {
            char *end; unsigned long v = strtoul(eq + 1, &end, 0);
            if (*end != '\0' || end == eq + 1 || v > 0xF) {
                jout_err(stderr, OCTL_USAGE, "bad_value", "TsPacketCount is 4 bits (0..15)");
                conn_close(); return OCTL_USAGE;
            }
            o.TsPacketCount = (unsigned)v;
            mutations++;
            continue;
        }
        if (strcmp(name, "FramePeriod") == 0) {
            char *end; unsigned long v = strtoul(eq + 1, &end, 0);
            if (*end != '\0' || end == eq + 1 || v > 7) {
                jout_err(stderr, OCTL_USAGE, "bad_value", "FramePeriod is 3 bits (0..7)");
                conn_close(); return OCTL_USAGE;
            }
            o.FramePeriod = (unsigned)v;
            mutations++;
            continue;
        }

        const nd_t *f = find_nvd(name);
        if (!f) {
            jout_err(stderr, OCTL_USAGE, "unknown_field",
                     "unknown NetworkVideo field: %s", name);
            conn_close();
            return OCTL_USAGE;
        }
        if (set_one(&o, f, eq + 1) != 0) {
            jout_err(stderr, OCTL_USAGE, "bad_value",
                     "could not parse value for %s: %s", name, eq + 1);
            conn_close();
            return OCTL_USAGE;
        }
        mutations++;
    }

    OrionPkt_t out_pkt;
    encodeOrionNetworkVideoPacketStructure(&out_pkt, &o);
    if (conn_send(&out_pkt) != 0) {
        jout_err(stderr, OCTL_CONN_FAILED, "send_failed", "could not send NetworkVideo");
        conn_close();
        return OCTL_CONN_FAILED;
    }
    OrionPkt_t echo;
    int echoed = (conn_wait_for(ORION_PKT_NETWORK_VIDEO, &echo, ctx->timeout_ms) == 0);
    OrionNetworkVideo_t echo_o;
    int echo_decoded = echoed && decodeOrionNetworkVideoPacketStructure(&echo, &echo_o);
    conn_close();

    jout_t j;
    jout_init(&j, stdout);
    jout_obj_open(&j);
    jout_kv_int (&j, "mutations", mutations);
    jout_kv_bool(&j, "echo_seen", echoed);
    if (echo_decoded) {
        jout_key(&j, "echo");
        emit_video_net(&j, &echo_o);
    } else {
        jout_key(&j, "written");
        emit_video_net(&j, &o);
    }
    jout_obj_close(&j);
    jout_done(&j);
    return OCTL_OK;
}

int cmd_video_net(octl_ctx_t *ctx)
{
    /* pos[0]=video, pos[1]=net, pos[2]=get|set */
    if (ctx->npos < 3) {
        jout_err(stderr, OCTL_USAGE, "missing_subverb",
                 "video net requires get | set");
        return OCTL_USAGE;
    }
    const char *sub = ctx->pos[2];
    if (strcmp(sub, "get") == 0) return nv_get(ctx);
    if (strcmp(sub, "set") == 0) return nv_set(ctx);
    jout_err(stderr, OCTL_USAGE, "unknown_subverb",
             "unknown video net subverb: %s", sub);
    return OCTL_USAGE;
}
