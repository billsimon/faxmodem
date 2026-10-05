/* Configuration: command line flags, environment variables and an optional
 * key=value config file, merged in that order of precedence. */
#ifndef FAXMODEM_CONFIG_H
#define FAXMODEM_CONFIG_H

#include <stdbool.h>
#include <stddef.h>

#include "faxmodem/log.h"

#define FM_STR_MAX 512

/* Inbound deadline default: eight hours. Long enough never to be the thing
 * that ends a working fax; the media and stall watchdogs end the dead ones. */
#define FM_INBOUND_TIMEOUT_DEFAULT 28800

/* Process exit codes. Stable: scripts and the spool daemon depend on them. */
typedef enum
{
    FM_EXIT_OK = 0,        /* fax transferred (or command succeeded) */
    FM_EXIT_USAGE = 2,     /* bad flags */
    FM_EXIT_CONFIG = 3,    /* missing credentials, unreadable file, bad TIFF */
    FM_EXIT_SIP = 4,       /* transport or registration failure */
    FM_EXIT_CALL = 5,      /* call rejected, busy, or never answered */
    FM_EXIT_FAX = 6,       /* call connected but T.30 failed */
    FM_EXIT_TIMEOUT = 7,   /* overall deadline hit */
    FM_EXIT_INTERNAL = 8   /* we broke */
} fm_exit_code_t;

typedef enum
{
    FM_CMD_NONE = 0,
    FM_CMD_SEND,
    FM_CMD_RECEIVE,
    FM_CMD_DAEMON,
    FM_CMD_ENQUEUE,
    FM_CMD_SELFTEST,
    FM_CMD_PROBE,
    FM_CMD_VERSION,
    FM_CMD_HELP
} fm_command_t;

typedef struct
{
    fm_command_t command;

    /* SIP account */
    char server[FM_STR_MAX];       /* registrar / domain, e.g. sip.example.com or host:5060 */
    char username[FM_STR_MAX];     /* SIP user part */
    char auth_user[FM_STR_MAX];    /* auth username, defaults to username */
    char password[FM_STR_MAX];
    char realm[FM_STR_MAX];        /* digest realm, "*" by default */
    char from_uri[FM_STR_MAX];     /* overrides the derived sip:user@server */
    char proxy[FM_STR_MAX];        /* outbound proxy URI */
    char caller_id[FM_STR_MAX];    /* display name / P-Asserted number */
    bool do_register;
    bool register_explicit;        /* --register/--no-register was given, so do not
                                    * override it with the per-command default */
    int reg_timeout_s;             /* how long to wait for a 200 to REGISTER */
    int reg_expires_s;

    /* SIP transport */
    char transport[16];            /* udp | tcp | tls */
    char bind_addr[FM_STR_MAX];
    int local_port;                /* 0 = ephemeral */
    char public_addr[FM_STR_MAX];  /* advertised address when behind NAT */
    char stun_server[FM_STR_MAX];
    char nameserver[FM_STR_MAX];
    int rtp_port;                  /* base RTP port, 0 = pjsip default */
    int rtp_port_range;            /* ports above rtp_port that media may use */

    /* Media */
    char codec[16];                /* pcmu | pcma */
    int jitter_buffer_ms;
    bool user_phone;               /* append ;user=phone to the request URI */

    /* Fax / T.30 */
    char station_id[41];           /* TSI/CSI, 20 chars is the T.30 limit */
    char header[128];              /* page header text, "" disables */
    bool ecm;
    int max_speed;                 /* 2400..14400 */
    bool fine_resolution_only;
    bool unlimited_page_length;    /* advertise unbounded page length in DIS */

    /* Job */
    char to[FM_STR_MAX];           /* destination number or full SIP URI */
    char file[FM_STR_MAX];         /* TIFF to send */
    char output_dir[FM_STR_MAX];   /* where received faxes land */
    char spool_dir[FM_STR_MAX];
    char job_ref[FM_STR_MAX];      /* opaque caller reference, echoed in logs */
    int timeout_s;                 /* overall deadline for one outbound fax */
    int inbound_timeout_s;         /* overall deadline for one inbound fax */
    bool timeout_explicit;         /* --timeout was given, so do not auto-scale it */
    int seconds_per_page;          /* budget used to scale the deadline; 0 = no scaling */
    int media_timeout_s;           /* give up after this long with no inbound RTP; 0 = off */
    int progress_timeout_s;        /* no T.30 frame from the far end; 0 = off */
    int advance_timeout_s;         /* no page or image-data advance; 0 = off */
    int poll_interval_s;           /* daemon queue scan interval */
    int max_attempts;              /* daemon retry count */
    int retry_backoff_s;
    bool daemon_receive;           /* daemon also answers inbound calls */

    /* Logging */
    fm_log_level_t log_level;
    bool log_json;
    int pjsip_log_level;           /* -1 = derive from log_level */
    int spandsp_log_level;         /* -1 = derive from log_level */
} fm_config_t;

void fm_config_defaults(fm_config_t *cfg);

/* Applies FAXMODEM_* environment variables over the defaults. */
void fm_config_apply_env(fm_config_t *cfg);

/* Reads a key=value file (# comments, blank lines ignored). Returns false and
 * fills err on a syntax error or unknown key. A config file also allows a
 * trailing comment after whitespace (`media-timeout = 20  # seconds`); a spool
 * job file, read with fm_config_read_kv(), takes every value verbatim. */
bool fm_config_apply_file(fm_config_t *cfg, const char *path, char *err, size_t err_len);
bool fm_config_read_kv(fm_config_t *cfg, const char *path, char *err, size_t err_len);

/* Sets one option by its long-flag name, e.g. "station-id". Used by the file
 * parser, the env loader and the spool job reader so every route accepts the
 * same vocabulary. Returns false for an unknown key or a bad value. */
bool fm_config_set(fm_config_t *cfg, const char *key, const char *value, char *err, size_t err_len);

/* Parses argv. Returns an exit code; FM_EXIT_OK means cfg is ready. */
int fm_config_parse_args(fm_config_t *cfg, int argc, char *const argv[]);

/* Validates the config for cfg->command. */
bool fm_config_validate(const fm_config_t *cfg, char *err, size_t err_len);

void fm_config_log(const fm_config_t *cfg);
void fm_usage(const char *command);

#endif /* FAXMODEM_CONFIG_H */
