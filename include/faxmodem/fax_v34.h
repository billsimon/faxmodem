/* V.34 for spandsp's fax front end: what third_party/spandsp/fax.c adds
 * to fax_state_t to run T.30 Annex F ("Super G3") over the half-duplex V.34
 * modem of v34hdx.c.
 *
 * With V.34 enabled, the answering side sends ANSam in place of CED and the
 * calling side listens for it alongside its usual CNG and V.21 receiver; if
 * V.8 settles on V.34, T.30 runs over the control and primary channels with
 * no TCF and with ECM throughout, and otherwise the call carries on as an
 * ordinary G3 one, exactly as without V.34. */
#ifndef FAXMODEM_FAX_V34_H
#define FAXMODEM_FAX_V34_H

#include <stdbool.h>

#include "faxmodem/v34hdx.h"

typedef struct fax_state_s fax_state_t;

typedef struct
{
    int max_rate;             /* primary channel ceiling, 2400 ... 33600 */
    float tx_power;           /* dBm0 */
    unsigned symbol_rates;    /* test hook: 0 = all */
    const char *tag;
} fax_v34_config_t;

/* Offer V.34. Call once, after fax_init() and the T.30 set-up, before any
 * audio: it restarts phase A. Returns 0, or -1 if fax_init() did not make
 * room for V.34 or the modem could not be created. */
int fax_v34_enable(fax_state_t *s, const fax_v34_config_t *cfg);

/* This call is running on V.34 - V.8 agreed on it and the control channel
 * came up - rather than as a G3 one. */
bool fax_v34_active(fax_state_t *s);

/* Statistics of the V.34 modem; false if V.34 was never used. */
bool fax_v34_stats(fax_state_t *s, fm_v34h_stats_t *out);

#endif /* FAXMODEM_FAX_V34_H */
