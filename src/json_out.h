#ifndef ORIONCTL_JSON_OUT_H
#define ORIONCTL_JSON_OUT_H

#include <stdio.h>

#define JOUT_MAX_DEPTH 16

typedef struct {
    FILE *f;
    int   depth;
    int   first[JOUT_MAX_DEPTH];
    int   is_obj[JOUT_MAX_DEPTH];
} jout_t;

void jout_init(jout_t *j, FILE *f);
void jout_done(jout_t *j);

void jout_obj_open(jout_t *j);
void jout_obj_close(jout_t *j);
void jout_arr_open(jout_t *j);
void jout_arr_close(jout_t *j);

void jout_key(jout_t *j, const char *k);
void jout_str(jout_t *j, const char *s);
void jout_int(jout_t *j, long long v);
void jout_uint(jout_t *j, unsigned long long v);
void jout_dbl(jout_t *j, double v);
void jout_bool(jout_t *j, int v);
void jout_null(jout_t *j);
void jout_raw(jout_t *j, const char *raw);

void jout_kv_str(jout_t *j, const char *k, const char *s);
void jout_kv_int(jout_t *j, const char *k, long long v);
void jout_kv_uint(jout_t *j, const char *k, unsigned long long v);
void jout_kv_dbl(jout_t *j, const char *k, double v);
void jout_kv_bool(jout_t *j, const char *k, int v);
void jout_kv_null(jout_t *j, const char *k);

void jout_err(FILE *err, int exit_code, const char *err_id, const char *fmt, ...);

#endif
