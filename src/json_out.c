#include "json_out.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static void emit_sep(jout_t *j)
{
    if (j->depth == 0) return;
    int top = j->depth - 1;
    if (j->first[top]) {
        j->first[top] = 0;
    } else {
        fputc(',', j->f);
    }
}

static void emit_str(FILE *f, const char *s)
{
    fputc('"', f);
    if (!s) { fputs("\"", f); return; }
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        unsigned char c = *p;
        switch (c) {
            case '"':  fputs("\\\"", f); break;
            case '\\': fputs("\\\\", f); break;
            case '\b': fputs("\\b", f);  break;
            case '\f': fputs("\\f", f);  break;
            case '\n': fputs("\\n", f);  break;
            case '\r': fputs("\\r", f);  break;
            case '\t': fputs("\\t", f);  break;
            default:
                if (c < 0x20) fprintf(f, "\\u%04x", c);
                else fputc(c, f);
        }
    }
    fputc('"', f);
}

void jout_init(jout_t *j, FILE *f)
{
    j->f = f;
    j->depth = 0;
    memset(j->first, 0, sizeof(j->first));
    memset(j->is_obj, 0, sizeof(j->is_obj));
}

void jout_done(jout_t *j)
{
    fputc('\n', j->f);
    fflush(j->f);
}

void jout_obj_open(jout_t *j)
{
    emit_sep(j);
    fputc('{', j->f);
    if (j->depth < JOUT_MAX_DEPTH) {
        j->first[j->depth] = 1;
        j->is_obj[j->depth] = 1;
        j->depth++;
    }
}

void jout_obj_close(jout_t *j)
{
    if (j->depth > 0) j->depth--;
    fputc('}', j->f);
}

void jout_arr_open(jout_t *j)
{
    emit_sep(j);
    fputc('[', j->f);
    if (j->depth < JOUT_MAX_DEPTH) {
        j->first[j->depth] = 1;
        j->is_obj[j->depth] = 0;
        j->depth++;
    }
}

void jout_arr_close(jout_t *j)
{
    if (j->depth > 0) j->depth--;
    fputc(']', j->f);
}

void jout_key(jout_t *j, const char *k)
{
    emit_sep(j);
    emit_str(j->f, k);
    fputc(':', j->f);
    if (j->depth > 0) j->first[j->depth - 1] = 1;
}

void jout_str(jout_t *j, const char *s)
{
    emit_sep(j);
    if (!s) fputs("null", j->f);
    else emit_str(j->f, s);
}

void jout_int(jout_t *j, long long v)
{
    emit_sep(j);
    fprintf(j->f, "%lld", v);
}

void jout_uint(jout_t *j, unsigned long long v)
{
    emit_sep(j);
    fprintf(j->f, "%llu", v);
}

void jout_dbl(jout_t *j, double v)
{
    emit_sep(j);
    if (isnan(v) || isinf(v)) {
        fputs("null", j->f);
    } else {
        fprintf(j->f, "%.10g", v);
    }
}

void jout_bool(jout_t *j, int v)
{
    emit_sep(j);
    fputs(v ? "true" : "false", j->f);
}

void jout_null(jout_t *j)
{
    emit_sep(j);
    fputs("null", j->f);
}

void jout_raw(jout_t *j, const char *raw)
{
    emit_sep(j);
    fputs(raw, j->f);
}

void jout_kv_str(jout_t *j, const char *k, const char *s)  { jout_key(j, k); jout_str(j, s); }
void jout_kv_int(jout_t *j, const char *k, long long v)     { jout_key(j, k); jout_int(j, v); }
void jout_kv_uint(jout_t *j, const char *k, unsigned long long v) { jout_key(j, k); jout_uint(j, v); }
void jout_kv_dbl(jout_t *j, const char *k, double v)        { jout_key(j, k); jout_dbl(j, v); }
void jout_kv_bool(jout_t *j, const char *k, int v)          { jout_key(j, k); jout_bool(j, v); }
void jout_kv_null(jout_t *j, const char *k)                 { jout_key(j, k); jout_null(j); }

void jout_err(FILE *err, int exit_code, const char *err_id, const char *fmt, ...)
{
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    jout_t j;
    jout_init(&j, err);
    jout_obj_open(&j);
    jout_kv_str(&j, "error", err_id);
    jout_kv_str(&j, "message", msg);
    jout_kv_int(&j, "exit_code", exit_code);
    jout_obj_close(&j);
    jout_done(&j);
}
