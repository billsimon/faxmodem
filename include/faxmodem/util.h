#ifndef FAXMODEM_UTIL_H
#define FAXMODEM_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* mkdir -p. Returns false and fills err on failure. */
bool fm_mkdir_p(const char *path, char *err, size_t err_len);

void fm_path_join(char *out, size_t out_len, const char *dir, const char *name);

/* Monotonic milliseconds, for deadlines. */
int64_t fm_now_ms(void);

/* "20260914T114903Z", for generated filenames. */
void fm_timestamp_compact(char *out, size_t out_len);

/* Strips a directory and any extension: "/a/b/c.tif" -> "c". */
void fm_basename_stem(const char *path, char *out, size_t out_len);

bool fm_file_exists(const char *path);

/* Replaces characters that have no business in a filename. */
void fm_sanitise_filename(char *s);

#endif /* FAXMODEM_UTIL_H */
