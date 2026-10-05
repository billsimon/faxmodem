/* Two half-duplex V.34 modems back to back over a simulated line, driven the
 * way T.30 Annex F drives them: start-up to the control channel, data both
 * ways on it, a page on the primary channel, back to the control channel,
 * and again. Every bit is checked.
 *
 *   build/v34hdxtest [delay_ms] [loss_db] [noise_dbm0] [max_rate] [pages] [ppm] [seed]
 *   build/v34hdxtest soak [runs] [first_seed]
 *
 * With no arguments, runs a small matrix of lines; soak runs random ones and
 * prints, for each failure, the arguments that repeat it. */
#include "faxmodem/log.h"
#include "faxmodem/v34hdx.h"
#include "v34_dsp.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHUNK 160
#define GAP_SAMPLES 1600          /* a pause this long between bits starts a new session */

static uint32_t grng = 12345;

static double gauss(void)
{
    double u1, u2;

    grng = grng * 1664525u + 1013904223u;
    u1 = (grng + 1.0) / 4294967297.0;
    grng = grng * 1664525u + 1013904223u;
    u2 = (grng + 1.0) / 4294967297.0;
    return sqrt(-2.0 * log(u1)) * cos(2.0 * 3.14159265358979 * u2);
}

static uint8_t linear_to_ulaw(int16_t x)
{
    int sign = (x < 0) ? 0x80 : 0, mag = (x < 0) ? -(int) x : x, seg = 0;

    mag += 0x84;
    if (mag > 0x7FFF)
        mag = 0x7FFF;
    for (int m = mag >> 7; m > 1 && seg < 7; m >>= 1)
        seg++;
    return (uint8_t) ~(sign | (seg << 4) | ((mag >> (seg + 3)) & 0x0F));
}

static int16_t ulaw_to_linear(uint8_t u)
{
    int t;

    u = (uint8_t) ~u;
    t = (((u & 0x0F) << 3) + 0x84) << ((u & 0x70) >> 4);
    return (int16_t) ((u & 0x80) ? 0x84 - t : t - 0x84);
}

/* A stream of bits that both ends can generate: the sender pulls them, the
 * receiver checks them, each restarting at the beginning of a session. */
typedef struct
{
    uint32_t seed;
    uint32_t st;
    long long last;           /* line time of the last bit */
    long long count;          /* bits this session */
    long long sessions;
} stream_t;

static int stream_bit(stream_t *s, long long now)
{
    if (s->count == 0 || now - s->last > GAP_SAMPLES)
    {
        s->st = s->seed;
        s->count = 0;
        s->sessions++;
    }
    s->last = now;
    s->count++;
    s->st ^= s->st << 13;
    s->st ^= s->st >> 17;
    s->st ^= s->st << 5;
    return (int) (s->st & 1);
}

typedef struct
{
    stream_t ref;             /* what should arrive */
    long long good;           /* consecutive right ones this session */
    long long best;           /* the best session's run */
    bool broken;
    long long sessions_ok;
    long long expect;         /* a session counts if this many arrive right */
} check_t;

static void check_bit(check_t *c, int bit, long long now)
{
    bool fresh = c->ref.count == 0 || now - c->ref.last > GAP_SAMPLES;
    int want;

    if (fresh)
    {
        if (getenv("V34_PAGES") && c->ref.count > 0)
            fprintf(stderr, "    session %lld: %lld of %lld right before the first error\n", c->ref.sessions,
                    c->good, c->ref.count);
        if (c->good >= c->expect && c->expect > 0)
            c->sessions_ok++;
        c->good = 0;
        c->broken = false;
    }
    want = stream_bit(&c->ref, now);
    if (c->broken)
        return;
    if (bit == want)
    {
        c->good++;
        if (c->good > c->best)
            c->best = c->good;
    }
    else
    {
        c->broken = true;
    }
}

typedef struct
{
    const char *name;
    fm_v34h_t *m;
    long long *now;
    stream_t cc_out;
    check_t cc_in;
    stream_t pc_out;
    check_t pc_in;
    long long pc_limit;       /* page length in bits */
    long long pc_sent;
    bool cc_up, pc_up, pc_down, failed, not_v34;
    int cc_ups, pc_ups, pc_downs;
} side_t;

static int cc_get(void *user)
{
    side_t *s = user;

    return stream_bit(&s->cc_out, *s->now);
}

static void cc_put(void *user, int bit)
{
    side_t *s = user;

    check_bit(&s->cc_in, bit, *s->now);
}

static int pc_get(void *user)
{
    side_t *s = user;

    if (s->pc_sent >= s->pc_limit)
        return -1;
    s->pc_sent++;
    return stream_bit(&s->pc_out, *s->now);
}

static void pc_put(void *user, int bit)
{
    side_t *s = user;

    check_bit(&s->pc_in, bit, *s->now);
}

static void event(void *user, fm_v34h_event_t ev)
{
    side_t *s = user;

    switch (ev)
    {
    case FM_V34H_CC_UP:
        s->cc_up = true;
        s->cc_ups++;
        break;
    case FM_V34H_PRIMARY_UP:
        s->pc_up = true;
        s->pc_ups++;
        break;
    case FM_V34H_PRIMARY_DOWN:
        s->pc_down = true;
        s->pc_downs++;
        break;
    case FM_V34H_NOT_V34:
        s->not_v34 = true;
        break;
    case FM_V34H_FAILED:
        s->failed = true;
        break;
    }
}

typedef struct
{
    float buf[16384];
    int delay;
    double gain;
    double noise;
    double ppm;               /* the sender's clock against the receiver's */
} path_t;

/* Fractional delay by windowed sinc. V.34 at 3429 baud reaches 3.85 kHz,
 * close to Nyquist: linear interpolation took several dB off the top of the
 * band, and a short sinc's error there, changing as the fractional delay
 * drifts round, made bursts of errors every second that the modems then got
 * the blame for. 96 taps with a Kaiser window are accurate to that edge. */
#define SINC_TAPS 96
#define SINC_PHASES 512
static float sinc_tab[SINC_PHASES + 1][SINC_TAPS];
static bool sinc_ready;

static double bessel_i0(double x)
{
    double sum = 1.0, term = 1.0;

    for (int k = 1; k < 30; k++)
    {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
    }
    return sum;
}

static void sinc_init(void)
{
    for (int p = 0; p <= SINC_PHASES; p++)
    {
        double f = (double) p / SINC_PHASES;

        for (int k = 0; k < SINC_TAPS; k++)
        {
            double t = (double) (k - SINC_TAPS / 2 + 1) - f;
            double sinc = (fabs(t) < 1e-9) ? 1.0 : sin(3.14159265358979 * t) / (3.14159265358979 * t);
            double r = t / (SINC_TAPS / 2);
            double w = (fabs(r) < 1.0) ? bessel_i0(8.0 * sqrt(1.0 - r * r)) / bessel_i0(8.0) : 0.0;

            sinc_tab[p][k] = (float) (sinc * w);
        }
    }
    sinc_ready = true;
}

static float path_run(path_t *p, long long n, float x)
{
    /* A sender whose clock runs ppm fast: its samples come that much
     * quicker, and the delay shrinks. 120 samples in hand cover 100 ppm for
     * the two minutes a run may last. */
    double at = (double) n - p->delay - 160.0 + (double) n * p->ppm * 1e-6;
    long long i0 = (long long) floor(at);
    int ph = (int) lrint((at - (double) i0) * SINC_PHASES);
    float y = 0.0f;

    if (!sinc_ready)
        sinc_init();
    p->buf[n & 16383] = x;
    /* Taps reach SINC_TAPS/2 ahead, which the 120 samples in hand allow. */
    if (i0 - SINC_TAPS >= 0 && i0 + SINC_TAPS / 2 <= n)
        for (int k = 0; k < SINC_TAPS; k++)
            y += p->buf[(i0 + k - SINC_TAPS / 2 + 1) & 16383] * sinc_tab[ph][k];
    return (float) (y * p->gain + p->noise * gauss());
}

static int run(int delay_ms, double loss_db, double noise_dbm0, int max_rate, int pages, double ppm, unsigned seed,
               bool verbose)
{
    static side_t a, b;
    static path_t ab, ba;
    fm_v34h_params_t pa = { 0 }, pb = { 0 };
    long long now_a = 0, now_b = 0, n = 0;
    int16_t ta[CHUNK], tb[CHUNK], ra[CHUNK], rb[CHUNK];
    int phase = 0, done_pages = 0;
    long long phase_at = 0;
    int ok = 1;
    fm_v34h_stats_t sa, sb;

    /* Each run its own noise, so that any one can be repeated. */
    grng = seed ? seed : 12345;
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    memset(&ab, 0, sizeof(ab));
    memset(&ba, 0, sizeof(ba));
    a.name = "caller";
    b.name = "answerer";
    a.now = &now_a;
    b.now = &now_b;
    a.cc_out.seed = 0x1111;
    b.cc_in.ref.seed = 0x1111;
    b.cc_out.seed = 0x2222;
    a.cc_in.ref.seed = 0x2222;
    a.pc_out.seed = 0x3333;
    b.pc_in.ref.seed = 0x3333;
    a.cc_in.expect = b.cc_in.expect = 400;
    a.pc_limit = 60000;
    b.pc_in.expect = a.pc_limit - 64;
    ab.delay = ba.delay = delay_ms * 8;
    ab.gain = ba.gain = pow(10.0, -loss_db / 20.0);
    ab.noise = ba.noise = V34_DBM0_RMS * pow(10.0, noise_dbm0 / 20.0);
    ab.ppm = ppm;
    ba.ppm = -ppm;

    pa.calling = true;
    pa.max_rate = max_rate;
    pa.tx_power = -10.0f;
    pa.v17 = pa.v29 = pa.v27ter = true;
    pa.tag = "caller";
    pa.cc_get_bit = cc_get;
    pa.cc_put_bit = cc_put;
    pa.pc_get_bit = pc_get;
    pa.pc_put_bit = pc_put;
    pa.event = event;
    pa.user = &a;
    pb = pa;
    pb.calling = false;
    pb.tag = "answerer";
    pb.user = &b;
    a.m = fm_v34h_create(&pa);
    b.m = fm_v34h_create(&pb);
    if (a.m == NULL || b.m == NULL)
    {
        printf("cannot create the modems\n");
        return 0;
    }

    for (n = 0; n < 8000LL * 120; n += CHUNK)
    {
        fm_v34h_tx(a.m, ta, CHUNK);
        fm_v34h_tx(b.m, tb, CHUNK);
        for (int i = 0; i < CHUNK; i++)
        {
            float to_b = path_run(&ab, n + i, ta[i]);
            float to_a = path_run(&ba, n + i, tb[i]);
            /* A two-wire line: each end hears its own signal too. */
            float ea = 0.1f * ta[i], eb = 0.1f * tb[i];

            ra[i] = ulaw_to_linear(linear_to_ulaw((int16_t) lrintf(fmaxf(-32768.0f, fminf(32767.0f, to_a + ea)))));
            rb[i] = ulaw_to_linear(linear_to_ulaw((int16_t) lrintf(fmaxf(-32768.0f, fminf(32767.0f, to_b + eb)))));
        }
        for (int i = 0; i < CHUNK; i++)
        {
            now_a = n + i;
            fm_v34h_rx(a.m, &ra[i], 1);
            now_b = n + i;
            fm_v34h_rx(b.m, &rb[i], 1);
        }
        if (a.failed || b.failed || a.not_v34 || b.not_v34)
            break;

        /* T.30's part. */
        switch (phase)
        {
        case 0:
            /* Start-up: wait for the control channel on both. */
            if (a.cc_up && b.cc_up)
            {
                phase = 1;
                phase_at = n;
            }
            break;
        case 1:
            /* A second of control channel data both ways, then the page. */
            if (n - phase_at > 8000)
            {
                a.cc_up = b.cc_up = false;
                a.pc_up = b.pc_up = b.pc_down = false;
                a.pc_sent = 0;
                fm_v34h_primary(b.m);
                fm_v34h_primary(a.m);
                phase = 2;
            }
            break;
        case 2:
            /* The page goes, and the control channel comes back. */
            if (a.cc_up && b.cc_up && b.pc_down)
            {
                done_pages++;
                phase = (done_pages >= pages) ? 3 : 1;
                phase_at = n;
            }
            break;
        case 3:
            if (n - phase_at > 8000)
                goto out;
            break;
        }
    }
out:
    /* Close the last sessions. */
    check_bit(&a.cc_in, 0, n + 100000);
    check_bit(&b.cc_in, 0, n + 100000);
    check_bit(&b.pc_in, 0, n + 100000);
    fm_v34h_stats(a.m, &sa);
    fm_v34h_stats(b.m, &sb);
    if (verbose || phase != 3)
        printf("  [%u] delay %d ms, loss %.0f dB, noise %.0f dBm0, %+.0f ppm, cap %d: %s; %d bit/s at %d baud, SNR %.1f dB, "
               "round trip "
               "%d ms; control channel sessions clean %lld/%lld one way, %lld/%lld the other; pages clean "
               "%lld/%d\n",
               seed, delay_ms, loss_db, noise_dbm0, ppm, max_rate,
               phase == 3 ? "done" : (a.failed || b.failed) ? "FAILED" : (a.not_v34 || b.not_v34) ? "NOT V.34" : "stuck",
               sb.rate, sb.symbol_rate, sb.snr_db, sb.round_trip_ms, b.cc_in.sessions_ok, a.cc_out.sessions,
               a.cc_in.sessions_ok, b.cc_out.sessions, b.pc_in.sessions_ok, pages);
    if (phase != 3)
    {
        printf("    caller at '%s', answerer at '%s'\n", sa.stage, sb.stage);
        ok = 0;
    }
    else if (b.pc_in.sessions_ok != pages || b.cc_in.sessions_ok < pages || a.cc_in.sessions_ok < pages)
    {
        ok = 0;
    }
    fm_v34h_free(a.m);
    fm_v34h_free(b.m);
    return ok;
}

int main(int argc, char **argv)
{
    fm_log_init(getenv("V34_DEBUG") ? FM_LOG_DEBUG : FM_LOG_WARN, false);
    if (argc > 1)
    {
        int ok;

        if (strcmp(argv[1], "soak") == 0)
        {
            /* Random lines: soak [runs] [first seed]. Each failure prints
             * the arguments that repeat it. */
            int runs = argc > 2 ? atoi(argv[2]) : 50;
            unsigned seed = argc > 3 ? (unsigned) atoi(argv[3]) : 1;
            int fails = 0;
            static const int caps[] = { 33600, 33600, 33600, 28800, 24000, 14400, 9600 };

            for (int i = 0; i < runs; i++, seed++)
            {
                uint32_t r = seed * 2654435761u;
                int delay = (int) (r % 400);
                double loss = (double) ((r >> 9) % 28);
                double noise = -(double) (55 + (r >> 14) % 25);
                double ppm = (double) ((int) ((r >> 19) % 201) - 100);
                int cap = caps[(r >> 27) % 7];

                if (!run(delay, loss, noise, cap, 2 + (int) (seed % 3), ppm, seed, false))
                {
                    fails++;
                    printf("    repeat: v34hdxtest %d %.0f %.0f %d %d %.0f %u\n", delay, loss, noise, cap,
                           2 + (int) (seed % 3), ppm, seed);
                }
            }
            printf("soak: %d of %d failed\n", fails, runs);
            fm_log_close();
            return fails ? 1 : 0;
        }
        ok = run(atoi(argv[1]), argc > 2 ? atof(argv[2]) : 10.0, argc > 3 ? atof(argv[3]) : -60.0,
                 argc > 4 ? atoi(argv[4]) : 33600, argc > 5 ? atoi(argv[5]) : 2, argc > 6 ? atof(argv[6]) : 0.0,
                 argc > 7 ? (unsigned) atoi(argv[7]) : 12345, true);
        fm_log_close();
        return ok ? 0 : 1;
    }
    {
        static const struct
        {
            int delay;
            double loss, noise;
            int cap;
            double ppm;
            int pages;
        } lines[] = { { 0, 0.0, -80.0, 33600, 0.0, 2 },      { 20, 10.0, -60.0, 33600, 0.0, 2 },
                      { 150, 20.0, -55.0, 33600, 0.0, 2 },   { 300, 6.0, -50.0, 14400, 0.0, 2 },
                      { 40, 12.0, -60.0, 33600, 80.0, 4 },   { 90, 8.0, -58.0, 33600, -100.0, 4 },
                      { 10, 25.0, -58.0, 33600, 30.0, 3 },   { 60, 30.0, -60.0, 33600, -50.0, 3 },
                      { 5, 6.0, -45.0, 33600, 0.0, 2 },      { 250, 15.0, -55.0, 9600, 60.0, 3 } };
        int fails = 0;

        printf("V.34 half duplex, modem to modem\n");
        for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++)
            if (!run(lines[i].delay, lines[i].loss, lines[i].noise, lines[i].cap, lines[i].pages, lines[i].ppm,
                     (unsigned) (i + 1), true))
                fails++;
        printf(fails ? "%d FAILED\n" : "all passed\n", fails);
        fm_log_close();
        return fails ? 1 : 0;
    }
}
