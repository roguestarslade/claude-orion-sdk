#include "orionctl.h"
#include "json_out.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int (*verb_fn)(octl_ctx_t *);

typedef struct {
    const char *name;
    verb_fn     fn;
    const char *help;
} verb_t;

static int cmd_help(octl_ctx_t *ctx);

static const verb_t VERBS[] = {
    { "status",        cmd_status,        "one-shot health snapshot"                       },
    { "telem",         cmd_telem,         "telemetry [--watch --n N]"                      },
    { "cameras",       cmd_cameras,       "list all 4 camera slots"                        },
    { "camera",        cmd_camera,        "camera get|switch|fov|zoom|focus|set [--idx N]" },
    { "ins",           cmd_ins,           "INS quality (0xD3)"                             },
    { "diag",          cmd_diag,          "electrical diagnostics (0x41)"                  },
    { "perf",          cmd_perf,          "stab performance (0x43)"                        },
    { "sw-diag",       cmd_sw_diag,       "software diagnostics (0x44)"                    },
    { "vibration",     cmd_vibration,     "platform vibration + FFT (0x45)"                },
    { "net-diag",      cmd_net_diag,      "network diagnostics (0x46)"                     },
    { "faults",        cmd_faults,        "collect faults for --since SEC (default 5)"     },
    { "debug-strings", cmd_debug_strings, "collect debug strings for --since SEC (def 5)"  },
    { "range",         cmd_range,         "current range value + source (0xD6)"            },
    { "versions",      cmd_versions,      "version/identity sweep across 9 packets"        },
    { "track",         cmd_track,         "track options|create|resize|nudge|destroy|status|watch" },
    { "geotrack",      cmd_geotrack,      "geotrack run|stop|restart|status"               },
    { "tle",           cmd_tle,           "tle start|stop|status"                          },
    { "video",         cmd_video,         "video get|set|net get|net set"                  },
    { "record",        cmd_record,        "record status|start|stop|set (writes --i-know)" },
    { "klv",           cmd_klv,           "klv get|list|query|set|delete"                  },
    { "mode",          cmd_mode,          "mode disable|rate|georate|position|scene|track|ffc|calibration|null-gyros (--allow-motion)" },
    { "point",         cmd_point,         "point geopoint|path|nadir|home|stow (--allow-motion)" },
    { "positions",     cmd_positions,     "positions get|set (set: --allow-motion --i-know)" },
    { "limits",        cmd_limits,        "limits get|set (set: --allow-motion --i-know)"   },
    { "startup-cmd",   cmd_startup_cmd,   "startup-cmd get|set (set: --allow-motion --i-know)" },
    { "laser",         cmd_laser,         "laser status|fire|stop (fire/stop: --allow-laser)" },
    { "reset",         cmd_reset,         "fire-and-exit reset (--allow-motion --i-know)"  },
    { "network",       cmd_network,       "network get|set (set: --i-know)"                },
    { "uart",          cmd_uart,          "uart get|set (set: --i-know)"                   },
    { "crown-mode",    cmd_crown_mode,    "crown-mode get|set <mode-name>"                 },
    { "user-data",     cmd_user_data,     "user-data send|listen --port N (--hex|--str)"   },
    { "scan-plan",     cmd_scan_plan,     "scan-plan get|set F=V"                          },
    { "ins-options",   cmd_ins_options,   "ins-options get|set F=V"                        },
    { "gps-feed",      cmd_gps_feed,      "send GpsData (0xD1) [--rate Hz for streaming]"  },
    { "heading-feed",  cmd_heading_feed,  "send ExtHeading (0xD2) --heading <rad>"         },
    { "autopilot-feed",cmd_autopilot_feed,"send AutopilotData (0x80) F=V"                  },
    { "geoid-feed",    cmd_geoid_feed,    "send GeoidUndulation (0xDC) --undulation <m>"   },
    { "retract",       cmd_retract,       "retract status|deploy|stow (move: --allow-motion --i-know)" },
    { "stare-ack",     cmd_stare_ack,     "ack OrionStareStart (0xD9) [--watch]"           },
    { "prf",           cmd_prf,           "prf get|set F=V"                                },
    { "raw",           cmd_raw,           "raw <pkt_id_hex> [hex_payload]"                 },
    { "listen",        cmd_listen,        "listen --id <hex> [--n N] [--since SEC]"        },
    { "cache",         cmd_cache,         "cache clear [--ip A]"                           },
    { "help",          cmd_help,          "print this help"                                },
    { NULL, NULL, NULL }
};

int octl_resolve_ip(octl_ctx_t *ctx)
{
    if (ctx->ip && *ctx->ip) return 0;
    const char *env = getenv("ORION_GIMBAL_IP");
    if (env && *env) { ctx->ip = env; return 0; }
    if (ctx->discover) { ctx->ip = "255.255.255.255"; return 0; }
    return -1;
}

static int parse_int(const char *s, int *out)
{
    if (!s) return -1;
    char *end;
    long v = strtol(s, &end, 10);
    if (*end != '\0' || end == s) return -1;
    *out = (int)v;
    return 0;
}

static int parse_dbl(const char *s, double *out)
{
    if (!s) return -1;
    char *end;
    double v = strtod(s, &end);
    if (*end != '\0' || end == s) return -1;
    *out = v;
    return 0;
}

static int parse_pair(const char *s, double out[2])
{
    if (!s) return -1;
    char *end;
    double a = strtod(s, &end);
    if (end == s || *end != ',') return -1;
    const char *t = end + 1;
    double b = strtod(t, &end);
    if (end == t || *end != '\0') return -1;
    out[0] = a;
    out[1] = b;
    return 0;
}

int octl_parse_global_flags(int argc, char **argv, octl_ctx_t *ctx)
{
    int i = 1;
    while (i < argc) {
        const char *a = argv[i];
        if (strcmp(a, "--ip") == 0 && i + 1 < argc) {
            ctx->ip = argv[++i];
        } else if (strcmp(a, "--timeout") == 0 && i + 1 < argc) {
            if (parse_int(argv[++i], &ctx->timeout_ms) != 0) return -1;
        } else if (strcmp(a, "--discover") == 0) {
            ctx->discover = 1;
        } else if (strcmp(a, "--allow-motion") == 0) {
            ctx->allow_motion = 1;
        } else if (strcmp(a, "--allow-laser") == 0) {
            ctx->allow_laser = 1;
        } else if (strcmp(a, "--i-know") == 0) {
            ctx->iknow = 1;
        } else if (strcmp(a, "--idx") == 0 && i + 1 < argc) {
            if (parse_int(argv[++i], &ctx->idx) != 0) return -1;
            ctx->idx_set = 1;
        } else if (strcmp(a, "--zoom") == 0 && i + 1 < argc) {
            if (parse_dbl(argv[++i], &ctx->zoom) != 0) return -1;
            ctx->zoom_set = 1;
        } else if (strcmp(a, "--watch") == 0) {
            ctx->watch = 1;
        } else if (strcmp(a, "--n") == 0 && i + 1 < argc) {
            if (parse_int(argv[++i], &ctx->watch_n) != 0) return -1;
        } else if (strcmp(a, "--since") == 0 && i + 1 < argc) {
            if (parse_int(argv[++i], &ctx->since_s) != 0) return -1;
            ctx->since_set = 1;
        } else if (strcmp(a, "--dx") == 0 && i + 1 < argc) {
            if (parse_dbl(argv[++i], &ctx->dx) != 0) return -1;
            ctx->dx_set = 1;
        } else if (strcmp(a, "--dy") == 0 && i + 1 < argc) {
            if (parse_dbl(argv[++i], &ctx->dy) != 0) return -1;
            ctx->dy_set = 1;
        } else if (strcmp(a, "--delta") == 0 && i + 1 < argc) {
            if (parse_dbl(argv[++i], &ctx->delta) != 0) return -1;
            ctx->delta_set = 1;
        } else if (strcmp(a, "--all") == 0) {
            ctx->all = 1;
        } else if (strcmp(a, "--filter") == 0 && i + 1 < argc) {
            ctx->filter = argv[++i];
        } else if (strcmp(a, "--target") == 0 && i + 1 < argc) {
            ctx->target = argv[++i];
        } else if (strcmp(a, "--ins") == 0) {
            ctx->tle_ins = 1;
        } else if (strcmp(a, "--dted") == 0) {
            ctx->tle_dted = 1;
        } else if (strcmp(a, "--lrf") == 0) {
            ctx->tle_lrf = 1;
        } else if (strcmp(a, "--offboard") == 0) {
            ctx->tle_offboard = 1;
        } else if (strcmp(a, "--key") == 0 && i + 1 < argc) {
            if (parse_int(argv[++i], &ctx->key) != 0) return -1;
            ctx->key_set = 1;
        } else if (strcmp(a, "--subkey") == 0 && i + 1 < argc) {
            if (parse_int(argv[++i], &ctx->subkey) != 0) return -1;
            ctx->subkey_set = 1;
        } else if (strcmp(a, "--value") == 0 && i + 1 < argc) {
            ctx->value = argv[++i];
        } else if (strcmp(a, "--value-hex") == 0 && i + 1 < argc) {
            ctx->value_hex = argv[++i];
        } else if (strcmp(a, "--port") == 0 && i + 1 < argc) {
            if (parse_int(argv[++i], &ctx->port) != 0) return -1;
            ctx->port_set = 1;
        } else if (strcmp(a, "--pan") == 0 && i + 1 < argc) {
            if (parse_dbl(argv[++i], &ctx->pan_rad) != 0) return -1;
            ctx->pan_set = 1;
        } else if (strcmp(a, "--tilt") == 0 && i + 1 < argc) {
            if (parse_dbl(argv[++i], &ctx->tilt_rad) != 0) return -1;
            ctx->tilt_set = 1;
        } else if (strcmp(a, "--pan-deg") == 0 && i + 1 < argc) {
            if (parse_dbl(argv[++i], &ctx->pan_deg) != 0) return -1;
            ctx->pan_deg_set = 1;
        } else if (strcmp(a, "--tilt-deg") == 0 && i + 1 < argc) {
            if (parse_dbl(argv[++i], &ctx->tilt_deg) != 0) return -1;
            ctx->tilt_deg_set = 1;
        } else if (strcmp(a, "--stab") == 0 && i + 1 < argc) {
            if (parse_int(argv[++i], &ctx->stab) != 0) return -1;
            ctx->stab_set = 1;
        } else if (strcmp(a, "--rate-x") == 0 && i + 1 < argc) {
            if (parse_dbl(argv[++i], &ctx->rate_x) != 0) return -1;
            ctx->rate_x_set = 1;
        } else if (strcmp(a, "--rate-y") == 0 && i + 1 < argc) {
            if (parse_dbl(argv[++i], &ctx->rate_y) != 0) return -1;
            ctx->rate_y_set = 1;
        } else if (strcmp(a, "--box-x") == 0 && i + 1 < argc) {
            if (parse_dbl(argv[++i], &ctx->box_x) != 0) return -1;
            ctx->box_x_set = 1;
        } else if (strcmp(a, "--box-y") == 0 && i + 1 < argc) {
            if (parse_dbl(argv[++i], &ctx->box_y) != 0) return -1;
            ctx->box_y_set = 1;
        } else if (strcmp(a, "--vel-n") == 0 && i + 1 < argc) {
            if (parse_dbl(argv[++i], &ctx->vel_n) != 0) return -1;
            ctx->vel_n_set = 1;
        } else if (strcmp(a, "--vel-e") == 0 && i + 1 < argc) {
            if (parse_dbl(argv[++i], &ctx->vel_e) != 0) return -1;
            ctx->vel_e_set = 1;
        } else if (strcmp(a, "--vel-d") == 0 && i + 1 < argc) {
            if (parse_dbl(argv[++i], &ctx->vel_d) != 0) return -1;
            ctx->vel_d_set = 1;
        } else if (strcmp(a, "--stare") == 0) {
            ctx->stare_flag = 1;
        } else if (strcmp(a, "--closure") == 0) {
            ctx->closure_flag = 1;
        } else if (strcmp(a, "--step-count") == 0 && i + 1 < argc) {
            if (parse_int(argv[++i], &ctx->step_count) != 0) return -1;
            ctx->step_count_set = 1;
        } else if (strcmp(a, "--axis") == 0 && i + 1 < argc) {
            ctx->axis = argv[++i];
        } else if (strcmp(a, "--max-current") == 0 && i + 1 < argc) {
            if (parse_dbl(argv[++i], &ctx->max_current) != 0) return -1;
            ctx->max_current_set = 1;
        } else if (strcmp(a, "--max-accel") == 0 && i + 1 < argc) {
            if (parse_dbl(argv[++i], &ctx->max_accel) != 0) return -1;
            ctx->max_accel_set = 1;
        } else if (strcmp(a, "--max-velocity") == 0 && i + 1 < argc) {
            if (parse_dbl(argv[++i], &ctx->max_velocity) != 0) return -1;
            ctx->max_velocity_set = 1;
        } else if (strcmp(a, "--min-pos") == 0 && i + 1 < argc) {
            if (parse_dbl(argv[++i], &ctx->min_pos) != 0) return -1;
            ctx->min_pos_set = 1;
        } else if (strcmp(a, "--max-pos") == 0 && i + 1 < argc) {
            if (parse_dbl(argv[++i], &ctx->max_pos) != 0) return -1;
            ctx->max_pos_set = 1;
        } else if (strcmp(a, "--mode") == 0 && i + 1 < argc) {
            ctx->mode_name = argv[++i];
        } else if (strcmp(a, "--baud") == 0 && i + 1 < argc) {
            if (parse_int(argv[++i], &ctx->baud) != 0) return -1;
            ctx->baud_set = 1;
        } else if (strcmp(a, "--protocol") == 0 && i + 1 < argc) {
            ctx->protocol = argv[++i];
        } else if (strcmp(a, "--netmask") == 0 && i + 1 < argc) {
            ctx->netmask = argv[++i];
        } else if (strcmp(a, "--gateway") == 0 && i + 1 < argc) {
            ctx->gateway = argv[++i];
        } else if (strcmp(a, "--hex") == 0 && i + 1 < argc) {
            ctx->hex_arg = argv[++i];
        } else if (strcmp(a, "--str") == 0 && i + 1 < argc) {
            ctx->str_arg = argv[++i];
        } else if (strcmp(a, "--id") == 0 && i + 1 < argc) {
            ctx->id_arg = argv[++i];
        } else if (strcmp(a, "--rate") == 0 && i + 1 < argc) {
            if (parse_dbl(argv[++i], &ctx->rate_hz) != 0) return -1;
            ctx->rate_hz_set = 1;
        } else if (strcmp(a, "--lat") == 0 && i + 1 < argc) {
            if (parse_dbl(argv[++i], &ctx->lat) != 0) return -1;
            ctx->lla_set |= 1;
        } else if (strcmp(a, "--lon") == 0 && i + 1 < argc) {
            if (parse_dbl(argv[++i], &ctx->lon) != 0) return -1;
            ctx->lla_set |= 2;
        } else if (strcmp(a, "--alt") == 0 && i + 1 < argc) {
            if (parse_dbl(argv[++i], &ctx->alt) != 0) return -1;
            ctx->lla_set |= 4;
        } else if (strcmp(a, "--heading") == 0 && i + 1 < argc) {
            if (parse_dbl(argv[++i], &ctx->heading) != 0) return -1;
            ctx->heading_set = 1;
        } else if (strcmp(a, "--accuracy") == 0 && i + 1 < argc) {
            if (parse_dbl(argv[++i], &ctx->heading_acc) != 0) return -1;
            ctx->heading_acc_set = 1;
        } else if (strcmp(a, "--undulation") == 0 && i + 1 < argc) {
            if (parse_dbl(argv[++i], &ctx->undulation) != 0) return -1;
            ctx->undulation_set = 1;
        } else if (strcmp(a, "--source") == 0 && i + 1 < argc) {
            ctx->gps_source = argv[++i];
        } else if (strcmp(a, "--deg") == 0 && i + 1 < argc) {
            if (parse_dbl(argv[++i], &ctx->deg_value) != 0) return -1;
            ctx->deg_set = 1;
        } else if (strcmp(a, "--persist") == 0) {
            ctx->persist = 1;
        } else if (strcmp(a, "--type") == 0 && i + 1 < argc) {
            ctx->set_type = argv[++i];
        } else if (strcmp(a, "--proto") == 0 && i + 1 < argc) {
            ctx->set_proto = argv[++i];
        } else if (strcmp(a, "--gpio") == 0 && i + 1 < argc) {
            if (parse_int(argv[++i], &ctx->set_gpio) != 0) return -1;
            ctx->set_gpio_set = 1;
        } else if (strcmp(a, "--gpio-active") == 0 && i + 1 < argc) {
            if (parse_int(argv[++i], &ctx->set_gpio_active) != 0) return -1;
            ctx->set_gpio_active_set = 1;
        } else if (strcmp(a, "--min-focal-mm") == 0 && i + 1 < argc) {
            if (parse_dbl(argv[++i], &ctx->set_min_focal) != 0) return -1;
            ctx->set_min_focal_set = 1;
        } else if (strcmp(a, "--max-focal-mm") == 0 && i + 1 < argc) {
            if (parse_dbl(argv[++i], &ctx->set_max_focal) != 0) return -1;
            ctx->set_max_focal_set = 1;
        } else if (strcmp(a, "--pixel-pitch-mm") == 0 && i + 1 < argc) {
            if (parse_dbl(argv[++i], &ctx->set_pixel_pitch) != 0) return -1;
            ctx->set_pixel_pitch_set = 1;
        } else if (strcmp(a, "--array-w") == 0 && i + 1 < argc) {
            if (parse_int(argv[++i], &ctx->set_array_w) != 0) return -1;
            ctx->set_array_w_set = 1;
        } else if (strcmp(a, "--array-h") == 0 && i + 1 < argc) {
            if (parse_int(argv[++i], &ctx->set_array_h) != 0) return -1;
            ctx->set_array_h_set = 1;
        } else if (strcmp(a, "--align-min-rad") == 0 && i + 1 < argc) {
            if (parse_pair(argv[++i], ctx->set_align_min) != 0) return -1;
            ctx->set_align_min_set = 1;
        } else if (strcmp(a, "--align-max-rad") == 0 && i + 1 < argc) {
            if (parse_pair(argv[++i], ctx->set_align_max) != 0) return -1;
            ctx->set_align_max_set = 1;
        } else if (strcmp(a, "--align-min-deg") == 0 && i + 1 < argc) {
            if (parse_pair(argv[++i], ctx->set_align_min) != 0) return -1;
            ctx->set_align_min[0] *= (M_PI / 180.0);
            ctx->set_align_min[1] *= (M_PI / 180.0);
            ctx->set_align_min_set = 1;
        } else if (strcmp(a, "--align-max-deg") == 0 && i + 1 < argc) {
            if (parse_pair(argv[++i], ctx->set_align_max) != 0) return -1;
            ctx->set_align_max[0] *= (M_PI / 180.0);
            ctx->set_align_max[1] *= (M_PI / 180.0);
            ctx->set_align_max_set = 1;
        } else if (strcmp(a, "--via-state") == 0) {
            ctx->via_state = 1;
        } else if (strcmp(a, "--wait-active") == 0) {
            ctx->wait_active = 1;
        } else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            if (ctx->npos < OCTL_MAX_POS) ctx->pos[ctx->npos++] = "help";
        } else if (strncmp(a, "--", 2) == 0) {
            return -1;
        } else {
            if (ctx->npos < OCTL_MAX_POS) ctx->pos[ctx->npos++] = a;
        }
        i++;
    }
    return 0;
}

static int cmd_help(octl_ctx_t *ctx)
{
    (void)ctx;
    jout_t j;
    jout_init(&j, stdout);
    jout_obj_open(&j);
    jout_kv_str(&j, "tool", "orionctl");
    jout_kv_str(&j, "version", "0.2.0-phase2");

    jout_key(&j, "global_flags");
    jout_arr_open(&j);
    jout_str(&j, "--ip <addr>");
    jout_str(&j, "--timeout <ms>");
    jout_str(&j, "--discover");
    jout_str(&j, "--allow-motion");
    jout_str(&j, "--allow-laser");
    jout_str(&j, "--i-know");
    jout_str(&j, "--idx <N>");
    jout_str(&j, "--zoom <N>");
    jout_str(&j, "--watch");
    jout_str(&j, "--n <N>");
    jout_str(&j, "--since <SEC>");
    jout_str(&j, "--deg <N>");
    jout_str(&j, "--persist");
    jout_str(&j, "--type <name>");
    jout_str(&j, "--proto <name>");
    jout_str(&j, "--gpio <N>");
    jout_str(&j, "--gpio-active <0|1>");
    jout_str(&j, "--min-focal-mm <F>");
    jout_str(&j, "--max-focal-mm <F>");
    jout_str(&j, "--pixel-pitch-mm <F>");
    jout_str(&j, "--array-w <N>");
    jout_str(&j, "--array-h <N>");
    jout_str(&j, "--align-min-rad PAN,TILT");
    jout_str(&j, "--align-max-rad PAN,TILT");
    jout_str(&j, "--align-min-deg PAN,TILT");
    jout_str(&j, "--align-max-deg PAN,TILT");
    jout_str(&j, "--via-state");
    jout_str(&j, "--wait-active");
    jout_str(&j, "--dx <pct>");
    jout_str(&j, "--dy <pct>");
    jout_str(&j, "--delta <pct>");
    jout_str(&j, "--all");
    jout_str(&j, "--filter <name>");
    jout_str(&j, "--target <name>");
    jout_str(&j, "--ins");
    jout_str(&j, "--dted");
    jout_str(&j, "--lrf");
    jout_str(&j, "--offboard");
    jout_str(&j, "--key <N>");
    jout_str(&j, "--subkey <N>");
    jout_str(&j, "--value <str>");
    jout_str(&j, "--value-hex <hexstr>");
    jout_str(&j, "--port <N>");
    jout_arr_close(&j);

    jout_key(&j, "verbs");
    jout_arr_open(&j);
    for (const verb_t *v = VERBS; v->name; v++) {
        jout_obj_open(&j);
        jout_kv_str(&j, "name", v->name);
        jout_kv_str(&j, "help", v->help);
        jout_obj_close(&j);
    }
    jout_arr_close(&j);

    jout_obj_close(&j);
    jout_done(&j);
    return OCTL_OK;
}

int main(int argc, char **argv)
{
    octl_ctx_t ctx = {0};
    ctx.timeout_ms = OCTL_DEFAULT_TIMEOUT_MS;
    ctx.idx = -1;
    ctx.argc = argc;
    ctx.argv = argv;

    if (octl_parse_global_flags(argc, argv, &ctx) != 0) {
        jout_err(stderr, OCTL_USAGE, "bad_flag", "unknown or malformed flag");
        return OCTL_USAGE;
    }
    if (ctx.npos == 0) {
        return cmd_help(&ctx);
    }

    const char *verb = ctx.pos[0];
    for (const verb_t *v = VERBS; v->name; v++) {
        if (strcmp(verb, v->name) == 0) {
            return v->fn(&ctx);
        }
    }

    jout_err(stderr, OCTL_USAGE, "unknown_verb", "unknown verb: %s", verb);
    return OCTL_USAGE;
}
