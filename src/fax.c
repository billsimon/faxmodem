#include "faxmodem/fax.h"
#include "faxmodem/log.h"
#include "faxmodem/tiff_probe.h"
#include "faxmodem/util.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <spandsp.h>

#define SELFTEST_CHUNK 160                 /* 20 ms at 8 kHz */
#define SELFTEST_MAX_SECONDS (30 * 60)

struct fm_fax
{
    fax_state_t *fax;
    pthread_mutex_t lock;
    bool completed;
    int result;
    char tag[64];
    bool calling;
    int64_t last_rx_frame_ms;   /* last T.30 frame received from the far end */
    int64_t last_advance_ms;    /* last time pages or image bytes grew */
    int seen_pages;
    int seen_image_size;
    /* Refreshed from the spandsp callbacks, which already run under lock. */
    fm_fax_status_t status;
};

static int g_spandsp_level = SPAN_LOG_WARNING;

void fm_fax_init_logging(const fm_config_t *cfg)
{
    if (cfg->spandsp_log_level >= 0)
    {
        g_spandsp_level = cfg->spandsp_log_level;
    }
    else
    {
        switch (cfg->log_level)
        {
        case FM_LOG_ERROR:
            g_spandsp_level = SPAN_LOG_ERROR;
            break;
        case FM_LOG_WARN:
            g_spandsp_level = SPAN_LOG_WARNING;
            break;
        case FM_LOG_INFO:
            g_spandsp_level = SPAN_LOG_PROTOCOL_WARNING;
            break;
        case FM_LOG_DEBUG:
            g_spandsp_level = SPAN_LOG_FLOW_2;
            break;
        default:
            g_spandsp_level = SPAN_LOG_DEBUG_2;
            break;
        }
    }
    span_set_message_handler(fm_log_spandsp_message);
    span_set_error_handler(fm_log_spandsp_error);
}

static void attach_logging(logging_state_t *lg, const char *tag)
{
    if (lg == NULL)
        return;
    span_log_set_message_handler(lg, fm_log_spandsp_message);
    span_log_set_error_handler(lg, fm_log_spandsp_error);
    span_log_set_level(lg, SPAN_LOG_SHOW_SEVERITY | SPAN_LOG_SHOW_PROTOCOL | SPAN_LOG_SHOW_TAG |
                               g_spandsp_level);
    if (tag != NULL && *tag != '\0')
        span_log_set_tag(lg, tag);
}

static int modems_for_speed(int max_speed)
{
    if (max_speed >= 14400)
        return T30_SUPPORT_V27TER | T30_SUPPORT_V29 | T30_SUPPORT_V17;
    if (max_speed >= 7200)
        return T30_SUPPORT_V27TER | T30_SUPPORT_V29;
    return T30_SUPPORT_V27TER;
}

/* Called from inside fax_rx()/fax_tx(), i.e. already under s->lock. */
static void refresh_status(struct fm_fax *s)
{
    t30_state_t *t30 = fax_get_t30_state(s->fax);
    t30_stats_t stats;
    const char *ident;

    t30_get_transfer_statistics(t30, &stats);
    s->status.pages_tx = stats.pages_tx;
    s->status.pages_rx = stats.pages_rx;
    s->status.bit_rate = stats.bit_rate;
    s->status.ecm = stats.error_correcting_mode != 0;
    s->status.bad_rows = stats.bad_rows;

    ident = t30_get_rx_ident(t30);
    if (ident != NULL)
        snprintf(s->status.remote_ident, sizeof(s->status.remote_ident), "%s", ident);
}

static int phase_b_handler(t30_state_t *t30, void *user_data, int result)
{
    struct fm_fax *s = user_data;

    refresh_status(s);
    fm_log_event(FM_LOG_INFO, "fax", "negotiating", "tag=%s remote_id=\"%s\" frame=%s", s->tag,
                 s->status.remote_ident, t30_frametype((uint8_t) result));
    return T30_ERR_OK;
}

static int phase_d_handler(t30_state_t *t30, void *user_data, int result)
{
    struct fm_fax *s = user_data;

    refresh_status(s);
    fm_log_event(FM_LOG_INFO, "fax", "page complete",
                 "tag=%s pages_tx=%d pages_rx=%d bit_rate=%d ecm=%s bad_rows=%d frame=%s", s->tag,
                 s->status.pages_tx, s->status.pages_rx, s->status.bit_rate, s->status.ecm ? "yes" : "no",
                 s->status.bad_rows, t30_frametype((uint8_t) result));
    return T30_ERR_OK;
}

static void phase_e_handler(t30_state_t *t30, void *user_data, int completion_code)
{
    struct fm_fax *s = user_data;

    refresh_status(s);
    s->result = completion_code;
    s->status.result = completion_code;
    s->status.result_text = t30_completion_code_to_str(completion_code);
    s->status.completed = true;
    s->completed = true;

    fm_log_event(completion_code == T30_ERR_OK ? FM_LOG_INFO : FM_LOG_ERROR, "fax", "transfer finished",
                 "tag=%s result=%d result_text=\"%s\" pages_tx=%d pages_rx=%d bit_rate=%d ecm=%s "
                 "remote_id=\"%s\"",
                 s->tag, completion_code, s->status.result_text, s->status.pages_tx, s->status.pages_rx,
                 s->status.bit_rate, s->status.ecm ? "yes" : "no", s->status.remote_ident);
}

/* Only frames arriving from the far end count. Page completions alone are too
 * coarse - a single page over a slow line with ECM retransmissions can run for
 * minutes without one - but counting our own output would be worse: a far end
 * that has stopped listening still makes us answer it, and those answers must
 * not look like liveness. */
static void real_time_frame_handler(t30_state_t *t30, void *user_data, int direction, const uint8_t *msg,
                                    int len)
{
    struct fm_fax *s = user_data;

    (void) t30;
    (void) msg;
    (void) len;
    if (direction)
        s->last_rx_frame_ms = fm_now_ms();
}

static void configure_t30(struct fm_fax *s, const fm_fax_params_t *p)
{
    t30_state_t *t30 = fax_get_t30_state(s->fax);

    if (p->station_id != NULL && *p->station_id != '\0')
        t30_set_tx_ident(t30, p->station_id);
    if (p->header != NULL && *p->header != '\0')
        t30_set_tx_page_header_info(t30, p->header);

    t30_set_ecm_capability(t30, p->ecm ? TRUE : FALSE);
    t30_set_supported_compressions(t30, T30_SUPPORT_T4_1D_COMPRESSION | T30_SUPPORT_T4_2D_COMPRESSION |
                                            T30_SUPPORT_T6_COMPRESSION);
    t30_set_supported_modems(t30, modems_for_speed(p->max_speed));
    t30_set_supported_resolutions(t30, (p->fine_only ? 0 : T30_SUPPORT_STANDARD_RESOLUTION) |
                                           T30_SUPPORT_FINE_RESOLUTION |
                                           T30_SUPPORT_SUPERFINE_RESOLUTION);
    /* Advertising unlimited page length invites a sender whose page never ends:
     * a stuck gateway can hold the carrier up and we would keep absorbing rows
     * indefinitely, because in T.30 the page ends when the sender says so.
     * Legal is the longest standard page; bound it there unless asked. */
    t30_set_supported_image_sizes(t30, T30_SUPPORT_215MM_WIDTH | T30_SUPPORT_255MM_WIDTH |
                                           T30_SUPPORT_303MM_WIDTH | T30_SUPPORT_A4_LENGTH |
                                           T30_SUPPORT_B4_LENGTH | T30_SUPPORT_US_LETTER_LENGTH |
                                           T30_SUPPORT_US_LEGAL_LENGTH |
                                           (p->unlimited_length ? T30_SUPPORT_UNLIMITED_LENGTH : 0));

    if (p->tx_file != NULL)
        t30_set_tx_file(t30, p->tx_file, -1, -1);
    if (p->rx_file != NULL)
        t30_set_rx_file(t30, p->rx_file, -1);

    t30_set_phase_b_handler(t30, phase_b_handler, s);
    t30_set_phase_d_handler(t30, phase_d_handler, s);
    t30_set_phase_e_handler(t30, phase_e_handler, s);
    t30_set_real_time_frame_handler(t30, real_time_frame_handler, s);

    attach_logging(t30_get_logging_state(t30), s->tag);
}

fm_fax_t *fm_fax_create(const fm_fax_params_t *params)
{
    struct fm_fax *s = calloc(1, sizeof(*s));

    if (s == NULL)
        return NULL;

    pthread_mutex_init(&s->lock, NULL);
    s->calling = params->calling;
    s->last_rx_frame_ms = fm_now_ms();
    s->last_advance_ms = s->last_rx_frame_ms;
    s->seen_image_size = -1;
    s->result = -1;
    s->status.result = -1;
    s->status.result_text = "in progress";
    snprintf(s->tag, sizeof(s->tag), "%s", params->tag ? params->tag : (params->calling ? "tx" : "rx"));

    s->fax = fax_init(NULL, params->calling ? TRUE : FALSE);
    if (s->fax == NULL)
    {
        FM_ERROR("fax", "fax_init failed");
        pthread_mutex_destroy(&s->lock);
        free(s);
        return NULL;
    }

    /* RTP never stops, so the modem must always have something to send. */
    fax_set_transmit_on_idle(s->fax, TRUE);
    attach_logging(fax_get_logging_state(s->fax), s->tag);
    configure_t30(s, params);

    fm_log_event(FM_LOG_INFO, "fax", "engine started",
                 "tag=%s role=%s ecm=%s max_speed=%d station_id=\"%s\" tx_file=\"%s\" rx_file=\"%s\"", s->tag,
                 params->calling ? "transmit" : "receive", params->ecm ? "on" : "off", params->max_speed,
                 params->station_id ? params->station_id : "", params->tx_file ? params->tx_file : "",
                 params->rx_file ? params->rx_file : "");
    return s;
}

void fm_fax_destroy(fm_fax_t *fax)
{
    struct fm_fax *s = fax;

    if (s == NULL)
        return;
    pthread_mutex_lock(&s->lock);
    if (s->fax != NULL)
    {
        fax_release(s->fax);
        fax_free(s->fax);
        s->fax = NULL;
    }
    pthread_mutex_unlock(&s->lock);
    pthread_mutex_destroy(&s->lock);
    free(s);
}

void fm_fax_rx(fm_fax_t *fax, const int16_t *samples, int count)
{
    struct fm_fax *s = fax;

    if (s == NULL || count <= 0)
        return;
    pthread_mutex_lock(&s->lock);
    if (s->fax != NULL)
        fax_rx(s->fax, (int16_t *) samples, count);
    pthread_mutex_unlock(&s->lock);
}

void fm_fax_rx_missing(fm_fax_t *fax, int count)
{
    struct fm_fax *s = fax;

    if (s == NULL || count <= 0)
        return;
    pthread_mutex_lock(&s->lock);
    if (s->fax != NULL)
        fax_rx_fillin(s->fax, count);
    pthread_mutex_unlock(&s->lock);
}

int fm_fax_tx(fm_fax_t *fax, int16_t *samples, int max_count)
{
    struct fm_fax *s = fax;
    int n = 0;

    if (s == NULL || max_count <= 0)
        return 0;
    pthread_mutex_lock(&s->lock);
    if (s->fax != NULL)
        n = fax_tx(s->fax, samples, max_count);
    pthread_mutex_unlock(&s->lock);
    if (n < 0)
        n = 0;
    return n;
}

int64_t fm_fax_since_rx_frame_ms(const fm_fax_t *fax)
{
    struct fm_fax *s = (struct fm_fax *) fax;
    int64_t since;

    if (s == NULL)
        return 0;
    pthread_mutex_lock(&s->lock);
    since = fm_now_ms() - s->last_rx_frame_ms;
    pthread_mutex_unlock(&s->lock);
    return since;
}

int64_t fm_fax_since_advance_ms(fm_fax_t *fax)
{
    struct fm_fax *s = fax;
    int64_t since;

    if (s == NULL)
        return 0;
    pthread_mutex_lock(&s->lock);
    if (s->fax != NULL)
    {
        t30_stats_t stats;
        int pages;

        t30_get_transfer_statistics(fax_get_t30_state(s->fax), &stats);
        pages = stats.pages_tx + stats.pages_rx;
        if (pages != s->seen_pages || stats.image_size != s->seen_image_size)
        {
            s->seen_pages = pages;
            s->seen_image_size = stats.image_size;
            s->last_advance_ms = fm_now_ms();
        }
    }
    since = fm_now_ms() - s->last_advance_ms;
    pthread_mutex_unlock(&s->lock);
    return since;
}

bool fm_fax_completed(const fm_fax_t *fax)
{
    struct fm_fax *s = (struct fm_fax *) fax;
    bool done;

    if (s == NULL)
        return true;
    pthread_mutex_lock(&s->lock);
    done = s->completed;
    pthread_mutex_unlock(&s->lock);
    return done;
}

void fm_fax_status(const fm_fax_t *fax, fm_fax_status_t *out)
{
    struct fm_fax *s = (struct fm_fax *) fax;

    memset(out, 0, sizeof(*out));
    if (s == NULL)
    {
        out->result = -1;
        out->result_text = "no session";
        return;
    }
    pthread_mutex_lock(&s->lock);
    *out = s->status;
    pthread_mutex_unlock(&s->lock);
    if (out->result_text == NULL)
        out->result_text = out->completed ? t30_completion_code_to_str(out->result) : "in progress";
}

int fm_fax_exit_code(int t30_result)
{
    if (t30_result == T30_ERR_OK)
        return FM_EXIT_OK;
    return FM_EXIT_FAX;
}

/* ------------------------------------------------------------------------- */

static void pump(fm_fax_t *from, fm_fax_t *to, int16_t *buf, int chunk)
{
    int n = fm_fax_tx(from, buf, chunk);

    if (n < chunk)
    {
        memset(buf + n, 0, (size_t) (chunk - n) * sizeof(int16_t));
        n = chunk;
    }
    fm_fax_rx(to, buf, n);
}

int fm_fax_selftest(const fm_config_t *cfg)
{
    fm_fax_params_t tx_params;
    fm_fax_params_t rx_params;
    fm_fax_t *tx = NULL;
    fm_fax_t *rx = NULL;
    fm_fax_status_t tx_status;
    fm_fax_status_t rx_status;
    fm_tiff_info_t info;
    char err[512];
    char out_path[FM_STR_MAX + 64];
    char stem[128];
    int16_t buf[SELFTEST_CHUNK];
    int rc = FM_EXIT_OK;
    int64_t started;
    long iterations = 0;
    const long max_iterations = (long) SELFTEST_MAX_SECONDS * 8000 / SELFTEST_CHUNK;

    if (!fm_tiff_probe(cfg->file, &info, err, sizeof(err)))
    {
        FM_ERROR("selftest", "%s", err);
        return FM_EXIT_CONFIG;
    }
    fm_tiff_log(&info, cfg->file);

    if (!fm_mkdir_p(cfg->output_dir, err, sizeof(err)))
    {
        FM_ERROR("selftest", "%s", err);
        return FM_EXIT_CONFIG;
    }
    fm_basename_stem(cfg->file, stem, sizeof(stem));
    fm_sanitise_filename(stem);
    snprintf(out_path, sizeof(out_path), "%s/selftest-%s.tif", cfg->output_dir, stem);

    memset(&tx_params, 0, sizeof(tx_params));
    tx_params.calling = true;
    tx_params.tx_file = cfg->file;
    tx_params.station_id = cfg->station_id[0] ? cfg->station_id : "faxmodem-tx";
    tx_params.header = cfg->header;
    tx_params.ecm = cfg->ecm;
    tx_params.max_speed = cfg->max_speed;
    tx_params.unlimited_length = cfg->unlimited_page_length;
    tx_params.tag = "selftest-tx";

    memset(&rx_params, 0, sizeof(rx_params));
    rx_params.calling = false;
    rx_params.rx_file = out_path;
    rx_params.station_id = "faxmodem-rx";
    rx_params.ecm = cfg->ecm;
    rx_params.max_speed = cfg->max_speed;
    rx_params.unlimited_length = cfg->unlimited_page_length;
    rx_params.tag = "selftest-rx";

    tx = fm_fax_create(&tx_params);
    rx = fm_fax_create(&rx_params);
    if (tx == NULL || rx == NULL)
    {
        fm_fax_destroy(tx);
        fm_fax_destroy(rx);
        return FM_EXIT_INTERNAL;
    }

    FM_INFO("selftest", "looping two T.30 engines back to back, writing %s", out_path);
    started = fm_now_ms();

    while (iterations < max_iterations)
    {
        iterations++;
        pump(tx, rx, buf, SELFTEST_CHUNK);
        pump(rx, tx, buf, SELFTEST_CHUNK);
        if (fm_fax_completed(tx) && fm_fax_completed(rx))
            break;
    }

    fm_fax_status(tx, &tx_status);
    fm_fax_status(rx, &rx_status);

    if (!tx_status.completed || !rx_status.completed)
    {
        FM_ERROR("selftest", "engines did not finish within %d simulated seconds", SELFTEST_MAX_SECONDS);
        rc = FM_EXIT_TIMEOUT;
    }
    else if (tx_status.result != T30_ERR_OK || rx_status.result != T30_ERR_OK)
    {
        rc = FM_EXIT_FAX;
    }

    fm_log_event(rc == FM_EXIT_OK ? FM_LOG_INFO : FM_LOG_ERROR, "selftest", "result",
                 "tx_result=\"%s\" rx_result=\"%s\" pages_tx=%d pages_rx=%d bit_rate=%d ecm=%s "
                 "simulated_seconds=%.1f wall_ms=%" PRId64 " output=\"%s\"",
                 tx_status.result_text, rx_status.result_text, tx_status.pages_tx, rx_status.pages_rx,
                 rx_status.bit_rate, rx_status.ecm ? "yes" : "no",
                 (double) iterations * SELFTEST_CHUNK / 8000.0, fm_now_ms() - started, out_path);

    fm_fax_destroy(tx);
    fm_fax_destroy(rx);
    return rc;
}
