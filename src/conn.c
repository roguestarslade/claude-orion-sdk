#include "conn.h"

#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/time.h>

#include "OrionComm.h"

static int g_opened;

static long long now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

int conn_open(const char *ip)
{
    if (!ip || !*ip) return -1;
    if (!OrionCommIpStringValid(ip)) return -1;

    fflush(stdout);
    int saved = dup(STDOUT_FILENO);
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
        dup2(devnull, STDOUT_FILENO);
        close(devnull);
    }
    int ok = OrionCommOpenNetworkIp(ip);
    fflush(stdout);
    if (saved >= 0) {
        dup2(saved, STDOUT_FILENO);
        close(saved);
    }
    if (!ok) return -1;
    g_opened = 1;
    return 0;
}

void conn_close(void)
{
    if (g_opened) {
        OrionCommClose();
        g_opened = 0;
    }
}

int conn_drain(int ms)
{
    OrionPkt_t pkt;
    long long deadline = now_ms() + ms;
    int n = 0;
    while (now_ms() < deadline) {
        while (OrionCommReceive(&pkt)) n++;
        usleep(5000);
    }
    return n;
}

int conn_send(const OrionPkt_t *pkt)
{
    return OrionCommSend(pkt) ? 0 : -1;
}

int conn_wait_for(uint8_t pkt_id, OrionPkt_t *out, int timeout_ms)
{
    long long deadline = now_ms() + timeout_ms;
    OrionPkt_t pkt;
    while (now_ms() < deadline) {
        while (OrionCommReceive(&pkt)) {
            if (pkt.ID == pkt_id) {
                if (out) *out = pkt;
                return 0;
            }
        }
        usleep(5000);
    }
    return -1;
}
