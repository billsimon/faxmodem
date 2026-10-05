/* The half-duplex control channel (10.2.4). See v34_cc.h. */
#include "v34_cc.h"
#include "v34_dsp.h"

#include <math.h>
#include <string.h>

#define PI 3.14159265358979323846
#define CC_ALPHA 0.75                   /* as Phase 2's DPSK, on the same carriers */
#define T2 (V34_CC_SPS / 2.0)           /* T/2, in samples */
#define EQ_C (V34_CC_EQ / 2)            /* the equaliser's main tap */

static v34_cf_t pph_tab[8];
static bool pph_ready;

static double pulse(double t, double a, double span)
{
    if (fabs(t) >= span)
        return 0.0;
    return v34_rrc(t, a) * 0.5 * (1.0 + cos(PI * t / span));
}

v34_cf_t v34_pph(int i)
{
    i &= 7;
    if (!pph_ready)
    {
        for (int j = 0; j < 8; j++)
        {
            int k = j / 2, b = j % 2;
            double th = PI * (2.0 * k * (k - b) + 1.0) / 4.0;

            pph_tab[j] = (float) cos(th) + I * (float) sin(th);
        }
        pph_ready = true;
    }
    return pph_tab[i];
}

v34_cf_t v34_cc_point(int rot)
{
    static const v34_cf_t p[4] = { 0.70710678f + 0.70710678f * I, 0.70710678f - 0.70710678f * I,
                                   -0.70710678f - 0.70710678f * I, -0.70710678f + 0.70710678f * I };

    return p[rot & 3];
}

/* The clockwise quarter turns from point 0. */
static int quad_of(v34_cf_t u)
{
    if (crealf(u) >= 0.0f)
        return (cimagf(u) >= 0.0f) ? 0 : 1;
    return (cimagf(u) < 0.0f) ? 2 : 3;
}

/* ---------------------------------------------------------- transmitter */

void v34_cctx_init(v34_cctx_t *t, bool answerer, double power_dbm0, v34_cc_symbol_fn next, void *user)
{
    double rms = V34_DBM0_RMS * pow(10.0, power_dbm0 / 20.0);
    double sum = 0.0, k;

    memset(t, 0, sizeof(*t));
    t->answerer = answerer;
    /* 10.2.4: the answerer a dB below nominal, with the guard tone 7 below. */
    t->amp = (float) (sqrt(2.0) * rms * (answerer ? pow(10.0, -1.0 / 20.0) : 1.0));
    t->guard_amp = answerer ? (float) (sqrt(2.0) * rms * pow(10.0, -7.0 / 20.0)) : 0.0f;
    t->cden = answerer ? 10 : 20;
    for (int i = 0; i < t->cden; i++)
    {
        t->cosv[i] = (float) cos(2.0 * PI * i / t->cden);
        t->sinv[i] = (float) sin(2.0 * PI * i / t->cden);
    }
    for (int mu = 0; mu < 40; mu++)
        for (int m = 0; m < V34_CC_TAPS; m++)
        {
            double g = pulse(mu / 40.0 + m - V34_CC_L, CC_ALPHA, V34_CC_L);

            t->g[mu][m] = (float) g;
            sum += g * g;
        }
    /* Unit-power symbols come out at unit power. */
    k = sqrt(40.0 / sum);
    for (int mu = 0; mu < 40; mu++)
        for (int m = 0; m < V34_CC_TAPS; m++)
            t->g[mu][m] = (float) (t->g[mu][m] * k);
    t->next = next;
    t->user = user;
}

void v34_cctx_on(v34_cctx_t *t, bool on)
{
    if (on && !t->on)
    {
        /* Nothing before now is still in the pulse; the next symbol is the
         * first. */
        memset(t->hist, 0, sizeof(t->hist));
        t->k = (t->n * 3) / 40;
    }
    t->on = on;
}

float v34_cctx_sample(v34_cctx_t *t)
{
    long long kk;
    int mu, ph;
    v34_cf_t b = 0.0f;
    float x;

    if (!t->on)
    {
        t->n++;
        return 0.0f;
    }
    kk = (t->n * 3) / 40;
    mu = (int) ((t->n * 3) % 40);
    while (t->k <= kk)
    {
        t->hist[t->k & 15] = t->next(t->user);
        t->k++;
    }
    for (int m = 0; m < V34_CC_TAPS; m++)
        b += t->hist[(kk - m) & 15] * t->g[mu][m];
    ph = (int) ((t->n * 3) % t->cden);
    x = t->amp * (crealf(b) * t->cosv[ph] - cimagf(b) * t->sinv[ph]);
    if (t->answerer)
        x += t->guard_amp * cosf((float) (2.0 * PI * 9.0 * (double) (t->n % 40) / 40.0));
    t->n++;
    return x;
}

/* ------------------------------------------------------------- receiver */

/* 6.6.2: circuit 109 on above -43 dBm0, off below -48. */
#define ON_DB -43.0
#define OFF_DB -48.0

void v34_ccrx_init(v34_ccrx_t *r, bool far_is_answerer, double nominal_dbm0)
{
    double sum = 0.0, c0 = 0.0, k = 0.0;
    double sum2 = 0.0;

    (void) nominal_dbm0;
    memset(r, 0, sizeof(*r));
    r->far_answerer = far_is_answerer;
    r->dscr_tap = far_is_answerer ? 5 : 18;
    r->cden = far_is_answerer ? 10 : 20;
    for (int i = 0; i < r->cden; i++)
    {
        r->cosv[i] = (float) cos(2.0 * PI * i / r->cden);
        r->sinv[i] = (float) sin(2.0 * PI * i / r->cden);
    }
    r->mf_half = (int) (V34_CC_L * V34_CC_SPS);
    for (int i = -r->mf_half; i <= r->mf_half; i++)
    {
        double g = pulse(i / V34_CC_SPS, CC_ALPHA, V34_CC_L);

        r->mf[i + r->mf_half] = (float) g;
        sum += g;
    }
    for (int i = 0; i <= 2 * r->mf_half; i++)
        r->mf[i] = (float) (r->mf[i] / sum);
    /* Scale so that a symbol sent at 0 dBm0 comes out of the matched filter
     * at unit size: the transmitter's pulse, normalised as it is there, put
     * through ours at its centre. */
    for (int mu = 0; mu < 40; mu++)
        for (int m = -V34_CC_L; m <= V34_CC_L; m++)
        {
            double g = pulse(mu / 40.0 + m, CC_ALPHA, V34_CC_L);

            sum2 += g * g;
        }
    k = sqrt(40.0 / sum2);
    for (int i = -r->mf_half; i <= r->mf_half; i++)
        c0 += k * pulse(i / V34_CC_SPS, CC_ALPHA, V34_CC_L) * r->mf[i + r->mf_half];
    /* A carrier of amplitude A mixes down to A/2. */
    r->floor_ = (float) (1.0 / (sqrt(2.0) * V34_DBM0_RMS / 2.0 * c0));
    r->tau = 2.0 * r->mf_half + 2.0;
    r->lpos = V34_CC_EQ;
    r->pph_at = r->sbar_at = -1000000;
}

void v34_ccrx_reset_sync(v34_ccrx_t *r)
{
    r->s_run = r->ac_run = r->neg_run = 0;
    r->s_seen = false;
    r->hq = r->l2 = 0.0f;
    r->hp = 0.0f;
    r->pph_run = 0;
    r->zprev = 0;
    r->symbols = 0;
}

static v34_cf_t zf_at(const v34_ccrx_t *r, double t)
{
    long long i0 = (long long) floor(t);
    float f = (float) (t - (double) i0);

    return r->zf[i0 % V34_CC_HIST] * (1.0f - f) + r->zf[(i0 + 1) % V34_CC_HIST] * f;
}

/* PPh against the last sixteen symbols at this sample's T/2 phase, at each
 * of its eight cyclic shifts: the best normalised correlation, and the
 * complex gain and shift that gave it. */
static float pph_corr(const v34_ccrx_t *r, long long idx, v34_cf_t *gain, int *shift)
{
    float e = 0.0f, best = 0.0f;

    for (int j = 0; j < 16; j++)
    {
        v34_cf_t y = r->h[(idx - 30 + 2 * j) % V34_CC_H];

        e += crealf(y) * crealf(y) + cimagf(y) * cimagf(y);
    }
    if (e <= 0.0f)
        return 0.0f;
    for (int s = 0; s < 8; s++)
    {
        v34_cf_t c = 0.0f;
        float rho;

        for (int j = 0; j < 16; j++)
            c += r->h[(idx - 30 + 2 * j) % V34_CC_H] * conjf(v34_pph(j + s));
        rho = (crealf(c) * crealf(c) + cimagf(c) * cimagf(c)) / (16.0f * e);
        if (rho > best)
        {
            best = rho;
            *gain = c / 16.0f;
            *shift = s;
        }
    }
    return best;
}

/* The main tap from the signal's level alone; the phase is the phase loop's
 * to find, and its four-fold ambiguity is harmless. */
static void rescale(v34_ccrx_t *r)
{
    memset(r->c, 0, sizeof(r->c));
    r->c[EQ_C] = 1.0f / sqrtf(r->pwr_sym);
    r->theta = r->nu = 0.0f;
    r->mse = 1.0f;
    r->bad_run = 0;
    r->trained = true;
}

static int symbol(v34_ccrx_t *r, v34_cf_t y)
{
    v34_cf_t vout = 0.0f, u, d, err;
    const v34_cf_t *w;
    float norm = 0.0f;
    int quad, diff;

    r->lpos = (r->lpos == 0) ? V34_CC_EQ - 1 : r->lpos - 1;
    r->line[r->lpos] = y;
    r->line[r->lpos + V34_CC_EQ] = y;
    w = &r->line[r->lpos];
    r->since_on++;
    if (!r->trained)
    {
        /* Until something better is known, the main tap scales the signal
         * to unit size; the phase loop and the decisions do the rest. Not
         * before the level has settled: set up on the first symbol out of a
         * silence, it came out thousands of times too big, and every
         * decision after that was too far off to learn from. */
        if (r->since_on < 8 || r->pwr_sym <= 0.0f)
            return 0;
        rescale(r);
    }
    for (int i = 0; i < V34_CC_EQ; i++)
        vout += r->c[i] * w[i];
    u = vout * (cosf(r->theta) - I * sinf(r->theta));
    quad = quad_of(u);
    d = v34_cc_point(quad);
    {
        v34_cf_t ue = u - d;
        float e2 = crealf(ue) * crealf(ue) + cimagf(ue) * cimagf(ue);
        float pe = cimagf(u * conjf(d));

        r->mse += 0.05f * (e2 - r->mse);
        /* Decisions that stay this bad mean the scale is wrong, whatever
         * set it: start again from the level. */
        if (r->mse > 0.8f)
        {
            if (++r->bad_run >= 32)
                rescale(r);
        }
        else
        {
            r->bad_run = 0;
        }
        r->theta += 0.08f * pe + r->nu;
        r->nu += 0.002f * pe;
        if (r->nu > 0.05f)
            r->nu = 0.05f;
        else if (r->nu < -0.05f)
            r->nu = -0.05f;
        if (r->theta > PI)
            r->theta -= (float) (2.0 * PI);
        else if (r->theta < -PI)
            r->theta += (float) (2.0 * PI);
        /* A decision this far out is a guess; do not learn from it. */
        if (e2 < 0.5f)
        {
            err = d * (cosf(r->theta) + I * sinf(r->theta)) - vout;
            for (int i = 0; i < V34_CC_EQ; i++)
                norm += crealf(w[i]) * crealf(w[i]) + cimagf(w[i]) * cimagf(w[i]);
            if (norm > 0.0f)
                for (int i = 0; i < V34_CC_EQ; i++)
                    r->c[i] += 0.03f * err * conjf(w[i]) / norm;
        }
    }
    /* 10.2.4: Z(n) = Z(n-1) + I1 + 2 I2, mod 4. */
    diff = (quad - r->zprev) & 3;
    r->zprev = quad;
    r->bits[r->nbits++] = (uint8_t) v34_descramble(&r->dscr, r->dscr_tap, diff & 1);
    r->bits[r->nbits++] = (uint8_t) v34_descramble(&r->dscr, r->dscr_tap, diff >> 1);
    r->symbols++;
    return V34_CC_EV_SYMBOL;
}

static int half(v34_ccrx_t *r, v34_cf_t y)
{
    long long idx = r->nh++;
    v34_cf_t y4, y2, prod;
    float p = crealf(y) * crealf(y) + cimagf(y) * cimagf(y);
    float on = r->floor_ * r->floor_ * (float) pow(10.0, ON_DB / 10.0);
    float off = r->floor_ * r->floor_ * (float) pow(10.0, OFF_DB / 10.0);
    int ev = 0;

    r->h[idx % V34_CC_H] = y;
    r->pwr += 0.05f * (p - r->pwr);
    if (!r->carrier && r->pwr > on)
    {
        r->carrier = true;
        r->trained = false;
        r->since_on = 0;
    }
    else if (r->carrier && r->pwr < off)
    {
        r->carrier = false;
        v34_ccrx_reset_sync(r);
    }
    if (idx < 32 || r->pwr < off)
        return 0;

    /* Sh and AC: a period of two symbols, so each T/2 sample matches the one
     * four before, whatever the clock's phase. Across one symbol Sh turns a
     * quarter, and its midpoints sit still; AC turns half way round. */
    y4 = r->h[(idx - 4) % V34_CC_H];
    y2 = r->h[(idx - 2) % V34_CC_H];
    prod = y * conjf(y4);
    {
        float pp = 0.5f * (p + crealf(y4) * crealf(y4) + cimagf(y4) * cimagf(y4));
        bool periodic;

        r->hq += 0.1f * (prod - r->hq);
        r->hp += 0.1f * (pp - r->hp);
        r->l2 += 0.1f * (y * conjf(y2) - r->l2);
        periodic = r->hp > 0.0f && crealf(r->hq) > 0.8f * r->hp;
        if (periodic && crealf(r->l2) < -0.3f * r->hp)
        {
            r->s_run = 0;
            if (++r->ac_run == (int) (0.1 * 2 * V34_CC_BAUD))
                ev |= V34_CC_EV_AC;
        }
        else if (periodic && crealf(r->l2) > -0.1f * r->hp)
        {
            r->ac_run = 0;
            if (++r->s_run >= 24)
                r->s_seen = true;
        }
        else
        {
            r->ac_run = 0;
            r->s_run = 0;
        }
        /* The turn from Sh to S-bar-h: three samples together, since at T/2
         * the ones between symbol centres are smaller. */
        r->prod3[idx % 3] = crealf(prod);
        r->p3[idx % 3] = pp;
        if (r->s_seen && r->prod3[0] + r->prod3[1] + r->prod3[2] < -0.5f * (r->p3[0] + r->p3[1] + r->p3[2]))
        {
            if (++r->neg_run == 1)
            {
                r->s_seen = false;
                r->sbar_at = idx;
                /* The level is the far end's own now, not a silence's. */
                if (r->pwr_sym > 0.0f)
                    rescale(r);
                ev |= V34_CC_EV_SHBAR;
            }
        }
        else
        {
            r->neg_run = 0;
        }
    }

    /* PPh: four periods of eight symbols, recognised by its shape after two
     * of them. The best of the two T/2 phases is where the symbols are. */
    {
        v34_cf_t g = 0.0f;
        int s = 0;
        float rho = pph_corr(r, idx, &g, &s);

        if (rho > 0.75f && rho >= r->pph_rho && idx - r->pph_at > 64)
        {
            if (++r->pph_run >= 2)
            {
                r->pph_at = idx;
                r->parity = (int) (idx & 1);
                /* The equaliser's main tap undoes the gain and phase PPh
                 * arrived with. */
                memset(r->c, 0, sizeof(r->c));
                r->c[EQ_C] = 1.0f / g;
                r->theta = r->nu = 0.0f;
                r->trained = true;
                ev |= V34_CC_EV_PPH;
            }
        }
        else if (rho < 0.5f)
        {
            r->pph_run = 0;
        }
        r->pph_rho = rho;
    }

    /* The symbol clock: Gardner's detector on the samples either side of a
     * midpoint. Quick while it finds the clock, then gentle. */
    if ((idx & 1) == r->parity)
    {
        v34_cf_t mid = r->h[(idx - 1) % V34_CC_H];
        float e = crealf((y - y2) * conjf(mid));
        float sp = r->pwr_sym > 0.0f ? r->pwr_sym : p;
        double kp = (r->symbols < 40) ? 0.06 : 0.015;

        r->pwr_sym += 0.1f * (p - r->pwr_sym);
        if (sp > 0.0f)
        {
            e /= sp;
            if (e > 0.5f)
                e = 0.5f;
            else if (e < -0.5f)
                e = -0.5f;
            r->tau -= kp * e * T2;
            r->tfreq -= kp * kp / 4.0 * e * T2;
            if (r->tfreq > 0.0005 * T2)
                r->tfreq = 0.0005 * T2;
            else if (r->tfreq < -0.0005 * T2)
                r->tfreq = -0.0005 * T2;
        }
        if (r->carrier)
            ev |= symbol(r, y);
    }
    return ev;
}

int v34_ccrx_sample(v34_ccrx_t *r, float x)
{
    long long i = r->n;
    int ph = (int) ((i * 3) % r->cden);
    v34_cf_t m = r->floor_ * x * (r->cosv[ph] - I * r->sinv[ph]);
    v34_cf_t z = 0.0f;
    long long zi;
    int ev = 0;

    r->nbits = 0;
    r->mix[i % V34_CC_HIST] = m;
    if (i >= 2 * r->mf_half)
        for (int k = 0; k <= 2 * r->mf_half; k++)
            z += r->mix[(i - k) % V34_CC_HIST] * r->mf[k];
    zi = i - r->mf_half;
    if (zi >= 0)
        r->zf[zi % V34_CC_HIST] = z;
    while (zi >= 1 && r->tau <= (double) (zi - 1))
    {
        v34_cf_t y = zf_at(r, r->tau);

        r->tau += T2 + r->tfreq;
        ev |= half(r, y);
    }
    r->n++;
    return ev;
}
