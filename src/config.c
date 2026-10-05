#include "faxmodem/config.h"
#include "faxmodem/version.h"

#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum
{
    OPT_STR,
    OPT_INT,
    OPT_BOOL,
    OPT_LOGLEVEL
} opt_kind_t;

typedef struct
{
    const char *name;   /* long flag name, also the config-file and job-file key */
    opt_kind_t kind;
    size_t offset;      /* into fm_config_t */
    size_t size;        /* for OPT_STR */
    long min;
    long max;
    const char *help;
} opt_def_t;

#define STR_OPT(name, field, help) \
    {name, OPT_STR, offsetof(fm_config_t, field), sizeof(((fm_config_t *) 0)->field), 0, 0, help}
#define INT_OPT(name, field, lo, hi, help) \
    {name, OPT_INT, offsetof(fm_config_t, field), 0, lo, hi, help}
#define BOOL_OPT(name, field, help) \
    {name, OPT_BOOL, offsetof(fm_config_t, field), 0, 0, 0, help}

static const opt_def_t OPTS[] = {
    /* SIP account */
    STR_OPT("server", server, "SIP registrar/domain, e.g. sip.example.com[:5060]"),
    STR_OPT("username", username, "SIP user part"),
    STR_OPT("auth-user", auth_user, "digest auth username (defaults to --username)"),
    STR_OPT("password", password, "digest password (prefer FAXMODEM_PASSWORD)"),
    STR_OPT("realm", realm, "digest realm (default *)"),
    STR_OPT("from", from_uri, "override the From/contact URI"),
    STR_OPT("proxy", proxy, "outbound proxy URI, e.g. sip:edge.example.com;lr"),
    STR_OPT("caller-id", caller_id,
            "asserted caller ID: sets P-Asserted-Identity on outbound INVITEs (and the From display name)"),
    BOOL_OPT("register", do_register,
             "maintain a SIP registration (default: off for send, on for receive and for a daemon "
             "with --serve-inbound)"),
    INT_OPT("reg-timeout", reg_timeout_s, 1, 600, "seconds to wait for REGISTER to succeed"),
    INT_OPT("reg-expires", reg_expires_s, 30, 86400, "registration expiry requested"),

    /* Transport */
    STR_OPT("transport", transport, "udp | tcp | tls (default udp)"),
    STR_OPT("bind-addr", bind_addr, "local address to bind SIP to"),
    INT_OPT("local-port", local_port, 0, 65535, "local SIP port (0 = ephemeral)"),
    STR_OPT("public-addr", public_addr, "advertised address when behind NAT"),
    STR_OPT("stun", stun_server, "STUN server for NAT discovery"),
    STR_OPT("nameserver", nameserver, "DNS server for SRV/NAPTR resolution"),
    INT_OPT("rtp-port", rtp_port, 0, 65535, "base RTP port"),
    INT_OPT("rtp-port-range", rtp_port_range, 0, 60000,
            "ports above --rtp-port that media may use (default 100); open exactly this range"),

    /* Media */
    STR_OPT("codec", codec, "pcmu | pcma (default pcmu)"),
    INT_OPT("jitter-buffer-ms", jitter_buffer_ms, 0, 500,
            "fixed jitter buffer size (default 150); smaller starves the modem on a real path"),
    BOOL_OPT("user-phone", user_phone, "append ;user=phone to the request URI"),

    /* Fax */
    STR_OPT("station-id", station_id, "local station identifier (TSI/CSI), max 20 chars"),
    STR_OPT("header", header, "page header text (empty disables)"),
    BOOL_OPT("ecm", ecm, "error correction mode (default on)"),
    INT_OPT("max-speed", max_speed, 2400, 14400, "ceiling modem speed: 14400, 9600 or 4800"),
    BOOL_OPT("fine-only", fine_resolution_only, "refuse standard resolution on receive"),
    BOOL_OPT("unlimited-page-length", unlimited_page_length,
             "advertise unbounded page length; lets a stuck sender feed rows forever"),

    /* Job */
    STR_OPT("to", to, "destination number or full SIP URI"),
    STR_OPT("file", file, "TIFF-F file to send"),
    STR_OPT("output-dir", output_dir, "directory for received faxes (default ./received)"),
    STR_OPT("spool-dir", spool_dir, "spool root for daemon/enqueue (default ./spool)"),
    STR_OPT("ref", job_ref, "opaque reference echoed in logs and results"),
    INT_OPT("timeout", timeout_s, 10, 86400,
            "overall deadline for one fax, seconds; scaled up for long documents unless set explicitly"),
    INT_OPT("seconds-per-page", seconds_per_page, 0, 600,
            "per-page budget used to scale the deadline when sending (default 90, 0 disables)"),
    INT_OPT("media-timeout", media_timeout_s, 0, 600,
            "abandon a call after this many seconds with no inbound RTP (default 20, 0 disables)"),
    INT_OPT("progress-timeout", progress_timeout_s, 0, 3600,
            "abandon a call after this many seconds with no T.30 frame from the far end "
            "(default 180, 0 disables)"),
    INT_OPT("advance-timeout", advance_timeout_s, 0, 7200,
            "abandon a call after this many seconds with no page or image-data advance "
            "(default 300, 0 disables)"),
    INT_OPT("poll-interval", poll_interval_s, 1, 3600, "daemon queue scan interval, seconds"),
    INT_OPT("max-attempts", max_attempts, 1, 100, "daemon send attempts per job"),
    INT_OPT("retry-backoff", retry_backoff_s, 1, 86400, "daemon backoff between attempts"),
    BOOL_OPT("serve-inbound", daemon_receive, "daemon also answers inbound calls"),

    /* Logging */
    {"log-level", OPT_LOGLEVEL, offsetof(fm_config_t, log_level), 0, 0, 0,
     "error | warn | info | debug | trace (default info)"},
    BOOL_OPT("log-json", log_json, "emit one JSON object per line"),
    INT_OPT("pjsip-log-level", pjsip_log_level, 0, 6, "override pjsip verbosity"),
    INT_OPT("spandsp-log-level", spandsp_log_level, 0, 10, "override spandsp verbosity"),
};

static const size_t N_OPTS = sizeof(OPTS) / sizeof(OPTS[0]);

void fm_config_defaults(fm_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->command = FM_CMD_NONE;
    snprintf(cfg->realm, sizeof(cfg->realm), "%s", "*");
    snprintf(cfg->transport, sizeof(cfg->transport), "%s", "udp");
    snprintf(cfg->codec, sizeof(cfg->codec), "%s", "pcmu");
    snprintf(cfg->output_dir, sizeof(cfg->output_dir), "%s", "received");
    snprintf(cfg->spool_dir, sizeof(cfg->spool_dir), "%s", "spool");
    snprintf(cfg->header, sizeof(cfg->header), "%s", "");
    cfg->do_register = true;
    cfg->reg_timeout_s = 30;
    cfg->reg_expires_s = 300;
    cfg->local_port = 0;
    cfg->rtp_port = 0;
    cfg->rtp_port_range = 100;
    cfg->media_timeout_s = 20;
    cfg->progress_timeout_s = 180;
    cfg->advance_timeout_s = 300;
    cfg->jitter_buffer_ms = 150;
    cfg->ecm = true;
    cfg->max_speed = 14400;
    cfg->timeout_s = 600;
    cfg->inbound_timeout_s = FM_INBOUND_TIMEOUT_DEFAULT;
    cfg->seconds_per_page = 90;
    cfg->poll_interval_s = 2;
    cfg->max_attempts = 3;
    cfg->retry_backoff_s = 60;
    cfg->log_level = FM_LOG_INFO;
    cfg->pjsip_log_level = -1;
    cfg->spandsp_log_level = -1;
}

static const opt_def_t *find_opt(const char *key)
{
    for (size_t i = 0; i < N_OPTS; i++)
    {
        if (strcmp(OPTS[i].name, key) == 0)
            return &OPTS[i];
    }
    return NULL;
}

static bool parse_bool(const char *v, bool *out)
{
    if (v == NULL || *v == '\0')
    {
        *out = true;
        return true;
    }
    if (strcasecmp(v, "1") == 0 || strcasecmp(v, "true") == 0 || strcasecmp(v, "yes") == 0 ||
        strcasecmp(v, "on") == 0)
    {
        *out = true;
        return true;
    }
    if (strcasecmp(v, "0") == 0 || strcasecmp(v, "false") == 0 || strcasecmp(v, "no") == 0 ||
        strcasecmp(v, "off") == 0)
    {
        *out = false;
        return true;
    }
    return false;
}

bool fm_config_set(fm_config_t *cfg, const char *key, const char *value, char *err, size_t err_len)
{
    bool negate = false;
    const opt_def_t *def = find_opt(key);

    if (def == NULL && strncmp(key, "no-", 3) == 0)
    {
        def = find_opt(key + 3);
        if (def != NULL && def->kind == OPT_BOOL)
            negate = true;
        else
            def = NULL;
    }
    if (def == NULL)
    {
        snprintf(err, err_len, "unknown option '%s'", key);
        return false;
    }

    char *base = (char *) cfg;
    switch (def->kind)
    {
    case OPT_STR:
        if (value == NULL)
        {
            snprintf(err, err_len, "option '%s' needs a value", key);
            return false;
        }
        if (strlen(value) >= def->size)
        {
            snprintf(err, err_len, "value for '%s' is too long (max %zu chars)", key, def->size - 1);
            return false;
        }
        snprintf(base + def->offset, def->size, "%s", value);
        return true;

    case OPT_INT:
    {
        char *end = NULL;
        long n;
        if (value == NULL)
        {
            snprintf(err, err_len, "option '%s' needs a value", key);
            return false;
        }
        errno = 0;
        n = strtol(value, &end, 10);
        if (errno != 0 || end == value || *end != '\0')
        {
            snprintf(err, err_len, "value for '%s' is not a number: '%s'", key, value);
            return false;
        }
        if (n < def->min || n > def->max)
        {
            snprintf(err, err_len, "value for '%s' must be between %ld and %ld", key, def->min, def->max);
            return false;
        }
        *(int *) (base + def->offset) = (int) n;
        /* Remember that the deadline was chosen rather than defaulted; the
         * sender only auto-scales a deadline nobody asked for. */
        if (def->offset == offsetof(fm_config_t, timeout_s))
            cfg->timeout_explicit = true;
        return true;
    }

    case OPT_BOOL:
    {
        bool b;
        if (!parse_bool(value, &b))
        {
            snprintf(err, err_len, "value for '%s' is not a boolean: '%s'", key, value);
            return false;
        }
        *(bool *) (base + def->offset) = negate ? !b : b;
        if (def->offset == offsetof(fm_config_t, do_register))
            cfg->register_explicit = true;
        return true;
    }

    case OPT_LOGLEVEL:
    {
        fm_log_level_t lvl;
        if (!fm_log_level_parse(value, &lvl))
        {
            snprintf(err, err_len, "unknown log level '%s'", value ? value : "");
            return false;
        }
        *(fm_log_level_t *) (base + def->offset) = lvl;
        return true;
    }
    }
    snprintf(err, err_len, "internal: unhandled option kind for '%s'", key);
    return false;
}

void fm_config_apply_env(fm_config_t *cfg)
{
    char env_name[128];
    char err[256];

    for (size_t i = 0; i < N_OPTS; i++)
    {
        const char *v;
        size_t n = snprintf(env_name, sizeof(env_name), "FAXMODEM_%s", OPTS[i].name);
        if (n >= sizeof(env_name))
            continue;
        for (char *p = env_name; *p != '\0'; p++)
            *p = (*p == '-') ? '_' : (char) toupper((unsigned char) *p);

        v = getenv(env_name);
        if (v == NULL || *v == '\0')
            continue;
        if (!fm_config_set(cfg, OPTS[i].name, v, err, sizeof(err)))
            FM_WARN("config", "ignoring %s: %s", env_name, err);
    }
}

static char *trim(char *s)
{
    char *end;
    while (*s == ' ' || *s == '\t')
        s++;
    end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n'))
        *--end = '\0';
    return s;
}

/* "media-timeout = 20   # seconds": a # after whitespace, with something
 * already in front of it, starts a comment. Only then - so that a password
 * like `abc#123` still means what it says. */
static void strip_comment(char *value)
{
    if (*value == '\0')
        return;
    for (char *p = value + 1; *p != '\0'; p++)
    {
        if (*p == '#' && (p[-1] == ' ' || p[-1] == '\t'))
        {
            *p = '\0';
            trim(value);
            return;
        }
    }
}

/* Shared by the config file and the spool job files: one key=value per line.
 * Only a config file, which people write by hand, gets trailing comments: a
 * job file carries values verbatim, and a header like "Acme #42" has to come
 * back out of it intact. */
static bool read_kv(fm_config_t *cfg, const char *path, bool trailing_comments, char *err, size_t err_len)
{
    FILE *f = fopen(path, "r");
    char line[1024];
    int lineno = 0;

    if (f == NULL)
    {
        snprintf(err, err_len, "cannot read '%s': %s", path, strerror(errno));
        return false;
    }

    while (fgets(line, sizeof(line), f) != NULL)
    {
        char *key;
        char *value;
        char *eq;

        lineno++;
        key = trim(line);
        if (*key == '\0' || *key == '#' || *key == ';')
            continue;
        eq = strchr(key, '=');
        if (eq == NULL)
        {
            snprintf(err, err_len, "%s:%d: expected key=value", path, lineno);
            fclose(f);
            return false;
        }
        *eq = '\0';
        value = trim(eq + 1);
        if (trailing_comments)
            strip_comment(value);
        key = trim(key);
        /* Tolerate leading dashes so a config file can be copy-pasted flags. */
        while (*key == '-')
            key++;
        if (!fm_config_set(cfg, key, value, err, err_len))
        {
            char detail[512];
            snprintf(detail, sizeof(detail), "%s:%d: %s", path, lineno, err);
            snprintf(err, err_len, "%s", detail);
            fclose(f);
            return false;
        }
    }
    fclose(f);
    return true;
}

bool fm_config_read_kv(fm_config_t *cfg, const char *path, char *err, size_t err_len)
{
    return read_kv(cfg, path, false, err, err_len);
}

bool fm_config_apply_file(fm_config_t *cfg, const char *path, char *err, size_t err_len)
{
    return read_kv(cfg, path, true, err, err_len);
}

static fm_command_t command_from_string(const char *s)
{
    if (strcmp(s, "send") == 0)
        return FM_CMD_SEND;
    if (strcmp(s, "receive") == 0 || strcmp(s, "recv") == 0)
        return FM_CMD_RECEIVE;
    if (strcmp(s, "daemon") == 0 || strcmp(s, "serve") == 0)
        return FM_CMD_DAEMON;
    if (strcmp(s, "enqueue") == 0)
        return FM_CMD_ENQUEUE;
    if (strcmp(s, "selftest") == 0)
        return FM_CMD_SELFTEST;
    if (strcmp(s, "probe") == 0)
        return FM_CMD_PROBE;
    if (strcmp(s, "version") == 0 || strcmp(s, "--version") == 0 || strcmp(s, "-V") == 0)
        return FM_CMD_VERSION;
    if (strcmp(s, "help") == 0 || strcmp(s, "--help") == 0 || strcmp(s, "-h") == 0)
        return FM_CMD_HELP;
    return FM_CMD_NONE;
}

int fm_config_parse_args(fm_config_t *cfg, int argc, char *const argv[])
{
    struct option *longopts;
    char err[512];
    size_t n_long = 0;
    int rc = FM_EXIT_OK;
    const char *positional[2] = {NULL, NULL};
    int n_positional = 0;

    if (argc < 2)
    {
        cfg->command = FM_CMD_HELP;
        return FM_EXIT_USAGE;
    }

    cfg->command = command_from_string(argv[1]);
    if (cfg->command == FM_CMD_NONE)
    {
        fprintf(stdout, "faxmodem: unknown command '%s'\n\n", argv[1]);
        cfg->command = FM_CMD_HELP;
        return FM_EXIT_USAGE;
    }
    if (cfg->command == FM_CMD_HELP || cfg->command == FM_CMD_VERSION)
        return FM_EXIT_OK;

    /* Environment first; explicit flags below win. */
    fm_config_apply_env(cfg);

    /* Two entries per bool (--x / --no-x), plus --config, --help and the
     * terminator. */
    longopts = calloc(N_OPTS * 2 + 4, sizeof(*longopts));
    if (longopts == NULL)
        return FM_EXIT_INTERNAL;

    for (size_t i = 0; i < N_OPTS; i++)
    {
        longopts[n_long].name = OPTS[i].name;
        longopts[n_long].has_arg = (OPTS[i].kind == OPT_BOOL) ? optional_argument : required_argument;
        longopts[n_long].flag = NULL;
        longopts[n_long].val = 1000 + (int) i;
        n_long++;
        if (OPTS[i].kind == OPT_BOOL)
        {
            char *neg = malloc(strlen(OPTS[i].name) + 4);
            if (neg == NULL)
            {
                free(longopts);
                return FM_EXIT_INTERNAL;
            }
            sprintf(neg, "no-%s", OPTS[i].name);
            longopts[n_long].name = neg; /* leaked deliberately: lives for the process */
            longopts[n_long].has_arg = no_argument;
            longopts[n_long].flag = NULL;
            longopts[n_long].val = 2000 + (int) i;
            n_long++;
        }
    }
    longopts[n_long].name = "config";
    longopts[n_long].has_arg = required_argument;
    longopts[n_long].val = 'c';
    n_long++;
    longopts[n_long].name = "help";
    longopts[n_long].has_arg = no_argument;
    longopts[n_long].val = 'h';
    n_long++;
    longopts[n_long].name = "verbose";
    longopts[n_long].has_arg = no_argument;
    longopts[n_long].val = 'v';
    n_long++;

    optind = 2;
    for (;;)
    {
        int idx = 0;
        /* '+' makes getopt stop at the first non-option argument; we collect it
         * and resume, so flags may appear before or after the positionals
         * regardless of whether this libc permutes argv. */
        int c = getopt_long(argc, argv, "+c:hv", longopts, &idx);
        if (c == -1)
        {
            if (optind < argc && argv[optind][0] != '-')
            {
                if (n_positional < 2)
                    positional[n_positional++] = argv[optind];
                else
                {
                    fprintf(stdout, "faxmodem: unexpected argument '%s'\n", argv[optind]);
                    free(longopts);
                    return FM_EXIT_USAGE;
                }
                optind++;
                continue;
            }
            break;
        }

        if (c == 'h')
        {
            free(longopts);
            cfg->command = FM_CMD_HELP;
            return FM_EXIT_OK;
        }
        if (c == 'v')
        {
            if (cfg->log_level < FM_LOG_TRACE)
                cfg->log_level++;
            continue;
        }
        if (c == 'c')
        {
            if (!fm_config_apply_file(cfg, optarg, err, sizeof(err)))
            {
                fprintf(stdout, "faxmodem: %s\n", err);
                free(longopts);
                return FM_EXIT_CONFIG;
            }
            continue;
        }
        if (c >= 2000)
        {
            const opt_def_t *def = &OPTS[c - 2000];
            if (!fm_config_set(cfg, def->name, "false", err, sizeof(err)))
                rc = FM_EXIT_USAGE;
            continue;
        }
        if (c >= 1000)
        {
            const opt_def_t *def = &OPTS[c - 1000];
            const char *value = optarg;
            if (def->kind == OPT_BOOL && value == NULL)
                value = "true";
            if (!fm_config_set(cfg, def->name, value, err, sizeof(err)))
            {
                fprintf(stdout, "faxmodem: %s\n", err);
                free(longopts);
                return FM_EXIT_USAGE;
            }
            continue;
        }
        /* getopt_long already printed the diagnostic. */
        free(longopts);
        return FM_EXIT_USAGE;
    }

    while (optind < argc && n_positional < 2)
        positional[n_positional++] = argv[optind++];
    if (optind < argc)
    {
        fprintf(stdout, "faxmodem: unexpected argument '%s'\n", argv[optind]);
        free(longopts);
        return FM_EXIT_USAGE;
    }

    /* `faxmodem send +15551234567 doc.tif` as shorthand for --to/--file. */
    if (cfg->command == FM_CMD_SEND || cfg->command == FM_CMD_ENQUEUE)
    {
        if (n_positional > 0 && cfg->to[0] == '\0')
            snprintf(cfg->to, sizeof(cfg->to), "%s", positional[0]);
        if (n_positional > 1 && cfg->file[0] == '\0')
            snprintf(cfg->file, sizeof(cfg->file), "%s", positional[1]);
    }
    else if (cfg->command == FM_CMD_SELFTEST || cfg->command == FM_CMD_PROBE)
    {
        if (n_positional > 0 && cfg->file[0] == '\0')
            snprintf(cfg->file, sizeof(cfg->file), "%s", positional[0]);
    }

    /* Receiving cannot budget a deadline the way sending can: the page count,
     * the resolution and the content density are all unknown until the fax
     * arrives, and one dense halftone page at 9600 runs for ten minutes. So the
     * inbound deadline is a backstop measured in hours, and the watchdogs are
     * what actually clear a dead call. */
    if (cfg->timeout_explicit)
        cfg->inbound_timeout_s = cfg->timeout_s;
    else if (cfg->command == FM_CMD_RECEIVE)
        cfg->timeout_s = cfg->inbound_timeout_s;

    /* Registration only earns its keep when someone has to be able to call us.
     * Sending needs credentials, not a registration: the trunk challenges the
     * INVITE and pjsip answers it. */
    if (!cfg->register_explicit)
    {
        switch (cfg->command)
        {
        case FM_CMD_RECEIVE:
            cfg->do_register = true;
            break;
        case FM_CMD_DAEMON:
            cfg->do_register = cfg->daemon_receive;
            break;
        default:
            cfg->do_register = false;
            break;
        }
    }

    free(longopts);
    return rc;
}

static bool needs_sip(fm_command_t cmd)
{
    return cmd == FM_CMD_SEND || cmd == FM_CMD_RECEIVE || cmd == FM_CMD_DAEMON;
}

bool fm_config_validate(const fm_config_t *cfg, char *err, size_t err_len)
{
    if (needs_sip(cfg->command))
    {
        if (cfg->server[0] == '\0')
        {
            snprintf(err, err_len, "--server is required (or FAXMODEM_SERVER)");
            return false;
        }
        if (cfg->username[0] == '\0' && cfg->from_uri[0] == '\0')
        {
            snprintf(err, err_len, "--username is required (or FAXMODEM_USERNAME)");
            return false;
        }
        if (cfg->do_register && cfg->password[0] == '\0')
        {
            snprintf(err, err_len,
                     "--password is required when registering (or FAXMODEM_PASSWORD); "
                     "use --no-register for IP-authenticated trunks");
            return false;
        }
        if (strcasecmp(cfg->transport, "udp") != 0 && strcasecmp(cfg->transport, "tcp") != 0 &&
            strcasecmp(cfg->transport, "tls") != 0)
        {
            snprintf(err, err_len, "--transport must be udp, tcp or tls");
            return false;
        }
        if (strcasecmp(cfg->codec, "pcmu") != 0 && strcasecmp(cfg->codec, "pcma") != 0)
        {
            snprintf(err, err_len, "--codec must be pcmu or pcma");
            return false;
        }
    }

    if (cfg->command == FM_CMD_SEND || cfg->command == FM_CMD_ENQUEUE)
    {
        if (cfg->to[0] == '\0')
        {
            snprintf(err, err_len, "--to is required");
            return false;
        }
        if (cfg->file[0] == '\0')
        {
            snprintf(err, err_len, "--file is required");
            return false;
        }
    }
    if (cfg->command == FM_CMD_SELFTEST || cfg->command == FM_CMD_PROBE)
    {
        if (cfg->file[0] == '\0')
        {
            snprintf(err, err_len, "--file is required");
            return false;
        }
    }
    if (strlen(cfg->station_id) > 20)
    {
        snprintf(err, err_len, "--station-id is limited to 20 characters by T.30");
        return false;
    }
    return true;
}

void fm_config_log(const fm_config_t *cfg)
{
    if (!fm_log_enabled(FM_LOG_DEBUG))
        return;
    FM_DEBUG("config", "server=%s username=%s auth_user=%s password=%s register=%s transport=%s local_port=%d",
             cfg->server, cfg->username, cfg->auth_user[0] ? cfg->auth_user : cfg->username,
             cfg->password[0] ? "<set>" : "<unset>", cfg->do_register ? "yes" : "no", cfg->transport,
             cfg->local_port);
    FM_DEBUG("config", "codec=%s jitter_buffer_ms=%d ecm=%s max_speed=%d station_id=%s timeout=%ds",
             cfg->codec, cfg->jitter_buffer_ms, cfg->ecm ? "on" : "off", cfg->max_speed, cfg->station_id,
             cfg->timeout_s);
}

static void print_options(void)
{
    printf("Options (every flag also reads FAXMODEM_<FLAG_IN_CAPS>, and any of them\n"
           "can be set in a --config file as key=value):\n\n");
    for (size_t i = 0; i < N_OPTS; i++)
    {
        const char *arg = "";
        switch (OPTS[i].kind)
        {
        case OPT_STR:
            arg = " <value>";
            break;
        case OPT_INT:
            arg = " <n>";
            break;
        case OPT_LOGLEVEL:
            arg = " <level>";
            break;
        case OPT_BOOL:
            arg = "";
            break;
        }
        printf("  --%s%-*s %s\n", OPTS[i].name, (int) (22 - strlen(OPTS[i].name)), arg, OPTS[i].help);
    }
    printf("  --config <path>         read key=value settings from a file\n");
    printf("  -v, --verbose           raise log verbosity (repeatable)\n");
    printf("  -h, --help              this text\n");
}

void fm_usage(const char *command)
{
    printf("faxmodem %s - command line fax modem over SIP (T.30 via spandsp)\n\n", FAXMODEM_VERSION);
    printf("Usage:\n"
           "  faxmodem send <to> <file.tif> [options]     place a call and send a fax\n"
           "  faxmodem receive [options]                  answer calls and write received faxes\n"
           "  faxmodem daemon [options]                   run the spool queue (and optionally receive)\n"
           "  faxmodem enqueue <to> <file.tif> [options]  drop a job into the spool for the daemon\n"
           "  faxmodem selftest <file.tif> [options]      loop two T.30 engines back to back, no SIP\n"
           "  faxmodem probe <file.tif>                   inspect a TIFF for fax suitability\n"
           "  faxmodem version\n\n");
    printf("Examples:\n"
           "  export FAXMODEM_PASSWORD=...\n"
           "  faxmodem send +15551234567 invoice.tif \\\n"
           "      --server sip.example.com --username 1001 --station-id '+15550001111'\n\n"
           "  faxmodem receive --server sip.example.com --username 1001 --output-dir ./inbox\n\n"
           "  faxmodem daemon --spool-dir /var/spool/faxmodem --serve-inbound --config /etc/faxmodem.conf\n\n");
    printf("All logs go to stdout. Nothing is written to stderr except crashes.\n\n");
    print_options();
    (void) command;
}
