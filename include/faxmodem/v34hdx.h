/* ITU-T V.34 (02/98) half duplex, for facsimile: what T.30 Annex F ("Super
 * G3") runs on.
 *
 * Phase 1 is V.8, with a call function of T.30 transmit: the answering fax
 * sends ANSam, the calling one CM, and they agree on V.34 half duplex.
 * Phase 2 probes the line (12.2), and Phase 3 trains the recipient's
 * equaliser on the source's signal (12.3). From then on the two modems
 * alternate between two channels:
 *
 *   the control channel (10.2.4), 600 baud, both ways at once, carrying
 *   T.30's frames - DIS, DCS, CFR, PPS, MCF, DCN - at 1200 bit/s;
 *
 *   the primary channel, one way, source to recipient, at up to 33 600
 *   bit/s, carrying the ECM frames of each partial page.
 *
 * Start-up ends with the control channel up (12.4). The source turns to the
 * primary channel when T.30 has a page to send (12.6.3, 12.5), and back to
 * the control channel when its frames run out (12.5.3, 12.6); the recipient
 * follows. There is no TCF: training is V.34's business.
 *
 * Here the calling modem is always the source - faxmodem calls to send and
 * answers to receive - so 12.2.1 is implemented and 12.2.2, the answerer
 * as source, is not: a CM asking for it is answered by declining V.34, and
 * the call carries on as an ordinary G3 one.
 *
 * Section, table and figure numbers are V.34's unless they say otherwise. */
#ifndef FAXMODEM_V34HDX_H
#define FAXMODEM_V34HDX_H

#include <stdbool.h>
#include <stdint.h>

typedef struct fm_v34h fm_v34h_t;

typedef enum
{
    FM_V34H_CC_UP = 0,       /* the control channel carries data both ways */
    FM_V34H_PRIMARY_UP,      /* source: B1 is out, page data is being taken; recipient: B1 is in */
    FM_V34H_PRIMARY_DOWN,    /* recipient: the source's primary channel has ended */
    FM_V34H_NOT_V34,         /* V.8 did not settle on V.34 half duplex, or there was no V.8 */
    FM_V34H_FAILED           /* a stage timed out past recovery */
} fm_v34h_event_t;

typedef struct
{
    bool calling;
    int max_rate;            /* primary channel, 2400 ... 33600; 0 = 33600 */
    float tx_power;          /* nominal, dBm0 */
    unsigned symbol_rates;   /* test hook: which to allow, bit 0 = 2400; 0 = all */
    bool v17, v29, v27ter;   /* the G3 modulations to declare in V.8 alongside V.34 */
    const char *tag;

    /* Called on whichever thread drives fm_v34h_tx and fm_v34h_rx. A
     * cc_get_bit or pc_get_bit returning a negative number has nothing more:
     * on the control channel that is ignored (it idles on ones), on the
     * primary channel it ends the page. */
    int (*cc_get_bit)(void *user);
    void (*cc_put_bit)(void *user, int bit);
    int (*pc_get_bit)(void *user);
    void (*pc_put_bit)(void *user, int bit);
    void (*event)(void *user, fm_v34h_event_t ev);
    void *user;
} fm_v34h_params_t;

typedef struct
{
    const char *stage;
    bool source;
    int rate;                /* primary channel, bit/s; 0 until agreed */
    int symbol_rate;         /* rounded, 0 until agreed */
    int carrier;             /* Hz */
    int pre_emphasis;
    float snr_db;            /* recipient: its receiver's estimate from Phase 3 */
    int round_trip_ms;       /* recipient: measured in Phase 2, -1 if not */
    unsigned pages;          /* primary channel transmissions */
} fm_v34h_stats_t;

fm_v34h_t *fm_v34h_create(const fm_v34h_params_t *params);
void fm_v34h_free(fm_v34h_t *v);

/* 16-bit linear, 8000 Hz; as in datamodem's V.34, feed the receiver the same
 * number of samples as the transmitter produced, transmit first. */
int fm_v34h_tx(fm_v34h_t *v, int16_t *amp, int len);
void fm_v34h_rx(fm_v34h_t *v, const int16_t *amp, int len);
void fm_v34h_rx_fillin(fm_v34h_t *v, int len);

/* T.30 wants the primary channel: the source has a page to send, the
 * recipient is ready to receive one. Ignored unless the control channel is
 * up. */
void fm_v34h_primary(fm_v34h_t *v);

/* Still in V.8 or Phase 1 tones: what the front end needs to know to run a
 * G3 receiver alongside, for a far end that turns out not to do V.8. */
bool fm_v34h_in_v8(const fm_v34h_t *v);
bool fm_v34h_control_up(const fm_v34h_t *v);
bool fm_v34h_is_source(const fm_v34h_t *v);
void fm_v34h_stats(const fm_v34h_t *v, fm_v34h_stats_t *out);

#endif /* FAXMODEM_V34HDX_H */
