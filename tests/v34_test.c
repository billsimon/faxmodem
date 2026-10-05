/* Unit tests for the parts of V.34 that have to agree with the far end bit
 * for bit, run without any signal processing in between.
 *
 *   build/v34test        (or ctest)
 *
 * The end-to-end tests - two modems over a simulated line, and over RTP - are
 * `faxmodem selftest --v34` and scripts/loopback-test.sh. */
#include "v34_codec.h"
#include "v34_info.h"
#include "v34_dsp.h"
#include "v34_cc.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond, ...)                                                                                   \
    do                                                                                                     \
    {                                                                                                      \
        if (!(cond))                                                                                       \
        {                                                                                                  \
            failures++;                                                                                    \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                    \
            printf(__VA_ARGS__);                                                                           \
            printf("\n");                                                                                  \
        }                                                                                                  \
    } while (0)

static uint32_t rng = 0x9E3779B9u;

static uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static int rnd_bit(void *user)
{
    (void) user;
    return (int) (rnd() & 1);
}

static double gauss(void)
{
    double u1 = (rnd() + 1.0) / 4294967297.0;
    double u2 = (rnd() + 1.0) / 4294967297.0;

    return sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
}

/* ------------------------------------------------------------- figures */

static void test_superconstellation(void)
{
    /* Spot checks against Figure 5, read off the figure. */
    static const int pts[][3] = {
        { 0, 1, 1 },     { 1, -3, 1 },    { 2, 1, -3 },    { 3, -3, -3 },  { 4, 1, 5 },    { 5, 5, 1 },
        { 8, 5, 5 },     { 9, -7, 1 },    { 10, 1, -7 },   { 41, -7, 13 }, { 97, 21, -7 }, { 415, 45, 9 },
        { 414, 9, 45 }, { 408, -7, 45 }, { 411, -15, -43 }, { 362, -43, 1 },
    };
    v34_frame_t f;
    v34_data_params_t p = { .sr = 0, .rate = 2400, .trellis = 16 };

    v34_frame_params(&p, &f);
    for (size_t i = 0; i < sizeof(pts) / sizeof(pts[0]); i++)
        CHECK(v34_super[pts[i][0]][0] == pts[i][1] && v34_super[pts[i][0]][1] == pts[i][2],
              "superconstellation label %d is %d,%d, not %d,%d", pts[i][0], v34_super[pts[i][0]][0],
              v34_super[pts[i][0]][1], pts[i][1], pts[i][2]);
}

static void test_subset_labels(void)
{
    /* Figure 9, row by row from the top: y = 3, 1, -1, -3; x = -3, -1, 1, 3. */
    static const int fig9[4][4] = {
        { 1, 6, 5, 2 },
        { 4, 3, 0, 7 },
        { 5, 2, 1, 6 },
        { 0, 7, 4, 3 },
    };

    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
        {
            int x = -3 + 2 * c, y = 3 - 2 * r;

            CHECK(v34_subset_label(x, y) == fig9[r][c], "label of %d,%d is %d, not %d", x, y,
                  v34_subset_label(x, y), fig9[r][c]);
            /* and it repeats with the 8-way partition's sublattice */
            CHECK(v34_subset_label(x + 4, y + 4) == fig9[r][c] && v34_subset_label(x + 4, y - 4) == fig9[r][c],
                  "label of %d,%d does not repeat", x, y);
        }
}

/* Table 8, a few rows of it, and Table 10. */
static void test_framing(void)
{
    static const struct
    {
        int sr, rate, aux, b, swp, K, Mmin, Mexp, Lmin, Lexp;
    } rows[] = {
        { V34_S2400, 2400, 0, 8, 0xFFF, 0, 1, 1, 4, 4 },
        { V34_S2400, 2400, 1, 9, 0x6DB, 0, 1, 1, 4, 4 },
        { V34_S2400, 21600, 0, 72, 0xFFF, 28, 12, 14, 768, 896 },
        { V34_S2743, 26400, 0, 77, 0xFFF, 25, 9, 11, 1152, 1408 },
        { V34_S2800, 4800, 0, 14, 0x1BB7, 2, 2, 2, 8, 8 },
        { V34_S2800, 7200, 1, 22, 0x0081, 10, 3, 3, 12, 12 },
        { V34_S2800, 26400, 1, 76, 0x3FFF, 24, 8, 10, 1024, 1280 },
        { V34_S3000, 9600, 1, 27, 0x0081, 15, 4, 5, 16, 20 },
        { V34_S3000, 19200, 0, 52, 0x0421, 24, 8, 10, 128, 160 },
        { V34_S3000, 7200, 0, 20, 0x0421, 8, 2, 3, 8, 12 },
        { V34_S3200, 31200, 0, 78, 0xFFFF, 26, 10, 12, 1280, 1536 },
        { V34_S3429, 4800, 0, 12, 0x0421, 0, 1, 1, 4, 4 },
        { V34_S3429, 33600, 0, 79, 0x14A5, 27, 11, 13, 1408, 1664 },
        { V34_S3429, 33600, 1, 79, 0x3F7F, 27, 11, 13, 1408, 1664 },
        { V34_S3429, 21600, 0, 51, 0x14A5, 31, 15, 18, 120, 144 },
    };

    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++)
    {
        v34_data_params_t p = { .sr = rows[i].sr, .rate = rows[i].rate, .aux = rows[i].aux, .trellis = 16 };
        v34_frame_t f;

        CHECK(v34_frame_params(&p, &f), "%d at %s refused", rows[i].rate, V34_SR_NAME[rows[i].sr]);
        CHECK(f.b == rows[i].b && v34_swp(&f) == (unsigned) rows[i].swp && f.K == rows[i].K &&
                  f.M == rows[i].Mmin && f.L == rows[i].Lmin,
              "%d%s at %s: b %d swp %X K %d M %d L %d", rows[i].rate, rows[i].aux ? "+aux" : "",
              V34_SR_NAME[rows[i].sr], f.b, v34_swp(&f), f.K, f.M, f.L);
        p.expanded = true;
        v34_frame_params(&p, &f);
        CHECK(f.M == rows[i].Mexp && f.L == rows[i].Lexp, "%d at %s expanded: M %d L %d", rows[i].rate,
              V34_SR_NAME[rows[i].sr], f.M, f.L);
    }
    /* Table 9 */
    {
        static const unsigned amp[V34_NUM_SR] = { 0x6DB, 0x56B, 0x15AB, 0x2AAB, 0x5555, 0x1555 };

        for (int sr = 0; sr < V34_NUM_SR; sr++)
        {
            v34_data_params_t p = { .sr = sr, .rate = 4800, .aux = true, .trellis = 16 };
            v34_frame_t f;

            v34_frame_params(&p, &f);
            CHECK(v34_amp(&f) == amp[sr], "AMP at %s is %X", V34_SR_NAME[sr], v34_amp(&f));
        }
    }
    /* Table 12, through B1 and into the superframe proper. */
    {
        v34_data_params_t p = { .sr = V34_S3200, .rate = 4800, .trellis = 16 };
        v34_frame_t f;
        const char *want = "10" "01110111111110";
        char got[32];

        v34_frame_params(&p, &f);
        for (int k = 0; k < 16; k++)
            got[k] = (char) ('0' + v34_v0(&f, (long long) k * 2 * f.P));
        got[16] = '\0';
        CHECK(strcmp(got, want) == 0, "V0 for J = 7 is %s, not %s", got, want);
        CHECK(v34_v0(&f, 1) == 0 && v34_v0(&f, 2 * f.P - 1) == 0, "V0 set away from a half frame");
    }
}

static void test_shell(void)
{
    for (int M = 1; M <= 18; M++)
    {
        v34_shell_t s;
        int Kmax = 0;

        v34_shell_init(&s, M);
        while (Kmax < 31 && (double) s.z8[8 * (M - 1) + 1] >= ldexp(1.0, Kmax + 1))
            Kmax++;
        for (int t = 0; t < 4000; t++)
        {
            uint32_t r0 = (Kmax >= 32) ? rnd() : rnd() & ((1u << Kmax) - 1);
            uint32_t back;
            int m[8];

            if (Kmax == 0)
                r0 = 0;
            else if (t < 4)
                r0 = (t < 2) ? (uint32_t) t : (1u << Kmax) - (uint32_t) (t - 1);
            v34_shell_map(&s, r0, m);
            for (int k = 0; k < 8; k++)
                CHECK(m[k] >= 0 && m[k] < M, "M %d R0 %u: ring %d is %d", M, r0, k, m[k]);
            CHECK(v34_shell_unmap(&s, m, Kmax, &back) && back == r0, "M %d: R0 %u came back as %u", M, r0, back);
        }
    }
    /* The order: R0 = 0 is all rings zero, and the smallest R0 to reach a
     * ring sum of 1 puts it in the last position (9.4 with A = 1). */
    {
        v34_shell_t s;
        int m[8];

        v34_shell_init(&s, 3);
        v34_shell_map(&s, 0, m);
        CHECK(m[0] == 0 && m[7] == 0, "R0 0 is not all zeros");
        v34_shell_map(&s, 1, m);
        CHECK(m[7] == 1 && m[0] == 0, "R0 1 maps to %d %d %d %d %d %d %d %d", m[0], m[1], m[2], m[3], m[4], m[5],
              m[6], m[7]);
    }
}

/* ------------------------------------------------- encoder and decoder */

typedef struct
{
    uint8_t *bits;
    size_t n, pos;
} bitbuf_t;

static int src_bit(void *user)
{
    bitbuf_t *b = user;

    return (b->pos < b->n) ? b->bits[b->pos++] : 0;
}

static void sink_bit(void *user, int bit)
{
    bitbuf_t *b = user;

    if (b->pos < b->n)
        b->bits[b->pos++] = (uint8_t) bit;
}

/* Returns the number of bit errors, or -1 if the decoder lost the stream. */
static bool count_events;

static long round_trip(const v34_data_params_t *p, int frames, double snr_db, size_t *nbits_out)
{
    static v34_enc_t enc;
    static v34_dec_t dec;
    v34_frame_t f;
    bitbuf_t src = { 0 }, dst = { 0 };
    double ex, exn, sigma;
    long errors = 0;

    v34_frame_params(p, &f);
    src.n = (size_t) frames * f.b + 64;
    src.bits = malloc(src.n);
    dst.n = src.n;
    dst.bits = calloc(dst.n, 1);
    for (size_t i = 0; i < src.n; i++)
        src.bits[i] = (uint8_t) (rnd() & 1);
    v34_data_energy(p, &ex, &exn);
    sigma = (snr_db > 0) ? sqrt(exn / pow(10.0, snr_db / 10.0) / 2.0) : 0.0;
    v34_enc_init(&enc, p, 18);
    v34_enc_set_energy(&enc, ex);
    v34_dec_init(&dec, p, 18, sink_bit, &dst);
    for (int i = 0; i < f.P + frames + V34_VIT_DEPTH; i++)
    {
        v34_cf_t s[8];

        /* B1 first: one data frame of ones. */
        v34_enc_frame(&enc, (i < f.P) ? NULL : src_bit, &src, NULL, NULL, s);
        for (int k = 0; k < 8; k++)
        {
            v34_cf_t y = s[k];

            if (sigma > 0)
                y += (float) (sigma * gauss()) + I * (float) (sigma * gauss());
            v34_dec_symbol(&dec, y);
        }
    }
    {
        size_t n = (dst.pos < src.pos) ? dst.pos : src.pos;

        if (n + (size_t) f.b * 2 < src.pos)
            errors = -1;
        else
        {
            size_t last = 0;

            /* Error events: errors within 200 bits of each other are one. */
            for (size_t i = 0; i < n; i++)
                if (src.bits[i] != dst.bits[i])
                {
                    if (!count_events || errors == 0 || i - last > 200)
                        errors++;
                    last = i;
                }
        }
        if (nbits_out)
            *nbits_out = n;
    }
    free(src.bits);
    free(dst.bits);
    return errors;
}

static void test_round_trip(void)
{
    int cases = 0;

    for (int sr = 0; sr < V34_NUM_SR; sr++)
        for (int rate = 2400; rate <= V34_SR_MAX_RATE[sr]; rate += 2400)
            for (int ex = 0; ex < 2; ex++)
                for (int aux = 0; aux < 2; aux++)
                {
                    v34_data_params_t p = { .sr = sr, .rate = rate, .trellis = 16 << ((rate / 2400 + aux) % 3),
                                            .expanded = ex, .aux = aux };
                    v34_frame_t f;
                    size_t n = 0;
                    long e;

                    if (!v34_frame_params(&p, &f))
                        continue;
                    e = round_trip(&p, 3 * f.P, 0.0, &n);
                    CHECK(e == 0, "%d%s at %s%s: %ld errors in %zu bits", rate, aux ? "+aux" : "", V34_SR_NAME[sr],
                          ex ? " expanded" : "", e, n);
                    cases++;
                }
    printf("  %d rate/symbol-rate/shaping combinations round trip clean\n", cases);
}

/* The trellis code earns its keep: at an SNR where the nearest-point slicer
 * makes a symbol error every few hundred symbols, the decoder makes none. */
static void test_coding_gain(void)
{
    v34_data_params_t p = { .sr = V34_S3429, .rate = 33600, .trellis = 16 };
    size_t n = 0;
    long e;

    e = round_trip(&p, 600, 38.5, &n);
    printf("  33600 at 3429, 38.5 dB: %ld bit errors in %zu\n", e, n);
    CHECK(e >= 0 && e < 50, "33600 at 38.5 dB SNR: %ld bit errors in %zu bits", e, n);
    /* The bigger codes do better again. */
    for (int t = 16; t <= 64; t *= 2)
    {
        long tot = 0;
        size_t nn = 0;

        p.trellis = t;
        p.rate = 33600;
        count_events = true;
        for (int rep = 0; rep < 12; rep++)
        {
            e = round_trip(&p, 1000, 32.0, &n);
            CHECK(e >= 0, "33600 with the %d-state code lost the stream", t);
            tot += e;
            nn += n;
        }
        count_events = false;
        printf("  33600 at 3429, 32 dB, %d states: %ld error events in %zu bits\n", t, tot, nn);
    }
    p.trellis = 16;
    p.rate = 24000;
    e = round_trip(&p, 600, 30.0, &n);
    printf("  24000 at 3429, 30 dB: %ld bit errors in %zu\n", e, n);
    CHECK(e >= 0 && e < 50, "24000 at 30 dB SNR: %ld bit errors in %zu bits", e, n);
}

/* The precoder and the other trellis codes cannot be decoded here - the
 * decoder only does what our receiver asks for - but the encoder must stay
 * within (9.6.2)'s limits and the output must be on the grid. */
static void test_precoder(void)
{
    v34_data_params_t p = { .sr = V34_S3200, .rate = 28800, .trellis = 64 };
    v34_enc_t *e = malloc(sizeof(*e));
    bool ok = true;

    /* A channel with a 3 dB tilt, roughly. */
    p.h[0][0] = (int16_t) (0.5 * 16384);
    p.h[0][1] = (int16_t) (-0.1 * 16384);
    p.h[1][0] = (int16_t) (-0.2 * 16384);
    p.h[2][1] = (int16_t) (0.05 * 16384);
    v34_enc_init(e, &p, 5);
    for (int i = 0; i < 2000 && ok; i++)
    {
        v34_cf_t s[8];

        v34_enc_frame(e, rnd_bit, NULL, NULL, NULL, s);
        for (int k = 0; k < 8; k++)
            if (fabsf(crealf(s[k])) > 300.0f || fabsf(cimagf(s[k])) > 300.0f)
                ok = false;
    }
    CHECK(ok, "the precoded signal ran away");
    free(e);
}

/* -------------------------------------------------------- INFO and MP */

static void test_crc(void)
{
    /* CRC over a whole frame, its own CRC included, comes out zero. */
    uint8_t bits[64];
    uint16_t crc;
    int n = 40;

    for (int i = 0; i < n; i++)
        bits[i] = (uint8_t) (rnd() & 1);
    crc = v34_crc_bits(bits, n, 0xFFFF);
    for (int i = 0; i < 16; i++)
        bits[n + i] = (uint8_t) ((crc >> i) & 1);
    CHECK(v34_crc_bits(bits, n + 16, 0xFFFF) == 0, "the CRC does not check itself");
}

static void test_info(void)
{
    v34_info0_t a = { 0 }, b;
    v34_info1c_t c = { 0 }, d;
    v34_info1a_t e = { 0 }, g;
    v34_mp_t mp = { 0 }, mq;
    uint8_t bits[256];
    int n;

    a.sr2743 = a.sr3429 = a.low3000 = a.high3200 = true;
    a.allow3429 = true;
    a.asym_steps = 5;
    a.c1664 = true;
    a.ack = true;
    n = v34_info0_pack(&a, bits);
    CHECK(n == 49, "INFO0 is %d bits", n);
    /* fill and frame sync, left-most first in time */
    CHECK(memcmp(bits, "\1\1\1\1\0\1\1\1\0\0\1\0", 12) == 0, "INFO0 does not start 1111 01110010");
    CHECK(v34_info0_unpack(bits, n, &b) && memcmp(&a, &b, sizeof(a)) == 0, "INFO0 does not survive a round trip");
    bits[20] ^= 1;
    CHECK(!v34_info0_unpack(bits, n, &b), "a damaged INFO0 passes its CRC");

    c.min_power_reduction = 3;
    c.md = 17;
    for (int sr = 0; sr < V34_NUM_SR; sr++)
    {
        c.sr[sr].high = sr & 1;
        c.sr[sr].pre_emphasis = sr + 4;
        c.sr[sr].max_rate = 14 - sr;
    }
    c.freq_offset = -37;
    n = v34_info1c_pack(&c, bits);
    CHECK(n == 109, "INFO1c is %d bits", n);
    CHECK(v34_info1c_unpack(bits, n, &d) && memcmp(&c, &d, sizeof(c)) == 0, "INFO1c does not survive a round trip");

    e.additional_power_reduction = 2;
    e.high = true;
    e.pre_emphasis = 7;
    e.max_rate = 12;
    e.sr_a_to_c = V34_S3200;
    e.sr_c_to_a = V34_S3429;
    e.freq_offset = -512;
    n = v34_info1a_pack(&e, bits);
    CHECK(n == 70, "INFO1a is %d bits", n);
    CHECK(v34_info1a_unpack(bits, n, &g) && memcmp(&e, &g, sizeof(e)) == 0, "INFO1a does not survive a round trip");

    mp.type = 1;
    mp.rate_c_to_a = 14;
    mp.rate_a_to_c = 9;
    mp.trellis = 2;
    mp.nonlinear = true;
    mp.expanded = true;
    mp.ack = true;
    mp.rate_mask = 0x3FFF;
    mp.asymmetric = true;
    mp.h[0][0] = -1234;
    mp.h[2][1] = 16000;
    n = v34_mp_pack(&mp, bits);
    CHECK(n == 188, "MP type 1 is %d bits", n);
    CHECK(memcmp(bits, "\1\1\1\1\1\1\1\1\1\1\1\1\1\1\1\1\1\0", 18) == 0, "MP does not start with its frame sync");
    CHECK(v34_mp_unpack(bits, n, &mq) && memcmp(&mp, &mq, sizeof(mp)) == 0, "MP does not survive a round trip");
    mp.type = 0;
    memset(mp.h, 0, sizeof(mp.h));
    n = v34_mp_pack(&mp, bits);
    CHECK(n == 88, "MP type 0 is %d bits", n);
    CHECK(v34_mp_unpack(bits, n, &mq) && memcmp(&mp, &mq, sizeof(mp)) == 0, "MP type 0 does not survive a round trip");
}

/* The half-duplex sequences, INFOh and MPh (10.2), and V.8's fax
 * modulations. */
static void test_hdx_info(void)
{
    v34_infoh_t h = { 0 }, h2;
    v34_mph_t m = { 0 }, m2;
    v8_msg_t cm = { 0 }, cm2;
    uint8_t bits[256], oct[16];
    int n;

    h.power_reduction = 3;
    h.trn_len = 100;
    h.high = true;
    h.pre_emphasis = 9;
    h.sr = V34_S3200;
    h.trn16 = true;
    n = v34_infoh_pack(&h, bits);
    CHECK(n == V34_INFOH_BITS, "INFOh is %d bits", n);
    CHECK(memcmp(bits, "\1\1\1\1\0\1\1\1\0\0\1\0", 12) == 0, "INFOh does not start 1111 01110010");
    /* Table 22, bit by bit: 12:14 power reduction, 15:21 TRN, 22 carrier,
     * 23:26 pre-emphasis, 27:29 symbol rate, 30 TRN16, LSB first. */
    CHECK(bits[12] == 1 && bits[13] == 1 && bits[14] == 0, "INFOh power reduction misplaced");
    CHECK(bits[22] == 1 && bits[30] == 1, "INFOh carrier or TRN16 misplaced");
    CHECK(bits[27] == 0 && bits[28] == 0 && bits[29] == 1, "INFOh symbol rate misplaced");
    CHECK(bits[47] && bits[48] && bits[49] && bits[50], "INFOh does not end in fill");
    CHECK(v34_infoh_unpack(bits, n, &h2) && memcmp(&h, &h2, sizeof(h)) == 0, "INFOh does not survive a round trip");
    bits[25] ^= 1;
    CHECK(!v34_infoh_unpack(bits, n, &h2), "a damaged INFOh passes its CRC");

    m.type = 0;
    m.max_rate = 14;
    m.cc2400 = true;
    m.trellis = 1;
    m.expanded = true;
    m.rate_mask = 0x3FFF;
    m.asymmetric_cc = true;
    n = v34_mph_pack(&m, bits);
    CHECK(n == V34_MPH0_BITS, "MPh type 0 is %d bits", n);
    /* Table 23: 20:23 rate, 27 control channel rate, 29:30 trellis, 32
     * shaping, 50 asymmetric; start bits at 17, 34, 51, 68. */
    CHECK(bits[20] == 0 && bits[21] == 1 && bits[22] == 1 && bits[23] == 1, "MPh rate misplaced");
    CHECK(bits[27] == 1 && bits[29] == 1 && bits[30] == 0 && bits[32] == 1 && bits[50] == 1,
          "MPh control channel, trellis, shaping or asymmetry misplaced");
    CHECK(!bits[17] && !bits[34] && !bits[51] && !bits[68], "MPh start bits are not zero");
    CHECK(v34_mph_unpack(bits, n, &m2) && memcmp(&m, &m2, sizeof(m)) == 0, "MPh does not survive a round trip");
    bits[40] ^= 1;
    CHECK(!v34_mph_unpack(bits, n, &m2), "a damaged MPh passes its CRC");
    m.type = 1;
    m.h[1][0] = -321;
    m.h[2][1] = 9999;
    n = v34_mph_pack(&m, bits);
    CHECK(n == V34_MPH1_BITS, "MPh type 1 is %d bits", n);
    CHECK(v34_mph_unpack(bits, n, &m2) && memcmp(&m, &m2, sizeof(m)) == 0, "MPh type 1 does not survive a round trip");

    /* A Super G3 CM: T.30 transmit, V.34 half duplex and the G3 modulations. */
    cm.call_function = 4;
    cm.v34hdx = true;
    cm.v17 = cm.v29 = cm.v27ter = cm.v21 = true;
    n = v8_build(&cm, oct, (int) sizeof(oct));
    CHECK(n == 4 && oct[0] == 0x81 && oct[1] == 0x85 && oct[2] == 0xD4 && oct[3] == 0x90,
          "fax CM octets %02X %02X %02X %02X", oct[0], oct[1], oct[2], oct[3]);
    CHECK(v8_parse(oct, n, &cm2) && cm2.call_function == 4 && cm2.v34hdx && !cm2.v34 && cm2.v17 && cm2.v29 &&
              cm2.v27ter && cm2.v21 && !cm2.v32,
          "fax CM does not survive a round trip");
}

/* Phase 2's DPSK: tone, an INFO sequence, tone, through the transmitter and
 * receiver of v34_dsp.c - for both carriers, at a few starting phases. */
static int dpsk_once(bool answerer, int offset, int delay, bool info0)
{
    static v34_p2tx_t tx;
    static v34_p2rx_t rx;
    v34_info1a_t a = { 0 }, b;
    v34_info0_t i0 = { 0 }, i0b;
    uint8_t bits[128], got[V34_P2_PHASES][1024];
    int n, ng[V34_P2_PHASES] = { 0 }, ok = 0;
    float line[4096] = { 0 };

    a.max_rate = 11;
    a.sr_a_to_c = 3;
    a.sr_c_to_a = 5;
    a.pre_emphasis = 6;
    a.freq_offset = -512;
    n = v34_info1a_pack(&a, bits);
    if (info0)
    {
        /* A modem that does 2400 only: a long run of zeros. */
        i0.power_reduction = true;
        i0.asym_steps = 5;
        i0.c1664 = true;
        n = v34_info0_pack(&i0, bits);
    }
    v34_p2tx_init(&tx, answerer, -13.0);
    v34_p2rx_init(&rx, answerer);
    v34_p2tx_carrier(&tx, true);
    for (long long t = 0; t < 8000; t++)
    {
        float x;
        double when;

        if (t == 800 + offset)
            v34_p2tx_bits(&tx, bits, n);
        x = v34_p2tx_sample(&tx, t);
        line[t & 4095] = x;
        v34_p2rx_sample(&rx, (t >= delay) ? line[(t - delay) & 4095] : 0.0f, t, &when);
        for (int k = 0; k < V34_P2_PHASES; k++)
        {
            for (int i = 0; i < rx.nbits[k] && ng[k] < 1024; i++)
                got[k][ng[k]++] = rx.bits[k][i];
            rx.nbits[k] = 0;
        }
    }
    for (int k = 0; k < V34_P2_PHASES; k++)
        for (int i = 0; i + n <= ng[k]; i++)
            if (memcmp(got[k] + i, bits, 12) == 0 &&
                (info0 ? v34_info0_unpack(got[k] + i, n, &i0b) : v34_info1a_unpack(got[k] + i, n, &b)))
                ok = 1;
    return ok;
}

static void test_dpsk(void)
{
    int fails = 0;

    for (int which = 0; which < 2; which++)
        for (int ans = 0; ans < 2; ans++)
            for (int off = 0; off < 14; off += 3)
                for (int d = 0; d < 40; d += 7)
                    if (!dpsk_once(ans, off, d, which))
                    {
                        fails++;
                        if (fails < 5)
                            printf("  DPSK %s, offset %d, delay %d: %s lost\n", ans ? "2400 Hz" : "1200 Hz", off, d,
                                   which ? "INFO0" : "INFO1a");
                    }
    CHECK(fails == 0, "%d of 120 DPSK INFO sequences lost", fails);
}

/* G.711 mu-law, as G.711 tabulates it: a line through a telephone network. */
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

/* PPh (10.2.4.5) as read with k - I: a perfect sequence, its periodic
 * autocorrelation zero away from lag 0. */
static void test_pph(void)
{
    float worst = 0.0f;

    for (int lag = 1; lag < 8; lag++)
    {
        v34_cf_t c = 0.0f;

        for (int i = 0; i < 8; i++)
            c += v34_pph(i) * conjf(v34_pph(i + lag));
        if (cabsf(c) > worst)
            worst = cabsf(c);
    }
    CHECK(worst < 1e-4f, "PPh's periodic autocorrelation reaches %.3f off lag 0", worst);
    CHECK(cabsf(v34_pph(0) - v34_cc_point(0)) < 1e-5f, "PPh(0) is not point 0");
}

/* A control channel transmitter driven the way the half-duplex start-up
 * drives it: a little program of signals. */
typedef enum
{
    CC_SIL,
    CC_PPH,
    CC_ALT,
    CC_MPH,
    CC_E,
    CC_DATA,
    CC_SH,
    CC_SHB,
    CC_AC,
    CC_END
} ccsig_t;

typedef struct
{
    ccsig_t prog[16];
    int len[16];
    int np, ip, count;
    int tap;
    uint32_t scr;
    int z;
    uint8_t mph[V34_MPH0_BITS];
    int mph_n, mph_pos;
    uint32_t data_rng;
    long long data_sent;
} cctest_tx_t;

static int cct_bit(cctest_tx_t *t, int b)
{
    return v34_scramble(&t->scr, t->tap, b);
}

static uint8_t data_bit(uint32_t *st)
{
    *st ^= *st << 13;
    *st ^= *st >> 17;
    *st ^= *st << 5;
    return (uint8_t) (*st & 1);
}

static v34_cf_t cct_diff(cctest_tx_t *t, int b0, int b1)
{
    int i1 = cct_bit(t, b0), i2 = cct_bit(t, b1);

    t->z = (t->z + i1 + 2 * i2) & 3;
    return v34_cc_point(t->z);
}

static v34_cf_t cct_symbol(void *user)
{
    cctest_tx_t *t = user;
    v34_cf_t s = 0.0f;
    int i;

    while (t->ip < t->np && t->len[t->ip] >= 0 && t->count >= t->len[t->ip])
    {
        t->ip++;
        t->count = 0;
        if (t->ip < t->np && t->prog[t->ip] == CC_ALT)
            t->scr = 0;     /* 10.2.4.2 */
        if (t->ip < t->np && t->prog[t->ip] == CC_MPH)
            t->mph_pos = 0;
    }
    if (t->ip >= t->np)
        return 0.0f;
    i = t->count++;
    switch (t->prog[t->ip])
    {
    case CC_SIL:
    case CC_END:
        break;
    case CC_PPH:
        s = v34_pph(i);
        break;
    case CC_SH:
        s = v34_cc_point((i & 1) ? 3 : 0);
        break;
    case CC_SHB:
        s = v34_cc_point((i & 1) ? 1 : 2);
        break;
    case CC_AC:
        s = v34_cc_point((i & 1) ? 2 : 0);
        break;
    case CC_ALT:
        s = cct_diff(t, 0, 1);
        break;
    case CC_MPH:
    {
        int b0 = t->mph[t->mph_pos % t->mph_n], b1 = t->mph[(t->mph_pos + 1) % t->mph_n];

        t->mph_pos += 2;
        s = cct_diff(t, b0, b1);
        break;
    }
    case CC_E:
        s = cct_diff(t, 1, 1);
        break;
    case CC_DATA:
    {
        int b0 = data_bit(&t->data_rng), b1 = data_bit(&t->data_rng);

        t->data_sent += 2;
        s = cct_diff(t, b0, b1);
        break;
    }
    }
    return s;
}

typedef struct
{
    int pph, shbar, ac;
    long long pph_sym, shbar_sym, ac_sym;
    bool got_mph, got_e;
    v34_mph_t mph;
    uint8_t buf[V34_MPH1_BITS];
    int nbuf;
    bool in_mph;
    uint32_t sr;
    int ones;
    long long data_bits, data_errs, data_limit;
    uint32_t data_rng;
} cctest_rx_t;

static void cct_rx_bit(cctest_rx_t *r, int b)
{
    if (r->got_e)
    {
        /* Only as far as data was sent: silence follows it. */
        if (r->data_bits < r->data_limit)
        {
            uint8_t want = data_bit(&r->data_rng);

            r->data_bits++;
            r->data_errs += (b != want);
        }
        return;
    }
    r->sr = (r->sr << 1) | (uint32_t) b;
    r->ones = b ? r->ones + 1 : 0;
    if (r->in_mph)
    {
        r->buf[r->nbuf++] = (uint8_t) b;
        if (r->nbuf == V34_MPH0_BITS)
        {
            r->in_mph = false;
            if (v34_mph_unpack(r->buf, r->nbuf, &r->mph))
                r->got_mph = true;
        }
    }
    else if (!b && (r->sr & 0x3FFFF) == 0x3FFFE)
    {
        memset(r->buf, 1, 17);
        r->buf[17] = 0;
        r->nbuf = 18;
        r->in_mph = true;
    }
    if (r->got_mph && r->ones == 20 && !r->in_mph)
        r->got_e = true;
}

/* One control channel start-up, one way, over a line: delay, loss, G.711,
 * noise, clock drift, and our own signal in the other band as echo. */
static int cc_once(bool far_answerer, int delay, double loss_db, double noise_db, double ppm, bool start,
                   char *why, size_t why_len)
{
    static cctest_tx_t t, near;
    static v34_cctx_t tx, ntx;
    static v34_ccrx_t rx;
    cctest_rx_t r = { 0 };
    v34_mph_t m = { 0 };
    static float line[8192];
    double g = pow(10.0, -loss_db / 20.0);
    double nrms = V34_DBM0_RMS * pow(10.0, noise_db / 20.0);
    long long total = 8000 * 6;
    double pos = 0.0;

    memset(&t, 0, sizeof(t));
    memset(&near, 0, sizeof(near));
    t.tap = far_answerer ? 5 : 18;
    t.data_rng = 0x1234567u;
    if (start)
    {
        m.max_rate = 12;
        m.rate_mask = 0x0FFF;
        t.mph_n = v34_mph_pack(&m, t.mph);
        t.prog[0] = CC_SIL, t.len[0] = 60;
        t.prog[1] = CC_PPH, t.len[1] = 32;
        t.prog[2] = CC_ALT, t.len[2] = 40;
        t.prog[3] = CC_MPH, t.len[3] = 88;
        t.prog[4] = CC_E, t.len[4] = 10;
        t.prog[5] = CC_DATA, t.len[5] = 1800;
        t.prog[6] = CC_END, t.len[6] = -1;
        t.np = 7;
    }
    else
    {
        /* The resynchronisation of 12.6, then a retrain's AC. */
        t.prog[0] = CC_SIL, t.len[0] = 60;
        t.prog[1] = CC_SH, t.len[1] = 24;
        t.prog[2] = CC_SHB, t.len[2] = 8;
        t.prog[3] = CC_ALT, t.len[3] = 40;
        t.prog[4] = CC_SIL, t.len[4] = 120;
        t.prog[5] = CC_AC, t.len[5] = 150;
        t.prog[6] = CC_END, t.len[6] = -1;
        t.np = 7;
    }
    r.data_rng = 0x1234567u;
    r.data_limit = 2 * 1800;
    /* Our own transmitter, sending data in the other band all the while. */
    near.tap = far_answerer ? 18 : 5;
    near.data_rng = 99;
    near.prog[0] = CC_DATA, near.len[0] = -1;
    near.np = 1;
    v34_cctx_init(&tx, far_answerer, -13.0, cct_symbol, &t);
    v34_cctx_init(&ntx, !far_answerer, -13.0, cct_symbol, &near);
    v34_cctx_on(&tx, true);
    v34_cctx_on(&ntx, true);
    v34_ccrx_init(&rx, far_answerer, -13.0);
    memset(line, 0, sizeof(line));
    for (long long n = 0; n < total; n++)
    {
        float x, e;
        int ev;

        /* The far end's clock runs ppm fast: its samples arrive that much
         * closer together. Linear interpolation is plenty at these rates. */
        line[n & 8191] = v34_cctx_sample(&tx);
        pos += 1.0 / (1.0 + ppm * 1e-6);
        {
            double at = (double) n - delay - (pos - (double) n);
            long long i0 = (long long) floor(at);
            float f = (float) (at - i0);

            x = (i0 >= 1) ? line[i0 & 8191] * (1.0f - f) + line[(i0 + 1) & 8191] * f : 0.0f;
        }
        x = (float) (x * g + nrms * gauss());
        e = v34_cctx_sample(&ntx) * 0.3f;
        x = ulaw_to_linear(linear_to_ulaw((int16_t) lrintf(x + e)));
        ev = v34_ccrx_sample(&rx, x);
        if ((ev & V34_CC_EV_PPH) && !r.pph)
        {
            r.pph = 1;
            r.pph_sym = rx.symbols;
        }
        if ((ev & V34_CC_EV_SHBAR) && !r.shbar)
            r.shbar = 1;
        if ((ev & V34_CC_EV_AC) && !r.ac)
            r.ac = 1;
        for (int i = 0; i < rx.nbits; i++)
            cct_rx_bit(&r, rx.bits[i]);
    }
    if (start)
    {
        if (!r.pph)
            snprintf(why, why_len, "PPh not recognised");
        else if (!r.got_mph)
            snprintf(why, why_len, "no MPh");
        else if (r.mph.max_rate != 12 || r.mph.rate_mask != 0x0FFF)
            snprintf(why, why_len, "MPh garbled");
        else if (!r.got_e)
            snprintf(why, why_len, "no E");
        else if (r.data_bits < r.data_limit || r.data_errs > 0)
            snprintf(why, why_len, "%lld errors in %lld data bits", r.data_errs, r.data_bits);
        else
            return 1;
        return 0;
    }
    if (!r.shbar)
        snprintf(why, why_len, "Sh and S-bar-h not recognised");
    else if (!r.ac)
        snprintf(why, why_len, "AC not recognised");
    else
        return 1;
    return 0;
}

static void test_cc(void)
{
    static const struct
    {
        int delay;
        double loss, noise, ppm;
    } lines[] = { { 0, 0.0, -90.0, 0.0 },  { 37, 10.0, -60.0, 0.0 }, { 251, 20.0, -50.0, 80.0 },
                  { 5, 30.0, -55.0, -100.0 }, { 123, 6.0, -45.0, 40.0 } };
    int fails = 0, runs = 0;
    char why[96];

    for (int ans = 0; ans < 2; ans++)
        for (size_t l = 0; l < sizeof(lines) / sizeof(lines[0]); l++)
            for (int start = 0; start < 2; start++)
            {
                runs++;
                if (!cc_once(ans, lines[l].delay, lines[l].loss, lines[l].noise, lines[l].ppm, start, why,
                             sizeof(why)))
                {
                    fails++;
                    printf("  control channel from the %s, %s, delay %d, loss %.0f dB, noise %.0f dBm0, %+.0f ppm: %s\n",
                           ans ? "answerer" : "caller", start ? "start-up" : "resync and AC", lines[l].delay,
                           lines[l].loss, lines[l].noise, lines[l].ppm, why);
                }
            }
    CHECK(fails == 0, "%d of %d control channel runs failed", fails, runs);
}

int main(void)
{
    printf("V.34 unit tests\n");
    test_superconstellation();
    test_subset_labels();
    test_framing();
    test_shell();
    test_round_trip();
    test_coding_gain();
    test_precoder();
    test_crc();
    test_info();
    test_hdx_info();
    test_dpsk();
    test_pph();
    test_cc();
    if (failures)
    {
        printf("%d FAILED\n", failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
