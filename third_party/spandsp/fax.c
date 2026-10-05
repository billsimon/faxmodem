//#define LOG_FAX_AUDIO
/*
 * SpanDSP - a series of DSP components for telephony
 *
 * fax.c - Analogue line ITU T.30 FAX transfer processing
 *
 * Written by Steve Underwood <steveu@coppice.org>
 *
 * Copyright (C) 2003, 2005, 2006 Steve Underwood
 *
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License version 2.1,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/*! \file */

#if defined(HAVE_CONFIG_H)
#include "config.h"
#endif

#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#if defined(HAVE_TGMATH_H)
#include <tgmath.h>
#endif
#if defined(HAVE_MATH_H)
#include <math.h>
#endif
#include "floating_fudge.h"
#include <assert.h>
#include <fcntl.h>
#include <time.h>
#if defined(LOG_FAX_AUDIO)
#include <unistd.h>
#endif
#include <tiffio.h>

#include "spandsp/telephony.h"
#include "spandsp/logging.h"
#include "spandsp/queue.h"
#include "spandsp/dc_restore.h"
#include "spandsp/vector_int.h"
#include "spandsp/power_meter.h"
#include "spandsp/complex.h"
#include "spandsp/tone_detect.h"
#include "spandsp/tone_generate.h"
#include "spandsp/async.h"
#include "spandsp/hdlc.h"
#include "spandsp/silence_gen.h"
#include "spandsp/super_tone_rx.h"
#include "spandsp/fsk.h"
#include "spandsp/modem_connect_tones.h"
#include "spandsp/v8.h"
#include "spandsp/v29tx.h"
#include "spandsp/v29rx.h"
#include "spandsp/v27ter_tx.h"
#include "spandsp/v27ter_rx.h"
#include "spandsp/v17tx.h"
#include "spandsp/v17rx.h"
#include "spandsp/timezone.h"
#include "spandsp/t4_rx.h"
#include "spandsp/t4_tx.h"
#if defined(SPANDSP_SUPPORT_T85)
#include "spandsp/t81_t82_arith_coding.h"
#include "spandsp/t85.h"
#endif
#include "spandsp/t4_t6_decode.h"
#include "spandsp/t4_t6_encode.h"

#include "spandsp/t30_fcf.h"
#include "spandsp/t35.h"
#include "spandsp/t30.h"
#include "spandsp/t30_api.h"
#include "spandsp/t30_logging.h"

#include "spandsp/fax_modems.h"
#include "spandsp/fax.h"

#include "spandsp/private/logging.h"
#include "spandsp/private/silence_gen.h"
#include "spandsp/private/fsk.h"
#include "spandsp/private/modem_connect_tones.h"
#include "spandsp/private/v8.h"
#include "spandsp/private/v17tx.h"
#include "spandsp/private/v17rx.h"
#include "spandsp/private/v27ter_tx.h"
#include "spandsp/private/v27ter_rx.h"
#include "spandsp/private/v29tx.h"
#include "spandsp/private/v29rx.h"
#include "spandsp/private/hdlc.h"
#include "spandsp/private/fax_modems.h"
#include "spandsp/private/timezone.h"
#if defined(SPANDSP_SUPPORT_T85)
#include "spandsp/private/t81_t82_arith_coding.h"
#include "spandsp/private/t85.h"
#endif
#include "spandsp/private/t4_t6_decode.h"
#include "spandsp/private/t4_t6_encode.h"
#include "spandsp/private/t4_rx.h"
#include "spandsp/private/t4_tx.h"
#include "spandsp/private/t30.h"
#include "spandsp/private/fax.h"

#define HDLC_FRAMING_OK_THRESHOLD       8

/* faxmodem: V.34 (T.30 Annex F). fax_state_t is laid out as the installed
   library lays it out, so what V.34 needs goes in a wrapper that fax_init()
   allocates, with fax_state_t first; see include/faxmodem/fax_v34.h. */
#include <stddef.h>
#include "faxmodem/fax_v34.h"

#define FAX_V34_MAGIC 0x56333448u   /* "V34H" */

enum
{
    V34_OFF = 0,        /* not offered, or V.8 did not settle on it: plain G3 */
    V34_PHASE1,         /* V.8 under way; the caller's G3 CNG and V.21 run alongside */
    V34_ON              /* T.30 runs over the control and primary channels */
};

typedef struct
{
    fax_state_t fax;
    uint32_t magic;
    int state;
    bool calling;
    fm_v34h_t *modem;
    bool drop_modem;    /* V.8 came to nothing: free the modem out of its callback */
    bool step_due;      /* a SEND_STEP_COMPLETE to give T.30 once the modem call returns */
    int tx_type, rx_type;
    long long pause_left;
    int idle_bit;       /* flags for the control channel while T.30 has nothing for it */
    fm_v34h_stats_t last;
    bool used;
} fax_x_t;

static fax_x_t *fax_x(fax_state_t *s)
{
    fax_x_t *x = (fax_x_t *) s;

    return (x != NULL  &&  x->magic == FAX_V34_MAGIC)  ?  x  :  NULL;
}

static fax_x_t *fax_x_from_t30(t30_state_t *t)
{
    return fax_x((fax_state_t *) ((char *) t - offsetof(fax_state_t, t30)));
}

/* Hooks for the patched t30.c. */
int fm_t30_v8_capable(t30_state_t *t)
{
    fax_x_t *x = fax_x_from_t30(t);

    return x != NULL  &&  x->state != V34_OFF;
}

int fm_t30_v34_active(t30_state_t *t)
{
    fax_x_t *x = fax_x_from_t30(t);

    return x != NULL  &&  x->state == V34_ON;
}

static void fax_send_hdlc(void *user_data, const uint8_t *msg, int len)
{
    fax_state_t *s;

    s = (fax_state_t *) user_data;

    fax_modems_hdlc_tx_frame(&s->modems, msg, len);
}
/*- End of function --------------------------------------------------------*/

static void tone_detected(void *user_data, int tone, int level, int delay)
{
    t30_state_t *s;

    s = (t30_state_t *) user_data;
    span_log(&s->logging, SPAN_LOG_FLOW, "%s detected (%ddBm0)\n", modem_connect_tone_to_str(tone), level);
}
/*- End of function --------------------------------------------------------*/

#if 0
static void v8_handler(void *user_data, v8_parms_t *result)
{
    fax_state_t *s;

    s = (fax_state_t *) user_data;
    span_log(&s->logging, SPAN_LOG_FLOW, "V.8 report received\n");
}
/*- End of function --------------------------------------------------------*/
#endif

static void hdlc_underflow_handler(void *user_data)
{
    t30_state_t *s;

    s = (t30_state_t *) user_data;
    t30_front_end_status(s, T30_FRONT_END_SEND_STEP_COMPLETE);
}
/*- End of function --------------------------------------------------------*/

static void set_rx_handler(fax_state_t *s,
                           span_rx_handler_t *rx_handler,
                           span_rx_fillin_handler_t *fillin_handler,
                           void *user_data)
{
    s->modems.rx_handler = rx_handler;
    s->modems.rx_fillin_handler = fillin_handler;
    s->modems.rx_user_data = user_data;
}
/*- End of function --------------------------------------------------------*/

static void fax_modems_set_tx_handler(fax_state_t *s, span_tx_handler_t *handler, void *user_data)
{
    s->modems.tx_handler = handler;
    s->modems.tx_user_data = user_data;
}
/*- End of function --------------------------------------------------------*/

static void fax_modems_set_next_tx_handler(fax_state_t *s, span_tx_handler_t *handler, void *user_data)
{
    s->modems.next_tx_handler = handler;
    s->modems.next_tx_user_data = user_data;
}
/*- End of function --------------------------------------------------------*/

static int v17_v21_rx(void *user_data, const int16_t amp[], int len)
{
    fax_state_t *t;
    fax_modems_state_t *s;

    t = (fax_state_t *) user_data;
    s = &t->modems;
    v17_rx(&s->fast_modems.v17_rx, amp, len);
    if (t->t30.rx_trained)
    {
        /* The fast modem has trained, so we no longer need to run the slow one in parallel. */
        span_log(&t->logging, SPAN_LOG_FLOW, "Switching from V.17 + V.21 to V.17 (%.2fdBm0)\n", v17_rx_signal_power(&s->fast_modems.v17_rx));
        set_rx_handler(t, (span_rx_handler_t *) &v17_rx, (span_rx_fillin_handler_t *) &v17_rx_fillin, &s->fast_modems.v17_rx);
    }
    else
    {
        fsk_rx(&s->v21_rx, amp, len);
        if (t->t30.rx_frame_received)
        {
            /* We have received something, and the fast modem has not trained. We must
               be receiving valid V.21 */
            span_log(&t->logging, SPAN_LOG_FLOW, "Switching from V.17 + V.21 to V.21 (%.2fdBm0)\n", fsk_rx_signal_power(&s->v21_rx));
            set_rx_handler(t, (span_rx_handler_t *) &fsk_rx, (span_rx_fillin_handler_t *) &fsk_rx_fillin, &s->v21_rx);
        }
    }
    return 0;
}
/*- End of function --------------------------------------------------------*/

static int v17_v21_rx_fillin(void *user_data, int len)
{
    fax_state_t *t;
    fax_modems_state_t *s;

    t = (fax_state_t *) user_data;
    s = &t->modems;
    v17_rx_fillin(&s->fast_modems.v17_rx, len);
    fsk_rx_fillin(&s->v21_rx, len);
    return 0;
}
/*- End of function --------------------------------------------------------*/

static int v27ter_v21_rx(void *user_data, const int16_t amp[], int len)
{
    fax_state_t *t;
    fax_modems_state_t *s;

    t = (fax_state_t *) user_data;
    s = &t->modems;
    v27ter_rx(&s->fast_modems.v27ter_rx, amp, len);
    if (t->t30.rx_trained)
    {
        /* The fast modem has trained, so we no longer need to run the slow one in parallel. */
        span_log(&t->logging, SPAN_LOG_FLOW, "Switching from V.27ter + V.21 to V.27ter (%.2fdBm0)\n", v27ter_rx_signal_power(&s->fast_modems.v27ter_rx));
        set_rx_handler(t, (span_rx_handler_t *) &v27ter_rx, (span_rx_fillin_handler_t *) &v27ter_rx_fillin, &s->fast_modems.v27ter_rx);
    }
    else
    {
        fsk_rx(&s->v21_rx, amp, len);
        if (t->t30.rx_frame_received)
        {
            /* We have received something, and the fast modem has not trained. We must
               be receiving valid V.21 */
            span_log(&s->logging, SPAN_LOG_FLOW, "Switching from V.27ter + V.21 to V.21 (%.2fdBm0)\n", fsk_rx_signal_power(&s->v21_rx));
            set_rx_handler(t, (span_rx_handler_t *) &fsk_rx, (span_rx_fillin_handler_t *) &fsk_rx_fillin, &s->v21_rx);
        }
    }
    return 0;
}
/*- End of function --------------------------------------------------------*/

static int v27ter_v21_rx_fillin(void *user_data, int len)
{
    fax_state_t *t;
    fax_modems_state_t *s;

    t = (fax_state_t *) user_data;
    s = &t->modems;
    v27ter_rx_fillin(&s->fast_modems.v27ter_rx, len);
    fsk_rx_fillin(&s->v21_rx, len);
    return 0;
}
/*- End of function --------------------------------------------------------*/

static int v29_v21_rx(void *user_data, const int16_t amp[], int len)
{
    fax_state_t *t;
    fax_modems_state_t *s;

    t = (fax_state_t *) user_data;
    s = &t->modems;
    v29_rx(&s->fast_modems.v29_rx, amp, len);
    if (t->t30.rx_trained)
    {
        /* The fast modem has trained, so we no longer need to run the slow one in parallel. */
        span_log(&t->logging, SPAN_LOG_FLOW, "Switching from V.29 + V.21 to V.29 (%.2fdBm0)\n", v29_rx_signal_power(&s->fast_modems.v29_rx));
        set_rx_handler(t, (span_rx_handler_t *) &v29_rx, (span_rx_fillin_handler_t *) &v29_rx_fillin, &s->fast_modems.v29_rx);
    }
    else
    {
        fsk_rx(&s->v21_rx, amp, len);
        if (t->t30.rx_frame_received)
        {
            /* We have received something, and the fast modem has not trained. We must
               be receiving valid V.21 */
            span_log(&t->logging, SPAN_LOG_FLOW, "Switching from V.29 + V.21 to V.21 (%.2fdBm0)\n", fsk_rx_signal_power(&s->v21_rx));
            set_rx_handler(t, (span_rx_handler_t *) &fsk_rx, (span_rx_fillin_handler_t *) &fsk_rx_fillin, &s->v21_rx);
        }
    }
    return 0;
}
/*- End of function --------------------------------------------------------*/

static int v29_v21_rx_fillin(void *user_data, int len)
{
    fax_state_t *t;
    fax_modems_state_t *s;

    t = (fax_state_t *) user_data;
    s = &t->modems;
    v29_rx_fillin(&s->fast_modems.v29_rx, len);
    fsk_rx_fillin(&s->v21_rx, len);
    return 0;
}
/*- End of function --------------------------------------------------------*/

static int fax_rx_g3(fax_state_t *s, int16_t *amp, int len);
static int fax_tx_g3(fax_state_t *s, int16_t *amp, int max_len);

/* The control channel idles on flags while T.30 has nothing for it. */
static int v34_flag_bit(fax_x_t *x)
{
    int b = (0x7E >> (7 - x->idle_bit)) & 1;

    x->idle_bit = (x->idle_bit + 1) & 7;
    return b;
}

static int v34_cc_get_bit(void *user)
{
    fax_x_t *x = (fax_x_t *) user;
    int b;

    /* Always from the HDLC transmitter, which idles on flags between
       frames: one stream of flags, unbroken. Switching between a flag
       generator of our own and it broke a flag where they met, and the far
       receiver, counting flags afresh, missed the frame that followed. */
    b = hdlc_tx_get_bit(&x->fax.modems.hdlc_tx);
    if (b < 0)
    {
        /* T.30's frames are out: tell it, as the end of a V.21
           transmission would. The transmitter idles on flags after this,
           which is what the control channel wants. The transmit type is
           left alone: changed here, behind spandsp's back, T.30's next
           request for V.21 matched spandsp's record of the current type,
           was ignored, and its frames never went. */
        x->step_due = TRUE;
        b = hdlc_tx_get_bit(&x->fax.modems.hdlc_tx);
        if (b < 0)
            b = v34_flag_bit(x);
    }
    return b;
}

static void v34_cc_put_bit(void *user, int bit)
{
    fax_x_t *x = (fax_x_t *) user;

    if (x->rx_type == T30_MODEM_V21)
        hdlc_rx_put_bit(&x->fax.modems.hdlc_rx, bit);
}

static int v34_pc_get_bit(void *user)
{
    fax_x_t *x = (fax_x_t *) user;
    int b;

    b = hdlc_tx_get_bit(&x->fax.modems.hdlc_tx);
    if (b < 0)
    {
        /* The partial page is out; the modem turns back to the control
           channel, where T.30's PPS will go. */
        x->step_due = TRUE;
        return -1;
    }
    return b;
}

static void v34_pc_put_bit(void *user, int bit)
{
    fax_x_t *x = (fax_x_t *) user;

    if (x->rx_type == T30_MODEM_V17  ||  x->rx_type == T30_MODEM_V29  ||  x->rx_type == T30_MODEM_V27TER)
        hdlc_rx_put_bit(&x->fax.modems.hdlc_rx, bit);
}

static void v34_event(void *user, fm_v34h_event_t ev)
{
    fax_x_t *x = (fax_x_t *) user;
    fax_state_t *s = &x->fax;

    switch (ev)
    {
    case FM_V34H_CC_UP:
        if (x->state == V34_PHASE1)
        {
            span_log(&s->logging, SPAN_LOG_FLOW, "V.34 control channel up: T.30 runs over V.34\n");
            x->state = V34_ON;
            x->used = TRUE;
            x->tx_type = T30_MODEM_NONE;
            x->rx_type = T30_MODEM_V21;
            hdlc_rx_init(&s->modems.hdlc_rx, FALSE, TRUE, HDLC_FRAMING_OK_THRESHOLD, t30_hdlc_accept, &s->t30);
            /* The answerer's "CED" - V.8 and V.34's start-up - is done: T.30
               sends DIS now. The caller is already waiting for one. */
            if (!x->calling)
                x->step_due = TRUE;
        }
        break;
    case FM_V34H_PRIMARY_UP:
        if (!fm_v34h_is_source(x->modem))
        {
            hdlc_rx_put_bit(&s->modems.hdlc_rx, SIG_STATUS_CARRIER_UP);
            hdlc_rx_put_bit(&s->modems.hdlc_rx, SIG_STATUS_TRAINING_SUCCEEDED);
        }
        break;
    case FM_V34H_PRIMARY_DOWN:
        hdlc_rx_put_bit(&s->modems.hdlc_rx, SIG_STATUS_CARRIER_DOWN);
        break;
    case FM_V34H_NOT_V34:
        span_log(&s->logging, SPAN_LOG_FLOW, "No V.34: carrying on as G3\n");
        x->state = V34_OFF;
        x->drop_modem = TRUE;
        /* The answerer's ANSam stood in for CED; DIS goes on V.21. */
        if (!x->calling)
            x->step_due = TRUE;
        break;
    case FM_V34H_FAILED:
        span_log(&s->logging, SPAN_LOG_WARNING, "V.34 failed\n");
        break;
    }
}

/* Whatever the modem's callbacks left for T.30, now that the modem is no
   longer on the stack. */
static void v34_after(fax_x_t *x)
{
    if (x->modem != NULL)
        fm_v34h_stats(x->modem, &x->last);
    if (x->drop_modem)
    {
        x->drop_modem = FALSE;
        fm_v34h_free(x->modem);
        x->modem = NULL;
    }
    if (x->step_due)
    {
        x->step_due = FALSE;
        t30_front_end_status(&x->fax.t30, T30_FRONT_END_SEND_STEP_COMPLETE);
    }
}

/* The caller in V.8's first moments: CNG and the V.21 receiver are T.30's,
   and a G3 answerer's DIS there settles it. */
static bool v34_g3_alongside(fax_x_t *x)
{
    return x->state == V34_PHASE1  &&  x->calling  &&  x->modem != NULL  &&  fm_v34h_in_v8(x->modem);
}

SPAN_DECLARE_NONSTD(int) fax_rx(fax_state_t *s, int16_t *amp, int len)
{
    fax_x_t *x = fax_x(s);

    if (x != NULL  &&  x->state != V34_OFF  &&  x->modem != NULL)
    {
        if (v34_g3_alongside(x))
        {
            int16_t g3[len];
            int i;

            for (i = 0;  i < len;  i++)
                g3[i] = dc_restore(&s->modems.dc_restore, amp[i]);
            s->modems.rx_handler(s->modems.rx_user_data, g3, len);
            if (s->t30.rx_frame_received)
            {
                /* A G3 answerer: its DIS came on V.21. */
                span_log(&s->logging, SPAN_LOG_FLOW, "A V.21 frame arrived first: no V.34\n");
                x->state = V34_OFF;
                x->drop_modem = TRUE;
            }
        }
        if (x->modem != NULL  &&  !x->drop_modem)
            fm_v34h_rx(x->modem, amp, len);
        v34_after(x);
        t30_timer_update(&s->t30, len);
        return 0;
    }
    return fax_rx_g3(s, amp, len);
}

static int fax_rx_g3(fax_state_t *s, int16_t *amp, int len)
{
    int i;

#if defined(LOG_FAX_AUDIO)
    if (s->modems.audio_rx_log >= 0)
        write(s->modems.audio_rx_log, amp, len*sizeof(int16_t));
#endif
    for (i = 0;  i < len;  i++)
        amp[i] = dc_restore(&s->modems.dc_restore, amp[i]);
    s->modems.rx_handler(s->modems.rx_user_data, amp, len);
    t30_timer_update(&s->t30, len);
    return 0;
}
/*- End of function --------------------------------------------------------*/

SPAN_DECLARE_NONSTD(int) fax_rx_fillin(fax_state_t *s, int len)
{
    fax_x_t *x = fax_x(s);

    if (x != NULL  &&  x->state != V34_OFF  &&  x->modem != NULL)
    {
        fm_v34h_rx_fillin(x->modem, len);
        v34_after(x);
        t30_timer_update(&s->t30, len);
        return 0;
    }
    /* To mitigate the effect of lost packets on a packet network we should
       try to sustain the status quo. If there is no receive modem running, keep
       things that way. If there is a receive modem running, try to sustain its
       operation, without causing a phase hop, or letting its adaptive functions
       diverge. */
#if defined(LOG_FAX_AUDIO)
    if (s->modems.audio_rx_log >= 0)
    {
        int i;
#if defined(_MSC_VER)
        int16_t *amp = (int16_t *) _alloca(sizeof(int16_t)*len);
#else
        int16_t amp[len];
#endif

        vec_zeroi16(amp, len);
        write(s->modems.audio_rx_log, amp, len*sizeof(int16_t));
    }
#endif
    /* Call the fillin function of the current modem (if there is one). */
    s->modems.rx_fillin_handler(s->modems.rx_user_data, len);
    t30_timer_update(&s->t30, len);
    return 0;
}
/*- End of function --------------------------------------------------------*/

static int set_next_tx_type(fax_state_t *s)
{
    fax_modems_state_t *t;

    t = &s->modems;
    if (t->next_tx_handler)
    {
        fax_modems_set_tx_handler(s, t->next_tx_handler, t->next_tx_user_data);
        t->next_tx_handler = NULL;
        return 0;
    }
    /* If there is nothing else to change to, so use zero length silence */
    silence_gen_alter(&t->silence_gen, 0);
    fax_modems_set_tx_handler(s, (span_tx_handler_t *) &silence_gen, &t->silence_gen);
    fax_modems_set_next_tx_handler(s, (span_tx_handler_t *) NULL, NULL);
    t->transmit = FALSE;
    return -1;
}
/*- End of function --------------------------------------------------------*/

SPAN_DECLARE_NONSTD(int) fax_tx(fax_state_t *s, int16_t *amp, int max_len)
{
    fax_x_t *x = fax_x(s);

    if (x != NULL  &&  x->state != V34_OFF  &&  x->modem != NULL)
    {
        if (v34_g3_alongside(x))
        {
            /* CNG is T.30's; the modem is listening, and silent. */
            int16_t quiet[max_len];
            int len = fax_tx_g3(s, amp, max_len);

            if (len < max_len)
                memset(amp + len, 0, (max_len - len)*sizeof(int16_t));
            fm_v34h_tx(x->modem, quiet, max_len);
        }
        else
        {
            fm_v34h_tx(x->modem, amp, max_len);
            if (x->pause_left > 0  &&  (x->pause_left -= max_len) <= 0)
                x->step_due = TRUE;
        }
        v34_after(x);
        return max_len;
    }
    return fax_tx_g3(s, amp, max_len);
}

static int fax_tx_g3(fax_state_t *s, int16_t *amp, int max_len)
{
    int len;
#if defined(LOG_FAX_AUDIO)
    int required_len;
    
    required_len = max_len;
#endif
    len = 0;
    if (s->modems.transmit)
    {
        while ((len += s->modems.tx_handler(s->modems.tx_user_data, amp + len, max_len - len)) < max_len)
        {
            /* Allow for a change of tx handler within a block */
            if (set_next_tx_type(s)  &&  s->modems.current_tx_type != T30_MODEM_NONE  &&  s->modems.current_tx_type != T30_MODEM_DONE)
                t30_front_end_status(&s->t30, T30_FRONT_END_SEND_STEP_COMPLETE);
            if (!s->modems.transmit)
            {
                if (s->modems.transmit_on_idle)
                {
                    /* Pad to the requested length with silence */
                    memset(amp + len, 0, (max_len - len)*sizeof(int16_t));
                    len = max_len;        
                }
                break;
            }
        }
    }
    else
    {
        if (s->modems.transmit_on_idle)
        {
            /* Pad to the requested length with silence */
            memset(amp, 0, max_len*sizeof(int16_t));
            len = max_len;        
        }
    }
#if defined(LOG_FAX_AUDIO)
    if (s->modems.audio_tx_log >= 0)
    {
        if (len < required_len)
            memset(amp + len, 0, (required_len - len)*sizeof(int16_t));
        write(s->modems.audio_tx_log, amp, required_len*sizeof(int16_t));
    }
#endif
    return len;
}
/*- End of function --------------------------------------------------------*/

static void fax_set_rx_type(void *user_data, int type, int bit_rate, int short_train, int use_hdlc)
{
    fax_state_t *s;
    put_bit_func_t put_bit_func;
    void *put_bit_user_data;
    fax_modems_state_t *t;

    s = (fax_state_t *) user_data;
    t = &s->modems;
    span_log(&s->logging, SPAN_LOG_FLOW, "Set rx type %d\n", type);
    if (t->current_rx_type == type)
        return;
    {
        fax_x_t *x = fax_x(s);

        if (x != NULL  &&  x->state == V34_ON)
        {
            /* V.21 is the control channel, any fast modem the primary. */
            t->current_rx_type = type;
            x->rx_type = type;
            if (use_hdlc)
                hdlc_rx_init(&t->hdlc_rx, FALSE, TRUE, HDLC_FRAMING_OK_THRESHOLD, t30_hdlc_accept, &s->t30);
            if (type == T30_MODEM_V17  ||  type == T30_MODEM_V29  ||  type == T30_MODEM_V27TER)
                fm_v34h_primary(x->modem);
            set_rx_handler(s, (span_rx_handler_t *) &span_dummy_rx, (span_rx_fillin_handler_t *) &span_dummy_rx_fillin, s);
            return;
        }
    }
    t->current_rx_type = type;
    t->rx_bit_rate = bit_rate;
    if (use_hdlc)
    {
        put_bit_func = (put_bit_func_t) hdlc_rx_put_bit;
        put_bit_user_data = (void *) &t->hdlc_rx;
        hdlc_rx_init(&t->hdlc_rx, FALSE, TRUE, HDLC_FRAMING_OK_THRESHOLD, t30_hdlc_accept, &s->t30);
    }
    else
    {
        put_bit_func = t30_non_ecm_put_bit;
        put_bit_user_data = (void *) &s->t30;
    }
    switch (type)
    {
    case T30_MODEM_V21:
        fsk_rx_init(&t->v21_rx, &preset_fsk_specs[FSK_V21CH2], FSK_FRAME_MODE_SYNC, (put_bit_func_t) hdlc_rx_put_bit, put_bit_user_data);
        fsk_rx_signal_cutoff(&t->v21_rx, -45.5f);
        set_rx_handler(s, (span_rx_handler_t *) &fsk_rx, (span_rx_fillin_handler_t *) &fsk_rx_fillin, &t->v21_rx);
        break;
    case T30_MODEM_V27TER:
        v27ter_rx_restart(&t->fast_modems.v27ter_rx, bit_rate, FALSE);
        v27ter_rx_set_put_bit(&t->fast_modems.v27ter_rx, put_bit_func, put_bit_user_data);
        set_rx_handler(s, &v27ter_v21_rx, &v27ter_v21_rx_fillin, s);
        break;
    case T30_MODEM_V29:
        v29_rx_restart(&t->fast_modems.v29_rx, bit_rate, FALSE);
        v29_rx_set_put_bit(&t->fast_modems.v29_rx, put_bit_func, put_bit_user_data);
        set_rx_handler(s, &v29_v21_rx, &v29_v21_rx_fillin, s);
        break;
    case T30_MODEM_V17:
        v17_rx_restart(&t->fast_modems.v17_rx, bit_rate, short_train);
        v17_rx_set_put_bit(&t->fast_modems.v17_rx, put_bit_func, put_bit_user_data);
        set_rx_handler(s, &v17_v21_rx, &v17_v21_rx_fillin, s);
        break;
    case T30_MODEM_DONE:
        span_log(&s->logging, SPAN_LOG_FLOW, "FAX exchange complete\n");
    default:
        set_rx_handler(s, (span_rx_handler_t *) &span_dummy_rx, (span_rx_fillin_handler_t *) &span_dummy_rx_fillin, s);
        break;
    }
}
/*- End of function --------------------------------------------------------*/

static void fax_set_tx_type(void *user_data, int type, int bit_rate, int short_train, int use_hdlc)
{
    fax_state_t *s;
    get_bit_func_t get_bit_func;
    void *get_bit_user_data;
    fax_modems_state_t *t;
    int tone;

    s = (fax_state_t *) user_data;
    t = &s->modems;
    span_log(&s->logging, SPAN_LOG_FLOW, "Set tx type %d\n", type);
    {
        fax_x_t *x = fax_x(s);

        /* Under V.34 every request counts, repeated or not: the modem
           decides what a repeat means. */
        if (x != NULL  &&  x->state == V34_ON)
        {
            t->current_tx_type = type;
            t->tx_bit_rate = bit_rate;
            x->tx_type = type;
            switch (type)
            {
            case T30_MODEM_V21:
                /* The control channel is up already, idling on flags; ten
                   more ahead of the frame give a receiver that has just
                   restarted the eight it wants before it takes a frame. */
                hdlc_tx_flags(&t->hdlc_tx, 10);
                break;
            case T30_MODEM_V27TER:
            case T30_MODEM_V29:
            case T30_MODEM_V17:
                hdlc_tx_flags(&t->hdlc_tx, 16);
                fm_v34h_primary(x->modem);
                break;
            case T30_MODEM_PAUSE:
                x->pause_left = ms_to_samples(short_train);
                if (x->pause_left <= 0)
                    x->pause_left = 1;
                break;
            default:
                break;
            }
            t->transmit = TRUE;
            return;
        }
        if (x != NULL  &&  x->state == V34_PHASE1  &&  type == T30_MODEM_CED)
        {
            /* The answerer's ANSam, from the modem, stands in for CED; the
               end of V.8 - one way or the other - is the end of it. */
            t->current_tx_type = type;
            t->transmit = TRUE;
            return;
        }
    }
    if (t->current_tx_type == type)
        return;
    if (use_hdlc)
    {
        get_bit_func = (get_bit_func_t) hdlc_tx_get_bit;
        get_bit_user_data = (void *) &t->hdlc_tx;
    }
    else
    {
        get_bit_func = t30_non_ecm_get_bit;
        get_bit_user_data = (void *) &s->t30;
    }
    switch (type)
    {
    case T30_MODEM_PAUSE:
        silence_gen_alter(&t->silence_gen, ms_to_samples(short_train));
        fax_modems_set_tx_handler(s, (span_tx_handler_t *) &silence_gen, &t->silence_gen);
        fax_modems_set_next_tx_handler(s, (span_tx_handler_t *) NULL, NULL);
        t->transmit = TRUE;
        break;
    case T30_MODEM_CED:
    case T30_MODEM_CNG:
        if (type == T30_MODEM_CED)
            tone = MODEM_CONNECT_TONES_FAX_CED;
        else
            tone = MODEM_CONNECT_TONES_FAX_CNG;
        modem_connect_tones_tx_init(&t->connect_tx, tone);
        fax_modems_set_tx_handler(s, (span_tx_handler_t *) &modem_connect_tones_tx, &t->connect_tx);
        fax_modems_set_next_tx_handler(s, (span_tx_handler_t *) NULL, NULL);
        t->transmit = TRUE;
        break;
    case T30_MODEM_V21:
        fsk_tx_init(&t->v21_tx, &preset_fsk_specs[FSK_V21CH2], get_bit_func, get_bit_user_data);
        /* The spec says 1s +-15% of preamble. So, the minimum is 32 octets. */
        hdlc_tx_flags(&t->hdlc_tx, 32);
        /* Pause before switching from phase C, as per T.30 5.3.2.2. If we omit this, the receiver
           might not see the carrier fall between the high speed and low speed sections. In practice,
           a 75ms gap before any V.21 transmission is harmless, adds little to the overall length of
           a call, and ensures the receiving end is ready. */
        silence_gen_alter(&t->silence_gen, ms_to_samples(75));
        fax_modems_set_tx_handler(s, (span_tx_handler_t *) &silence_gen, &t->silence_gen);
        fax_modems_set_next_tx_handler(s, (span_tx_handler_t *) &fsk_tx, &t->v21_tx);
        t->transmit = TRUE;
        break;
    case T30_MODEM_V27TER:
        silence_gen_alter(&t->silence_gen, ms_to_samples(75));
        /* For any fast modem, set 200ms of preamble flags */
        hdlc_tx_flags(&t->hdlc_tx, bit_rate/(8*5));
        v27ter_tx_restart(&t->fast_modems.v27ter_tx, bit_rate, t->use_tep);
        v27ter_tx_set_get_bit(&t->fast_modems.v27ter_tx, get_bit_func, get_bit_user_data);
        fax_modems_set_tx_handler(s, (span_tx_handler_t *) &silence_gen, &t->silence_gen);
        fax_modems_set_next_tx_handler(s, (span_tx_handler_t *) &v27ter_tx, &t->fast_modems.v27ter_tx);
        t->transmit = TRUE;
        break;
    case T30_MODEM_V29:
        silence_gen_alter(&t->silence_gen, ms_to_samples(75));
        /* For any fast modem, set 200ms of preamble flags */
        hdlc_tx_flags(&t->hdlc_tx, bit_rate/(8*5));
        v29_tx_restart(&t->fast_modems.v29_tx, bit_rate, t->use_tep);
        v29_tx_set_get_bit(&t->fast_modems.v29_tx, get_bit_func, get_bit_user_data);
        fax_modems_set_tx_handler(s, (span_tx_handler_t *) &silence_gen, &t->silence_gen);
        fax_modems_set_next_tx_handler(s, (span_tx_handler_t *) &v29_tx, &t->fast_modems.v29_tx);
        t->transmit = TRUE;
        break;
    case T30_MODEM_V17:
        silence_gen_alter(&t->silence_gen, ms_to_samples(75));
        /* For any fast modem, set 200ms of preamble flags */
        hdlc_tx_flags(&t->hdlc_tx, bit_rate/(8*5));
        v17_tx_restart(&t->fast_modems.v17_tx, bit_rate, t->use_tep, short_train);
        v17_tx_set_get_bit(&t->fast_modems.v17_tx, get_bit_func, get_bit_user_data);
        fax_modems_set_tx_handler(s, (span_tx_handler_t *) &silence_gen, &t->silence_gen);
        fax_modems_set_next_tx_handler(s, (span_tx_handler_t *) &v17_tx, &t->fast_modems.v17_tx);
        t->transmit = TRUE;
        break;
    case T30_MODEM_DONE:
        span_log(&s->logging, SPAN_LOG_FLOW, "FAX exchange complete\n");
        /* Fall through */
    default:
        silence_gen_alter(&t->silence_gen, 0);
        fax_modems_set_tx_handler(s, (span_tx_handler_t *) &silence_gen, &t->silence_gen);
        fax_modems_set_next_tx_handler(s, (span_tx_handler_t *) NULL, NULL);
        t->transmit = FALSE;
        break;
    }
    t->tx_bit_rate = bit_rate;
    t->current_tx_type = type;
}
/*- End of function --------------------------------------------------------*/

SPAN_DECLARE(void) fax_set_transmit_on_idle(fax_state_t *s, int transmit_on_idle)
{
    s->modems.transmit_on_idle = transmit_on_idle;
}
/*- End of function --------------------------------------------------------*/

SPAN_DECLARE(void) fax_set_tep_mode(fax_state_t *s, int use_tep)
{
    s->modems.use_tep = use_tep;
}
/*- End of function --------------------------------------------------------*/

SPAN_DECLARE(t30_state_t *) fax_get_t30_state(fax_state_t *s)
{
    return &s->t30;
}
/*- End of function --------------------------------------------------------*/

SPAN_DECLARE(logging_state_t *) fax_get_logging_state(fax_state_t *s)
{
    return &s->logging;
}
/*- End of function --------------------------------------------------------*/

SPAN_DECLARE(int) fax_restart(fax_state_t *s, int calling_party)
{
#if 0
    v8_parms_t v8_parms;
#endif

    fax_modems_restart(&s->modems);
#if 0
    v8_parms.modem_connect_tone = MODEM_CONNECT_TONES_ANSAM_PR;
    v8_parms.call_function = V8_CALL_T30_RX;
    v8_parms.modulations = V8_MOD_V21;
    if (s->t30.supported_modems & T30_SUPPORT_V27TER)
        v8_parms.modulations |= V8_MOD_V27TER;
    if (s->t30.supported_modems & T30_SUPPORT_V29)
        v8_parms.modulations |= V8_MOD_V29;
    if (s->t30.supported_modems & T30_SUPPORT_V17)
        v8_parms.modulations |= V8_MOD_V17;
    if (s->t30.supported_modems & T30_SUPPORT_V34HDX)
        v8_parms.modulations |= V8_MOD_V34HDX;
    v8_parms.protocol = V8_PROTOCOL_NONE;
    v8_parms.pcm_modem_availability = 0;
    v8_parms.pstn_access = 0;
    v8_parms.nsf = -1;
    v8_parms.t66 = -1;
    v8_restart(&s->v8, calling_party, &v8_parms);
#endif
    t30_restart(&s->t30);
#if defined(LOG_FAX_AUDIO)
    {
        char buf[100 + 1];
        struct tm *tm;
        time_t now;

        time(&now);
        tm = localtime(&now);
        sprintf(buf,
                "/tmp/fax-rx-audio-%p-%02d%02d%02d%02d%02d%02d",
                s,
                tm->tm_year%100,
                tm->tm_mon + 1,
                tm->tm_mday,
                tm->tm_hour,
                tm->tm_min,
                tm->tm_sec);
        s->modems.audio_rx_log = open(buf, O_CREAT | O_TRUNC | O_WRONLY, 0666);
        sprintf(buf,
                "/tmp/fax-tx-audio-%p-%02d%02d%02d%02d%02d%02d",
                s,
                tm->tm_year%100,
                tm->tm_mon + 1,
                tm->tm_mday,
                tm->tm_hour,
                tm->tm_min,
                tm->tm_sec);
        s->modems.audio_tx_log = open(buf, O_CREAT | O_TRUNC | O_WRONLY, 0666);
    }
#endif
    return 0;
}
/*- End of function --------------------------------------------------------*/

SPAN_DECLARE(fax_state_t *) fax_init(fax_state_t *s, int calling_party)
{
#if 0
    v8_parms_t v8_parms;
#endif

    if (s == NULL)
    {
        /* faxmodem: room for V.34 beside fax_state_t. */
        fax_x_t *x;

        if ((x = (fax_x_t *) calloc(1, sizeof(*x))) == NULL)
            return NULL;
        x->magic = FAX_V34_MAGIC;
        x->calling = calling_party;
        s = &x->fax;
    }
    else
    {
        memset(s, 0, sizeof(*s));
    }
    span_log_init(&s->logging, SPAN_LOG_NONE, NULL);
    span_log_set_protocol(&s->logging, "FAX");
    fax_modems_init(&s->modems,
                    FALSE,
                    t30_hdlc_accept,
                    hdlc_underflow_handler,
                    t30_non_ecm_put_bit,
                    t30_non_ecm_get_bit,
                    tone_detected,
                    &s->t30);
    t30_init(&s->t30,
             calling_party,
             fax_set_rx_type,
             (void *) s,
             fax_set_tx_type,
             (void *) s,
             fax_send_hdlc,
             (void *) s);
    t30_set_supported_modems(&s->t30, T30_SUPPORT_V27TER | T30_SUPPORT_V29 | T30_SUPPORT_V17);
#if 0
    v8_parms.modem_connect_tone = MODEM_CONNECT_TONES_ANSAM_PR;
    v8_parms.call_function = V8_CALL_T30_RX;
    v8_parms.modulations = V8_MOD_V21;
    if (s->t30.supported_modems & T30_SUPPORT_V27TER)
        v8_parms.modulations |= V8_MOD_V27TER;
    if (s->t30.supported_modems & T30_SUPPORT_V29)
        v8_parms.modulations |= V8_MOD_V29;
    if (s->t30.supported_modems & T30_SUPPORT_V17)
        v8_parms.modulations |= V8_MOD_V17;
    if (s->t30.supported_modems & T30_SUPPORT_V34HDX)
        v8_parms.modulations |= V8_MOD_V34HDX;
    v8_parms.protocol = V8_PROTOCOL_NONE;
    v8_parms.pcm_modem_availability = 0;
    v8_parms.pstn_access = 0;
    v8_parms.nsf = -1;
    v8_parms.t66 = -1;
    v8_init(&s->v8, calling_party, &v8_parms, v8_handler, s);
#endif
    fax_restart(s, calling_party);
    return s;
}
/*- End of function --------------------------------------------------------*/

SPAN_DECLARE(int) fax_release(fax_state_t *s)
{
    fax_x_t *x = fax_x(s);

    t30_release(&s->t30);
    if (x != NULL  &&  x->modem != NULL)
    {
        fm_v34h_free(x->modem);
        x->modem = NULL;
    }
    return 0;
}
/*- End of function --------------------------------------------------------*/

SPAN_DECLARE(int) fax_free(fax_state_t *s)
{
    fax_release(s);
    free(s);
    return 0;
}

int fax_v34_enable(fax_state_t *s, const fax_v34_config_t *cfg)
{
    fax_x_t *x = fax_x(s);
    fm_v34h_params_t p;

    if (x == NULL  ||  x->modem != NULL)
        return -1;
    memset(&p, 0, sizeof(p));
    p.calling = x->calling;
    p.max_rate = cfg->max_rate;
    p.tx_power = cfg->tx_power;
    p.symbol_rates = cfg->symbol_rates;
    p.v17 = (s->t30.supported_modems & T30_SUPPORT_V17) != 0;
    p.v29 = (s->t30.supported_modems & T30_SUPPORT_V29) != 0;
    p.v27ter = (s->t30.supported_modems & T30_SUPPORT_V27TER) != 0;
    p.tag = cfg->tag;
    p.cc_get_bit = v34_cc_get_bit;
    p.cc_put_bit = v34_cc_put_bit;
    p.pc_get_bit = v34_pc_get_bit;
    p.pc_put_bit = v34_pc_put_bit;
    p.event = v34_event;
    p.user = x;
    if ((x->modem = fm_v34h_create(&p)) == NULL)
        return -1;
    x->state = V34_PHASE1;
    /* Phase A again, with V.34 offered: DIS says V.8, and the answerer's
       CED becomes the modem's ANSam. fax_modems_restart() does nothing in
       0.0.6, so forget the modem types T.30 chose the first time round, or
       asking for them again would be ignored. */
    s->modems.current_tx_type = -1;
    s->modems.current_rx_type = -1;
    fax_restart(s, x->calling);
    return 0;
}

bool fax_v34_active(fax_state_t *s)
{
    fax_x_t *x = fax_x(s);

    return x != NULL  &&  x->state == V34_ON;
}

bool fax_v34_stats(fax_state_t *s, fm_v34h_stats_t *out)
{
    fax_x_t *x = fax_x(s);

    if (x == NULL  ||  !x->used)
        return false;
    *out = x->last;
    return true;
}
/*- End of function --------------------------------------------------------*/
/*- End of file ------------------------------------------------------------*/
