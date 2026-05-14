#include "orionctl.h"
#include "conn.h"
#include "json_out.h"

#include <stdio.h>

#include "OrionPublicPacket.h"
#include "OrionPublicPacketShim.h"

int cmd_net_diag(octl_ctx_t *ctx)
{
    if (ctx->timeout_ms < OCTL_TIMEOUT_MIN_NET_DIAG)
        ctx->timeout_ms = OCTL_TIMEOUT_MIN_NET_DIAG;

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

    OrionPkt_t req, resp;
    MakeOrionPacket(&req, getNetworkDiagnosticsPacketID(), 0);
    if (conn_send(&req) != 0) {
        jout_err(stderr, OCTL_CONN_FAILED, "send_failed", "could not send NetworkDiagnostics request");
        conn_close();
        return OCTL_CONN_FAILED;
    }
    if (conn_wait_for(ORION_PKT_NETWORK_DIAGNOSTICS, &resp, ctx->timeout_ms) != 0) {
        jout_err(stderr, OCTL_TIMEOUT, "net_diag_timeout",
                 "no NetworkDiagnostics within %d ms", ctx->timeout_ms);
        conn_close();
        return OCTL_TIMEOUT;
    }
    NetworkDiagnostics_t n;
    if (!decodeNetworkDiagnosticsPacketStructure(&resp, &n)) {
        jout_err(stderr, OCTL_INTERNAL, "decode_failed", "NetworkDiagnostics decode failed");
        conn_close();
        return OCTL_INTERNAL;
    }
    conn_close();

    jout_t j;
    jout_init(&j, stdout);
    jout_obj_open(&j);
    jout_kv_uint(&j, "flags", n.Flags);
    jout_kv_uint(&j, "rx_bytes",       n.RxBytes);
    jout_kv_uint(&j, "tx_bytes",       n.TxBytes);
    jout_kv_uint(&j, "rx_packets",     n.RxPackets);
    jout_kv_uint(&j, "tx_packets",     n.TxPackets);
    jout_kv_uint(&j, "rx_errors",      n.RxErrors);
    jout_kv_uint(&j, "tx_errors",      n.TxErrors);
    jout_kv_uint(&j, "rx_drops",       n.RxDrops);
    jout_kv_uint(&j, "tx_drops",       n.TxDrops);
    jout_kv_uint(&j, "rx_fifo_errors", n.RxFifoErrors);
    jout_kv_uint(&j, "tx_fifo_errors", n.TxFifoErrors);
    jout_kv_uint(&j, "frame_errors",   n.FrameErrors);
    jout_kv_uint(&j, "collisions",     n.Collisions);
    jout_kv_uint(&j, "carrier_errors", n.CarrierErrors);
    jout_obj_close(&j);
    jout_done(&j);
    return OCTL_OK;
}
