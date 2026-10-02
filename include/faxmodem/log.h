/* Logging. Everything faxmodem emits goes to stdout, one line per event. */
#ifndef FAXMODEM_LOG_H
#define FAXMODEM_LOG_H

#include <stdbool.h>

typedef enum
{
    FM_LOG_ERROR = 0,
    FM_LOG_WARN,
    FM_LOG_INFO,
    FM_LOG_DEBUG,
    FM_LOG_TRACE
} fm_log_level_t;

/* Initialise the logger. Safe to call more than once. */
void fm_log_init(fm_log_level_t level, bool json);

/* Parse "error"/"warn"/"info"/"debug"/"trace". Returns false on a bad name. */
bool fm_log_level_parse(const char *name, fm_log_level_t *out);
const char *fm_log_level_name(fm_log_level_t level);

fm_log_level_t fm_log_get_level(void);
bool fm_log_enabled(fm_log_level_t level);

void fm_logf(fm_log_level_t level, const char *component, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* Emits a line that carries structured key=value pairs after the message, e.g.
 * fm_log_event(FM_LOG_INFO, "fax", "transfer complete", "pages=%d result=%s", ...).
 * In JSON mode the pairs are parsed out into fields. */
void fm_log_event(fm_log_level_t level, const char *component, const char *event,
                  const char *fmt, ...) __attribute__((format(printf, 4, 5)));

#define FM_ERROR(comp, ...) fm_logf(FM_LOG_ERROR, (comp), __VA_ARGS__)
#define FM_WARN(comp, ...) fm_logf(FM_LOG_WARN, (comp), __VA_ARGS__)
#define FM_INFO(comp, ...) fm_logf(FM_LOG_INFO, (comp), __VA_ARGS__)
#define FM_DEBUG(comp, ...) fm_logf(FM_LOG_DEBUG, (comp), __VA_ARGS__)
#define FM_TRACE(comp, ...) fm_logf(FM_LOG_TRACE, (comp), __VA_ARGS__)

/* Sinks for the two libraries, so their output lands on stdout in our format. */
void fm_log_pjsip_writer(int level, const char *data, int len);
void fm_log_spandsp_message(int level, const char *text);
void fm_log_spandsp_error(const char *text);

#endif /* FAXMODEM_LOG_H */
