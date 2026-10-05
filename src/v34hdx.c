/* ITU-T V.34 (02/98) half duplex, for T.30 Annex F. See v34hdx.h.
 *
 * Built from the pieces datamodem's duplex V.34 is built from - V.8, Phase
 * 2's tones and probes (v34_dsp.c), the data mode's coding (v34_codec.c),
 * and its QAM receiver, whose code is carried over here nearly unchanged -
 * put together in the order clause 12 asks for, with the control channel of
 * v34_cc.c alongside.
 *
 * The transmit side is one of five things at any moment: V.8's signals, the
 * Phase 2 transmitter, the QAM modulator (the source's primary channel),
 * the control channel modulator, or silence. Each of the last two takes its
 * symbols from a little program of segments.
 *
 * TIME. Line time is a sample count, as in datamodem: transmit sample k and
 * receive sample k are the same instant at the line terminals, which is what
 * the 40 ms turn-round of Phase 2 and the round trip measurement rely on.
 * There is no echo canceller: in half duplex the two directions never share
 * a band at the same moment. */
#include "faxmodem/v34hdx.h"
#include "faxmodem/log.h"

#include "v34_cc.h"
#include "v34_codec.h"
#include "v34_dsp.h"
#include "v34_info.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <spandsp.h>

#define PI 3.14159265358979323846
#define MS(x) ((long long) ((x) * 8))
#define CC_SYMS(s) ((long long) ((s) * V34_CC_BAUD))   /* seconds to control channel symbols */
#define TRN_UNITS 17                                    /* the TRN we ask for: 17 x 35 ms */

/* ----------------------------------------------------------------- stages */

typedef enum
{
    ST_V8_C_LISTEN = 0,  /* calling: waiting for ANSam */
    ST_V8_C_TE,          /* ANSam heard; silent for Te */
    ST_V8_C_CM,          /* CM out, waiting for two identical JMs */
    ST_V8_C_CJ,          /* CJ out */
    ST_V8_A_ANSAM,       /* answering: ANSam out, waiting for two identical CMs */
    ST_V8_A_JM,          /* JM out, waiting for CJ */
    ST_V8_DONE,          /* 75 ms of silence before Phase 2 */

    ST_C2_INFO0,         /* source: INFO0c and tone B; waiting for INFO0a and tone A's reversal */
    ST_C2_L,             /* B reversed, L1 and L2 out; waiting for tone A */
    ST_C2_INFOH,         /* tone B; waiting for INFOh */

    ST_A2_INFO0,         /* recipient: INFO0a and tone A; waiting for INFO0c and tone B */
    ST_A2_REV,           /* A reversed, silent; waiting for B's reversal */
    ST_A2_PROBE,         /* receiving L1 and L2 */
    ST_A2_TONEB,         /* tone A; waiting for tone B */
    ST_A2_INFOH,         /* tone A for 25 ms more, then INFOh */

    ST_P3_SRC,           /* source: silence, S, S-bar, PP, TRN */
    ST_P3_RCP,           /* recipient: training on them */
    ST_CC_START,         /* PPh, ALT, MPh, E (12.4) */
    ST_CC_DATA,          /* the control channel is up */
    ST_CC_RESYNC,        /* Sh, S-bar-h, ALT, E (12.6) */
    ST_PC_TX,            /* source: the primary channel (12.5.1, 12.5.3.1) */
    ST_PC_RX,            /* recipient: the primary channel (12.5.2) */
    ST_DEAD
} stage_t;

static const char *const STAGE_NAMES[] = {
    "V.8: listening for ANSam",
    "V.8: waiting out Te",
    "V.8: CM, waiting for JM",
    "V.8: CJ",
    "V.8: ANSam, waiting for CM",
    "V.8: JM, waiting for CJ",
    "V.8 done",
    "INFO0c, waiting for INFO0a",
    "probing: sending L1/L2",
    "waiting for INFOh",
    "INFO0a, waiting for INFO0c",
    "measuring the round trip",
    "probing: receiving L1/L2",
    "tone A, waiting for tone B",
    "sending INFOh",
    "training: S, PP, TRN",
    "training on the source",
    "starting the control channel",
    "control channel",
    "resynchronising the control channel",
    "primary channel: sending",
    "primary channel: receiving",
    "cleared down",
};

typedef enum
{
    TXM_SILENCE = 0,
    TXM_V8,
    TXM_P2,
    TXM_QAM,
    TXM_CC
} txm_t;

/* The primary channel's segments. */
typedef enum
{
    TS_SILENCE = 0,
    TS_S,
    TS_SBAR,
    TS_PP,
    TS_TRN,
    TS_DATA
} tseg_t;

/* The control channel's. */
typedef enum
{
    CS_SILENCE = 0,
    CS_PPH,
    CS_ALT,
    CS_MPH,
    CS_E,
    CS_DATA,
    CS_SH,
    CS_SHB,
    CS_ONES,
    CS_AC,
    CS_OFF                  /* stop the carrier */
} cseg_t;

typedef struct
{
    int seg;
    long long len;
} step_t;

#define PROG_MAX 12

/* ------------------------------------------------------------- receiver */

#define MF_PHASES 64
#define MF_MAX 64
#define ZBUF 512
#define ZMASK (ZBUF - 1)
#define NEQ 48
#define EQ_D 16              /* the main tap */
#define HBUF 4096            /* T/2 samples held while looking for S and PP */
#define HMASK (HBUF - 1)

typedef enum
{
    RQ_OFF = 0,
    RQ_HUNT,                 /* untrained: looking for S, then S-bar */
    RQ_ALIGN,                /* gathering PP to find exactly where it starts */
    RQ_TRAIN,                /* data-aided: PP, and in Phase 3 TRN */
    RQ_B1,                   /* B1: one data frame, known in advance */
    RQ_DATA
} rq_t;

typedef struct
{
    int sr;
    bool high;
    double sps, th;
    int cnum, cden;
    float *cosv, *sinv;
    int mf_half;
    float hr[MF_PHASES + 1][2 * MF_MAX + 1];
    v34_cf_t z[ZBUF];
    double tau;

    v34_cf_t h[HBUF];
    long long nh;
    long long eq_next;
    long long sym0;

    v34_cf_t line[2 * NEQ];
    int lpos;
    v34_cf_t c[NEQ];
    double complex *P;
    double lambda;
    float beta;
    float theta, nu, a1, a2;
    float mse;
    float mse_fast;
    double tfreq;
    long long k;
    rq_t mode;

    v34_cf_t hq;
    float hp;
    int s_run;
    bool s_seen;
    int neg_run;
    float prod3[3], p3[3];
    long long s_bar_at;

    uint32_t trn_scr;
    float pwr;
    float pwr_ref;
    int hold;
    float pwr_q;
    double trn_err;          /* the last stretch of TRN, for the SNR */
    long long trn_err_n;
} qrx_t;

struct fm_v34h
{
    bool calling;
    bool source;
    char tag[48];
    int max_rate;
    unsigned sr_allow;
    bool g3_v17, g3_v29, g3_v27ter;
    float nominal_dbm0;
    int (*cc_get_bit)(void *user);
    void (*cc_put_bit)(void *user, int bit);
    int (*pc_get_bit)(void *user);
    void (*pc_put_bit)(void *user, int bit);
    void (*event)(void *user, fm_v34h_event_t ev);
    void *user;
    int scr_tap, dscr_tap;
    float p0, pmin;

    stage_t stage;
    long long deadline;
    long long t_stage;
    long long n;              /* receive line time */
    long long ntx;            /* transmit line time */

    /* transmit */
    txm_t txm;
    fsk_tx_state_t *fsk_tx;
    modem_connect_tones_tx_state_t *ansam_tx;
    long long ansam_done_at;
    uint8_t v8q[1024];
    int v8h, v8t;
    uint8_t v8msg[256];
    int v8msg_len;
    bool v8_repeat;
    v34_p2tx_t p2tx;
    long long p2_off_at;
    long long p2_probe_at;
    long long p2_l2_at;
    bool p2_off_after_bits;
    long long p2_tone_from;
    long long infoh_at;       /* recipient: INFOh goes at this line time */
    uint8_t infoh_bits[V34_INFOH_BITS];
    int infoh_n;

    v34_qtx_t qtx;
    bool qtx_on;
    tseg_t seg;
    long long seg_count, seg_len;
    step_t prog[PROG_MAX];
    int nprog, iprog;
    uint32_t tscr;
    bool tx16;                /* TRN on the 16-point set */
    long long trn_symbols;
    v34_enc_t enc;
    v34_cf_t dbuf[8];
    int dpos;
    float data_scale;
    bool tx_data;             /* page data may go */
    bool pc_end;              /* the page has run out */
    long long qam_quiet_at;   /* line time the modulator's last symbol has left it */
    int ones_frames;          /* mapping frames of ones still to send after it */

    v34_cctx_t cctx;
    bool cctx_ready;
    cseg_t cseg;
    long long ccount, clen;
    step_t cprog[PROG_MAX];
    int cnp, cip;
    uint32_t cscr;
    int cz;
    uint8_t mphbits[V34_MPH1_BITS];
    int mph_len, mph_pos;
    bool mph_finish;          /* the far end's MPh is in: finish this one, then E */
    int e_pos;
    bool cc_tx_data;
    long long alt_since;      /* control channel symbol our ALT started at */

    /* receive */
    fsk_rx_state_t *fsk_rx;
    float bpf[2][5];          /* V.8's receiver: the far end's V.21 channel, two biquads */
    float bpz[2][2];
    modem_connect_tones_rx_state_t *ansam_rx;
    uint32_t v8_sr;
    int v8_bitcnt;
    bool v8_synced;
    uint8_t v8_rx[64];
    int v8_rxn;
    uint8_t v8_last[64];
    int v8_lastn;
    int v8_zero_octets;
    bool v8_got_msg;
    bool v8_got_cj;
    v8_msg_t v8_far;
    v34_p2rx_t p2rx;
    uint8_t ibits[V34_P2_PHASES][128];
    int ibits_n[V34_P2_PHASES];
    uint32_t isr[V34_P2_PHASES];
    bool i_collect[V34_P2_PHASES];
    int i_expect;
    v34_probe_t probe;
    bool probing;
    long long probe_from;
    qrx_t q;
    bool p3_training;         /* the receiver is in Phase 3, not a 12.5 resynchronisation */
    double far_tfreq;         /* the source's clock against ours, as Phase 3 found it */
    v34_dec_t dec;
    v34_enc_t b1ref;
    v34_cf_t b1u[256];
    v34_cf_t b1r[256];
    int b1n;
    v34_cf_t rx_gain;
    float rx_energy;
    bool rx_in_data;
    long long quiet_since;

    v34_ccrx_t ccrx;
    bool ccrx_on;
    double cc_tfreq;          /* the far end's control channel clock, as last found */
    uint32_t cc_sr;
    int cc_ones;
    bool cc_in_mph;
    uint8_t cc_mphbuf[V34_MPH1_BITS];
    int cc_mphn;
    bool cc_expect_e;
    bool cc_rx_data;
    bool far_pph, far_shbar;
    long long far_pph_at;     /* control channel symbol it was seen at */
    v34_mph_t mph_far;
    bool have_mph_far;

    /* what has been learnt */
    v34_info0_t info0_far;
    bool have_info0;
    unsigned info0_count;
    bool ack_info0;
    v34_infoh_t infoh;
    long long rtd;
    long long rev_sent;
    int sr;                   /* source to recipient */
    bool high;
    int pe;
    float pr_db;
    int rate;
    float snr_db;
    v34_data_params_t dp;
    bool cc_up_said;
    unsigned pages;
};

/* ------------------------------------------------------------- helpers */

static void emit(fm_v34h_t *v, fm_v34h_event_t ev)
{
    if (v->event != NULL)
        v->event(v->user, ev);
}

static void stage_enter(fm_v34h_t *v, stage_t st, double timeout_s)
{
    FM_TRACE("v34", "%.3f s: '%s' (tag=%s)", v->n / 8000.0, STAGE_NAMES[st], v->tag);
    v->stage = st;
    v->t_stage = v->n;
    v->deadline = (timeout_s > 0.0) ? v->n + (long long) (timeout_s * 8000.0) : 0;
}

static double rtd_s(const fm_v34h_t *v)
{
    return (v->rtd >= 0) ? v->rtd / 8000.0 : 0.5;
}

static float snr_from_mse(float mse)
{
    return 10.0f * log10f(1.0f / (mse + 1e-9f));
}

/* The SNR a data rate needs at a symbol rate: what the 16-state code and a
 * linear equaliser need for an error rate around 1e-6, with a little in
 * hand. As datamodem's. */
static float rate_needs_db(int rate, int sr)
{
    double b = rate / v34_symbol_rate(sr);

    return (float) (10.0 * log10(pow(2.0, b) - 1.0) + 7.0);
}

static int best_rate_for(float snr, int sr, int cap)
{
    int best = 0;

    for (int r = (sr == V34_S2400) ? 2400 : 4800; r <= V34_SR_MAX_RATE[sr] && r <= cap; r += 2400)
        if (snr >= rate_needs_db(r, sr))
            best = r;
    return best;
}

static void fail(fm_v34h_t *v, const char *why)
{
    FM_WARN("v34", "%s (tag=%s)", why, v->tag);
    v->txm = TXM_SILENCE;
    v->q.mode = RQ_OFF;
    v->ccrx_on = false;
    v->rx_in_data = false;
    stage_enter(v, ST_DEAD, 0.0);
    emit(v, FM_V34H_FAILED);
}

static void not_v34(fm_v34h_t *v, const char *why)
{
    FM_INFO("v34", "%s; carrying on as an ordinary G3 call (tag=%s)", why, v->tag);
    v->txm = TXM_SILENCE;
    stage_enter(v, ST_DEAD, 0.0);
    emit(v, FM_V34H_NOT_V34);
}

/* ------------------------------------------------------------------ V.8 */

static int v8_get_bit(void *user)
{
    fm_v34h_t *v = user;
    int b;

    if (v->v8h == v->v8t)
    {
        if (!v->v8_repeat || v->v8msg_len <= 0)
            return 1;
        for (int i = 0; i < v->v8msg_len; i++)
        {
            v->v8q[v->v8t] = v->v8msg[i];
            v->v8t = (v->v8t + 1) % (int) sizeof(v->v8q);
        }
    }
    b = v->v8q[v->v8h];
    v->v8h = (v->v8h + 1) % (int) sizeof(v->v8q);
    return b;
}

static void v8_octet(uint8_t *dst, int *n, int o)
{
    dst[(*n)++] = 0;
    for (int i = 0; i < 8; i++)
        dst[(*n)++] = (uint8_t) ((o >> i) & 1);
    dst[(*n)++] = 1;
}

/* A CM or JM: ten ones, the sync octet 0xE0, then the octets, each least
 * significant bit first between a start and a stop bit. */
static void v8_build_msg(fm_v34h_t *v, const v8_msg_t *m)
{
    uint8_t oct[16];
    int no = v8_build(m, oct, (int) sizeof(oct));
    int n = 0;

    for (int i = 0; i < 10; i++)
        v->v8msg[n++] = 1;
    v8_octet(v->v8msg, &n, 0xE0);
    for (int i = 0; i < no; i++)
        v8_octet(v->v8msg, &n, oct[i]);
    v->v8msg_len = n;
}

static void v8_start_fsk(fm_v34h_t *v, bool repeat)
{
    v->v8h = v->v8t = 0;
    v->v8_repeat = repeat;
    if (v->fsk_tx == NULL)
        v->fsk_tx = fsk_tx_init(NULL, &preset_fsk_specs[v->calling ? FSK_V21CH1 : FSK_V21CH2], v8_get_bit, v);
    else
        fsk_tx_restart(v->fsk_tx, &preset_fsk_specs[v->calling ? FSK_V21CH1 : FSK_V21CH2]);
    if (v->fsk_tx != NULL)
        fsk_tx_power(v->fsk_tx, v->nominal_dbm0);
    v->txm = TXM_V8;
}

/* Two identical messages in a row are what counts (V.8 7.4); three zero
 * octets are CJ. */
static void v8_put_bit(void *user, int bit)
{
    fm_v34h_t *v = user;

    if (bit < 0)
        return;
    v->v8_sr = ((v->v8_sr << 1) | (uint32_t) (bit & 1)) & 0xFFFFFu;
    /* 1111111111 0 00000 111 1, in time order */
    if (v->v8_sr == 0xFFC0Fu)
    {
        if (v->v8_synced && v->v8_rxn > 0)
        {
            if (v->v8_rxn == v->v8_lastn && memcmp(v->v8_rx, v->v8_last, (size_t) v->v8_rxn) == 0 &&
                !v->v8_got_msg)
            {
                v8_msg_t m;

                if (v8_parse(v->v8_rx, v->v8_rxn, &m))
                {
                    v->v8_far = m;
                    v->v8_got_msg = true;
                }
            }
            memcpy(v->v8_last, v->v8_rx, (size_t) v->v8_rxn);
            v->v8_lastn = v->v8_rxn;
        }
        v->v8_synced = true;
        v->v8_rxn = 0;
        v->v8_bitcnt = 0;
        v->v8_zero_octets = 0;
        return;
    }
    if (!v->v8_synced)
        return;
    if (++v->v8_bitcnt < 10)
        return;
    if (((v->v8_sr >> 9) & 1) == 0 && (v->v8_sr & 1) == 1)
    {
        uint8_t o = 0;

        for (int i = 0; i < 8; i++)
            o |= (uint8_t) (((v->v8_sr >> (8 - i)) & 1) << i);
        v->v8_bitcnt = 0;
        if (o == 0)
        {
            if (++v->v8_zero_octets >= 2)
                v->v8_got_cj = true;
        }
        else
        {
            v->v8_zero_octets = 0;
            if (v->v8_rxn < (int) sizeof(v->v8_rx))
                v->v8_rx[v->v8_rxn++] = o;
        }
    }
    else if (v->v8_bitcnt > 20)
    {
        v->v8_synced = false;
    }
}

/* Two band-pass biquads (the RBJ cookbook's, Q 2) around the far end's V.21
 * channel: 980/1180 Hz for the caller's CM, 1650/1850 Hz for the answerer's
 * JM. spandsp's receiver has nothing in front of it, and our own signal
 * comes back through the hybrid: the answerer's own ANSam, still going, was
 * 10 dB stronger than a weak caller's CM and drowned it. About 19 dB off
 * ANSam, 14 off our own CM, half a dB on the wanted tones. */
static void v8_bpf_init(fm_v34h_t *v)
{
    double f0 = v->calling ? 1750.0 : 1080.0;
    double w = 2.0 * PI * f0 / 8000.0, alpha = sin(w) / (2.0 * 2.0), a0 = 1.0 + alpha;

    for (int k = 0; k < 2; k++)
    {
        v->bpf[k][0] = (float) (alpha / a0);
        v->bpf[k][1] = 0.0f;
        v->bpf[k][2] = (float) (-alpha / a0);
        v->bpf[k][3] = (float) (-2.0 * cos(w) / a0);
        v->bpf[k][4] = (float) ((1.0 - alpha) / a0);
        v->bpz[k][0] = v->bpz[k][1] = 0.0f;
    }
}

static float v8_bpf(fm_v34h_t *v, float x)
{
    for (int k = 0; k < 2; k++)
    {
        /* Transposed direct form II */
        float y = v->bpf[k][0] * x + v->bpz[k][0];

        v->bpz[k][0] = v->bpf[k][1] * x - v->bpf[k][3] * y + v->bpz[k][1];
        v->bpz[k][1] = v->bpf[k][2] * x - v->bpf[k][4] * y;
        x = y;
    }
    return x;
}

static void v8_start_rx(fm_v34h_t *v)
{
    v8_bpf_init(v);
    if (v->fsk_rx != NULL)
        fsk_rx_free(v->fsk_rx);
    v->fsk_rx = fsk_rx_init(NULL, &preset_fsk_specs[v->calling ? FSK_V21CH2 : FSK_V21CH1], FSK_FRAME_MODE_ASYNC,
                            v8_put_bit, v);
    if (v->fsk_rx != NULL)
        fsk_rx_signal_cutoff(v->fsk_rx, -45.5f);
    v->v8_synced = false;
    v->v8_lastn = 0;
    v->v8_got_msg = false;
    v->v8_got_cj = false;
}

static void v8_describe(const v8_msg_t *m, char *out, size_t len)
{
    static const char *const fn[8] = { "TBS", "H.324", "V.18", "T.101", "T.30 transmit", "T.30 receive",
                                       "V-series data", "extension" };

    snprintf(out, len, "%s:%s%s%s%s%s%s%s", fn[m->call_function & 7], m->v34hdx ? " V.34hdx" : "",
             m->v34 ? " V.34" : "", m->v17 ? " V.17" : "", m->v29 ? " V.29" : "", m->v27ter ? " V.27ter" : "",
             m->v32 ? " V.32bis" : "", m->v21 ? " V.21" : "");
}

/* What we say in CM or JM. */
static void v8_ours(const fm_v34h_t *v, v8_msg_t *m)
{
    memset(m, 0, sizeof(*m));
    m->call_function = 4;     /* T.30 transmit: the caller sends */
    m->v34hdx = true;
    m->v17 = v->g3_v17;
    m->v29 = v->g3_v29;
    m->v27ter = v->g3_v27ter;
    m->v21 = true;
}

/* --------------------------------------------------------- Phase 2: TX */

static void p2_carrier(fm_v34h_t *v, bool on)
{
    v34_p2tx_carrier(&v->p2tx, on);
    v->txm = TXM_P2;
    v->p2_off_at = -1;
    v->p2_probe_at = -1;
    v->p2_l2_at = -1;
    v->p2_off_after_bits = false;
    if (on)
        v->p2_tone_from = v->ntx;
}

static void p2_send_info(fm_v34h_t *v, const uint8_t *bits, int n)
{
    if (!v->p2tx.on || v->txm != TXM_P2)
        p2_carrier(v, true);
    v34_p2tx_bits(&v->p2tx, bits, n);
}

static void send_info0(fm_v34h_t *v)
{
    v34_info0_t i = { 0 };
    uint8_t bits[V34_INFO0_BITS];
    int n;

    i.sr2743 = (v->sr_allow >> V34_S2743) & 1;
    i.sr2800 = (v->sr_allow >> V34_S2800) & 1;
    i.sr3429 = (v->sr_allow >> V34_S3429) & 1;
    i.low3000 = i.high3000 = (v->sr_allow >> V34_S3000) & 1;
    i.low3200 = i.high3200 = (v->sr_allow >> V34_S3200) & 1;
    i.allow3429 = i.sr3429;
    i.power_reduction = true;
    i.asym_steps = 5;
    i.c1664 = true;
    i.ack = v->have_info0;
    n = v34_info0_pack(&i, bits);
    p2_send_info(v, bits, n);
}

/* At line time t the tone reverses; tail_ms after that the carrier stops,
 * and if probe, L1 and then L2 follow. */
static void p2_reverse(fm_v34h_t *v, long long t, int tail_ms, bool probe)
{
    if (t < v->ntx)
        t = v->ntx;
    v34_p2tx_reverse_at(&v->p2tx, t);
    v->rev_sent = t;
    v->p2_off_at = (tail_ms > 0) ? t + MS(tail_ms) : -1;
    v->p2_probe_at = probe ? v->p2_off_at : -1;
    v->p2_l2_at = probe ? v->p2_off_at + MS(160) : -1;
}

static void info_bits_reset(fm_v34h_t *v, int expect)
{
    for (int k = 0; k < V34_P2_PHASES; k++)
    {
        v->isr[k] = 0;
        v->i_collect[k] = false;
        v->ibits_n[k] = 0;
    }
    v->i_expect = expect;
}

static void p2_start_rx(fm_v34h_t *v)
{
    v34_p2rx_init(&v->p2rx, v->calling);
    info_bits_reset(v, V34_INFO0_BITS);
}

/* ------------------------------------------------------- QAM transmitter */

static v34_cf_t pt4(int rot)
{
    return v34_cc_point(rot);
}

/* The 16-point set: points 0 to 3 of Figure 5 turned clockwise, at unit
 * mean power. */
static v34_cf_t pt16(int idx, int rot)
{
    static const int8_t base[4][2] = { { 1, 1 }, { -3, 1 }, { 1, -3 }, { -3, -3 } };
    int x = base[idx & 3][0], y = base[idx & 3][1];
    int ox, oy;

    switch (rot & 3)
    {
    case 0:
        ox = x;
        oy = y;
        break;
    case 1:
        ox = y;
        oy = -x;
        break;
    case 2:
        ox = -x;
        oy = -y;
        break;
    default:
        ox = -y;
        oy = x;
        break;
    }
    return ((float) ox + I * (float) oy) * 0.31622777f;
}

static v34_cf_t pp_symbol(int k)
{
    int i = k % 48;
    int kk = i / 4, l = i % 4;
    double a = (kk % 3 == 1) ? PI * (kk * l + 4) / 6.0 : PI * kk * l / 6.0;

    return (float) cos(a) + I * (float) sin(a);
}

static void tx_enter(fm_v34h_t *v, tseg_t seg, long long len)
{
    v->seg = seg;
    v->seg_len = len;
    v->seg_count = 0;
    switch (seg)
    {
    case TS_SILENCE:
        /* The modulator takes symbols ahead of the line by its pulse's
         * half-span; the last one has gone out that much later. */
        if (len < 0)
            v->qam_quiet_at = v->ntx + (long long) ((2 * V34_QTX_L + 4) * 8000.0 / v34_symbol_rate(v->sr));
        break;
    case TS_TRN:
        /* 10.1.3.8: the scrambler starts from zero. */
        v->tscr = 0;
        break;
    case TS_DATA:
    {
        double ex, exn;

        /* 10.1.3.1: B1 starts the scrambler, the trellis and differential
         * encoders and the precoder afresh. The Note to 10.1.3: data goes
         * out at the power training did. */
        v34_enc_init(&v->enc, &v->dp, v->scr_tap);
        v34_data_energy(&v->dp, &ex, &exn);
        v34_enc_set_energy(&v->enc, ex);
        v->data_scale = (float) (1.0 / sqrt(exn));
        v->dpos = 8;
        v->tx_data = false;
        v->pc_end = false;
        break;
    }
    default:
        break;
    }
}

static void tx_set(fm_v34h_t *v, tseg_t seg, long long len)
{
    v->nprog = v->iprog = 0;
    tx_enter(v, seg, len);
}

static void tx_then(fm_v34h_t *v, tseg_t seg, long long len)
{
    if (v->nprog < PROG_MAX)
    {
        v->prog[v->nprog].seg = seg;
        v->prog[v->nprog].len = len;
        v->nprog++;
    }
}

static int tx_bit(fm_v34h_t *v, int bit)
{
    return v34_scramble(&v->tscr, v->scr_tap, bit);
}

static int get_data_bit(void *user)
{
    fm_v34h_t *v = user;
    int b;

    if (!v->tx_data || v->pc_end || v->pc_get_bit == NULL)
        return 1;
    b = v->pc_get_bit(v->user);
    if (b < 0)
    {
        /* 12.5.3.1: the page is out. Scrambled ones for 35 ms, then the
         * control channel. */
        v->pc_end = true;
        return 1;
    }
    return b & 1;
}

static v34_cf_t tx_symbol(void *user)
{
    fm_v34h_t *v = user;
    v34_cf_t s = 0.0f;

    while (v->seg_len >= 0 && v->seg_count >= v->seg_len)
    {
        if (v->iprog < v->nprog)
        {
            step_t st = v->prog[v->iprog++];

            tx_enter(v, (tseg_t) st.seg, st.len);
        }
        else
        {
            tx_enter(v, TS_SILENCE, -1);
        }
    }
    switch (v->seg)
    {
    case TS_SILENCE:
        break;
    /* 10.1.3.7: S alternates point 0 and point 0 turned a quarter
     * anticlockwise, ending on the latter; S-bar is S turned half way round. */
    case TS_S:
        s = pt4((v->seg_count & 1) ? 3 : 0);
        break;
    case TS_SBAR:
        s = pt4((v->seg_count & 1) ? 1 : 2);
        break;
    case TS_PP:
        s = pp_symbol((int) v->seg_count);
        break;
    case TS_TRN:
    {
        int i1 = tx_bit(v, 1), i2 = tx_bit(v, 1);
        int rot = i1 + 2 * i2;

        if (v->tx16)
        {
            int q1 = tx_bit(v, 1), q2 = tx_bit(v, 1);

            s = pt16(q1 + 2 * q2, rot);
        }
        else
        {
            s = pt4(rot);
        }
        v->trn_symbols++;
        break;
    }
    case TS_DATA:
        if (v->dpos >= 8)
        {
            bool b1 = v->enc.i < v->enc.f.P;

            if (v->pc_end)
            {
                if (v->ones_frames-- <= 0)
                {
                    /* The ones are out: silence, and the control channel. */
                    v->seg_len = v->seg_count;
                    v->nprog = v->iprog = 0;
                    tx_enter(v, TS_SILENCE, -1);
                    v->seg_count++;
                    return 0.0f;
                }
            }
            else if (!b1 && !v->tx_data)
            {
                v->tx_data = true;
                v->pages++;
                emit(v, FM_V34H_PRIMARY_UP);
            }
            v34_enc_frame(&v->enc, b1 ? NULL : get_data_bit, v, NULL, NULL, v->dbuf);
            v->dpos = 0;
        }
        s = v->dbuf[v->dpos++] * v->data_scale;
        break;
    }
    v->seg_count++;
    return s;
}

static void qtx_start(fm_v34h_t *v)
{
    double p = v->nominal_dbm0 - v->pr_db;

    v34_qtx_free(&v->qtx);
    v34_qtx_init(&v->qtx, v->sr, v->high, v->pe, p, tx_symbol, v);
    v->qtx_on = true;
    v->txm = TXM_QAM;
}

/* --------------------------------------------- control channel transmitter */

static void cc_enter(fm_v34h_t *v, cseg_t seg, long long len)
{
    v->cseg = seg;
    v->clen = len;
    v->ccount = 0;
    switch (seg)
    {
    case CS_ALT:
        /* 10.2.4.2 */
        v->cscr = 0;
        v->alt_since = v->cctx.k;
        break;
    case CS_MPH:
        v->mph_pos = 0;
        /* 12.4.1.3, 12.4.2.4: with the far end's MPh already in, this one
         * is the last. */
        if (v->have_mph_far)
            v->mph_finish = true;
        break;
    case CS_E:
        v->e_pos = 0;
        break;
    case CS_DATA:
        v->cc_tx_data = true;
        break;
    default:
        break;
    }
}

static void cc_set(fm_v34h_t *v, cseg_t seg, long long len)
{
    v->cnp = v->cip = 0;
    cc_enter(v, seg, len);
}

static void cc_then(fm_v34h_t *v, cseg_t seg, long long len)
{
    if (v->cnp < PROG_MAX)
    {
        v->cprog[v->cnp].seg = seg;
        v->cprog[v->cnp].len = len;
        v->cnp++;
    }
}

static v34_cf_t cc_diff(fm_v34h_t *v, int b0, int b1)
{
    int i1 = v34_scramble(&v->cscr, v->scr_tap, b0), i2 = v34_scramble(&v->cscr, v->scr_tap, b1);

    v->cz = (v->cz + i1 + 2 * i2) & 3;
    return pt4(v->cz);
}

static int cc_data_bit(fm_v34h_t *v)
{
    int b = (v->cc_get_bit != NULL) ? v->cc_get_bit(v->user) : 1;

    return (b < 0) ? 1 : (b & 1);
}

static v34_cf_t cc_symbol(void *user)
{
    fm_v34h_t *v = user;
    v34_cf_t s = 0.0f;
    long long i;

    while (v->clen >= 0 && v->ccount >= v->clen)
    {
        if (v->cip < v->cnp)
        {
            step_t st = v->cprog[v->cip++];

            cc_enter(v, (cseg_t) st.seg, st.len);
        }
        else
        {
            cc_enter(v, CS_SILENCE, -1);
        }
    }
    i = v->ccount++;
    switch (v->cseg)
    {
    case CS_SILENCE:
    case CS_OFF:
        break;
    case CS_PPH:
        s = v34_pph((int) i);
        break;
    /* 10.2.3.3: Sh as S, on the control channel's carrier. */
    case CS_SH:
        s = pt4((i & 1) ? 3 : 0);
        break;
    case CS_SHB:
        s = pt4((i & 1) ? 1 : 2);
        break;
    case CS_AC:
        s = pt4((i & 1) ? 2 : 0);
        break;
    case CS_ALT:
        s = cc_diff(v, 0, 1);
        break;
    case CS_MPH:
    {
        int b0 = v->mphbits[v->mph_pos++];
        int b1 = (v->mph_pos < v->mph_len) ? v->mphbits[v->mph_pos++] : 0;

        s = cc_diff(v, b0, b1);
        if (v->mph_pos >= v->mph_len)
        {
            v->mph_pos = 0;
            if (v->mph_finish)
            {
                /* 12.4.1.3, 12.4.2.4: the current MPh is complete; one E,
                 * then data. */
                v->mph_finish = false;
                v->clen = v->ccount;
                v->cnp = v->cip = 0;
                cc_then(v, CS_E, 10);
                cc_then(v, CS_DATA, -1);
            }
        }
        break;
    }
    case CS_E:
        s = cc_diff(v, 1, 1);
        break;
    case CS_ONES:
        s = cc_diff(v, 1, 1);
        break;
    case CS_DATA:
    {
        int b0 = cc_data_bit(v), b1 = cc_data_bit(v);

        s = cc_diff(v, b0, b1);
        break;
    }
    }
    return s;
}

static void cctx_start(fm_v34h_t *v)
{
    if (!v->cctx_ready)
    {
        v34_cctx_init(&v->cctx, !v->calling, v->nominal_dbm0, cc_symbol, v);
        v->cctx_ready = true;
    }
    v34_cctx_on(&v->cctx, true);
    v->txm = TXM_CC;
}

static void build_mph(fm_v34h_t *v)
{
    v34_mph_t m = { 0 };
    int cap = (v->max_rate < V34_SR_MAX_RATE[v->sr]) ? v->max_rate : V34_SR_MAX_RATE[v->sr];

    if (!v->source)
    {
        int want = best_rate_for(v->snr_db, v->sr, cap);

        cap = (want > 0) ? want : ((v->sr == V34_S2400) ? 2400 : 4800);
    }
    /* Note 1 to Tables 23 and 24 */
    if (!v->info0_far.c1664 && cap > 28800)
        cap = 28800;
    m.type = 0;
    m.max_rate = cap / 2400;
    m.cc2400 = false;
    m.trellis = 0;
    m.nonlinear = false;
    m.expanded = false;
    for (int r = 2400; r <= 33600; r += 2400)
        if (r <= v->max_rate)
            m.rate_mask |= 1u << (r / 2400 - 1);
    m.asymmetric_cc = false;
    v->mph_len = v34_mph_pack(&m, v->mphbits);
    FM_DEBUG("v34", "MPh out: up to %d bit/s, control channel 1200 bit/s (tag=%s)", cap, v->tag);
}

/* ------------------------------------------------------------ QAM receiver */

static double pulse(double t, double a, double span)
{
    if (fabs(t) >= span)
        return 0.0;
    return v34_rrc(t, a) * 0.5 * (1.0 + cos(PI * t / span));
}

static void qrx_free(qrx_t *q)
{
    free(q->cosv);
    free(q->sinv);
    free(q->P);
    q->cosv = q->sinv = NULL;
    q->P = NULL;
}

static int gcd_i(int a, int b)
{
    while (b)
    {
        int t = a % b;

        a = b;
        b = t;
    }
    return a;
}

static bool qrx_start(fm_v34h_t *v, int sr, bool high)
{
    qrx_t *q = &v->q;
    int a = V34_SR_A[sr], c = V34_SR_C[sr];
    int d = V34_CAR_D[sr][high], e = V34_CAR_E[sr][high];
    const double span = 8.0;
    double se = 0.0;
    int g;

    qrx_free(q);
    memset(q, 0, sizeof(*q));
    q->sr = sr;
    q->high = high;
    q->sps = 8000.0 / v34_symbol_rate(sr);
    q->th = q->sps / 2.0;
    q->cnum = 3 * a * d;
    q->cden = 10 * c * e;
    g = gcd_i(q->cnum, q->cden);
    q->cnum /= g;
    q->cden /= g;
    q->cosv = calloc((size_t) q->cden, sizeof(float));
    q->sinv = calloc((size_t) q->cden, sizeof(float));
    q->P = calloc(NEQ * NEQ, sizeof(double complex));
    if (q->cosv == NULL || q->sinv == NULL || q->P == NULL)
        return false;
    for (int i = 0; i < q->cden; i++)
    {
        q->cosv[i] = (float) cos(2.0 * PI * i / q->cden);
        q->sinv[i] = (float) sin(2.0 * PI * i / q->cden);
    }
    q->mf_half = (int) ceil(span * q->sps);
    if (q->mf_half > MF_MAX)
        q->mf_half = MF_MAX;
    for (int n = -q->mf_half; n <= q->mf_half; n++)
    {
        double p = pulse(n / q->sps, 0.10, span);

        se += p * p;
    }
    for (int p = 0; p <= MF_PHASES; p++)
        for (int j = -q->mf_half; j <= q->mf_half; j++)
            q->hr[p][j + q->mf_half] =
                (float) (pulse(((double) p / MF_PHASES - j) / q->sps, 0.10, span) / sqrt(q->sps * se));
    q->tau = (double) v->n + 2.0 * q->mf_half + 4.0;
    q->mode = RQ_HUNT;
    q->lpos = NEQ;
    return true;
}

static v34_cf_t mf_at(const qrx_t *q, double tau)
{
    long long n0 = (long long) floor(tau);
    int p = (int) lrint((tau - (double) n0) * MF_PHASES);
    const float *h = q->hr[p];
    v34_cf_t y = 0.0f;

    for (int j = -q->mf_half; j <= q->mf_half; j++)
        y += q->z[(n0 + j) & ZMASK] * h[j + q->mf_half];
    return y;
}

static void rls_init(qrx_t *q, double delta)
{
    memset(q->P, 0, sizeof(double complex) * NEQ * NEQ);
    for (int i = 0; i < NEQ; i++)
        q->P[i * NEQ + i] = 1.0 / delta;
}

/* Recursive least squares, for the output y = sum c w. */
static void rls_update(qrx_t *q, const v34_cf_t *w, v34_cf_t err)
{
    double complex pu[NEQ];
    double complex g[NEQ];
    double den = q->lambda;

    for (int i = 0; i < NEQ; i++)
    {
        double complex s = 0.0;

        for (int j = 0; j < NEQ; j++)
            s += q->P[i * NEQ + j] * w[j];
        pu[i] = s;
    }
    for (int i = 0; i < NEQ; i++)
        den += creal(conj(w[i]) * pu[i]);
    for (int i = 0; i < NEQ; i++)
        g[i] = pu[i] / den;
    for (int i = 0; i < NEQ; i++)
        q->c[i] += (float complex) (conj(g[i]) * err);
    for (int i = 0; i < NEQ; i++)
        for (int j = 0; j < NEQ; j++)
            q->P[i * NEQ + j] = (q->P[i * NEQ + j] - g[i] * conj(pu[j])) / q->lambda;
}

static void rx_symbol(fm_v34h_t *v, const v34_cf_t *w, v34_cf_t vout, v34_cf_t u);

static void eq_half(fm_v34h_t *v, v34_cf_t y, long long idx)
{
    qrx_t *q = &v->q;
    const v34_cf_t *w;
    v34_cf_t vout = 0.0f;
    v34_cf_t u;

    q->lpos = (q->lpos == 0) ? NEQ - 1 : q->lpos - 1;
    q->line[q->lpos] = y;
    q->line[q->lpos + NEQ] = y;
    if (idx < q->sym0 || ((idx - q->sym0) & 1))
        return;
    w = &q->line[q->lpos];
    for (int i = 0; i < NEQ; i++)
        vout += q->c[i] * w[i];
    u = vout * (cosf(q->theta) - I * sinf(q->theta));
    rx_symbol(v, w, vout, u);
    q->k++;
    q->tau += q->tfreq;
}

/* The far end's symbol clock against ours: the error a timing offset makes
 * is the output's slope times the offset. As datamodem's. */
static void timing_track(qrx_t *q, const v34_cf_t *w, v34_cf_t vout, v34_cf_t err)
{
    v34_cf_t older = 0.0f;
    v34_cf_t slope;
    float sp, e;

    for (int i = 0; i < NEQ - 1; i++)
        older += q->c[i] * w[i + 1];
    slope = vout - older;
    sp = crealf(slope) * crealf(slope) + cimagf(slope) * cimagf(slope);
    if (sp < 1e-6f)
        return;
    e = crealf(conjf(err) * slope) / sp;
    if (e > 0.5f)
        e = 0.5f;
    else if (e < -0.5f)
        e = -0.5f;
    {
        double kp = (q->mode == RQ_DATA) ? 0.004 : 0.008;

        q->tfreq += kp * kp / 4.0 * e * q->th;
        q->tau += kp * e * q->th;
    }
    if (q->tfreq > 0.0005 * q->sps)
        q->tfreq = 0.0005 * q->sps;
    else if (q->tfreq < -0.0005 * q->sps)
        q->tfreq = -0.0005 * q->sps;
}

/* Phase and equaliser update towards d - what was sent, or a decision. */
static void track(fm_v34h_t *v, const v34_cf_t *w, v34_cf_t vout, v34_cf_t u, v34_cf_t d)
{
    qrx_t *q = &v->q;
    float dd = crealf(d) * crealf(d) + cimagf(d) * cimagf(d);
    float pe = (dd > 0.0f) ? cimagf(u * conjf(d)) / dd : 0.0f;
    v34_cf_t err = d * (cosf(q->theta) + I * sinf(q->theta)) - vout;
    v34_cf_t ue = u - d;

    if (q->pwr < 0.3f * q->pwr_ref || q->hold > 0)
        return;
    {
        float e2 = (crealf(ue) * crealf(ue) + cimagf(ue) * cimagf(ue)) / (dd > 0 ? dd : 1.0f);

        q->mse += 0.01f * (e2 - q->mse);
        q->mse_fast += 0.05f * (e2 - q->mse_fast);
        if (q->mode != RQ_TRAIN && e2 > 0.15f)
        {
            float uu = crealf(u) * crealf(u) + cimagf(u) * cimagf(u);

            if (uu > 0.5f * dd && uu < 2.0f * dd)
            {
                q->theta += q->a1 * pe;
                if (q->theta > PI)
                    q->theta -= (float) (2.0 * PI);
                else if (q->theta < -PI)
                    q->theta += (float) (2.0 * PI);
            }
            return;
        }
    }
    if (q->mode == RQ_TRAIN)
    {
        if (q->lambda > 0.0)
            rls_update(q, w, err);
        if (q->k < 96)
            return;
    }
    q->theta += q->a1 * pe + q->nu;
    q->nu += q->a2 * pe;
    if (q->nu > 0.003f)
        q->nu = 0.003f;
    else if (q->nu < -0.003f)
        q->nu = -0.003f;
    if (q->theta > PI)
        q->theta -= (float) (2.0 * PI);
    else if (q->theta < -PI)
        q->theta += (float) (2.0 * PI);
    if (q->mode == RQ_TRAIN)
    {
        if (q->k >= 288)
            timing_track(q, w, vout, err);
        if (q->lambda > 0.0)
            return;
    }
    else
    {
        timing_track(q, w, vout, err);
    }
    {
        float norm = 0.0f;
        v34_cf_t g;

        for (int i = 0; i < NEQ; i++)
            norm += crealf(w[i]) * crealf(w[i]) + cimagf(w[i]) * cimagf(w[i]);
        g = q->beta * err / (norm + 1e-6f);
        for (int i = 0; i < NEQ; i++)
            q->c[i] += g * conjf(w[i]);
    }
}

static void rx_half(fm_v34h_t *v, v34_cf_t y);

static void qrx_sample(fm_v34h_t *v, float e, long long n)
{
    qrx_t *q = &v->q;
    int ph = (int) ((n * q->cnum) % q->cden);

    q->z[n & ZMASK] = (2.0f * e / (float) V34_DBM0_RMS) * (q->cosv[ph] - I * q->sinv[ph]);
    while (q->tau + q->mf_half <= (double) n)
    {
        v34_cf_t y = mf_at(q, q->tau);

        q->tau += q->th;
        rx_half(v, y);
        if (q->mode == RQ_OFF)
            return;
    }
}

static void p3_heard_sbar(fm_v34h_t *v);

/* The source's S (10.1.3.7) arrives with nothing trained: it has period 2T,
 * so each T/2 sample matches the one four before; and S-bar is S turned
 * round, so across the boundary they are opposite. */
static void hunt_half(fm_v34h_t *v, long long idx)
{
    qrx_t *q = &v->q;
    v34_cf_t r = q->h[idx & HMASK];
    v34_cf_t r4, prod;
    float p;

    if (idx < 8)
        return;
    r4 = q->h[(idx - 4) & HMASK];
    prod = r * conjf(r4);
    p = 0.5f * (crealf(r) * crealf(r) + cimagf(r) * cimagf(r) + crealf(r4) * crealf(r4) + cimagf(r4) * cimagf(r4));
    q->hq += 0.1f * (prod - q->hq);
    q->hp += 0.1f * (p - q->hp);
    if (q->hp > 1e-4f && crealf(q->hq) > 0.75f * q->hp)
    {
        if (++q->s_run >= 40 && !q->s_seen)
        {
            q->s_seen = true;
            q->pwr_ref = q->hp;
            FM_DEBUG("v34", "S heard (tag=%s)", v->tag);
        }
    }
    else
    {
        q->s_run = 0;
    }
    if (!q->s_seen)
        return;
    q->prod3[idx % 3] = crealf(prod);
    q->p3[idx % 3] = p;
    if (q->prod3[0] + q->prod3[1] + q->prod3[2] < -0.5f * (q->p3[0] + q->p3[1] + q->p3[2]) &&
        p > 0.2f * q->pwr_ref)
    {
        if (++q->neg_run == 1)
        {
            q->s_bar_at = idx - 2;
            q->mode = RQ_ALIGN;
            p3_heard_sbar(v);
        }
    }
    else
    {
        q->neg_run = 0;
    }
}

/* PP (10.1.3.6) begins 16 symbols after S-bar does. Correlating against its
 * first two periods finds the sample its first symbol lands on. */
static void align_pp(fm_v34h_t *v)
{
    qrx_t *q = &v->q;
    long long c0 = q->s_bar_at + 32;
    double best = -1.0;
    long long bc = c0;
    v34_cf_t bC = 0.0f;
    double pw = 0.0;
    double rho;

    for (long long c = c0 - 40; c <= c0 + 40; c++)
    {
        v34_cf_t C = 0.0f;

        for (int k = 0; k < 96; k++)
            C += q->h[(c + 2 * k) & HMASK] * conjf(pp_symbol(k));
        if (cabsf(C) > best)
        {
            best = cabsf(C);
            bc = c;
            bC = C;
        }
    }
    for (int k = 0; k < 96; k++)
    {
        v34_cf_t r = q->h[(bc + 2 * k) & HMASK];

        pw += crealf(r) * crealf(r) + cimagf(r) * cimagf(r);
    }
    rho = best / sqrt(96.0 * pw + 1e-12);
    if (rho < 0.5)
    {
        FM_DEBUG("v34", "PP is not where S-bar put it (correlation %.2f); listening again (tag=%s)", rho, v->tag);
        q->mode = RQ_HUNT;
        q->s_seen = false;
        q->s_run = q->neg_run = 0;
        return;
    }
    q->sym0 = bc + EQ_D;
    q->eq_next = bc + EQ_D - NEQ + 1;
    memset(q->c, 0, sizeof(q->c));
    memset(q->line, 0, sizeof(q->line));
    q->lpos = NEQ;
    q->c[EQ_D] = 96.0f / bC;
    q->k = 0;
    q->lambda = 0.995;
    rls_init(q, 0.02 * pw / 96.0 + 1e-6);
    q->theta = q->nu = 0.0f;
    q->a1 = 0.02f;
    q->a2 = 0.0002f;
    q->beta = 0.01f;
    q->mse = q->mse_fast = 1.0f;
    q->mode = RQ_TRAIN;
    q->trn_scr = 0;
    q->trn_err = 0.0;
    q->trn_err_n = 0;
    FM_DEBUG("v34", "PP found (correlation %.2f); training (tag=%s)", rho, v->tag);
}

static void rx_half(fm_v34h_t *v, v34_cf_t y)
{
    qrx_t *q = &v->q;
    long long idx = q->nh++;

    q->h[idx & HMASK] = y;
    q->pwr += 0.02f * (crealf(y) * crealf(y) + cimagf(y) * cimagf(y) - q->pwr);
    q->pwr_q += 0.3f * (crealf(y) * crealf(y) + cimagf(y) * cimagf(y) - q->pwr_q);
    if (q->pwr < 0.3f * q->pwr_ref || q->pwr_q < 0.05f * q->pwr_ref)
        q->hold = NEQ + 8;
    else if (q->hold > 0)
        q->hold--;
    switch (q->mode)
    {
    case RQ_HUNT:
        hunt_half(v, idx);
        break;
    case RQ_ALIGN:
        if (idx >= q->s_bar_at + 32 + 40 + 2 * 96 + 2)
        {
            align_pp(v);
            while (q->mode == RQ_TRAIN && q->eq_next <= idx)
            {
                eq_half(v, q->h[q->eq_next & HMASK], q->eq_next);
                q->eq_next++;
            }
        }
        break;
    case RQ_OFF:
        break;
    default:
        if (q->eq_next <= idx)
        {
            eq_half(v, y, idx);
            q->eq_next = idx + 1;
        }
        break;
    }
}

/* ----------------------------------------------------------- data mode */

static void dec_put_bit(void *user, int bit)
{
    fm_v34h_t *v = user;

    if (v->rx_in_data && v->pc_put_bit != NULL)
        v->pc_put_bit(v->user, bit);
}

static void start_b1(fm_v34h_t *v)
{
    double ex, exn;

    v34_dec_init(&v->dec, &v->dp, v->dscr_tap, dec_put_bit, v);
    v34_enc_init(&v->b1ref, &v->dp, v->dscr_tap);
    v34_data_energy(&v->dp, &ex, &exn);
    v->rx_energy = (float) exn;
    v->rx_gain = sqrtf(v->rx_energy);
    v->b1n = 0;
    v->q.mode = RQ_B1;
    v->q.beta = 0.005f;
    v->q.a1 = 0.03f;
    v->q.a2 = 0.0005f;
    v->q.lambda = 0.0;
}

static void rx_data_symbol(fm_v34h_t *v, const v34_cf_t *w, v34_cf_t vout, v34_cf_t u)
{
    qrx_t *q = &v->q;

    if (q->mode == RQ_B1)
    {
        /* B1 is one data frame of scrambled ones from a fresh encoder
         * (10.1.3.1), so it is known before it arrives, and gives the scale
         * and phase of the source's data constellation exactly. */
        int nb = 8 * v->dec.f.P;

        if (v->b1n % 8 == 0)
            v34_enc_frame(&v->b1ref, NULL, NULL, NULL, NULL, &v->b1r[v->b1n]);
        v->b1u[v->b1n++] = u;
        if (v->b1n >= nb)
        {
            v34_cf_t num = 0.0f;
            float den = 0.0f, res = 0.0f, ref = 0.0f;
            v34_cf_t g;

            for (int i = 0; i < nb; i++)
            {
                num += v->b1r[i] * conjf(v->b1u[i]);
                den += crealf(v->b1u[i]) * crealf(v->b1u[i]) + cimagf(v->b1u[i]) * cimagf(v->b1u[i]);
            }
            g = num / (den + 1e-9f);
            for (int i = 0; i < nb; i++)
            {
                v34_cf_t e = g * v->b1u[i] - v->b1r[i];

                res += crealf(e) * crealf(e) + cimagf(e) * cimagf(e);
                ref += crealf(v->b1r[i]) * crealf(v->b1r[i]) + cimagf(v->b1r[i]) * cimagf(v->b1r[i]);
            }
            if (res < 0.1f * ref && fabsf(cabsf(g) / sqrtf(v->rx_energy) - 1.0f) < 0.3f)
            {
                v->rx_gain = g;
                FM_DEBUG("v34", "B1 as expected: gain %.3f of nominal, phase %.1f degrees, SNR %.1f dB (tag=%s)",
                         cabsf(g) / sqrtf(v->rx_energy), carg(g) * 180.0 / PI, 10.0 * log10(ref / (res + 1e-9)),
                         v->tag);
            }
            else
            {
                FM_INFO("v34", "B1 is not what was expected (residual %.1f dB, gain %.2f of nominal); carrying "
                               "on at the nominal scale (tag=%s)",
                        10.0 * log10(res / (ref + 1e-9)), cabsf(g) / sqrtf(v->rx_energy), v->tag);
            }
            for (int i = 0; i < nb; i++)
                v34_dec_symbol(&v->dec, v->b1u[i] * v->rx_gain);
            q->mode = RQ_DATA;
            v->rx_in_data = true;
            v->pages++;
            FM_DEBUG("v34", "primary channel: receiving at %d bit/s (tag=%s)", v->rate, v->tag);
            emit(v, FM_V34H_PRIMARY_UP);
        }
        return;
    }
    {
        v34_cf_t y = u * v->rx_gain;
        v34_cf_t d = v34_dec_slice(&v->dec, y);

        track(v, w, vout, u, d / v->rx_gain);
        v34_dec_symbol(&v->dec, y);
    }
}

static void p3_done(fm_v34h_t *v);

static void rx_symbol(fm_v34h_t *v, const v34_cf_t *w, v34_cf_t vout, v34_cf_t u)
{
    qrx_t *q = &v->q;

    switch (q->mode)
    {
    case RQ_TRAIN:
    {
        v34_cf_t d;
        long long trn_end = 288 + (v->p3_training ? v->trn_symbols : 0);

        if (q->k < 288)
        {
            d = pp_symbol((int) q->k);
        }
        else
        {
            int i1 = v34_scramble(&q->trn_scr, v->dscr_tap, 1);
            int i2 = v34_scramble(&q->trn_scr, v->dscr_tap, 1);

            d = pt4(i1 + 2 * i2);
            /* After a while least squares has done its work; known symbols
             * still guide a gentler update through the rest of TRN. */
            if (q->k == 288 + 480)
            {
                q->lambda = 0.0;
                q->beta = 0.01f;
                q->a1 = 0.05f;
                q->a2 = 0.0004f;
            }
            /* The last stretch of TRN, for the SNR. */
            if (q->k >= trn_end - 400)
            {
                v34_cf_t e = u - d;

                q->trn_err += crealf(e) * crealf(e) + cimagf(e) * cimagf(e);
                q->trn_err_n++;
            }
        }
        track(v, w, vout, u, d);
        if (q->k + 1 >= trn_end)
        {
            if (v->p3_training)
            {
                q->mode = RQ_OFF;
                p3_done(v);
            }
            else
            {
                /* 12.5.2: resynchronised on PP; B1 comes next. */
                start_b1(v);
            }
        }
        break;
    }
    case RQ_B1:
    case RQ_DATA:
        rx_data_symbol(v, w, vout, u);
        break;
    default:
        break;
    }
}

/* ---------------------------------------------------------- Phase 3 and on */

static long long trn_symbols_for(const v34_infoh_t *h)
{
    return (long long) llround(h->trn_len * 0.035 * v34_symbol_rate(h->sr));
}

/* 12.3.1: the source, after INFOh: silence, S, S-bar, PP, TRN. */
static void phase3_source(fm_v34h_t *v)
{
    double S = v34_symbol_rate(v->sr);

    v->trn_symbols = 0;
    v->tx16 = v->infoh.trn16;
    qtx_start(v);
    tx_set(v, TS_SILENCE, (long long) (0.07 * S));
    tx_then(v, TS_S, 128);
    tx_then(v, TS_SBAR, 16);
    tx_then(v, TS_PP, 288);
    tx_then(v, TS_TRN, trn_symbols_for(&v->infoh));
    stage_enter(v, ST_P3_SRC, 0.0);
    FM_DEBUG("v34", "Phase 3: S, S-bar, PP, %lld symbols of TRN (tag=%s)", trn_symbols_for(&v->infoh), v->tag);
}

static void p3_heard_sbar(fm_v34h_t *v)
{
    FM_DEBUG("v34", "S-bar heard; training on PP (tag=%s)", v->tag);
}

/* Listen on the control channel, with everything about the bit stream
 * forgotten; what the receiver learnt of the channel is kept. */
static void ccrx_listen(fm_v34h_t *v, bool fresh)
{
    if (fresh || !v->ccrx_on)
    {
        v34_ccrx_init(&v->ccrx, v->calling, v->nominal_dbm0);
        /* The far end's clock is the same clock every time. */
        v->ccrx.tfreq = v->cc_tfreq;
    }
    else
    {
        v34_ccrx_reset_sync(&v->ccrx);
    }
    v->ccrx_on = true;
    v->cc_sr = 0;
    v->cc_ones = 0;
    v->cc_in_mph = false;
    v->cc_mphn = 0;
    v->cc_expect_e = false;
    v->cc_rx_data = false;
    v->far_pph = v->far_shbar = false;
}

/* 12.4.1.1: the source, after TRN: 70 ms of silence, then PPh and ALT. */
static void cc_start_source(fm_v34h_t *v)
{
    ccrx_listen(v, true);
    v->have_mph_far = false;
    v->cc_tx_data = false;
    v->mph_finish = false;
    build_mph(v);
    cctx_start(v);
    cc_set(v, CS_SILENCE, CC_SYMS(0.07));
    cc_then(v, CS_PPH, 32);
    cc_then(v, CS_ALT, -1);
    stage_enter(v, ST_CC_START, 3.0 + 2.0 * rtd_s(v));
}

/* 12.4.2.1: the recipient, after TRN, waits for the source's PPh. */
static void cc_start_recipient(fm_v34h_t *v)
{
    ccrx_listen(v, true);
    v->have_mph_far = false;
    v->cc_tx_data = false;
    v->mph_finish = false;
    build_mph(v);
    v->txm = TXM_SILENCE;
    stage_enter(v, ST_CC_START, 3.0);
}

static void p3_done(fm_v34h_t *v)
{
    qrx_t *q = &v->q;

    /* The source's clock is the same clock on every page. */
    v->far_tfreq = q->tfreq;
    if (q->trn_err_n > 50)
        v->snr_db = snr_from_mse((float) (q->trn_err / q->trn_err_n));
    else
        v->snr_db = snr_from_mse(q->mse);
    FM_DEBUG("v34", "Phase 3 training done: SNR %.1f dB, the far clock %+.1f ppm (tag=%s)", v->snr_db,
             -q->tfreq / q->sps * 1e6, v->tag);
    cc_start_recipient(v);
}

/* 12.4.1.3, 12.4.2.4: the primary channel's rate, the largest both MPh
 * sequences allow. */
static bool settle_rate(fm_v34h_t *v)
{
    v34_mph_t mine;
    unsigned mask;
    int lim;

    v34_mph_unpack(v->mphbits, v->mph_len, &mine);
    mask = mine.rate_mask & v->mph_far.rate_mask;
    lim = mine.max_rate < v->mph_far.max_rate ? mine.max_rate : v->mph_far.max_rate;
    v->rate = 0;
    for (int r = 2400; r <= 2400 * lim; r += 2400)
        if (mask & (1u << (r / 2400 - 1)))
            v->rate = r;
    if (v->rate > V34_SR_MAX_RATE[v->sr])
        v->rate = V34_SR_MAX_RATE[v->sr];
    if (v->rate == 2400 && v->sr != V34_S2400)
        v->rate = 0;
    if (v->rate <= 0)
        return false;
    memset(&v->dp, 0, sizeof(v->dp));
    v->dp.sr = v->sr;
    v->dp.rate = v->rate;
    if (v->source)
    {
        /* The recipient's receiver chooses; precoding coefficients only in a
         * type 1 MPh. */
        v->dp.trellis = (v->mph_far.trellis <= 2) ? 16 << v->mph_far.trellis : 16;
        v->dp.expanded = v->mph_far.expanded;
        v->dp.nonlinear = v->mph_far.nonlinear;
        if (v->mph_far.type == 1)
            memcpy(v->dp.h, v->mph_far.h, sizeof(v->dp.h));
    }
    else
    {
        v->dp.trellis = 16 << mine.trellis;
        v->dp.expanded = mine.expanded;
    }
    return true;
}

static void heard_mph(fm_v34h_t *v, const v34_mph_t *m)
{
    if (!v->have_mph_far)
        FM_DEBUG("v34", "MPh heard: up to %d bit/s, %d-state trellis%s%s%s, control channel %d bit/s (tag=%s)",
                 m->max_rate * 2400, 16 << m->trellis, m->expanded ? ", expanded shaping" : "",
                 m->nonlinear ? ", non-linear encoding" : "", m->type == 1 ? ", precoding" : "",
                 m->cc2400 ? 2400 : 1200, v->tag);
    v->mph_far = *m;
    v->have_mph_far = true;
    /* 12.4.1.3, 12.4.2.4: E once ours is complete. */
    if (v->cseg == CS_MPH && !v->mph_finish && !v->cc_tx_data)
        v->mph_finish = true;
    v->cc_expect_e = true;
}

static void cc_up_check(fm_v34h_t *v)
{
    if (v->cc_tx_data && v->cc_rx_data && !v->cc_up_said)
    {
        v->cc_up_said = true;
        stage_enter(v, ST_CC_DATA, 0.0);
        FM_DEBUG("v34", "control channel up (tag=%s)", v->tag);
        emit(v, FM_V34H_CC_UP);
    }
}

/* One bit from the far end's control channel, descrambled. */
static void cc_rx_bit(fm_v34h_t *v, int b, bool symbol_end)
{
    if (v->cc_rx_data)
    {
        if (v->cc_put_bit != NULL)
            v->cc_put_bit(v->user, b);
        return;
    }
    v->cc_sr = (v->cc_sr << 1) | (uint32_t) b;
    v->cc_ones = b ? v->cc_ones + 1 : 0;
    if (v->cc_in_mph)
    {
        v->cc_mphbuf[v->cc_mphn++] = (uint8_t) b;
        if ((v->cc_mphn == V34_MPH0_BITS && !v->cc_mphbuf[18]) || v->cc_mphn == V34_MPH1_BITS)
        {
            v34_mph_t m;

            v->cc_in_mph = false;
            if (v34_mph_unpack(v->cc_mphbuf, v->cc_mphn, &m))
                heard_mph(v, &m);
        }
    }
    else if (!b && (v->cc_sr & 0x3FFFF) == 0x3FFFE && v->stage == ST_CC_START)
    {
        memset(v->cc_mphbuf, 1, 17);
        v->cc_mphbuf[17] = 0;
        v->cc_mphn = 18;
        v->cc_in_mph = true;
    }
    /* E: twenty ones (10.2.4.3), ten whole symbols; data follows. Only at
     * a symbol's end: ALT's last bit is a one, and counting it put the
     * data a bit early. */
    if (v->cc_expect_e && !v->cc_in_mph && v->cc_ones >= 20 && symbol_end)
    {
        if (v->stage == ST_CC_START && !settle_rate(v))
        {
            fail(v, "the two modems have no primary channel rate in common");
            return;
        }
        v->cc_expect_e = false;
        v->cc_rx_data = true;
        FM_DEBUG("v34", "E heard; control channel data follows (tag=%s)", v->tag);
        cc_up_check(v);
    }
}

/* 12.6.1: the source, its page out: silence, Sh, S-bar-h, then ALT until
 * the recipient answers in kind. */
static void cc_resync_source(fm_v34h_t *v)
{
    ccrx_listen(v, false);
    v->cc_tx_data = false;
    v->cc_up_said = false;
    v->q.mode = RQ_OFF;
    cctx_start(v);
    cc_set(v, CS_SILENCE, CC_SYMS(0.07));
    cc_then(v, CS_SH, 24);
    cc_then(v, CS_SHB, 8);
    cc_then(v, CS_ALT, -1);
    stage_enter(v, ST_CC_RESYNC, 3.0 + 2.0 * rtd_s(v));
}

/* 12.6.2: the recipient, the page in, listens for Sh and S-bar-h - or PPh. */
static void cc_resync_recipient(fm_v34h_t *v)
{
    ccrx_listen(v, false);
    v->cc_tx_data = false;
    v->cc_up_said = false;
    v->txm = TXM_SILENCE;
    stage_enter(v, ST_CC_RESYNC, 3.0 + 2.0 * rtd_s(v));
}

/* The control channel's signals as the far end sends them. */
static void cc_events(fm_v34h_t *v, int ev)
{
    if (ev & V34_CC_EV_PPH)
    {
        FM_DEBUG("v34", "PPh heard (tag=%s)", v->tag);
        v->far_pph = true;
        v->far_pph_at = v->cctx.k;
        if (v->stage == ST_CC_START && !v->source && v->txm != TXM_CC)
        {
            /* 12.4.2.1 to 12.4.2.3: PPh, ALT for 16T or more, then MPh. */
            cctx_start(v);
            cc_set(v, CS_PPH, 32);
            cc_then(v, CS_ALT, 40);
            cc_then(v, CS_MPH, -1);
        }
        else if (v->stage == ST_CC_RESYNC && !v->source)
        {
            /* 12.6.2.1: the source wants new parameters; start-up again. */
            stage_enter(v, ST_CC_START, 3.0);
            v->have_mph_far = false;
            build_mph(v);
            cctx_start(v);
            cc_set(v, CS_PPH, 32);
            cc_then(v, CS_ALT, 40);
            cc_then(v, CS_MPH, -1);
        }
    }
    if (ev & V34_CC_EV_SHBAR)
    {
        v->far_shbar = true;
        if (v->stage == ST_CC_RESYNC && !v->source && v->txm != TXM_CC)
        {
            /* 12.6.2.2: Sh, S-bar-h, ALT, E, data. */
            FM_DEBUG("v34", "Sh and S-bar-h heard; answering (tag=%s)", v->tag);
            cctx_start(v);
            cc_set(v, CS_SH, 24);
            cc_then(v, CS_SHB, 8);
            cc_then(v, CS_ALT, 40);
            cc_then(v, CS_E, 10);
            cc_then(v, CS_DATA, -1);
            v->cc_expect_e = true;
        }
        else if (v->stage == ST_CC_RESYNC && v->source)
        {
            /* 12.6.1.4: ALT for 16T or more, then E. */
            FM_DEBUG("v34", "the recipient's Sh and S-bar-h heard (tag=%s)", v->tag);
            v->cc_expect_e = true;
        }
    }
    if (ev & V34_CC_EV_AC)
        FM_INFO("v34", "the far end is asking for a control channel retrain, which is not implemented (tag=%s)",
                v->tag);
}

/* ------------------------------------------------------------ primary */

void fm_v34h_primary(fm_v34h_t *v)
{
    if (v->stage != ST_CC_DATA)
        return;
    v->cc_up_said = false;
    v->cc_tfreq = v->ccrx.tfreq;
    if (v->source)
    {
        /* 12.6.3.1: 4T of scrambled ones, then 12.5.1. Our receiver is
         * deaf meanwhile: what it would hear is our own page coming back
         * through the hybrid, and it would learn that and start the resync
         * in a mess. */
        cc_set(v, CS_ONES, 4);
        cc_then(v, CS_OFF, -1);
        v->ccrx_on = false;
        stage_enter(v, ST_PC_TX, 0.0);
    }
    else
    {
        /* 12.6.3.2: 4T of ones, silence, and listen for S. */
        cc_set(v, CS_ONES, 4);
        cc_then(v, CS_OFF, -1);
        v->ccrx_on = false;
        v->rx_in_data = false;
        qrx_start(v, v->sr, v->high);
        /* A resynchronisation trains on PP alone, too short to find the
         * source's clock again: start from what Phase 3 found. */
        v->q.tfreq = v->far_tfreq;
        v->p3_training = false;
        v->quiet_since = 0;
        stage_enter(v, ST_PC_RX, 10.0);
    }
}

static void pc_start_source(fm_v34h_t *v)
{
    double S = v34_symbol_rate(v->sr);

    /* 12.5.1: silence, S, S-bar, PP, B1, data. */
    qtx_start(v);
    tx_set(v, TS_SILENCE, (long long) (0.07 * S));
    tx_then(v, TS_S, 128);
    tx_then(v, TS_SBAR, 16);
    tx_then(v, TS_PP, 288);
    tx_then(v, TS_DATA, -1);
    /* 12.5.3.1: 35 ms of ones once the page is out. */
    v->ones_frames = (int) ceil(0.035 * S / 8.0);
    FM_DEBUG("v34", "primary channel: sending at %d bit/s (tag=%s)", v->rate, v->tag);
}

/* ---------------------------------------------------------- Phase 2: RX */

static bool info_bit(fm_v34h_t *v, int k, int bit)
{
    v->isr[k] = ((v->isr[k] << 1) | (uint32_t) bit) & 0xFFFu;
    if (!v->i_collect[k])
    {
        /* 1111 01110010 */
        if (v->i_expect > 0 && v->isr[k] == 0xF72u)
        {
            static const uint8_t head[12] = { 1, 1, 1, 1, 0, 1, 1, 1, 0, 0, 1, 0 };

            memcpy(v->ibits[k], head, 12);
            v->ibits_n[k] = 12;
            v->i_collect[k] = true;
        }
        return false;
    }
    v->ibits[k][v->ibits_n[k]++] = (uint8_t) bit;
    if (v->ibits_n[k] >= v->i_expect - 4)
    {
        v->i_collect[k] = false;
        return true;
    }
    return false;
}

/* Can the far end transmit at symbol rate sr on that carrier? (Table 14) */
static bool far_can_tx(const fm_v34h_t *v, int sr, bool high)
{
    const v34_info0_t *i = &v->info0_far;

    switch (sr)
    {
    case V34_S2400:
        return true;
    case V34_S2743:
        return i->sr2743;
    case V34_S2800:
        return i->sr2800;
    case V34_S3000:
        return high ? i->high3000 : i->low3000;
    case V34_S3200:
        return high ? i->high3200 : i->low3200;
    case V34_S3429:
        return i->sr3429 && i->allow3429;
    }
    return false;
}

/* The recipient, from its probe of the source's L2: the symbol rate,
 * carrier and pre-emphasis that will carry the most. */
static void choose_infoh(fm_v34h_t *v, v34_infoh_t *h)
{
    int best_rate = -1;

    memset(h, 0, sizeof(*h));
    h->sr = V34_S2400;
    for (int sr = 0; sr < V34_NUM_SR; sr++)
    {
        float best = -100.0f;
        bool bh = false;
        int bpe = 0;

        if (!((v->sr_allow >> sr) & 1))
            continue;
        for (int hi = 0; hi < 2; hi++)
        {
            if (!far_can_tx(v, sr, hi))
                continue;
            for (int pe = 0; pe <= 10; pe++)
            {
                float snr = v34_probe_snr(&v->probe, sr, hi, pe);

                if (snr > best + 0.2f)
                {
                    best = snr;
                    bh = hi;
                    bpe = pe;
                }
            }
        }
        if (best > -100.0f)
        {
            int cap = v->max_rate;
            int r;

            if (!v->info0_far.c1664 && cap > 28800)
                cap = 28800;
            r = best_rate_for(best - 2.0f, sr, cap);
            FM_DEBUG("v34", "probing: %s symbols/s, %s carrier, pre-emphasis %d: %.1f dB, %d bit/s (tag=%s)",
                     V34_SR_NAME[sr], bh ? "high" : "low", bpe, best, r, v->tag);
            /* Ties go to the lower symbol rate, which is the more robust. */
            if (r > best_rate)
            {
                best_rate = r;
                h->sr = sr;
                h->high = bh;
                h->pre_emphasis = bpe;
            }
        }
    }
    h->power_reduction = 0;
    h->trn_len = TRN_UNITS;
    h->trn16 = false;
}

static void describe_info0(const v34_info0_t *i, char *out, size_t len)
{
    snprintf(out, len, "2400 3000%s 3200%s%s%s%s%s",
             (i->low3000 && i->high3000) ? "" : i->low3000 ? "(low)" : i->high3000 ? "(high)" : "(no tx)",
             (i->low3200 && i->high3200) ? "" : i->low3200 ? "(low)" : i->high3200 ? "(high)" : "(no tx)",
             i->sr2743 ? " 2743" : "", i->sr2800 ? " 2800" : "", (i->sr3429 && i->allow3429) ? " 3429" : "",
             i->c1664 ? ", 1664 points" : "");
}

static bool phase2_frame(fm_v34h_t *v, int k)
{
    const uint8_t *bits = v->ibits[k];
    int n = v->ibits_n[k];

    if (v->i_expect == V34_INFO0_BITS)
    {
        v34_info0_t i0;
        char d[128];

        if (!v34_info0_unpack(bits, n, &i0))
            return false;
        v->info0_count++;
        if (!v->have_info0)
        {
            describe_info0(&i0, d, sizeof(d));
            FM_DEBUG("v34", "INFO0%c: %s (tag=%s)", v->calling ? 'a' : 'c', d, v->tag);
        }
        v->info0_far = i0;
        v->have_info0 = true;
        if (i0.ack)
            v->ack_info0 = true;
        info_bits_reset(v, V34_INFO0_BITS);
        return true;
    }
    if (v->i_expect == V34_INFOH_BITS && v->stage == ST_C2_INFOH)
    {
        v34_infoh_t h;

        if (!v34_infoh_unpack(bits, n, &h))
            return false;
        v->infoh = h;
        v->sr = h.sr;
        v->high = h.high;
        v->pe = h.pre_emphasis;
        v->pr_db = (float) h.power_reduction;
        FM_DEBUG("v34", "INFOh: %s symbols/s, %s carrier, pre-emphasis %d, %d ms of %d-point TRN (tag=%s)",
                 V34_SR_NAME[h.sr], h.high ? "high" : "low", h.pre_emphasis, h.trn_len * 35, h.trn16 ? 16 : 4,
                 v->tag);
        info_bits_reset(v, 0);
        /* 12.3.1.1: silence, then Phase 3. */
        p2_carrier(v, false);
        v->txm = TXM_SILENCE;
        phase3_source(v);
        return true;
    }
    return false;
}

static void phase2_bits(fm_v34h_t *v)
{
    v34_p2rx_t *r = &v->p2rx;

    for (int k = 0; k < V34_P2_PHASES; k++)
    {
        int nb = r->nbits[k];

        r->nbits[k] = 0;
        for (int i = 0; i < nb; i++)
            if (info_bit(v, k, r->bits[k][i]) && phase2_frame(v, k))
            {
                for (int j = 0; j < V34_P2_PHASES; j++)
                    r->nbits[j] = 0;
                return;
            }
    }
}

/* --------------------------------------------------------- control loop */

static void begin_phase2(fm_v34h_t *v)
{
    v34_p2tx_init(&v->p2tx, !v->calling, v->nominal_dbm0);
    v->have_info0 = false;
    v->ack_info0 = false;
    v->info0_count = 0;
    send_info0(v);
    stage_enter(v, v->calling ? ST_C2_INFO0 : ST_A2_INFO0, 10.0);
    FM_DEBUG("v34", "Phase 2: INFO0%c (tag=%s)", v->calling ? 'c' : 'a', v->tag);
}

static void control(fm_v34h_t *v, long long n, double rev, bool reversed)
{
    v34_p2rx_t *r = &v->p2rx;
    double rt = (v->rtd >= 0) ? v->rtd / 8000.0 : 0.0;
    bool tone = r->tone_since >= 0;

    /* Our E may finish after theirs arrives, or before. */
    if (v->stage == ST_CC_START || v->stage == ST_CC_RESYNC)
        cc_up_check(v);
    switch (v->stage)
    {
    /* ---------------------------------------------------------- V.8 */
    case ST_V8_C_LISTEN:
    {
        int t = modem_connect_tones_rx_get(v->ansam_rx);

        if (t == MODEM_CONNECT_TONES_ANSAM || t == MODEM_CONNECT_TONES_ANSAM_PR)
        {
            FM_DEBUG("v34", "ANSam heard (tag=%s)", v->tag);
            stage_enter(v, ST_V8_C_TE, 0.5);
        }
        else if (t == MODEM_CONNECT_TONES_ANS || t == MODEM_CONNECT_TONES_ANS_PR)
        {
            not_v34(v, "the far end answered with a plain answer tone, no V.8");
        }
        break;
    }
    case ST_V8_C_CM:
        if (v->v8_got_msg)
        {
            char d[128];

            v8_describe(&v->v8_far, d, sizeof(d));
            FM_INFO("v34", "V.8: the answering fax offers %s (tag=%s)", d, v->tag);
            if (!v->v8_far.v34hdx)
            {
                not_v34(v, "the answering fax does not do V.34");
                break;
            }
            /* 11.1.1.1: complete the CM octet in hand, then CJ. */
            {
                int left = (v->v8t - v->v8h + (int) sizeof(v->v8q)) % (int) sizeof(v->v8q);
                uint8_t cj[30];
                int nb = 0;

                v->v8t = (v->v8h + left % 10) % (int) sizeof(v->v8q);
                v->v8_repeat = false;
                for (int k = 0; k < 3; k++)
                    v8_octet(cj, &nb, 0);
                for (int k = 0; k < nb; k++)
                {
                    v->v8q[v->v8t] = cj[k];
                    v->v8t = (v->v8t + 1) % (int) sizeof(v->v8q);
                }
            }
            stage_enter(v, ST_V8_C_CJ, 2.0);
        }
        break;
    case ST_V8_C_CJ:
        if (v->v8h == v->v8t)
        {
            p2_start_rx(v);
            stage_enter(v, ST_V8_DONE, 0.075);
        }
        break;
    case ST_V8_A_ANSAM:
        if (v->v8_got_msg)
        {
            char d[128];
            v8_msg_t jm;

            v8_describe(&v->v8_far, d, sizeof(d));
            FM_INFO("v34", "V.8: the calling fax offers %s (tag=%s)", d, v->tag);
            if (!v->v8_far.v34hdx || v->v8_far.call_function != 4)
            {
                not_v34(v, v->v8_far.v34hdx ? "the calling fax wants to receive, which V.34 here does not do"
                                            : "the calling fax does not do V.34 half duplex");
                break;
            }
            v8_ours(v, &jm);
            v8_build_msg(v, &jm);
            v8_start_fsk(v, true);
            v->v8_got_cj = false;
            stage_enter(v, ST_V8_A_JM, 5.0);
        }
        else if (v->ansam_done_at > 0 && n - v->ansam_done_at > MS(500))
        {
            not_v34(v, "no CM in answer to ANSam: the calling fax does not do V.8");
        }
        break;
    case ST_V8_A_JM:
        if (v->v8_got_cj)
        {
            v->txm = TXM_SILENCE;
            p2_start_rx(v);
            stage_enter(v, ST_V8_DONE, 0.075);
        }
        break;
    case ST_V8_DONE:
        if (v->txm == TXM_V8 && n - v->t_stage > MS(15))
            v->txm = TXM_SILENCE;
        break;

    /* ------------------------------------------- Phase 2, the source */
    case ST_C2_INFO0:
        /* 12.2.1.3.1: tone A without INFO0a, or INFO0a again, means the
         * answerer did not get ours. */
        if (v34_p2tx_pending(&v->p2tx) == 0 && v->txm == TXM_P2 && !v->ack_info0 &&
            ((tone && n - r->tone_since > MS(100) && !v->have_info0) || v->info0_count >= 2))
        {
            v->info0_count = 1;
            send_info0(v);
        }
        if (reversed && v->have_info0 && v34_p2tx_pending(&v->p2tx) == 0)
        {
            /* 12.2.1.1.3: our reversal 40 ms after theirs, B 10 ms more,
             * L1 for 160 ms, then L2. */
            p2_reverse(v, (long long) rev + MS(40), 10, true);
            stage_enter(v, ST_C2_L, 2.7 + rt + 0.5);
            FM_DEBUG("v34", "tone A reversed; tone B reversed, then L1 and L2 (tag=%s)", v->tag);
        }
        break;
    case ST_C2_L:
        /* The recipient listens to L2 for 500 ms at most (12.2.1.2.5); after
         * that L2 only hides tone A behind our own echo - 10 dB stronger
         * than A over a line with 30 dB of loss. So L2 stops, and tone A is
         * waited for in silence. */
        if (v->p2_l2_at < 0 && v->p2tx.probe && v->ntx > v->rev_sent + MS(10 + 160 + 650))
        {
            v34_p2tx_probe(&v->p2tx, false, false, v->nominal_dbm0);
            v->txm = TXM_SILENCE;
        }
        /* 12.2.1.1.4: tone A. */
        if (v->p2_l2_at < 0 && tone && n - r->tone_since > MS(30) && n > v->rev_sent + MS(10 + 160 + 100))
        {
            p2_carrier(v, true);
            info_bits_reset(v, V34_INFOH_BITS);
            stage_enter(v, ST_C2_INFOH, 2.0 + rt + 2.0);
            FM_DEBUG("v34", "tone A heard; tone B, waiting for INFOh (tag=%s)", v->tag);
        }
        break;
    case ST_C2_INFOH:
        break;

    /* ---------------------------------------- Phase 2, the recipient */
    case ST_A2_INFO0:
        if (v34_p2tx_pending(&v->p2tx) == 0 && v->txm == TXM_P2 && !v->ack_info0 &&
            ((tone && n - r->tone_since > MS(100) && !v->have_info0) || v->info0_count >= 2))
        {
            v->info0_count = 1;
            send_info0(v);
        }
        /* 12.2.1.2.3: tone B heard with INFO0c in, A sent for 50 ms: reverse
         * A, 10 ms more of it, then silence. */
        if (v->have_info0 && v34_p2tx_pending(&v->p2tx) == 0 && v->txm == TXM_P2 && tone &&
            n - r->tone_since > MS(20) && v->ntx - v->p2_tone_from > MS(50) && v->p2tx.flip_at < 0)
        {
            p2_reverse(v, v->ntx + 8, 10, false);
            stage_enter(v, ST_A2_REV, 2.0);
            FM_DEBUG("v34", "tone B heard; reversing A (tag=%s)", v->tag);
        }
        break;
    case ST_A2_REV:
        if (reversed && rev > (double) v->rev_sent)
        {
            /* 12.2.1.2.4: the round trip, then L1 and L2. */
            v->rtd = (long long) (rev - (double) v->rev_sent) - MS(40);
            if (v->rtd < 0)
                v->rtd = 0;
            FM_DEBUG("v34", "round trip %.1f ms; receiving L1 and L2 (tag=%s)", v->rtd / 8.0, v->tag);
            v34_probe_init(&v->probe);
            v->probing = true;
            v->probe_from = (long long) rev + MS(10 + 160 + 20);
            stage_enter(v, ST_A2_PROBE, 1.5);
        }
        break;
    case ST_A2_PROBE:
        if (!v->probing)
        {
            v34_probe_analyse(&v->probe);
            choose_infoh(v, &v->infoh);
            v->sr = v->infoh.sr;
            v->high = v->infoh.high;
            v->pe = v->infoh.pre_emphasis;
            v->infoh_n = v34_infoh_pack(&v->infoh, v->infoh_bits);
            /* 12.2.1.2.5: tone A, and listen for B. */
            p2_carrier(v, true);
            stage_enter(v, ST_A2_TONEB, 2.0 + rt);
        }
        break;
    case ST_A2_TONEB:
        if (tone && n - r->tone_since > MS(20))
        {
            /* 12.2.1.2.6: 25 ms more of A, then INFOh. */
            v->infoh_at = v->ntx + MS(25);
            stage_enter(v, ST_A2_INFOH, 1.0);
        }
        break;
    case ST_A2_INFOH:
        if (v->infoh_at > 0 && v->ntx >= v->infoh_at)
        {
            v->infoh_at = 0;
            p2_send_info(v, v->infoh_bits, v->infoh_n);
            v->p2_off_after_bits = true;
            FM_DEBUG("v34", "INFOh: %s symbols/s, %s carrier, pre-emphasis %d, %d ms of TRN (tag=%s)",
                     V34_SR_NAME[v->infoh.sr], v->infoh.high ? "high" : "low", v->infoh.pre_emphasis,
                     v->infoh.trn_len * 35, v->tag);
        }
        else if (v->infoh_at == 0 && v->txm == TXM_SILENCE)
        {
            /* 12.3.2.1: silent, listening for S. */
            v->trn_symbols = trn_symbols_for(&v->infoh);
            qrx_start(v, v->sr, v->high);
            v->p3_training = true;
            stage_enter(v, ST_P3_RCP, 2.0 + 2.0 * rt + 3.0);
        }
        break;

    /* ------------------------------------------------- Phase 3 and on */
    case ST_P3_SRC:
        if (v->seg == TS_SILENCE && v->seg_len < 0 && v->iprog >= v->nprog && v->ntx >= v->qam_quiet_at)
        {
            /* TRN is out: 12.4.1. */
            v->qtx_on = false;
            cc_start_source(v);
        }
        break;
    case ST_P3_RCP:
        if (v->q.mode == RQ_TRAIN)
            v->deadline = 0;
        break;
    case ST_CC_START:
        if (v->source && v->cseg == CS_ALT && v->far_pph && v->cctx.k - v->alt_since >= 16)
        {
            /* 12.4.1.2: MPh within 120T of the recipient's PPh. */
            v->cnp = v->cip = 0;
            v->clen = v->ccount;
            cc_then(v, CS_MPH, -1);
        }
        break;
    case ST_CC_RESYNC:
        if (v->source && v->cseg == CS_ALT && v->far_shbar && v->cctx.k - v->alt_since >= 16 && !v->cc_tx_data)
        {
            /* 12.6.1.4: ALT for 16T or more, then E and data. */
            v->cnp = v->cip = 0;
            v->clen = v->ccount;
            cc_then(v, CS_E, 10);
            cc_then(v, CS_DATA, -1);
        }
        break;
    case ST_PC_TX:
        if (v->txm == TXM_CC && v->cseg == CS_OFF)
        {
            /* The 4T of ones are out; 12.5.1. */
            v34_cctx_on(&v->cctx, false);
            pc_start_source(v);
        }
        else if (v->txm == TXM_QAM && v->pc_end && v->seg == TS_SILENCE && v->seg_len < 0 &&
                 v->ntx >= v->qam_quiet_at)
        {
            /* The page and its ones are out: 12.6.1. */
            v->qtx_on = false;
            cc_resync_source(v);
        }
        break;
    case ST_PC_RX:
        if (v->txm == TXM_CC && v->cseg == CS_OFF)
        {
            v34_cctx_on(&v->cctx, false);
            v->txm = TXM_SILENCE;
        }
        if (v->q.mode != RQ_HUNT && v->q.mode != RQ_OFF)
            v->deadline = 0;
        /* 12.5.3.2: the source's signal has gone; 109 off after 20 ms. */
        if (v->rx_in_data)
        {
            if (v->q.pwr < 0.1f * v->q.pwr_ref)
            {
                if (v->quiet_since == 0)
                    v->quiet_since = n;
                else if (n - v->quiet_since > MS(20))
                {
                    v->rx_in_data = false;
                    v->q.mode = RQ_OFF;
                    FM_DEBUG("v34", "primary channel: the source has stopped (tag=%s)", v->tag);
                    emit(v, FM_V34H_PRIMARY_DOWN);
                    cc_resync_recipient(v);
                }
            }
            else
            {
                v->quiet_since = 0;
            }
        }
        break;
    default:
        break;
    }

    if (v->deadline > 0 && n >= v->deadline)
    {
        char why[96];

        switch (v->stage)
        {
        case ST_V8_C_TE:
        {
            v8_msg_t cm;

            /* 11.1.1.1: CM, offering V.34 half duplex for T.30. */
            v8_ours(v, &cm);
            v8_build_msg(v, &cm);
            v8_start_fsk(v, true);
            v8_start_rx(v);
            stage_enter(v, ST_V8_C_CM, 6.0);
            return;
        }
        case ST_V8_DONE:
            v->txm = TXM_SILENCE;
            begin_phase2(v);
            return;
        case ST_V8_C_CM:
        case ST_V8_A_JM:
        case ST_V8_C_CJ:
            snprintf(why, sizeof(why), "V.8 stalled at '%s'", STAGE_NAMES[v->stage]);
            not_v34(v, why);
            return;
        case ST_A2_TONEB:
            /* 12.2.1.4.3: no tone B; send INFOh anyway. */
            v->infoh_at = v->ntx;
            stage_enter(v, ST_A2_INFOH, 1.0);
            return;
        default:
            break;
        }
        snprintf(why, sizeof(why), "timed out at '%s'", STAGE_NAMES[v->stage]);
        fail(v, why);
    }
}

/* ---------------------------------------------------------------- driving */

int fm_v34h_tx(fm_v34h_t *v, int16_t *amp, int len)
{
    for (int i = 0; i < len; i++)
    {
        float x = 0.0f;

        switch (v->txm)
        {
        case TXM_SILENCE:
            break;
        case TXM_V8:
        {
            int16_t s = 0;

            if (v->stage == ST_V8_A_ANSAM && v->ansam_tx != NULL)
            {
                if (modem_connect_tones_tx(v->ansam_tx, &s, 1) < 1 && v->ansam_done_at == 0)
                    v->ansam_done_at = v->ntx;
            }
            else if (v->fsk_tx != NULL)
            {
                fsk_tx(v->fsk_tx, &s, 1);
            }
            x = s;
            break;
        }
        case TXM_P2:
            if (v->p2_off_at >= 0 && v->ntx >= v->p2_off_at)
            {
                v34_p2tx_carrier(&v->p2tx, false);
                v->p2_off_at = -1;
                if (v->p2_probe_at >= 0)
                {
                    v34_p2tx_probe(&v->p2tx, true, true, v->nominal_dbm0);
                    v->p2_probe_at = -1;
                }
            }
            if (v->p2_l2_at >= 0 && v->ntx >= v->p2_l2_at)
            {
                v34_p2tx_probe(&v->p2tx, true, false, v->nominal_dbm0);
                v->p2_l2_at = -1;
            }
            {
                bool had_bits = v34_p2tx_pending(&v->p2tx) > 0;

                x = v34_p2tx_sample(&v->p2tx, v->ntx);
                /* The carrier is a plain tone from the moment its bits run
                 * out - which is when "tone for 50 ms" starts counting
                 * (12.2.1.2.3): timed from the start of INFO0a, the
                 * reversal came straight after it, with no steady tone in
                 * front for the far end to see it against. */
                if (had_bits && v34_p2tx_pending(&v->p2tx) == 0)
                    v->p2_tone_from = v->ntx;
            }
            if (v->p2_off_after_bits && v34_p2tx_pending(&v->p2tx) == 0)
            {
                v->p2_off_after_bits = false;
                v->p2_off_at = v->ntx + MS(8);
            }
            if (!v->p2tx.on && !v->p2tx.probe && v->p2_off_at < 0 && v->p2_l2_at < 0)
                v->txm = TXM_SILENCE;
            break;
        case TXM_QAM:
            if (v->qtx_on)
                x = v34_qtx_sample(&v->qtx);
            break;
        case TXM_CC:
            x = v34_cctx_sample(&v->cctx);
            break;
        }
        if (x > 32767.0f)
            x = 32767.0f;
        else if (x < -32768.0f)
            x = -32768.0f;
        amp[i] = (int16_t) lrintf(x);
        v->ntx++;
    }
    return len;
}

static void rx_sample(fm_v34h_t *v, float x)
{
    long long n = v->n;
    double rev = 0.0;
    bool reversed = false;

    v->n++;
    switch (v->stage)
    {
    case ST_V8_C_LISTEN:
    case ST_V8_C_TE:
    {
        int16_t s = (int16_t) lrintf(x);

        modem_connect_tones_rx(v->ansam_rx, &s, 1);
        break;
    }
    case ST_V8_C_CM:
    case ST_V8_C_CJ:
    case ST_V8_A_ANSAM:
    case ST_V8_A_JM:
    {
        int16_t s = (int16_t) lrintf(v8_bpf(v, x));

        if (v->fsk_rx != NULL)
            fsk_rx(v->fsk_rx, &s, 1);
        break;
    }
    case ST_DEAD:
        break;
    default:
        if (v->stage <= ST_A2_INFOH)
        {
            reversed = v34_p2rx_sample(&v->p2rx, x, n, &rev);
            phase2_bits(v);
            if (v->probing && n >= v->probe_from)
            {
                if (v34_probe_sample(&v->probe, x) || v->probe.blocks >= 20)
                    v->probing = false;
            }
        }
        else
        {
            if (v->q.mode != RQ_OFF)
                qrx_sample(v, x, n);
            if (v->ccrx_on)
            {
                int ev = v34_ccrx_sample(&v->ccrx, x);

                if (ev & ~V34_CC_EV_SYMBOL)
                    cc_events(v, ev);
                for (int i = 0; i < v->ccrx.nbits && v->ccrx_on; i++)
                    cc_rx_bit(v, v->ccrx.bits[i], (i & 1) == 1);
            }
        }
        break;
    }
    control(v, n, rev, reversed);
}

void fm_v34h_rx(fm_v34h_t *v, const int16_t *amp, int len)
{
    for (int i = 0; i < len; i++)
        rx_sample(v, (float) amp[i]);
}

void fm_v34h_rx_fillin(fm_v34h_t *v, int len)
{
    for (int i = 0; i < len; i++)
        rx_sample(v, 0.0f);
}

fm_v34h_t *fm_v34h_create(const fm_v34h_params_t *p)
{
    fm_v34h_t *v = calloc(1, sizeof(*v));

    if (v == NULL)
        return NULL;
    v->calling = p->calling;
    v->source = p->calling;
    snprintf(v->tag, sizeof(v->tag), "%s", p->tag ? p->tag : (p->calling ? "out" : "in"));
    v->max_rate = (p->max_rate <= 0 || p->max_rate > 33600) ? 33600 : p->max_rate;
    v->sr_allow = p->symbol_rates ? (p->symbol_rates | 1u) & 0x3Fu : 0x3Fu;
    v->g3_v17 = p->v17;
    v->g3_v29 = p->v29;
    v->g3_v27ter = p->v27ter;
    v->nominal_dbm0 = p->tx_power;
    v->cc_get_bit = p->cc_get_bit;
    v->cc_put_bit = p->cc_put_bit;
    v->pc_get_bit = p->pc_get_bit;
    v->pc_put_bit = p->pc_put_bit;
    v->event = p->event;
    v->user = p->user;
    /* Clause 7: GPC calling, GPA answering. */
    v->scr_tap = p->calling ? 18 : 5;
    v->dscr_tap = p->calling ? 5 : 18;
    v->p0 = (float) (V34_DBM0_RMS * V34_DBM0_RMS);
    v->pmin = v->p0 * powf(10.0f, -48.0f / 10.0f);
    v->rtd = -1;
    v->p2_off_at = v->p2_probe_at = v->p2_l2_at = -1;
    v->rx_gain = 1.0f;
    v->rx_energy = 1.0f;

    if (v->calling)
    {
        /* 11.1.1.1: listen for ANSam. Whatever the caller sends meanwhile -
         * CNG - is the front end's. */
        v->ansam_rx = modem_connect_tones_rx_init(NULL, MODEM_CONNECT_TONES_ANS_PR, NULL, NULL);
        v->txm = TXM_SILENCE;
        stage_enter(v, ST_V8_C_LISTEN, 0.0);
        if (v->ansam_rx == NULL)
        {
            fm_v34h_free(v);
            return NULL;
        }
    }
    else
    {
        /* 11.1.2.1: 200 ms of silence and then ANSam - spandsp's generator
         * starts with the silence - listening for CM all the while. Phase
         * reversals are optional in half duplex (11.1.2.1); they also
         * disable echo cancellers in the network, which do no harm here. */
        v->ansam_tx = modem_connect_tones_tx_init(NULL, MODEM_CONNECT_TONES_ANSAM_PR);
        v->txm = TXM_V8;
        v8_start_rx(v);
        stage_enter(v, ST_V8_A_ANSAM, 0.0);
        if (v->ansam_tx == NULL || v->fsk_rx == NULL)
        {
            fm_v34h_free(v);
            return NULL;
        }
    }
    return v;
}

void fm_v34h_free(fm_v34h_t *v)
{
    if (v == NULL)
        return;
    if (v->fsk_tx != NULL)
        fsk_tx_free(v->fsk_tx);
    if (v->fsk_rx != NULL)
        fsk_rx_free(v->fsk_rx);
    if (v->ansam_tx != NULL)
        modem_connect_tones_tx_free(v->ansam_tx);
    if (v->ansam_rx != NULL)
        modem_connect_tones_rx_free(v->ansam_rx);
    v34_qtx_free(&v->qtx);
    qrx_free(&v->q);
    free(v);
}

bool fm_v34h_in_v8(const fm_v34h_t *v)
{
    return v->stage <= ST_V8_A_ANSAM && v->stage != ST_V8_C_CM && v->stage != ST_V8_C_CJ;
}

bool fm_v34h_control_up(const fm_v34h_t *v)
{
    return v->stage == ST_CC_DATA;
}

bool fm_v34h_is_source(const fm_v34h_t *v)
{
    return v->source;
}

void fm_v34h_stats(const fm_v34h_t *v, fm_v34h_stats_t *out)
{
    memset(out, 0, sizeof(*out));
    out->stage = STAGE_NAMES[v->stage];
    out->source = v->source;
    out->rate = v->rate;
    if (v->stage >= ST_P3_SRC)
    {
        out->symbol_rate = (int) lrint(v34_symbol_rate(v->sr));
        out->carrier = (int) lrint(v34_carrier(v->sr, v->high));
    }
    out->pre_emphasis = v->pe;
    out->snr_db = v->snr_db;
    out->round_trip_ms = (v->rtd >= 0) ? (int) (v->rtd / 8) : -1;
    out->pages = v->pages;
}
