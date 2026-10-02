#include "faxmodem/tiff_probe.h"
#include "faxmodem/log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <tiffio.h>

static const char *compression_name(int c)
{
    switch (c)
    {
    case COMPRESSION_NONE:
        return "none";
    case COMPRESSION_CCITTRLE:
        return "ccitt-rle";
    case COMPRESSION_CCITTFAX3:
        return "g3";
    case COMPRESSION_CCITTFAX4:
        return "g4";
    case COMPRESSION_LZW:
        return "lzw";
    case COMPRESSION_JPEG:
        return "jpeg";
    case COMPRESSION_PACKBITS:
        return "packbits";
    case COMPRESSION_DEFLATE:
    case COMPRESSION_ADOBE_DEFLATE:
        return "deflate";
    default:
        return "other";
    }
}

/* libtiff shouts on stderr by default; route it through our stdout logger. */
static void tiff_error(const char *module, const char *fmt, va_list ap)
{
    char msg[512];
    vsnprintf(msg, sizeof(msg), fmt, ap);
    FM_WARN("tiff", "%s: %s", module ? module : "libtiff", msg);
}

static void tiff_warning(const char *module, const char *fmt, va_list ap)
{
    char msg[512];
    vsnprintf(msg, sizeof(msg), fmt, ap);
    FM_DEBUG("tiff", "%s: %s", module ? module : "libtiff", msg);
}

bool fm_tiff_probe(const char *path, fm_tiff_info_t *info, char *err, size_t err_len)
{
    TIFF *tif;
    uint32_t width = 0;
    uint32_t height = 0;
    uint16_t bps = 1;
    uint16_t spp = 1;
    uint16_t compression = 0;
    uint16_t res_unit = RESUNIT_INCH;
    float xres = 0.0f;
    float yres = 0.0f;

    memset(info, 0, sizeof(*info));

    TIFFSetErrorHandler(tiff_error);
    TIFFSetWarningHandler(tiff_warning);

    tif = TIFFOpen(path, "r");
    if (tif == NULL)
    {
        snprintf(err, err_len, "cannot open '%s' as a TIFF", path);
        return false;
    }

    do
    {
        info->pages++;
        if (info->pages > 1)
            continue; /* the first page's geometry is what we report */

        TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &width);
        TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &height);
        TIFFGetFieldDefaulted(tif, TIFFTAG_BITSPERSAMPLE, &bps);
        TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLESPERPIXEL, &spp);
        TIFFGetFieldDefaulted(tif, TIFFTAG_COMPRESSION, &compression);
        TIFFGetFieldDefaulted(tif, TIFFTAG_RESOLUTIONUNIT, &res_unit);
        TIFFGetField(tif, TIFFTAG_XRESOLUTION, &xres);
        TIFFGetField(tif, TIFFTAG_YRESOLUTION, &yres);
    } while (TIFFReadDirectory(tif));

    TIFFClose(tif);

    info->width = (int) width;
    info->height = (int) height;
    info->bits_per_sample = bps;
    info->samples_per_pixel = spp;
    info->compression = compression;
    info->x_resolution = xres;
    info->y_resolution = yres;
    info->resolution_unit = res_unit;
    info->bilevel = (bps == 1 && spp == 1);
    snprintf(info->compression_name, sizeof(info->compression_name), "%s", compression_name(compression));

    if (info->pages == 0)
    {
        snprintf(err, err_len, "'%s' contains no pages", path);
        return false;
    }
    if (!info->bilevel)
    {
        snprintf(err, err_len,
                 "'%s' is %d bits/sample x %d samples/pixel; fax needs a bilevel (1 bit) image. "
                 "Convert it, e.g.: gs -q -dNOPAUSE -dBATCH -sDEVICE=tiffg4 -r204x196 "
                 "-sOutputFile=out.tif in.pdf",
                 path, bps, spp);
        return false;
    }
    if (width != 1728 && width != 2048 && width != 2432)
    {
        info->warned = true;
        snprintf(info->warning, sizeof(info->warning),
                 "page width %u is not a standard fax width (1728/2048/2432); spandsp will rescale",
                 width);
    }
    return true;
}

void fm_tiff_log(const fm_tiff_info_t *info, const char *path)
{
    fm_log_event(FM_LOG_INFO, "tiff", "document",
                 "path=\"%s\" pages=%d width=%d height=%d compression=%s x_res=%.0f y_res=%.0f", path,
                 info->pages, info->width, info->height, info->compression_name, (double) info->x_resolution,
                 (double) info->y_resolution);
    if (info->warned)
        FM_WARN("tiff", "%s", info->warning);
}
