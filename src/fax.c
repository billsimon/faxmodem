#include "faxmodem/fax.h"
#include "faxmodem/log.h"
#include "faxmodem/tiff_probe.h"
#include "faxmodem/util.h"

#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* For the echo guard, which has to reach the HDLC receiver inside fax_state_t;
 * see echo_guard_hook(). */
#define SPANDSP_EXPOSE_INTERNAL_STRUCTURES
#include <spandsp.h>

#define SELFTEST_CHUNK 160                 /* 20 ms at 8 kHz */
#define ECHO_FRAMES 4                      /* our last few control frames */
#define ECHO_FRAME_MAX 64                  /* longer is never a control frame we would see again */
#define ECHO_WINDOW_SAMPLES (5 * 8000)     /* a 2.5 s round trip, plus the frame's own preamble */
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
    /* The echo guard: what we sent lately, by the audio clock, which counts
     * samples received - so it runs at the selftest's speed as well as a
     * call's. */
    int64_t samples;
    struct
    {
        uint8_t msg[ECHO_FRAME_MAX];
        int len;
        int64_t at;
    } sent[ECHO_FRAMES];
    int sent_next;
    int echoes;
    t30_set_handler_t *set_rx_type;
    void *set_rx_type_user_data;
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

/* spandsp 0.0.6 builds itself fixed point on any host its configure script
 * calls "arm" - which, through an old config.guess, includes Apple Silicon
 * Macs - and its fixed point V.17 does not work: it trains, then demodulates
 * nothing but noise, at every rate. T.30 copes - TCF fails at 14400 and 12000
 * and the call settles at 9600 - but every call spends two failed trainings
 * getting there, and as a receiver we would advertise a modem we cannot
 * receive. So V.17 is tried once, back to back, before it is offered. A
 * floating point spandsp passes; so would a fixed point one that had been
 * fixed. -1 until fm_fax_check_modems() has run. */
static int g_v17_ok = -1;

typedef struct
{
    long run;
    long longest;
} v17_probe_t;

static int v17_probe_get_bit(void *user_data)
{
    (void) user_data;
    return 0; /* all zeros, as in TCF */
}

static void v17_probe_put_bit(void *user_data, int bit)
{
    v17_probe_t *p = user_data;

    if (bit < 0)
        return; /* a status change, not data */
    if (bit == 0)
    {
        if (++p->run > p->longest)
            p->longest = p->run;
    }
    else
    {
        p->run = 0;
    }
}

bool fm_fax_check_modems(void)
{
    v17_probe_t probe = {0, 0};
    v17_tx_state_t *tx;
    v17_rx_state_t *rx;
    int16_t buf[SELFTEST_CHUNK];

    if (g_v17_ok >= 0)
        return g_v17_ok == 1;

    tx = v17_tx_init(NULL, 14400, FALSE, v17_probe_get_bit, NULL);
    rx = v17_rx_init(NULL, 14400, v17_probe_put_bit, &probe);
    if (tx == NULL || rx == NULL)
    {
        g_v17_ok = 0;
    }
    else
    {
        /* Three seconds: the long training takes about one and a half. */
        for (int i = 0; i < 3 * 8000 / SELFTEST_CHUNK; i++)
        {
            int n = v17_tx(tx, buf, SELFTEST_CHUNK);

            if (n < SELFTEST_CHUNK)
                memset(buf + n, 0, (size_t) (SELFTEST_CHUNK - n) * sizeof(int16_t));
            v17_rx(rx, buf, SELFTEST_CHUNK);
        }
        /* A working modem delivers over a second of unbroken zeros; the
         * broken one never more than a handful. */
        g_v17_ok = probe.longest >= 8000 ? 1 : 0;
    }
    if (tx != NULL)
        v17_tx_free(tx);
    if (rx != NULL)
        v17_rx_free(rx);
    return g_v17_ok == 1;
}

static int modems_for_speed(int max_speed)
{
    if (max_speed >= 14400 && g_v17_ok != 0)
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
                 "remote_id=\"%s\" own_echoes=%d",
                 s->tag, completion_code, s->status.result_text, s->status.pages_tx, s->status.pages_rx,
                 s->status.bit_rate, s->status.ecm ? "yes" : "no", s->status.remote_ident, s->echoes);
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
    if (direction)
    {
        s->last_rx_frame_ms = fm_now_ms();
        return;
    }
    /* Ours, on its way out: remembered for the echo guard. ECM image frames
     * go out on the fast modem and would only push the control frames out. */
    if (len < 3 || len > ECHO_FRAME_MAX || msg[2] == T4_FCD || msg[2] == T4_RCP)
        return;
    memcpy(s->sent[s->sent_next].msg, msg, (size_t) len);
    s->sent[s->sent_next].len = len;
    s->sent[s->sent_next].at = s->samples;
    s->sent_next = (s->sent_next + 1) % ECHO_FRAMES;
}

/* The echo guard. On a call into the telephone network the far end's line
 * card returns our own signal a round trip later, and network echo
 * cancellers do not always take it out. When the round trip is long - from
 * about 600 ms - the tail of our own V.21 frame arrives after we have stopped
 * sending and are listening again, and spandsp 0.0.6 takes it for the far
 * end's: a receiver that hears its own CFR echo gives up with "Unexpected
 * command after page received", a sender that hears its own PPS with
 * "Invalid ECM response". So a frame received intact that is byte for byte
 * one we sent in the last few seconds is dropped before T.30 sees it. The
 * far end does not send our frames: T.30 marks each with the X bit of the
 * station that sent it. */
static bool is_own_echo(const struct fm_fax *s, const uint8_t *msg, int len)
{
    for (int i = 0; i < ECHO_FRAMES; i++)
    {
        if (s->sent[i].len == len && s->samples - s->sent[i].at <= ECHO_WINDOW_SAMPLES &&
            memcmp(s->sent[i].msg, msg, (size_t) len) == 0)
            return true;
    }
    return false;
}

/* Called from inside fax_rx(), i.e. already under s->lock. len < 0 is a
 * status report, not a frame; a damaged frame (ok false) goes through, as
 * T.30 counts those. */
static void echo_guard_hdlc_accept(void *user_data, const uint8_t *msg, int len, int ok)
{
    struct fm_fax *s = user_data;

    if (len > 0 && ok && is_own_echo(s, msg, len))
    {
        s->echoes++;
        FM_DEBUG("fax", "tag=%s ignored an echo of our own %s", s->tag, t30_frametype(msg[2]));
        return;
    }
    t30_hdlc_accept(fax_get_t30_state(s->fax), msg, len, ok);
}

/* spandsp re-initialises its HDLC receiver, with t30_hdlc_accept wired in,
 * each time T.30 changes receive mode - so the guard is put back each time,
 * right after. */
static void echo_guard_hook(struct fm_fax *s)
{
    hdlc_rx_state_t *rx = &s->fax->modems.hdlc_rx;

    if (rx->frame_handler == t30_hdlc_accept)
    {
        rx->frame_handler = echo_guard_hdlc_accept;
        rx->frame_user_data = s;
    }
}

static void echo_guard_set_rx_type(void *user_data, int type, int bit_rate, int short_train, int use_hdlc)
{
    struct fm_fax *s = user_data;

    s->set_rx_type(s->set_rx_type_user_data, type, bit_rate, short_train, use_hdlc);
    echo_guard_hook(s);
}

static void echo_guard_install(struct fm_fax *s)
{
    t30_state_t *t30 = fax_get_t30_state(s->fax);

    s->set_rx_type = t30->set_rx_type_handler;
    s->set_rx_type_user_data = t30->set_rx_type_user_data;
    t30->set_rx_type_handler = echo_guard_set_rx_type;
    t30->set_rx_type_user_data = s;
    echo_guard_hook(s);
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
    echo_guard_install(s);

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
    s->samples += count;
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
    s->samples += count;
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

/* The line between the two engines. By default a perfect one - each end
 * hears exactly what the other sent, in the same 20 ms frame - which is what
 * tells a T.30 problem from a line problem.
 *
 * FAXMODEM_SELFTEST_LINE describes a worse one, for the things a perfect line
 * cannot exercise: "delay=150,echo=-20,noise=-45,ulaw". delay is one way, in
 * ms, as jitter buffers and a network would make it - T.30's command/response
 * timers have to survive it; echo puts each end's own signal back into its
 * receiver that many dB down, a whole round trip later, the way the far end's
 * hybrid does on a call into the telephone network - an engine must not take
 * its own echoed V.21 for the far end; noise is white noise in dBm0; ulaw or
 * alaw passes everything through G.711, as every real call does. A test hook,
 * not an option. */
#define SELFTEST_LINE_MAX (SELFTEST_CHUNK * 128) /* 2.56 s of each direction */

typedef struct
{
    int delay;                /* one way, samples */
    int echo_delay;           /* samples */
    float echo_gain;          /* 0 = no echo */
    int g711;                 /* 0 linear, 'u' or 'a' */
    awgn_state_t *noise;
    int16_t hist[2][SELFTEST_LINE_MAX];
    long pos;
} selftest_line_t;

static bool selftest_line_init(selftest_line_t *ln, char *desc, size_t desc_len)
{
    const char *spec = getenv("FAXMODEM_SELFTEST_LINE");
    char buf[256];
    char *tok;
    char *save = NULL;
    double delay_ms = 0.0;
    double echo_db = 0.0;
    double noise_db = 0.0;
    bool echo = false;

    memset(ln, 0, sizeof(*ln));
    snprintf(desc, desc_len, "perfect");
    if (spec == NULL || *spec == '\0')
        return true;

    snprintf(buf, sizeof(buf), "%s", spec);
    for (tok = strtok_r(buf, ", ", &save); tok != NULL; tok = strtok_r(NULL, ", ", &save))
    {
        if (strncmp(tok, "delay=", 6) == 0)
            delay_ms = atof(tok + 6);
        else if (strncmp(tok, "echo=", 5) == 0)
        {
            echo_db = atof(tok + 5);
            echo = true;
        }
        else if (strncmp(tok, "noise=", 6) == 0)
            noise_db = atof(tok + 6);
        else if (strcmp(tok, "ulaw") == 0)
            ln->g711 = 'u';
        else if (strcmp(tok, "alaw") == 0)
            ln->g711 = 'a';
        else
        {
            FM_ERROR("selftest", "FAXMODEM_SELFTEST_LINE: '%s' is not delay=, echo=, noise=, ulaw or alaw", tok);
            return false;
        }
    }
    if (delay_ms < 0.0 || echo_db > 0.0 || noise_db > 0.0)
    {
        FM_ERROR("selftest", "FAXMODEM_SELFTEST_LINE: delay must be >= 0, echo and noise <= 0 dB");
        return false;
    }
    ln->delay = (int) (delay_ms * 8000.0 / 1000.0);
    /* Back from the far end's line card: twice the one-way path, plus a
     * millisecond of local loop. */
    ln->echo_delay = 2 * ln->delay + 8;
    if (ln->echo_delay >= SELFTEST_LINE_MAX - SELFTEST_CHUNK)
    {
        FM_ERROR("selftest", "FAXMODEM_SELFTEST_LINE: a %.0f ms delay is longer than the selftest's line can hold",
                 delay_ms);
        return false;
    }
    if (echo)
        ln->echo_gain = powf(10.0f, (float) echo_db / 20.0f);
    if (noise_db < 0.0)
    {
        ln->noise = awgn_init_dbm0(NULL, 1234567, (float) noise_db);
        if (ln->noise == NULL)
        {
            FM_ERROR("selftest", "could not create the noise generator");
            return false;
        }
    }
    snprintf(desc, desc_len, "delay %.0f ms each way, echo %s, noise %s, %s", delay_ms, echo ? "on" : "none",
             ln->noise ? "on" : "none",
             ln->g711 == 'u' ? "G.711 mu-law" : ln->g711 == 'a' ? "G.711 A-law" : "linear");
    if (echo || ln->noise)
        snprintf(desc + strlen(desc), desc_len - strlen(desc), " (echo %.0f dB, noise %.0f dBm0)", echo_db,
                 noise_db);
    return true;
}

static void selftest_line_free(selftest_line_t *ln)
{
    if (ln->noise != NULL)
        awgn_free(ln->noise);
    ln->noise = NULL;
}

/* One 20 ms frame each way. Both ends transmit before either receives, which
 * is the order pjmedia's conference bridge calls a port in. */
static void selftest_line_run(selftest_line_t *ln, fm_fax_t *caller, fm_fax_t *answerer)
{
    int16_t out[2][SELFTEST_CHUNK];
    int16_t in[2][SELFTEST_CHUNK];
    fm_fax_t *end[2] = {caller, answerer};

    for (int e = 0; e < 2; e++)
    {
        int n = fm_fax_tx(end[e], out[e], SELFTEST_CHUNK);

        if (n < SELFTEST_CHUNK)
            memset(out[e] + n, 0, (size_t) (SELFTEST_CHUNK - n) * sizeof(int16_t));
    }
    for (int i = 0; i < SELFTEST_CHUNK; i++)
    {
        long t = ln->pos + i;

        ln->hist[0][t % SELFTEST_LINE_MAX] = out[0][i];
        ln->hist[1][t % SELFTEST_LINE_MAX] = out[1][i];
        for (int e = 0; e < 2; e++)
        {
            float x = (t >= ln->delay) ? ln->hist[1 - e][(t - ln->delay) % SELFTEST_LINE_MAX] : 0.0f;

            if (ln->echo_gain > 0.0f && t >= ln->echo_delay)
                x += ln->echo_gain * ln->hist[e][(t - ln->echo_delay) % SELFTEST_LINE_MAX];
            if (ln->noise != NULL)
                x += awgn(ln->noise);
            if (x > 32767.0f)
                x = 32767.0f;
            else if (x < -32768.0f)
                x = -32768.0f;
            in[e][i] = (int16_t) lrintf(x);
            if (ln->g711 == 'u')
                in[e][i] = ulaw_to_linear(linear_to_ulaw(in[e][i]));
            else if (ln->g711 == 'a')
                in[e][i] = alaw_to_linear(linear_to_alaw(in[e][i]));
        }
    }
    ln->pos += SELFTEST_CHUNK;
    fm_fax_rx(answerer, in[1], SELFTEST_CHUNK);
    fm_fax_rx(caller, in[0], SELFTEST_CHUNK);
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
    char line_desc[160];
    /* 80 KB of history: static rather than on the stack. */
    static selftest_line_t line;
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
    if (!selftest_line_init(&line, line_desc, sizeof(line_desc)))
        return FM_EXIT_CONFIG;
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
        selftest_line_free(&line);
        return FM_EXIT_INTERNAL;
    }

    FM_INFO("selftest", "looping two T.30 engines back to back over a %s line, writing %s", line_desc,
            out_path);
    started = fm_now_ms();

    while (iterations < max_iterations)
    {
        iterations++;
        selftest_line_run(&line, tx, rx);
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
    selftest_line_free(&line);
    return rc;
}
