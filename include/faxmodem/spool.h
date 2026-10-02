/* Spool queue: job files dropped in a directory, picked up by the daemon.
 *
 *   <spool>/queue/    jobs waiting to be sent
 *   <spool>/active/   the job currently being sent (claimed by rename)
 *   <spool>/done/     jobs that went through, plus a .result file
 *   <spool>/failed/   jobs that ran out of attempts, plus a .result file
 *   <spool>/tmp/      staging, so a job only appears in queue/ once complete
 *
 * A job file is key=value lines using the same vocabulary as the command line
 * flags, plus "attempts" and "not-before" which the daemon maintains.
 */
#ifndef FAXMODEM_SPOOL_H
#define FAXMODEM_SPOOL_H

#include <signal.h>

#include "faxmodem/config.h"

/* Writes cfg->to / cfg->file (and any per-job overrides) into the queue. */
int fm_spool_enqueue(const fm_config_t *cfg);

/* Runs the queue until *stop is set. Assumes fm_sip_start() already ran. */
int fm_spool_run(const fm_config_t *cfg, volatile sig_atomic_t *stop);

#endif /* FAXMODEM_SPOOL_H */
