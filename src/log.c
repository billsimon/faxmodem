#include "faxmodem/log.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

static fm_log_level_t g_level = FM_LOG_INFO;
static bool g_json = false;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* Writing to stdout can block - docker's log driver falling behind, journald
 * throttling, a `| jq` that stopped reading - and lines come from the pjmedia
 * clock thread too, from inside spandsp, holding the fax engine's lock in the
 * middle of a 20 ms frame. A stall there is a hole in the fax carrier. So a
 * line from any thread but the main one is formatted and queued, and a logger
 * thread writes it out; one from the main thread is written at once - after
 * whatever is queued, so nothing comes out of order. If the queue ever fills,
 * lines are dropped and counted rather than waited for.
 *
 * g_io serialises writing to stdout, and is always taken before g_lock,
 * which guards the queue and the settings. */
#define LOG_QUEUE_BYTES (1024 * 1024)
#define LOG_LINE_MAX 8192

static pthread_mutex_t g_io = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_queued = PTHREAD_COND_INITIALIZER;
static unsigned char g_queue[LOG_QUEUE_BYTES];
static size_t g_q_head;
static size_t g_q_len;
static unsigned long g_q_dropped;
static pthread_t g_main;
static bool g_have_main = false;
static pthread_t g_writer;
static bool g_writer_running = false;
static bool g_writer_stop = false;

static const char *const LEVEL_NAMES[] = {"error", "warn", "info", "debug", "trace"};

void fm_log_init(fm_log_level_t level, bool json)
{
    pthread_mutex_lock(&g_lock);
    if (!g_have_main)
    {
        g_main = pthread_self();
        g_have_main = true;
    }
    g_level = level;
    g_json = json;
    /* Line buffering keeps ordering sane when stdout is a pipe (docker logs,
     * journald, a supervisor) rather than a terminal. */
    setvbuf(stdout, NULL, _IOLBF, 0);
    pthread_mutex_unlock(&g_lock);
}

bool fm_log_level_parse(const char *name, fm_log_level_t *out)
{
    if (name == NULL)
        return false;
    for (size_t i = 0; i < sizeof(LEVEL_NAMES) / sizeof(LEVEL_NAMES[0]); i++)
    {
        if (strcasecmp(name, LEVEL_NAMES[i]) == 0)
        {
            *out = (fm_log_level_t) i;
            return true;
        }
    }
    if (strcasecmp(name, "warning") == 0)
    {
        *out = FM_LOG_WARN;
        return true;
    }
    return false;
}

const char *fm_log_level_name(fm_log_level_t level)
{
    if (level < FM_LOG_ERROR || level > FM_LOG_TRACE)
        return "info";
    return LEVEL_NAMES[level];
}

fm_log_level_t fm_log_get_level(void)
{
    return g_level;
}

bool fm_log_enabled(fm_log_level_t level)
{
    return level <= g_level;
}

static void timestamp(char *buf, size_t len)
{
    struct timeval tv;
    struct tm tm;
    char base[32];

    gettimeofday(&tv, NULL);
    gmtime_r(&tv.tv_sec, &tm);
    strftime(base, sizeof(base), "%Y-%m-%dT%H:%M:%S", &tm);
    snprintf(buf, len, "%s.%03dZ", base, (int) (tv.tv_usec / 1000));
}

static void json_escape(const char *in, char *out, size_t out_len)
{
    size_t o = 0;
    for (size_t i = 0; in[i] != '\0' && o + 7 < out_len; i++)
    {
        unsigned char c = (unsigned char) in[i];
        switch (c)
        {
        case '"':
        case '\\':
            out[o++] = '\\';
            out[o++] = (char) c;
            break;
        case '\n':
            out[o++] = '\\';
            out[o++] = 'n';
            break;
        case '\r':
            out[o++] = '\\';
            out[o++] = 'r';
            break;
        case '\t':
            out[o++] = '\\';
            out[o++] = 't';
            break;
        default:
            if (c < 0x20)
                o += (size_t) snprintf(out + o, out_len - o, "\\u%04x", c);
            else
                out[o++] = (char) c;
            break;
        }
    }
    out[o] = '\0';
}

/* Trailing whitespace and newlines come in from pjsip/spandsp; strip them so a
 * log line stays a log line. */
static void rtrim(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t'))
        s[--n] = '\0';
}

/* One finished line, newline included, in whichever format is selected.
 * Returns its length, truncated to fit. */
static size_t format_line(char *line, size_t line_len, fm_log_level_t level, const char *component,
                          const char *event, const char *msg)
{
    char ts[40];
    int n;

    timestamp(ts, sizeof(ts));
    if (g_json)
    {
        char emsg[4096];
        char ecomp[128];
        char eevent[128];
        json_escape(msg, emsg, sizeof(emsg));
        json_escape(component ? component : "faxmodem", ecomp, sizeof(ecomp));
        if (event != NULL)
        {
            json_escape(event, eevent, sizeof(eevent));
            n = snprintf(line, line_len,
                         "{\"ts\":\"%s\",\"level\":\"%s\",\"component\":\"%s\",\"event\":\"%s\",\"msg\":\"%s\"}\n",
                         ts, fm_log_level_name(level), ecomp, eevent, emsg);
        }
        else
        {
            n = snprintf(line, line_len, "{\"ts\":\"%s\",\"level\":\"%s\",\"component\":\"%s\",\"msg\":\"%s\"}\n",
                         ts, fm_log_level_name(level), ecomp, emsg);
        }
    }
    else
    {
        n = snprintf(line, line_len, "%s %-5s [%s] %s\n", ts, fm_log_level_name(level),
                     component ? component : "faxmodem", msg);
    }
    if (n < 0)
        return 0;
    if ((size_t) n >= line_len)
    {
        n = (int) line_len - 1;
        line[n - 1] = '\n';
    }
    return (size_t) n;
}

/* ------------------------------------------------------------ the queue */

/* Callers hold g_lock. */
static void q_copy_in(const void *src, size_t n)
{
    size_t tail = (g_q_head + g_q_len) % LOG_QUEUE_BYTES;
    size_t first = LOG_QUEUE_BYTES - tail;

    if (first > n)
        first = n;
    memcpy(g_queue + tail, src, first);
    memcpy(g_queue, (const unsigned char *) src + first, n - first);
    g_q_len += n;
}

static void q_copy_out(void *dst, size_t n)
{
    size_t first = LOG_QUEUE_BYTES - g_q_head;

    if (first > n)
        first = n;
    memcpy(dst, g_queue + g_q_head, first);
    memcpy((unsigned char *) dst + first, g_queue, n - first);
    g_q_head = (g_q_head + n) % LOG_QUEUE_BYTES;
    g_q_len -= n;
}

/* A record is its length, then the line. Callers hold g_lock. */
static void q_push(const char *line, size_t len)
{
    uint32_t n = (uint32_t) len;

    if (g_q_len + sizeof(n) + len > LOG_QUEUE_BYTES)
    {
        g_q_dropped++;
        return;
    }
    q_copy_in(&n, sizeof(n));
    q_copy_in(line, len);
}

static size_t q_pop(char *out)
{
    uint32_t n;

    if (g_q_len == 0)
        return 0;
    q_copy_out(&n, sizeof(n));
    q_copy_out(out, n);
    return n;
}

/* Everything queued, written out. Callers hold g_io, and not g_lock. */
static void drain_locked_io(void)
{
    static char line[LOG_LINE_MAX];

    for (;;)
    {
        unsigned long dropped;
        size_t n;

        pthread_mutex_lock(&g_lock);
        n = q_pop(line);
        dropped = g_q_dropped;
        g_q_dropped = 0;
        pthread_mutex_unlock(&g_lock);
        if (dropped > 0)
        {
            char msg[96];
            char note[512];
            size_t len;

            snprintf(msg, sizeof(msg), "%lu log lines dropped: stdout could not keep up", dropped);
            len = format_line(note, sizeof(note), FM_LOG_WARN, "log", NULL, msg);
            fwrite(note, 1, len, stdout);
        }
        if (n == 0)
            break;
        fwrite(line, 1, n, stdout);
    }
}

static void *writer_main(void *arg)
{
    (void) arg;
    for (;;)
    {
        bool stop;

        pthread_mutex_lock(&g_lock);
        while (g_q_len == 0 && g_q_dropped == 0 && !g_writer_stop)
            pthread_cond_wait(&g_queued, &g_lock);
        stop = g_writer_stop && g_q_len == 0 && g_q_dropped == 0;
        pthread_mutex_unlock(&g_lock);
        if (stop)
            break;

        pthread_mutex_lock(&g_io);
        drain_locked_io();
        fflush(stdout);
        pthread_mutex_unlock(&g_io);
    }
    return NULL;
}

/* Everything queued so far, written out now. Runs at exit, so the last lines
 * of a run - the call result, the exit code - are never left in the queue. */
static void log_flush(void)
{
    pthread_mutex_lock(&g_io);
    drain_locked_io();
    fflush(stdout);
    pthread_mutex_unlock(&g_io);
}

void fm_log_close(void)
{
    bool join;

    pthread_mutex_lock(&g_lock);
    join = g_writer_running;
    g_writer_stop = true;
    pthread_cond_signal(&g_queued);
    pthread_mutex_unlock(&g_lock);
    if (join)
        pthread_join(g_writer, NULL);
    log_flush();
}

static void emit(fm_log_level_t level, const char *component, const char *event, const char *msg)
{
    /* Per thread rather than on the stack, which in pjsip's threads is not
     * ours to size. */
    static __thread char line[LOG_LINE_MAX];
    size_t n;

    if (!fm_log_enabled(level))
        return;

    n = format_line(line, sizeof(line), level, component, event, msg);
    if (n == 0)
        return;

    if (!g_have_main || pthread_equal(pthread_self(), g_main))
    {
        pthread_mutex_lock(&g_io);
        drain_locked_io();
        fwrite(line, 1, n, stdout);
        fflush(stdout);
        pthread_mutex_unlock(&g_io);
        return;
    }

    pthread_mutex_lock(&g_lock);
    if (!g_writer_running && !g_writer_stop)
    {
        if (pthread_create(&g_writer, NULL, writer_main, NULL) == 0)
        {
            g_writer_running = true;
            atexit(log_flush);
        }
    }
    if (g_writer_running && !g_writer_stop)
    {
        q_push(line, n);
        pthread_cond_signal(&g_queued);
        pthread_mutex_unlock(&g_lock);
        return;
    }
    pthread_mutex_unlock(&g_lock);

    /* No thread to hand it to: write it ourselves, as before. */
    pthread_mutex_lock(&g_io);
    fwrite(line, 1, n, stdout);
    fflush(stdout);
    pthread_mutex_unlock(&g_io);
}

void fm_logf(fm_log_level_t level, const char *component, const char *fmt, ...)
{
    char msg[4096];
    va_list ap;

    if (!fm_log_enabled(level))
        return;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    rtrim(msg);
    emit(level, component, NULL, msg);
}

void fm_log_event(fm_log_level_t level, const char *component, const char *event, const char *fmt, ...)
{
    char detail[3072];
    char msg[4096];
    va_list ap;

    if (!fm_log_enabled(level))
        return;

    va_start(ap, fmt);
    vsnprintf(detail, sizeof(detail), fmt, ap);
    va_end(ap);
    rtrim(detail);

    if (g_json)
    {
        /* Keep the key=value detail as a single field; downstream log shippers
         * split it far better than we can guess at types here. */
        snprintf(msg, sizeof(msg), "%s", detail);
        emit(level, component, event, msg);
    }
    else
    {
        snprintf(msg, sizeof(msg), "%s %s", event, detail);
        emit(level, component, NULL, msg);
    }
}

void fm_log_pjsip_writer(int level, const char *data, int len)
{
    char buf[4096];
    fm_log_level_t mapped;

    /* pjsip levels: 0 fatal, 1 error, 2 warn, 3 info, 4 debug, 5+ trace. */
    if (level <= 1)
        mapped = FM_LOG_ERROR;
    else if (level == 2)
        mapped = FM_LOG_WARN;
    else if (level == 3)
        mapped = FM_LOG_INFO;
    else if (level == 4)
        mapped = FM_LOG_DEBUG;
    else
        mapped = FM_LOG_TRACE;

    if (!fm_log_enabled(mapped))
        return;

    if (len < 0)
        len = 0;
    if ((size_t) len >= sizeof(buf))
        len = (int) sizeof(buf) - 1;
    memcpy(buf, data, (size_t) len);
    buf[len] = '\0';
    rtrim(buf);
    if (buf[0] == '\0')
        return;
    emit(mapped, "sip.stack", NULL, buf);
}

void fm_log_spandsp_message(int level, const char *text)
{
    char buf[4096];
    fm_log_level_t mapped;

    /* spandsp SPAN_LOG_* severities, see spandsp/logging.h. */
    if (level <= 1)
        mapped = FM_LOG_ERROR;
    else if (level <= 4)
        mapped = FM_LOG_WARN;
    else if (level <= 6)
        mapped = FM_LOG_DEBUG;
    else
        mapped = FM_LOG_TRACE;

    if (!fm_log_enabled(mapped))
        return;

    snprintf(buf, sizeof(buf), "%s", text);
    rtrim(buf);
    if (buf[0] == '\0')
        return;
    emit(mapped, "t30", NULL, buf);
}

void fm_log_spandsp_error(const char *text)
{
    char buf[4096];
    snprintf(buf, sizeof(buf), "%s", text);
    rtrim(buf);
    if (buf[0] == '\0')
        return;
    emit(FM_LOG_ERROR, "t30", NULL, buf);
}
