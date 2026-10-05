/* The control channel of half-duplex V.34 (10.2.4): 600 symbols a second of
 * QAM, the calling modem on 1200 Hz at nominal power, the answering one on
 * 2400 Hz a dB below with a guard tone at 1800 Hz 7 dB below - the same
 * carriers and levels as Phase 2's DPSK, and both directions at once.
 *
 * It carries T.30's frames between pages, and the training and handshake
 * signals that start and restart it: PPh, ALT, MPh, E, Sh and S-bar-h, AC.
 * Data goes at 1200 bit/s, two bits a symbol, differentially encoded onto
 * the four points of the quarter superconstellation's point 0 rotated. 2400
 * bit/s is only ever used when both ends allow asymmetric rates, which
 * faxmodem never does (10.2.4.4, bit 50), so it is not implemented here.
 *
 * The receiver needs no training to recognise what it is sent: PPh by
 * correlation against its 8-symbol period, Sh and AC by their period of two
 * symbols, at either T/2 phase. A Gardner loop finds the symbol clock from
 * whatever arrives, and a short decision-directed equaliser and phase loop
 * follow; differential decoding makes the phase's four-fold ambiguity
 * harmless. Section, table and figure numbers are V.34's. */
#ifndef FAXMODEM_V34_CC_H
#define FAXMODEM_V34_CC_H

#include <stdbool.h>
#include <stdint.h>

#include "v34_codec.h"

#define V34_CC_BAUD 600
#define V34_CC_SPS (8000.0 / V34_CC_BAUD)  /* 13 1/3 samples a symbol */

/* PPh (10.2.4.5): i = 2k + I, PPh(i) = exp(j pi (2k(k - I) + 1) / 4). V.34
 * prints k - 1 for k - I, which would make it a square wave; spandsp's
 * author reads it the same way. Eight symbols, sent four times. */
v34_cf_t v34_pph(int i);

/* The four points used at 1200 bit/s and by every training signal: point 0
 * of Figure 5, 1 + j, turned clockwise rot quarter turns, at unit power. */
v34_cf_t v34_cc_point(int rot);

/* ---------------------------------------------------------- transmitter */

#define V34_CC_L 4                      /* half-span of the pulse, symbols */
#define V34_CC_TAPS (2 * V34_CC_L + 1)

typedef v34_cf_t (*v34_cc_symbol_fn)(void *user);

typedef struct
{
    bool answerer;
    float amp;                          /* carrier amplitude, samples */
    float guard_amp;
    int cden;                           /* 1200 Hz: 20, 2400 Hz: 10 (carrier phase steps of 3) */
    float cosv[20], sinv[20];
    float g[40][V34_CC_TAPS];
    long long n;
    long long k;                        /* symbols taken */
    v34_cf_t hist[16];
    bool on;                            /* carrier on; off is silence, and no symbols are taken */
    v34_cc_symbol_fn next;
    void *user;
} v34_cctx_t;

void v34_cctx_init(v34_cctx_t *t, bool answerer, double power_dbm0, v34_cc_symbol_fn next, void *user);
/* Silence to signal: the first symbol is taken at once. */
void v34_cctx_on(v34_cctx_t *t, bool on);
float v34_cctx_sample(v34_cctx_t *t);

/* ------------------------------------------------------------- receiver */

#define V34_CC_HIST 512
#define V34_CC_H 256                    /* T/2 samples kept */
#define V34_CC_EQ 7                     /* T-spaced equaliser */

/* What a call to v34_ccrx_sample may report. */
enum
{
    V34_CC_EV_PPH = 1,                  /* PPh has been going for 16 symbols */
    V34_CC_EV_SHBAR = 2,                /* Sh, then the turn to S-bar-h */
    V34_CC_EV_AC = 4,                   /* AC has been going for 100 ms */
    V34_CC_EV_SYMBOL = 8                /* a symbol was decided; its bits are in bits[] */
};

typedef struct
{
    bool far_answerer;
    int dscr_tap;                       /* the far end's scrambler: 18 calling, 5 answering */
    int cden;
    float cosv[20], sinv[20];
    float mf[128];
    int mf_half;
    v34_cf_t mix[V34_CC_HIST];
    v34_cf_t zf[V34_CC_HIST];
    long long n;

    /* T/2 sampling, steered by the Gardner loop */
    double tau;
    double tfreq;                       /* samples per T/2 the far clock is off by */
    v34_cf_t h[V34_CC_H];
    long long nh;
    int parity;                         /* which T/2 samples are symbol centres */
    float pwr;                          /* T/2 sample power, smoothed */
    float pwr_sym;
    float epar[2];                      /* T/2 sample power at each phase */
    float floor_;                       /* below this the far end is silent */

    /* recognising signals, at T/2 */
    v34_cf_t hq, l2;
    float hp;
    int s_run, ac_run, neg_run;
    bool s_seen;
    float prod3[3], p3[3];
    float pph_rho;
    int pph_run;
    long long pph_at, sbar_at;          /* T/2 index of the last event */

    /* equaliser, phase, decisions */
    v34_cf_t line[2 * V34_CC_EQ];
    int lpos;
    v34_cf_t c[V34_CC_EQ];
    float theta, nu;
    float mse;
    bool trained;                       /* the equaliser has been set up from the signal's level */
    int since_on;                       /* symbols since the carrier came on */
    int bad_run;                        /* symbols the decisions have been poor for */
    int zprev;
    uint32_t dscr;
    long long symbols;

    /* outputs */
    uint8_t bits[16];
    int nbits;
    bool carrier;                       /* circuit 109 */
    int off_count;                      /* T/2 samples the level has been below the off threshold */
} v34_ccrx_t;

void v34_ccrx_init(v34_ccrx_t *r, bool far_is_answerer, double nominal_dbm0);
/* One sample. Returns the V34_CC_EV_ bits for what happened; the bits of a
 * decided symbol are left in bits[] (nbits of them, descrambled) for the
 * caller to take. */
int v34_ccrx_sample(v34_ccrx_t *r, float x);
/* The far end's signal has stopped: forget the symbol clock's phase and the
 * decision history, keep what was learnt of the channel. */
void v34_ccrx_reset_sync(v34_ccrx_t *r);

#endif /* FAXMODEM_V34_CC_H */
