#include "orionctl.h"
#include "conn.h"
#include "json_out.h"

#ifndef _WIN32
#include <arpa/inet.h>
#endif
#ifndef INET_ADDRSTRLEN
#define INET_ADDRSTRLEN 16
#endif
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "OrionPublicPacket.h"
#include "OrionPublicPacketShim.h"

static const char *rec_state_name(int s)
{
    switch (s) {
    case VideoRecordStateIdle:                return "idle";
    case VideoRecordStateRecording:           return "recording";
    case VideoRecordStateRecording_Streaming: return "recording_streaming";
    case VideoRecordStateStreaming:           return "streaming";
    case VideoRecordStateStalled:             return "stalled";
    default:                                  return "unknown";
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

static void ip_to_str(uint32_t ip, char out[INET_ADDRSTRLEN])
{
    snprintf(out, INET_ADDRSTRLEN, "%u.%u.%u.%u",
             (ip >> 24) & 0xFFu, (ip >> 16) & 0xFFu,
             (ip >> 8)  & 0xFFu,  ip        & 0xFFu);
}

static void emit_camera_info(jout_t *j, const CameraInformation_t *ci)
{
    jout_obj_open(j);
    jout_kv_str (j, "state",            rec_state_name(ci->Status));
    jout_kv_int (j, "state_id",         ci->Status);
    jout_kv_uint(j, "stream_id",        ci->StreamId);
    jout_kv_uint(j, "bitrate_kbps",     ci->Bitrate);
    jout_kv_dbl (j, "frame_statistics", ci->FrameStatistics);
    jout_kv_bool(j, "enabled",          ci->Enabled);
    jout_obj_close(j);
}

static void emit_status(jout_t *j, const VideoRecordStatus_t *s)
{
    char ver[17]; memcpy(ver, s->Version, 16); ver[16] = '\0';
    char ipstr[INET_ADDRSTRLEN]; ip_to_str(s->UdpDestIp, ipstr);

    jout_obj_open(j);
    jout_kv_str (j, "version",          ver);

    jout_key(j, "cameras"); jout_arr_open(j);
    for (int i = 0; i < 3; i++) emit_camera_info(j, &s->CameraInformation[i]);
    /* 4th camera lives separately */
    emit_camera_info(j, &s->CameraInformation3);
    jout_arr_close(j);

    jout_kv_bool(j, "udp_enabled",       s->UdpEnabled);
    jout_kv_bool(j, "recording_enabled", s->RecordingEnabled);
    jout_kv_bool(j, "enable_klv_data",   s->EnableKlvData);
    jout_kv_bool(j, "enable_orion_data", s->EnableOrionData);
    jout_kv_bool(j, "obr_enabled",       s->OBREnabled);

    jout_kv_str (j, "udp_dest_ip",       ipstr);
    jout_kv_uint(j, "udp_dest_ip_u32",   s->UdpDestIp);
    jout_kv_uint(j, "udp_port",          s->UdpPort);
    jout_kv_uint(j, "udp_bitrate_kbps",  s->UdpBitrate);

    jout_kv_str (j, "klv_mode",          klv_mode_name(s->KlvMode));
    jout_kv_int (j, "klv_mode_id",       s->KlvMode);
    jout_kv_str (j, "state",             rec_state_name(s->State));
    jout_kv_int (j, "state_id",          s->State);
    jout_kv_dbl (j, "disk_consumption",  s->DiskConsumption);

    jout_key(j, "error"); jout_obj_open(j);
    jout_kv_bool(j, "disk_full",       s->Error.DiskFull);
    jout_kv_bool(j, "disk_80_percent", s->Error.Disk80Percent);
    jout_kv_bool(j, "bad_statistics",  s->Error.BadStatistics);
    jout_obj_close(j);

    jout_obj_close(j);
}

static int read_status(octl_ctx_t *ctx, VideoRecordStatus_t *out)
{
    OrionPkt_t req, resp;
    MakeOrionPacket(&req, getVideoRecordStatusPacketID(), 0);
    if (conn_send(&req) != 0) return -1;
    if (conn_wait_for(ORION_PKT_VIDEORECORD_STATUS, &resp, ctx->timeout_ms) != 0) return -2;
    if (!decodeVideoRecordStatusPacketStructure(&resp, out)) return -3;
    return 0;
}

static int record_status(octl_ctx_t *ctx)
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

    VideoRecordStatus_t s;
    int rc = read_status(ctx, &s);
    conn_close();
    if (rc == -2) {
        jout_err(stderr, OCTL_TIMEOUT, "record_status_timeout",
                 "no VideoRecordStatus within %d ms", ctx->timeout_ms);
        return OCTL_TIMEOUT;
    }
    if (rc != 0) {
        jout_err(stderr, OCTL_INTERNAL, "record_status_failed",
                 "VideoRecordStatus read failed (rc=%d)", rc);
        return OCTL_INTERNAL;
    }
    jout_t j; jout_init(&j, stdout);
    emit_status(&j, &s);
    jout_done(&j);
    return OCTL_OK;
}

static void cmd_from_status(VideoRecordCmd_t *cmd, const VideoRecordStatus_t *st)
{
    memset(cmd, 0, sizeof(*cmd));
    cmd->EnableUdpStream  = st->UdpEnabled;
    cmd->EnableRecording  = st->RecordingEnabled;
    cmd->EnableKlvData    = st->EnableKlvData;
    cmd->EnableOrionData  = st->EnableOrionData;
    cmd->EnableOBR        = st->OBREnabled;
    cmd->StreamOnRestart  = 0;
    cmd->DeleteAllRecordings = 0;
    cmd->Ident            = 0;
    cmd->UdpDestIp        = st->UdpDestIp;
    cmd->UdpPort          = st->UdpPort;
    /* convert KBps -> bps (status reports KBps, cmd field is bps) */
    cmd->UdpBitrateCamera0 = (uint32_t)st->CameraInformation[0].Bitrate * 1000U;
    cmd->UdpBitrateCamera1 = (uint32_t)st->CameraInformation[1].Bitrate * 1000U;
    cmd->UdpBitrateCamera2 = (uint32_t)st->CameraInformation[2].Bitrate * 1000U;
    cmd->UdpBitrateCamera3 = (uint32_t)st->CameraInformation3.Bitrate    * 1000U;
    cmd->KlvMode          = st->KlvMode;
    cmd->EnableCamera0    = st->CameraInformation[0].Enabled;
    cmd->EnableCamera1    = st->CameraInformation[1].Enabled;
    cmd->EnableCamera2    = st->CameraInformation[2].Enabled;
    cmd->EnableCamera3    = st->CameraInformation3.Enabled;
}

static void emit_cmd(jout_t *j, const VideoRecordCmd_t *c)
{
    char ipstr[INET_ADDRSTRLEN]; ip_to_str(c->UdpDestIp, ipstr);
    jout_obj_open(j);
    jout_kv_bool(j, "enable_udp_stream", c->EnableUdpStream);
    jout_kv_bool(j, "enable_recording",  c->EnableRecording);
    jout_kv_bool(j, "enable_klv_data",   c->EnableKlvData);
    jout_kv_bool(j, "enable_orion_data", c->EnableOrionData);
    jout_kv_bool(j, "delete_all_recordings", c->DeleteAllRecordings);
    jout_kv_bool(j, "ident",             c->Ident);
    jout_kv_bool(j, "stream_on_restart", c->StreamOnRestart);
    jout_kv_bool(j, "enable_obr",        c->EnableOBR);
    jout_kv_str (j, "udp_dest_ip",       ipstr);
    jout_kv_uint(j, "udp_port",          c->UdpPort);
    jout_kv_uint(j, "udp_bitrate_cam0_bps", c->UdpBitrateCamera0);
    jout_kv_uint(j, "udp_bitrate_cam1_bps", c->UdpBitrateCamera1);
    jout_kv_uint(j, "udp_bitrate_cam2_bps", c->UdpBitrateCamera2);
    jout_kv_uint(j, "udp_bitrate_cam3_bps", c->UdpBitrateCamera3);
    jout_kv_str (j, "klv_mode",          klv_mode_name(c->KlvMode));
    jout_kv_int (j, "klv_mode_id",       c->KlvMode);
    jout_kv_bool(j, "enable_camera0",    c->EnableCamera0);
    jout_kv_bool(j, "enable_camera1",    c->EnableCamera1);
    jout_kv_bool(j, "enable_camera2",    c->EnableCamera2);
    jout_kv_bool(j, "enable_camera3",    c->EnableCamera3);
    jout_obj_close(j);
}

typedef enum { R_BIT, R_U32_IP, R_U16, R_U32_BPS, R_KLV } rtype_t;

typedef struct {
    const char *cli;
    rtype_t     type;
    int         which;   /* index into the cmd struct for bit fields */
} rd_t;

/* enum index for bit fields */
enum {
    R_EnableUdpStream, R_EnableRecording, R_EnableKlvData, R_EnableOrionData,
    R_StreamOnRestart, R_EnableOBR,
    R_EnableCamera0, R_EnableCamera1, R_EnableCamera2, R_EnableCamera3,
    R_UdpDestIp, R_UdpPort,
    R_UdpBitrateCamera0, R_UdpBitrateCamera1, R_UdpBitrateCamera2, R_UdpBitrateCamera3,
    R_KlvMode_
};

static const rd_t RD[] = {
    { "EnableUdpStream",     R_BIT,     R_EnableUdpStream     },
    { "EnableRecording",     R_BIT,     R_EnableRecording     },
    { "EnableKlvData",       R_BIT,     R_EnableKlvData       },
    { "EnableOrionData",     R_BIT,     R_EnableOrionData     },
    { "StreamOnRestart",     R_BIT,     R_StreamOnRestart     },
    { "EnableOBR",           R_BIT,     R_EnableOBR           },
    { "EnableCamera0",       R_BIT,     R_EnableCamera0       },
    { "EnableCamera1",       R_BIT,     R_EnableCamera1       },
    { "EnableCamera2",       R_BIT,     R_EnableCamera2       },
    { "EnableCamera3",       R_BIT,     R_EnableCamera3       },
    { "UdpDestIp",           R_U32_IP,  R_UdpDestIp           },
    { "UdpPort",             R_U16,     R_UdpPort             },
    { "UdpBitrateCamera0",   R_U32_BPS, R_UdpBitrateCamera0   },
    { "UdpBitrateCamera1",   R_U32_BPS, R_UdpBitrateCamera1   },
    { "UdpBitrateCamera2",   R_U32_BPS, R_UdpBitrateCamera2   },
    { "UdpBitrateCamera3",   R_U32_BPS, R_UdpBitrateCamera3   },
    { "KlvMode",             R_KLV,     R_KlvMode_            },
};
#define NUM_RD ((int)(sizeof(RD) / sizeof(RD[0])))

static int apply_rd(VideoRecordCmd_t *c, const rd_t *r, const char *vstr)
{
    char *end;
    if (r->type == R_BIT) {
        unsigned long v = strtoul(vstr, &end, 0);
        if (*end != '\0' || end == vstr || v > 1) return -1;
        unsigned b = (unsigned)v;
        switch (r->which) {
        case R_EnableUdpStream: c->EnableUdpStream = b; break;
        case R_EnableRecording: c->EnableRecording = b; break;
        case R_EnableKlvData:   c->EnableKlvData   = b; break;
        case R_EnableOrionData: c->EnableOrionData = b; break;
        case R_StreamOnRestart: c->StreamOnRestart = b; break;
        case R_EnableOBR:       c->EnableOBR       = b; break;
        case R_EnableCamera0:   c->EnableCamera0   = b; break;
        case R_EnableCamera1:   c->EnableCamera1   = b; break;
        case R_EnableCamera2:   c->EnableCamera2   = b; break;
        case R_EnableCamera3:   c->EnableCamera3   = b; break;
        default: return -1;
        }
        return 0;
    }
    if (r->type == R_U16) {
        unsigned long v = strtoul(vstr, &end, 0);
        if (*end != '\0' || end == vstr || v > 0xFFFF) return -1;
        c->UdpPort = (uint16_t)v;
        return 0;
    }
    if (r->type == R_U32_IP) {
        unsigned a, b, c1, d;
        if (sscanf(vstr, "%u.%u.%u.%u", &a, &b, &c1, &d) == 4) {
            if (a > 255 || b > 255 || c1 > 255 || d > 255) return -1;
            c->UdpDestIp = (a << 24) | (b << 16) | (c1 << 8) | d;
        } else {
            unsigned long v = strtoul(vstr, &end, 0);
            if (*end != '\0' || end == vstr) return -1;
            c->UdpDestIp = (uint32_t)v;
        }
        return 0;
    }
    if (r->type == R_U32_BPS) {
        unsigned long v = strtoul(vstr, &end, 0);
        if (*end != '\0' || end == vstr) return -1;
        uint32_t bps = (uint32_t)v;
        switch (r->which) {
        case R_UdpBitrateCamera0: c->UdpBitrateCamera0 = bps; break;
        case R_UdpBitrateCamera1: c->UdpBitrateCamera1 = bps; break;
        case R_UdpBitrateCamera2: c->UdpBitrateCamera2 = bps; break;
        case R_UdpBitrateCamera3: c->UdpBitrateCamera3 = bps; break;
        default: return -1;
        }
        return 0;
    }
    if (r->type == R_KLV) {
        unsigned long v = strtoul(vstr, &end, 0);
        if (*end != '\0' || end == vstr || v > 255) return -1;
        c->KlvMode = (KlvMode_t)v;
        return 0;
    }
    return -1;
}

static const rd_t *find_rd(const char *cli)
{
    for (int i = 0; i < NUM_RD; i++) {
        if (strcmp(cli, RD[i].cli) == 0) return &RD[i];
    }
    return NULL;
}

static int send_cmd(octl_ctx_t *ctx, const VideoRecordCmd_t *c)
{
    OrionPkt_t pkt;
    encodeVideoRecordCmdPacketStructure(&pkt, c);
    if (conn_send(&pkt) != 0) return -1;
    OrionPkt_t echo;
    int echoed = (conn_wait_for(ORION_PKT_VIDEORECORD_STATUS, &echo, ctx->timeout_ms) == 0);

    jout_t j; jout_init(&j, stdout);
    jout_obj_open(&j);
    jout_kv_bool(&j, "echo_seen", echoed);
    jout_key(&j, "sent");
    emit_cmd(&j, c);
    if (echoed) {
        VideoRecordStatus_t st;
        if (decodeVideoRecordStatusPacketStructure(&echo, &st)) {
            jout_key(&j, "status_after");
            emit_status(&j, &st);
        }
    }
    jout_obj_close(&j);
    jout_done(&j);
    return 0;
}

static int rmw_open(octl_ctx_t *ctx, VideoRecordStatus_t *out)
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

    int rc = read_status(ctx, out);
    if (rc == -2) {
        jout_err(stderr, OCTL_TIMEOUT, "record_status_timeout",
                 "no VideoRecordStatus pre-read within %d ms", ctx->timeout_ms);
        conn_close();
        return OCTL_TIMEOUT;
    }
    if (rc != 0) {
        jout_err(stderr, OCTL_INTERNAL, "record_status_failed",
                 "VideoRecordStatus pre-read failed (rc=%d)", rc);
        conn_close();
        return OCTL_INTERNAL;
    }
    return OCTL_OK;
}

static int record_start_or_stop(octl_ctx_t *ctx, int start)
{
    if (!ctx->iknow) {
        jout_err(stderr, OCTL_REJECTED, "missing_iknow",
                 "record %s mutates shared device state; pass --i-know to confirm",
                 start ? "start" : "stop");
        return OCTL_REJECTED;
    }
    VideoRecordStatus_t st;
    int rc = rmw_open(ctx, &st);
    if (rc != OCTL_OK) return rc;

    VideoRecordCmd_t cmd;
    cmd_from_status(&cmd, &st);
    cmd.EnableUdpStream = start ? 1 : 0;
    cmd.EnableRecording = start ? 1 : 0;
    /* Ident=0 and DeleteAllRecordings=0 already enforced in cmd_from_status */

    int sc = send_cmd(ctx, &cmd);
    conn_close();
    if (sc != 0) {
        jout_err(stderr, OCTL_CONN_FAILED, "send_failed", "VideoRecordCmd send failed");
        return OCTL_CONN_FAILED;
    }
    return OCTL_OK;
}

static int record_set(octl_ctx_t *ctx)
{
    if (!ctx->iknow) {
        jout_err(stderr, OCTL_REJECTED, "missing_iknow",
                 "record set mutates shared device state; pass --i-know to confirm");
        return OCTL_REJECTED;
    }
    if (ctx->npos < 3) {
        jout_err(stderr, OCTL_USAGE, "missing_args",
                 "record set requires at least one Field=Value pair");
        return OCTL_USAGE;
    }
    VideoRecordStatus_t st;
    int rc = rmw_open(ctx, &st);
    if (rc != OCTL_OK) return rc;

    VideoRecordCmd_t cmd;
    cmd_from_status(&cmd, &st);

    int mutations = 0;
    for (int i = 2; i < ctx->npos; i++) {
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
        const rd_t *r = find_rd(name);
        if (!r) {
            jout_err(stderr, OCTL_USAGE, "unknown_field",
                     "unknown VideoRecordCmd field: %s", name);
            conn_close();
            return OCTL_USAGE;
        }
        if (apply_rd(&cmd, r, eq + 1) != 0) {
            jout_err(stderr, OCTL_USAGE, "bad_value",
                     "could not parse value for %s: %s", name, eq + 1);
            conn_close();
            return OCTL_USAGE;
        }
        mutations++;
    }
    /* Always enforce safety bits */
    cmd.DeleteAllRecordings = 0;
    cmd.Ident = 0;

    int sc = send_cmd(ctx, &cmd);
    conn_close();
    if (sc != 0) {
        jout_err(stderr, OCTL_CONN_FAILED, "send_failed", "VideoRecordCmd send failed");
        return OCTL_CONN_FAILED;
    }
    (void)mutations;
    return OCTL_OK;
}

int cmd_record(octl_ctx_t *ctx)
{
    if (ctx->npos < 2) {
        jout_err(stderr, OCTL_USAGE, "missing_subverb",
                 "record requires subverb: status | start | stop | set");
        return OCTL_USAGE;
    }
    const char *sub = ctx->pos[1];
    if (strcmp(sub, "status") == 0) return record_status(ctx);
    if (strcmp(sub, "start")  == 0) return record_start_or_stop(ctx, 1);
    if (strcmp(sub, "stop")   == 0) return record_start_or_stop(ctx, 0);
    if (strcmp(sub, "set")    == 0) return record_set(ctx);
    jout_err(stderr, OCTL_USAGE, "unknown_subverb",
             "unknown record subverb: %s", sub);
    return OCTL_USAGE;
}
