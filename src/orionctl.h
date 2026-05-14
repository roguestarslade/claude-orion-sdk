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

/* per-verb minimum timeouts in ms. floor, not override.
 * if user passes --timeout < min, we use min. */
#define OCTL_TIMEOUT_MIN_SW_DIAG   6000
#define OCTL_TIMEOUT_MIN_NET_DIAG  6000

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
    int         since_s;
    int         since_set;

    /* Phase 6: track / geotrack / tle */
    double      dx, dy;        int dx_set, dy_set;
    double      delta;         int delta_set;
    int         all;
    const char *filter;
    const char *target;
    int         tle_ins, tle_dted, tle_lrf, tle_offboard;

    /* Phase 7: video / record / klv */
    int         key,    key_set;
    int         subkey, subkey_set;
    const char *value;
    const char *value_hex;
    int         port,   port_set;

    /* Phase 10: uart / network / user-data */
    int         baud,     baud_set;
    const char *protocol;
    const char *netmask;
    const char *gateway;
    const char *hex_arg;     /* user-data --hex */
    const char *str_arg;     /* user-data --str */
    const char *id_arg;      /* listen --id */
    double      rate_hz;     int rate_hz_set;
    double      lat, lon, alt;  int lla_set;
    double      heading;     int heading_set;
    double      heading_acc; int heading_acc_set;
    double      undulation;  int undulation_set;
    const char *gps_source;

    /* Phase 8: motion */
    double      pan_rad,  tilt_rad;  int pan_set, tilt_set;
    double      pan_deg,  tilt_deg;  int pan_deg_set, tilt_deg_set;
    int         stab,     stab_set;
    double      rate_x, rate_y;      int rate_x_set, rate_y_set;
    double      box_x,  box_y;       int box_x_set,  box_y_set;
    double      vel_n,  vel_e, vel_d; int vel_n_set, vel_e_set, vel_d_set;
    int         stare_flag, closure_flag;
    int         step_count;          int step_count_set;
    const char *axis;
    double      max_current; int max_current_set;
    double      max_accel;   int max_accel_set;
    double      max_velocity;int max_velocity_set;
    double      min_pos;     int min_pos_set;
    double      max_pos;     int max_pos_set;
    const char *mode_name;
    double      deg_value;
    int         deg_set;
    double      zoom;
    int         zoom_set;

    /* camera set --<field> */
    int         persist;
    const char *set_type;
    const char *set_proto;
    int         set_gpio,         set_gpio_set;
    int         set_gpio_active,  set_gpio_active_set;
    double      set_min_focal,    set_min_focal_set;
    double      set_max_focal,    set_max_focal_set;
    double      set_pixel_pitch,  set_pixel_pitch_set;
    int         set_array_w,      set_array_w_set;
    int         set_array_h,      set_array_h_set;
    double      set_align_min[2]; int set_align_min_set;
    double      set_align_max[2]; int set_align_max_set;

    /* camera switch experiment flags */
    int         via_state;
    int         wait_active;

    int         argc;
    char      **argv;
    const char *pos[OCTL_MAX_POS];
    int         npos;
} octl_ctx_t;

int octl_resolve_ip(octl_ctx_t *ctx);
int octl_parse_global_flags(int argc, char **argv, octl_ctx_t *ctx);

int gate_motion_allowed(const octl_ctx_t *ctx);
int gate_laser_allowed(const octl_ctx_t *ctx);
int gate_reset_allowed(const octl_ctx_t *ctx);

int cmd_status(octl_ctx_t *ctx);
int cmd_telem(octl_ctx_t *ctx);
int cmd_cameras(octl_ctx_t *ctx);
int cmd_camera(octl_ctx_t *ctx);
int cmd_ins(octl_ctx_t *ctx);
int cmd_diag(octl_ctx_t *ctx);
int cmd_perf(octl_ctx_t *ctx);
int cmd_sw_diag(octl_ctx_t *ctx);
int cmd_vibration(octl_ctx_t *ctx);
int cmd_net_diag(octl_ctx_t *ctx);
int cmd_faults(octl_ctx_t *ctx);
int cmd_debug_strings(octl_ctx_t *ctx);
int cmd_range(octl_ctx_t *ctx);
int cmd_versions(octl_ctx_t *ctx);
int cmd_track(octl_ctx_t *ctx);
int cmd_geotrack(octl_ctx_t *ctx);
int cmd_tle(octl_ctx_t *ctx);
int cmd_video(octl_ctx_t *ctx);
int cmd_record(octl_ctx_t *ctx);
int cmd_klv(octl_ctx_t *ctx);
int cmd_mode(octl_ctx_t *ctx);
int cmd_point(octl_ctx_t *ctx);
int cmd_positions(octl_ctx_t *ctx);
int cmd_limits(octl_ctx_t *ctx);
int cmd_startup_cmd(octl_ctx_t *ctx);
int cmd_laser(octl_ctx_t *ctx);
int cmd_reset(octl_ctx_t *ctx);
int cmd_network(octl_ctx_t *ctx);
int cmd_uart(octl_ctx_t *ctx);
int cmd_crown_mode(octl_ctx_t *ctx);
int cmd_user_data(octl_ctx_t *ctx);
int cmd_scan_plan(octl_ctx_t *ctx);
int cmd_ins_options(octl_ctx_t *ctx);
int cmd_gps_feed(octl_ctx_t *ctx);
int cmd_heading_feed(octl_ctx_t *ctx);
int cmd_autopilot_feed(octl_ctx_t *ctx);
int cmd_geoid_feed(octl_ctx_t *ctx);
int cmd_retract(octl_ctx_t *ctx);
int cmd_stare_ack(octl_ctx_t *ctx);
int cmd_prf(octl_ctx_t *ctx);
int cmd_raw(octl_ctx_t *ctx);
int cmd_listen(octl_ctx_t *ctx);
int cmd_cache(octl_ctx_t *ctx);

#endif
