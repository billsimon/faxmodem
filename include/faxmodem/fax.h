/* The T.30 engine: a spandsp fax_state_t wrapped so that audio can be pumped
 * at it from a pjmedia thread while the main thread watches for completion. */
#ifndef FAXMODEM_FAX_H
#define FAXMODEM_FAX_H

#include <stdbool.h>
#include <stdint.h>

#include "faxmodem/config.h"

typedef struct fm_fax fm_fax_t;

/* T30_ERR_OK, repeated here so callers need not pull in spandsp.h. */
#define FM_T30_OK 0

typedef struct
{
    bool calling;            /* true when we placed the call (transmitter) */
    const char *tx_file;     /* TIFF to send, NULL when receiving */
    const char *rx_file;     /* TIFF to write, NULL when sending */
    const char *station_id;
    const char *header;
    bool ecm;
    int max_speed;           /* 14400 | 9600 | 4800 | 2400 */
    bool fine_only;          /* refuse standard (98 dpi) resolution */
    bool unlimited_length;   /* accept pages of unbounded length */
    bool v34;                /* offer V.34 half duplex (T.30 Annex F); ECM is then always on */
    int v34_max_rate;        /* its ceiling, 2400 ... 33600 */
    const char *tag;         /* short label for logs, e.g. a call id */
} fm_fax_params_t;

typedef struct
{
    bool completed;
    int result;              /* T30_ERR_* */
    const char *result_text;
    int pages_tx;
    int pages_rx;
    int bit_rate;
    bool ecm;
    bool v34;                /* the call ran on V.34; bit_rate is its primary channel's */
    int bad_rows;
    char remote_ident[41];
} fm_fax_status_t;

fm_fax_t *fm_fax_create(const fm_fax_params_t *params);
void fm_fax_destroy(fm_fax_t *fax);

/* Audio pumps. 16-bit signed linear, 8000 Hz, mono. Called from the media
 * thread; both are internally locked. */
void fm_fax_rx(fm_fax_t *fax, const int16_t *samples, int count);
void fm_fax_rx_missing(fm_fax_t *fax, int count); /* lost/stretched frame */
int fm_fax_tx(fm_fax_t *fax, int16_t *samples, int max_count);

bool fm_fax_completed(const fm_fax_t *fax);
void fm_fax_status(const fm_fax_t *fax, fm_fax_status_t *out);

/* Two independent liveness signals, because a stuck transfer can keep one of
 * them alive indefinitely while making no progress at all.
 *
 * fm_fax_since_rx_frame_ms: time since the far end last sent a T.30 frame. Our
 * own retransmissions deliberately do not count - a sender that has stopped
 * listening still makes us talk.
 *
 * fm_fax_since_advance_ms: time since the transfer last moved forward, meaning
 * a page completed or more image data arrived. Catches the far end that keeps
 * signalling on a loop without ever advancing. */
int64_t fm_fax_since_rx_frame_ms(const fm_fax_t *fax);
int64_t fm_fax_since_advance_ms(fm_fax_t *fax);

/* Maps a T.30 completion code onto a process exit code. */
int fm_fax_exit_code(int t30_result);

/* Routes spandsp's own logging to stdout at a verbosity derived from
 * cfg->log_level (or cfg->spandsp_log_level when set). Call once at startup. */
void fm_fax_init_logging(const fm_config_t *cfg);

/* Tries spandsp's V.17 modem back to back, once, and stops offering it if it
 * does not work - as the receiver in a fixed point spandsp 0.0.6 did before
 * third_party/spandsp/v17rx.c replaced it. Call it from the main thread before any engine is
 * created; later calls return the first answer. */
bool fm_fax_check_modems(void);

/* Runs two engines back to back in memory: no SIP, no audio device. Returns a
 * process exit code. */
int fm_fax_selftest(const fm_config_t *cfg);

#endif /* FAXMODEM_FAX_H */
