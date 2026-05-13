#include "cache.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static int cache_dir(char *out, size_t outsz)
{
    const char *xdg = getenv("XDG_CACHE_HOME");
    if (xdg && *xdg) {
        snprintf(out, outsz, "%s/orionctl", xdg);
        return 0;
    }
    const char *home = getenv("HOME");
    if (!home || !*home) return -1;
    snprintf(out, outsz, "%s/.cache/orionctl", home);
    return 0;
}

static void sanitize_ip(const char *ip, char *out, size_t outsz)
{
    size_t j = 0;
    for (size_t i = 0; ip[i] && j + 1 < outsz; i++) {
        char c = ip[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '.' || c == ':' || c == '-' || c == '_') {
            out[j++] = c;
        } else {
            out[j++] = '_';
        }
    }
    out[j] = 0;
}

int cache_ensure_dir(void)
{
    char dir[512];
    if (cache_dir(dir, sizeof(dir)) != 0) return -1;

    char *p = dir;
    if (*p == '/') p++;
    for (; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (mkdir(dir, 0700) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(dir, 0700) != 0 && errno != EEXIST) return -1;
    return 0;
}

int cache_cameras_final_path(const char *ip, char *out, size_t outsz)
{
    char dir[512], sip[128];
    if (cache_dir(dir, sizeof(dir)) != 0) return -1;
    sanitize_ip(ip, sip, sizeof(sip));
    snprintf(out, outsz, "%s/cameras-%s.json", dir, sip);
    return 0;
}

int cache_cameras_tmp_path(const char *ip, char *out, size_t outsz)
{
    char dir[512], sip[128];
    if (cache_dir(dir, sizeof(dir)) != 0) return -1;
    sanitize_ip(ip, sip, sizeof(sip));
    snprintf(out, outsz, "%s/cameras-%s.json.tmp.%d", dir, sip, (int)getpid());
    return 0;
}

FILE *cache_cameras_open_write(const char *ip)
{
    if (cache_ensure_dir() != 0) return NULL;
    char tmp[768];
    if (cache_cameras_tmp_path(ip, tmp, sizeof(tmp)) != 0) return NULL;
    return fopen(tmp, "w");
}

int cache_cameras_commit(FILE *f, const char *ip)
{
    if (!f) return -1;
    fflush(f);
    fclose(f);
    char tmp[768], final[768];
    if (cache_cameras_tmp_path(ip, tmp, sizeof(tmp)) != 0) return -1;
    if (cache_cameras_final_path(ip, final, sizeof(final)) != 0) return -1;
    if (rename(tmp, final) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

int cache_cameras_abort(FILE *f, const char *ip)
{
    if (f) fclose(f);
    char tmp[768];
    if (cache_cameras_tmp_path(ip, tmp, sizeof(tmp)) != 0) return -1;
    unlink(tmp);
    return 0;
}

static int bin_final_path(const char *ip, char *out, size_t outsz)
{
    char dir[512], sip[128];
    if (cache_dir(dir, sizeof(dir)) != 0) return -1;
    sanitize_ip(ip, sip, sizeof(sip));
    snprintf(out, outsz, "%s/cameras-%s.bin", dir, sip);
    return 0;
}

static int bin_tmp_path(const char *ip, char *out, size_t outsz)
{
    char dir[512], sip[128];
    if (cache_dir(dir, sizeof(dir)) != 0) return -1;
    sanitize_ip(ip, sip, sizeof(sip));
    snprintf(out, outsz, "%s/cameras-%s.bin.tmp.%d", dir, sip, (int)getpid());
    return 0;
}

int cache_cameras_bin_write(const char *ip, const void *buf, size_t len)
{
    if (cache_ensure_dir() != 0) return -1;
    char tmp[768], final[768];
    if (bin_tmp_path(ip, tmp, sizeof(tmp)) != 0) return -1;
    if (bin_final_path(ip, final, sizeof(final)) != 0) return -1;
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    size_t w = fwrite(buf, 1, len, f);
    if (fclose(f) != 0 || w != len) { unlink(tmp); return -1; }
    if (rename(tmp, final) != 0) { unlink(tmp); return -1; }
    return 0;
}

long cache_cameras_bin_read(const char *ip, void *buf, size_t maxlen)
{
    char final[768];
    if (bin_final_path(ip, final, sizeof(final)) != 0) return -1;
    FILE *f = fopen(final, "rb");
    if (!f) return -1;
    size_t n = fread(buf, 1, maxlen, f);
    fclose(f);
    return (long)n;
}

int cache_clear(const char *ip)
{
    if (ip && *ip) {
        char path[768];
        if (cache_cameras_final_path(ip, path, sizeof(path)) == 0)
            if (unlink(path) != 0 && errno != ENOENT) return -1;
        if (bin_final_path(ip, path, sizeof(path)) == 0)
            if (unlink(path) != 0 && errno != ENOENT) return -1;
        return 0;
    }
    char dir[512];
    if (cache_dir(dir, sizeof(dir)) != 0) return -1;
    DIR *d = opendir(dir);
    if (!d) return errno == ENOENT ? 0 : -1;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (strncmp(de->d_name, "cameras-", 8) != 0) continue;
        char p[1024];
        snprintf(p, sizeof(p), "%s/%s", dir, de->d_name);
        unlink(p);
    }
    closedir(d);
    return 0;
}
