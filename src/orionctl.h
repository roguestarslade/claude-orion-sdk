#ifndef ORIONCTL_H
#define ORIONCTL_H

#include <stdint.h>

#define OCTL_OK              0
#define OCTL_USAGE           1
#define OCTL_CONN_FAILED     2
#define OCTL_TIMEOUT         3
#define OCTL_REJECTED        4
#define OCTL_MOTION_GATE     5
#define OCTL_CAMERA_IDX      6
#define OCTL_UNSUPPORTED     7
#define OCTL_VENDOR_UNIMPL   8
#define OCTL_LASER_GATE      9
#define OCTL_INTERNAL       10

#define OCTL_DEFAULT_TIMEOUT_MS 2000
#define OCTL_DRAIN_MS            100

#define OCTL_MAX_POS 16

typedef struct {
    const char *ip;
    int         timeout_ms;
    int         allow_motion;
    int         allow_laser;
    int         iknow;
    int         discover;
    int         idx;
    int         idx_set;
    int         watch;
    int         watch_n;
    double      deg_value;
    int         deg_set;
    double      zoom;
    int         zoom_set;
    int         argc;
    char      **argv;
    const char *pos[OCTL_MAX_POS];
    int         npos;
} octl_ctx_t;

int octl_resolve_ip(octl_ctx_t *ctx);
int octl_parse_global_flags(int argc, char **argv, octl_ctx_t *ctx);

int cmd_status(octl_ctx_t *ctx);
int cmd_telem(octl_ctx_t *ctx);
int cmd_cameras(octl_ctx_t *ctx);
int cmd_camera(octl_ctx_t *ctx);

#endif
