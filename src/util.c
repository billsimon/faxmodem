#include "faxmodem/util.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

bool fm_mkdir_p(const char *path, char *err, size_t err_len)
{
    char buf[1024];
    size_t len;

    if (path == NULL || *path == '\0')
    {
        snprintf(err, err_len, "empty path");
        return false;
    }
    len = strlen(path);
    if (len >= sizeof(buf))
    {
        snprintf(err, err_len, "path too long: %s", path);
        return false;
    }
    memcpy(buf, path, len + 1);
    if (buf[len - 1] == '/')
        buf[len - 1] = '\0';

    for (char *p = buf + 1; *p != '\0'; p++)
    {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(buf, 0755) != 0 && errno != EEXIST)
        {
            snprintf(err, err_len, "mkdir %s: %s", buf, strerror(errno));
            return false;
        }
        *p = '/';
    }
    if (mkdir(buf, 0755) != 0 && errno != EEXIST)
    {
        snprintf(err, err_len, "mkdir %s: %s", buf, strerror(errno));
        return false;
    }
    return true;
}

void fm_path_join(char *out, size_t out_len, const char *dir, const char *name)
{
    size_t n = strlen(dir);
    if (n > 0 && dir[n - 1] == '/')
        snprintf(out, out_len, "%s%s", dir, name);
    else
        snprintf(out, out_len, "%s/%s", dir, name);
}

int64_t fm_now_ms(void)
{
    struct timespec ts;
#if defined(CLOCK_MONOTONIC)
    clock_gettime(CLOCK_MONOTONIC, &ts);
#else
    clock_gettime(CLOCK_REALTIME, &ts);
#endif
    return (int64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void fm_timestamp_compact(char *out, size_t out_len)
{
    struct timeval tv;
    struct tm tm;
    char base[32];

    gettimeofday(&tv, NULL);
    gmtime_r(&tv.tv_sec, &tm);
    strftime(base, sizeof(base), "%Y%m%dT%H%M%S", &tm);
    snprintf(out, out_len, "%s%03dZ", base, (int) (tv.tv_usec / 1000));
}

void fm_basename_stem(const char *path, char *out, size_t out_len)
{
    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;
    const char *dot = strrchr(base, '.');
    size_t n = dot ? (size_t) (dot - base) : strlen(base);

    if (n >= out_len)
        n = out_len - 1;
    memcpy(out, base, n);
    out[n] = '\0';
}

bool fm_file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

void fm_sanitise_filename(char *s)
{
    for (char *p = s; *p != '\0'; p++)
    {
        unsigned char c = (unsigned char) *p;
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
                  c == '-' || c == '_' || c == '+';
        if (!ok)
            *p = '_';
    }
}
