#include "orionctl.h"
#include "conn.h"
#include "json_out.h"

#include <stdio.h>

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

int cmd_sw_diag(octl_ctx_t *ctx)
{
    if (ctx->timeout_ms < OCTL_TIMEOUT_MIN_SW_DIAG)
        ctx->timeout_ms = OCTL_TIMEOUT_MIN_SW_DIAG;

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
    MakeOrionPacket(&req, getOrionSoftwareDiagnosticsPacketID(), 0);
    if (conn_send(&req) != 0) {
        jout_err(stderr, OCTL_CONN_FAILED, "send_failed", "could not send SoftwareDiagnostics request");
        conn_close();
        return OCTL_CONN_FAILED;
    }
    if (conn_wait_for(ORION_PKT_SOFTWARE_DIAGNOSTICS, &resp, ctx->timeout_ms) != 0) {
        jout_err(stderr, OCTL_TIMEOUT, "sw_diag_timeout",
                 "no SoftwareDiagnostics within %d ms", ctx->timeout_ms);
        conn_close();
        return OCTL_TIMEOUT;
    }
    OrionSoftwareDiagnostics_t d;
    if (!decodeOrionSoftwareDiagnosticsPacketStructure(&resp, &d)) {
        jout_err(stderr, OCTL_INTERNAL, "decode_failed", "SoftwareDiagnostics decode failed");
        conn_close();
        return OCTL_INTERNAL;
    }
    conn_close();

    jout_t j;
    jout_init(&j, stdout);
    jout_obj_open(&j);
    jout_kv_str(&j, "source_board",    board_name(d.sourceBoard));
    jout_kv_int(&j, "source_board_id", d.sourceBoard);
    jout_kv_int(&j, "num_cores",       d.numCores);

    jout_key(&j, "cores");
    jout_arr_open(&j);
    int ncores = d.numCores;
    if (ncores < 0) ncores = 0;
    if (ncores > 2) ncores = 2;
    for (int c = 0; c < ncores; c++) {
        const CoreLoading_t *cl = &d.CoreLoading[c];
        jout_obj_open(&j);
        jout_kv_dbl(&j, "cpu_load",   cl->cpuLoad);
        jout_kv_dbl(&j, "heap_load",  cl->heapLoad);
        jout_kv_dbl(&j, "stack_load", cl->stackLoad);
        jout_kv_int(&j, "num_threads", cl->numThreads);
        jout_key(&j, "threads");
        jout_arr_open(&j);
        int nth = cl->numThreads;
        if (nth < 0) nth = 0;
        if (nth > 10) nth = 10;
        for (int t = 0; t < nth; t++) {
            const ThreadLoading_t *th = &cl->ThreadLoading[t];
            jout_obj_open(&j);
            jout_kv_dbl (&j, "cpu_load",       th->cpuLoad);
            jout_kv_dbl (&j, "heap_load",      th->heapLoad);
            jout_kv_dbl (&j, "stack_load",     th->stackLoad);
            jout_kv_dbl (&j, "watchdog_left",  th->watchdogLeft);
            jout_kv_int (&j, "num_iterations", th->numIterations);
            jout_kv_dbl (&j, "worstcase_ratio",th->worstcase);
            jout_obj_close(&j);
        }
        jout_arr_close(&j);
        jout_obj_close(&j);
    }
    jout_arr_close(&j);

    jout_obj_close(&j);
    jout_done(&j);
    return OCTL_OK;
}
