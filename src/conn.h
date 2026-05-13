#ifndef ORIONCTL_CONN_H
#define ORIONCTL_CONN_H

#include <stdint.h>

#include "OrionPublicPacketShim.h"

int  conn_open(const char *ip);
void conn_close(void);
int  conn_drain(int ms);
int  conn_send(const OrionPkt_t *pkt);
int  conn_wait_for(uint8_t pkt_id, OrionPkt_t *out, int timeout_ms);

#endif
