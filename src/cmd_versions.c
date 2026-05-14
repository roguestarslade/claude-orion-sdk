#include "orionctl.h"
#include "conn.h"
#include "json_out.h"

#include <stdio.h>
#include <string.h>

#include "OrionPublicPacket.h"
#include "OrionPublicPacketShim.h"

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

static void copy_fixed_str(char *dst, size_t dst_sz, const char *src, size_t src_sz)
{
    size_t n = src_sz < dst_sz - 1 ? src_sz : dst_sz - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static int try_clevis(jout_t *j, int timeout_ms)
{
    OrionPkt_t req, resp;
    MakeOrionPacket(&req, getOrionClevisVersionPacketID(), 0);
    if (conn_send(&req) != 0) return -1;
    if (conn_wait_for(ORION_PKT_CLEVIS_VERSION, &resp, timeout_ms) != 0) return -2;
    OrionClevisVersion_t v;
    if (!decodeOrionClevisVersionPacketStructure(&resp, &v)) return -3;
    char ver[17], pn[17];
    copy_fixed_str(ver, sizeof(ver), v.Version, 16);
    copy_fixed_str(pn,  sizeof(pn),  v.PartNumber, 16);
    jout_obj_open(j);
    jout_kv_str (j, "version",      ver);
    jout_kv_str (j, "part_number",  pn);
    jout_kv_uint(j, "on_time_min",  v.OnTime);
    jout_obj_close(j);
    return 0;
}

static int try_crown(jout_t *j, int timeout_ms)
{
    OrionPkt_t req, resp;
    MakeOrionPacket(&req, getOrionCrownVersionPacketID(), 0);
    if (conn_send(&req) != 0) return -1;
    if (conn_wait_for(ORION_PKT_CROWN_VERSION, &resp, timeout_ms) != 0) return -2;
    OrionCrownVersion_t v;
    if (!decodeOrionCrownVersionPacketStructure(&resp, &v)) return -3;
    char ver[17], pn[17];
    copy_fixed_str(ver, sizeof(ver), v.Version, 16);
    copy_fixed_str(pn,  sizeof(pn),  v.PartNumber, 16);
    jout_obj_open(j);
    jout_kv_str (j, "version",      ver);
    jout_kv_str (j, "part_number",  pn);
    jout_kv_uint(j, "on_time_min",  v.OnTime);
    jout_obj_close(j);
    return 0;
}

static int try_payload(jout_t *j, int timeout_ms)
{
    OrionPkt_t req, resp;
    MakeOrionPacket(&req, getOrionPayloadVersionPacketID(), 0);
    if (conn_send(&req) != 0) return -1;
    if (conn_wait_for(ORION_PKT_PAYLOAD_VERSION, &resp, timeout_ms) != 0) return -2;
    OrionPayloadVersion_t v;
    if (!decodeOrionPayloadVersionPacketStructure(&resp, &v)) return -3;
    char ver[25], pn[17];
    copy_fixed_str(ver, sizeof(ver), v.Version, 24);
    copy_fixed_str(pn,  sizeof(pn),  v.PartNumber, 16);
    jout_obj_open(j);
    jout_kv_str (j, "version",      ver);
    jout_kv_str (j, "part_number",  pn);
    jout_kv_uint(j, "on_time_min",  v.OnTime);
    jout_kv_int (j, "hw_type",      v.HwType);
    jout_kv_int (j, "hw_revision",  v.HwRevision);
    jout_obj_close(j);
    return 0;
}

static int try_tracker(jout_t *j, int timeout_ms)
{
    OrionPkt_t req, resp;
    MakeOrionPacket(&req, getOrionTrackerVersionPacketID(), 0);
    if (conn_send(&req) != 0) return -1;
    if (conn_wait_for(ORION_PKT_TRACKER_VERSION, &resp, timeout_ms) != 0) return -2;
    OrionTrackerVersion_t v;
    if (!decodeOrionTrackerVersionPacketStructure(&resp, &v)) return -3;
    char ver[17], pn[17];
    copy_fixed_str(ver, sizeof(ver), v.Version, 16);
    copy_fixed_str(pn,  sizeof(pn),  v.PartNumber, 16);
    jout_obj_open(j);
    jout_kv_str (j, "version",      ver);
    jout_kv_str (j, "part_number",  pn);
    jout_kv_uint(j, "app_bits",     v.AppBits);
    jout_obj_close(j);
    return 0;
}

static int try_retract(jout_t *j, int timeout_ms)
{
    OrionPkt_t req, resp;
    MakeOrionPacket(&req, getOrionRetractVersionPacketID(), 0);
    if (conn_send(&req) != 0) return -1;
    if (conn_wait_for(ORION_PKT_RETRACT_VERSION, &resp, timeout_ms) != 0) return -2;
    OrionRetractVersion_t v;
    if (!decodeOrionRetractVersionPacketStructure(&resp, &v)) return -3;
    char ver[17];
    copy_fixed_str(ver, sizeof(ver), v.Version, 16);
    jout_obj_open(j);
    jout_kv_str(j, "version", ver);
    jout_obj_close(j);
    return 0;
}

static int try_lensctl(jout_t *j, int timeout_ms)
{
    OrionPkt_t req, resp;
    MakeOrionPacket(&req, getOrionLensCtlVersionPacketID(), 0);
    if (conn_send(&req) != 0) return -1;
    if (conn_wait_for(ORION_PKT_LENSCTL_VERSION, &resp, timeout_ms) != 0) return -2;
    OrionLensCtlVersion_t v;
    if (!decodeOrionLensCtlVersionPacketStructure(&resp, &v)) return -3;
    char ver[17];
    copy_fixed_str(ver, sizeof(ver), v.Version, 16);
    jout_obj_open(j);
    jout_kv_str(j, "version", ver);
    jout_obj_close(j);
    return 0;
}

static int try_reset_source(jout_t *j, int timeout_ms)
{
    OrionPkt_t req, resp;
    MakeOrionPacket(&req, getOrionResetSourcePacketID(), 0);
    if (conn_send(&req) != 0) return -1;
    if (conn_wait_for(ORION_PKT_RESET_SOURCE, &resp, timeout_ms) != 0) return -2;
    uint32_t vector = 0, address = 0;
    OrionBoardEnumeration_t sb = BOARD_NONE;
    if (!decodeOrionResetSourcePacket(&resp, &vector, &address, &sb)) return -3;
    jout_obj_open(j);
    jout_kv_uint(j, "vector",  vector);
    jout_kv_uint(j, "address", address);
    jout_kv_str (j, "source_board",    board_name(sb));
    jout_kv_int (j, "source_board_id", sb);
    jout_obj_close(j);
    return 0;
}

static int try_board(jout_t *j, int timeout_ms)
{
    OrionPkt_t req, resp;
    MakeOrionPacket(&req, getOrionBoardPacketID(), 0);
    if (conn_send(&req) != 0) return -1;
    if (conn_wait_for(ORION_PKT_BOARD, &resp, timeout_ms) != 0) return -2;
    OrionBoard_t b;
    if (!decodeOrionBoardPacketStructure(&resp, &b)) return -3;
    jout_obj_open(j);
    jout_kv_uint(j, "board_sn",     b.boardSN);
    jout_kv_uint(j, "assembly_sn",  b.assemblySN);
    jout_kv_uint(j, "config",       b.config);
    jout_kv_str (j, "board",        board_name(b.boardEnum));
    jout_kv_int (j, "board_id",     b.boardEnum);
    jout_key(j, "manufacture_date"); jout_obj_open(j);
    jout_kv_int(j, "year_2000", b.manufactureDate.year);
    jout_kv_int(j, "month",     b.manufactureDate.month);
    jout_kv_int(j, "day",       b.manufactureDate.day);
    jout_obj_close(j);
    jout_key(j, "calibration_date"); jout_obj_open(j);
    jout_kv_int(j, "year_2000", b.calibrationDate.year);
    jout_kv_int(j, "month",     b.calibrationDate.month);
    jout_kv_int(j, "day",       b.calibrationDate.day);
    jout_obj_close(j);
    jout_obj_close(j);
    return 0;
}

static int try_product(jout_t *j, int timeout_ms)
{
    OrionPkt_t req, resp;
    MakeOrionPacket(&req, getProductPacketID(), 0);
    if (conn_send(&req) != 0) return -1;
    if (conn_wait_for(ORION_PKT_PRODUCT, &resp, timeout_ms) != 0) return -2;
    Product_t p;
    if (!decodeProductPacketStructure(&resp, &p)) return -3;
    char pn[17], name[65];
    copy_fixed_str(pn,   sizeof(pn),   p.partNumber, 16);
    copy_fixed_str(name, sizeof(name), p.productName, 64);
    jout_obj_open(j);
    jout_kv_str (j, "part_number",   pn);
    jout_kv_uint(j, "serial_number", p.serialNumber);
    jout_kv_str (j, "product_name",  name);
    jout_obj_close(j);
    return 0;
}

typedef int (*one_fn)(jout_t *, int);

typedef struct {
    const char *key;
    one_fn      fn;
} entry_t;

int cmd_versions(octl_ctx_t *ctx)
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

    int t = ctx->timeout_ms;

    entry_t list[] = {
        { "clevis",       try_clevis },
        { "crown",        try_crown },
        { "payload",      try_payload },
        { "tracker",      try_tracker },
        { "retract",      try_retract },
        { "lensctl",      try_lensctl },
        { "reset_source", try_reset_source },
        { "board",        try_board },
        { "product",      try_product },
    };
    int n = (int)(sizeof(list) / sizeof(list[0]));

    jout_t j;
    jout_init(&j, stdout);
    jout_obj_open(&j);

    int succeeded = 0;
    int timed_out = 0;
    char timeouts_buf[256];
    timeouts_buf[0] = '\0';

    for (int i = 0; i < n; i++) {
        jout_key(&j, list[i].key);
        int rc = list[i].fn(&j, t);
        if (rc == 0) {
            succeeded++;
        } else {
            jout_null(&j);
            if (rc == -2) {
                timed_out++;
                size_t used = strlen(timeouts_buf);
                snprintf(timeouts_buf + used, sizeof(timeouts_buf) - used,
                         "%s%s", used ? "," : "", list[i].key);
            }
        }
    }

    jout_key(&j, "summary");
    jout_obj_open(&j);
    jout_kv_int(&j, "succeeded", succeeded);
    jout_kv_int(&j, "timed_out", timed_out);
    jout_kv_str(&j, "timed_out_keys", timeouts_buf);
    jout_obj_close(&j);

    jout_obj_close(&j);
    jout_done(&j);
    conn_close();
    return OCTL_OK;
}
