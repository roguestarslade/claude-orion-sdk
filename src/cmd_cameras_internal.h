#ifndef ORIONCTL_CMD_CAMERAS_INTERNAL_H
#define ORIONCTL_CMD_CAMERAS_INTERNAL_H

#include "json_out.h"
#include "orionctl.h"

#include "OrionPublicPacket.h"

void cameras_emit(jout_t *j, const char *ip, const OrionCameras_t *c, int active_idx);

int cmd_cameras_fetch(octl_ctx_t *ctx, OrionCameras_t *out, int *active_idx_out);

int cameras_load_or_fetch(octl_ctx_t *ctx, OrionCameras_t *out, int *from_cache);

int telem_active_index(int timeout_ms);

#endif
