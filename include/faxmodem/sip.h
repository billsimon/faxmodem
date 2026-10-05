/* SIP user agent: registration, call setup, and the bridge between the RTP
 * audio stream and the T.30 engine. G.711 (audio) fax only - no T.38. */
#ifndef FAXMODEM_SIP_H
#define FAXMODEM_SIP_H

#include <signal.h>
#include <stdbool.h>

#include "faxmodem/config.h"
#include "faxmodem/fax.h"

typedef struct
{
    bool connected;        /* media came up */
    int sip_status;        /* final SIP status code */
    char sip_reason[128];
    int t30_result;        /* T30_ERR_*, -1 if T.30 never ran */
    char t30_text[128];
    int pages;
    int bit_rate;
    bool ecm;
    bool v34;              /* ran as V.34 (Super G3) */
    int duration_ms;
    char remote_ident[41];
} fm_call_result_t;

/* Brings up pjsua, the transport and the account. Returns an fm_exit_code_t. */
int fm_sip_start(const fm_config_t *cfg);
void fm_sip_stop(void);

/* Holds the line for an outbound fax, so an inbound call cannot take it while
 * the job is being claimed. False if a call already has it. Optional:
 * fm_sip_send_fax() takes the line itself if the caller did not, and gives it
 * back either way; fm_sip_release_line() is for a reservation that ends up
 * unused, and does nothing once a call has run. */
bool fm_sip_reserve_line(void);
void fm_sip_release_line(void);

/* Places a call and runs one fax. Returns an fm_exit_code_t and always fills
 * result. */
int fm_sip_send_fax(const fm_config_t *cfg, const char *to, const char *file, const char *tag,
                    fm_call_result_t *result);

/* Inbound answering. Enable it, then call fm_sip_poll_inbound() regularly from
 * the owning loop: it reaps finished calls and enforces the timeout. */
void fm_sip_set_inbound(bool enabled);
void fm_sip_poll_inbound(void);
/* True while a call, either way, has the line or is being set up to. */
bool fm_sip_call_active(void);

/* Blocks answering inbound calls until *stop becomes non-zero. */
int fm_sip_run_inbound(const fm_config_t *cfg, volatile sig_atomic_t *stop);

#endif /* FAXMODEM_SIP_H */
