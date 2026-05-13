#ifndef ORIONCTL_CACHE_H
#define ORIONCTL_CACHE_H

#include <stdio.h>

int   cache_ensure_dir(void);
FILE *cache_cameras_open_write(const char *ip);
int   cache_cameras_commit(FILE *f, const char *ip);
int   cache_cameras_abort(FILE *f, const char *ip);
int   cache_clear(const char *ip);

int   cache_cameras_tmp_path(const char *ip, char *out, size_t outsz);
int   cache_cameras_final_path(const char *ip, char *out, size_t outsz);

int   cache_cameras_bin_write(const char *ip, const void *buf, size_t len);
long  cache_cameras_bin_read(const char *ip, void *buf, size_t maxlen);

#endif
