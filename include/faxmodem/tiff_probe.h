/* Pre-flight checks on the TIFF we are about to feed to T.30, so a bad input
 * fails immediately with a clear message instead of halfway through a call. */
#ifndef FAXMODEM_TIFF_PROBE_H
#define FAXMODEM_TIFF_PROBE_H

#include <stdbool.h>
#include <stddef.h>

typedef struct
{
    int pages;
    int width;
    int height;
    int bits_per_sample;
    int samples_per_pixel;
    int compression;
    float x_resolution;
    float y_resolution;
    int resolution_unit;
    bool bilevel;
    bool warned;           /* something is odd but we will still try */
    char warning[256];
    char compression_name[32];
} fm_tiff_info_t;

/* Returns false only when the file is unusable as a fax. */
bool fm_tiff_probe(const char *path, fm_tiff_info_t *info, char *err, size_t err_len);

void fm_tiff_log(const fm_tiff_info_t *info, const char *path);

#endif /* FAXMODEM_TIFF_PROBE_H */
