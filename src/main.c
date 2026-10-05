#include "faxmodem/build_info.h"
#include "faxmodem/config.h"
#include "faxmodem/fax.h"
#include "faxmodem/log.h"
#include "faxmodem/sip.h"
#include "faxmodem/spool.h"
#include "faxmodem/tiff_probe.h"
#include "faxmodem/util.h"
#include "faxmodem/version.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>

static volatile sig_atomic_t g_stop = 0;

static void handle_signal(int sig)
{
    (void) sig;
    g_stop = 1;
}

static void install_signal_handlers(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* A hung up socket must not kill a fax in progress. */
    signal(SIGPIPE, SIG_IGN);
}

static int cmd_probe(const fm_config_t *cfg)
{
    fm_tiff_info_t info;
    char err[512];

    if (!fm_tiff_probe(cfg->file, &info, err, sizeof(err)))
    {
        FM_ERROR("probe", "%s", err);
        return FM_EXIT_CONFIG;
    }
    fm_tiff_log(&info, cfg->file);
    FM_INFO("probe", "%s looks sendable", cfg->file);
    return FM_EXIT_OK;
}

static int cmd_send(const fm_config_t *cfg)
{
    fm_call_result_t result;
    char tag[64];
    int rc;

    snprintf(tag, sizeof(tag), "%s", cfg->job_ref[0] ? cfg->job_ref : "out");

    rc = fm_sip_start(cfg);
    if (rc != FM_EXIT_OK)
        return rc;

    rc = fm_sip_send_fax(cfg, cfg->to, cfg->file, tag, &result);
    fm_sip_stop();
    return rc;
}

static int cmd_receive(const fm_config_t *cfg)
{
    int rc = fm_sip_start(cfg);

    if (rc != FM_EXIT_OK)
        return rc;
    rc = fm_sip_run_inbound(cfg, &g_stop);
    fm_sip_stop();
    return rc;
}

static int cmd_daemon(const fm_config_t *cfg)
{
    int rc = fm_sip_start(cfg);

    if (rc != FM_EXIT_OK)
        return rc;
    rc = fm_spool_run(cfg, &g_stop);
    fm_sip_stop();
    return rc;
}

int main(int argc, char *argv[])
{
    fm_config_t cfg;
    char err[512];
    int rc;

    fm_config_defaults(&cfg);
    fm_log_init(cfg.log_level, cfg.log_json);

    rc = fm_config_parse_args(&cfg, argc, argv);
    fm_log_init(cfg.log_level, cfg.log_json);

    if (cfg.command == FM_CMD_HELP)
    {
        fm_usage(NULL);
        return rc;
    }
    if (cfg.command == FM_CMD_VERSION)
    {
        printf("faxmodem %s (spandsp T.30 over SIP/G.711)\n", FAXMODEM_VERSION);
        printf("  build      %s, %s\n", FAXMODEM_BUILD_TYPE, FAXMODEM_BUILD_DATE);
        printf("  compiler   %s on %s\n", FAXMODEM_BUILD_COMPILER, FAXMODEM_BUILD_SYSTEM);
        printf("  spandsp    %s\n", FAXMODEM_SPANDSP_VERSION);
        printf("  pjproject  %s\n", FAXMODEM_PJPROJECT_VERSION);
        printf("  libtiff    %s\n", FAXMODEM_TIFF_VERSION);
        printf("  V.17       %s\n", fm_fax_check_modems()
                                       ? "works (14400 available)"
                                       : "does not work in this spandsp build (fixed point); 9600 at most");
        return FM_EXIT_OK;
    }
    if (rc != FM_EXIT_OK)
        return rc;

    if (!fm_config_validate(&cfg, err, sizeof(err)))
    {
        FM_ERROR("config", "%s", err);
        FM_INFO("config", "run 'faxmodem help' for the full list of options");
        return FM_EXIT_CONFIG;
    }

    install_signal_handlers();
    fm_fax_init_logging(&cfg);

    /* Before any SIP thread exists: inbound calls create engines there. */
    if (!fm_fax_check_modems() && cfg.max_speed > 9600 &&
        (cfg.command == FM_CMD_SEND || cfg.command == FM_CMD_RECEIVE || cfg.command == FM_CMD_DAEMON ||
         cfg.command == FM_CMD_SELFTEST))
    {
        FM_WARN("fax", "this spandsp's V.17 modem does not work - it is a fixed point build, which spandsp 0.0.6 "
                       "makes on Apple Silicon - so V.17 is not offered and faxes run at 9600 at most; see "
                       "'V.17 on Apple Silicon' in the README");
    }

    if (cfg.station_id[0] == '\0' &&
        (cfg.command == FM_CMD_SEND || cfg.command == FM_CMD_RECEIVE || cfg.command == FM_CMD_DAEMON))
    {
        /* T.30 carries our identity in the TSI/CSI. Sending none is legal but
         * leaves us anonymous in the far machine's journal and confirmation
         * page, and some senders are unhappy about it. */
        FM_WARN("config", "no --station-id set; this fax will identify itself as blank to the far end");
    }

    FM_INFO("faxmodem", "faxmodem %s starting (%s build, spandsp %s, pjproject %s)", FAXMODEM_VERSION,
            FAXMODEM_BUILD_TYPE, FAXMODEM_SPANDSP_VERSION, FAXMODEM_PJPROJECT_VERSION);
    fm_config_log(&cfg);

    switch (cfg.command)
    {
    case FM_CMD_PROBE:
        rc = cmd_probe(&cfg);
        break;
    case FM_CMD_SELFTEST:
        rc = fm_fax_selftest(&cfg);
        break;
    case FM_CMD_ENQUEUE:
        rc = fm_spool_enqueue(&cfg);
        break;
    case FM_CMD_SEND:
        rc = cmd_send(&cfg);
        break;
    case FM_CMD_RECEIVE:
        rc = cmd_receive(&cfg);
        break;
    case FM_CMD_DAEMON:
        rc = cmd_daemon(&cfg);
        break;
    default:
        fm_usage(NULL);
        rc = FM_EXIT_USAGE;
        break;
    }

    FM_INFO("faxmodem", "exit %d", rc);
    fm_log_close();
    return rc;
}
