#include "faxmodem/spool.h"
#include "faxmodem/log.h"
#include "faxmodem/sip.h"
#include "faxmodem/util.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define JOB_SUFFIX ".job"

typedef struct
{
    char name[256];        /* file name inside the spool sub-directory */
    fm_config_t cfg;       /* daemon config with the job's overrides applied */
    int attempts;
    long not_before;       /* unix time; 0 = now */
} fm_job_t;

typedef struct
{
    char root[FM_STR_MAX];
    char queue[FM_STR_MAX + 16];
    char active[FM_STR_MAX + 16];
    char done[FM_STR_MAX + 16];
    char failed[FM_STR_MAX + 16];
    char tmp[FM_STR_MAX + 16];
} fm_spool_t;

static void spool_paths(const char *root, fm_spool_t *s)
{
    snprintf(s->root, sizeof(s->root), "%s", root);
    snprintf(s->queue, sizeof(s->queue), "%s/queue", root);
    snprintf(s->active, sizeof(s->active), "%s/active", root);
    snprintf(s->done, sizeof(s->done), "%s/done", root);
    snprintf(s->failed, sizeof(s->failed), "%s/failed", root);
    snprintf(s->tmp, sizeof(s->tmp), "%s/tmp", root);
}

static bool spool_prepare(const fm_spool_t *s, char *err, size_t err_len)
{
    const char *dirs[] = {s->root, s->queue, s->active, s->done, s->failed, s->tmp};

    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++)
    {
        if (!fm_mkdir_p(dirs[i], err, err_len))
            return false;
    }
    return true;
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

static bool job_read(const char *path, const fm_config_t *base, fm_job_t *job, char *err, size_t err_len)
{
    FILE *f = fopen(path, "r");
    char line[1024];
    int lineno = 0;

    if (f == NULL)
    {
        snprintf(err, err_len, "cannot read %s: %s", path, strerror(errno));
        return false;
    }

    job->cfg = *base;
    job->cfg.to[0] = '\0';
    job->cfg.file[0] = '\0';
    job->cfg.job_ref[0] = '\0';
    job->attempts = 0;
    job->not_before = 0;

    while (fgets(line, sizeof(line), f) != NULL)
    {
        char *key;
        char *value;
        char *eq;

        lineno++;
        key = trim(line);
        if (*key == '\0' || *key == '#')
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
        key = trim(key);
        while (*key == '-')
            key++;

        if (strcmp(key, "attempts") == 0)
        {
            job->attempts = atoi(value);
            continue;
        }
        if (strcmp(key, "not-before") == 0)
        {
            job->not_before = atol(value);
            continue;
        }
        if (!fm_config_set(&job->cfg, key, value, err, err_len))
        {
            char detail[600];
            snprintf(detail, sizeof(detail), "%s:%d: %s", path, lineno, err);
            snprintf(err, err_len, "%s", detail);
            fclose(f);
            return false;
        }
    }
    fclose(f);

    if (job->cfg.to[0] == '\0' || job->cfg.file[0] == '\0')
    {
        snprintf(err, err_len, "%s: both 'to' and 'file' are required", path);
        return false;
    }
    return true;
}

static bool job_write(const char *path, const fm_job_t *job, char *err, size_t err_len)
{
    FILE *f = fopen(path, "w");

    if (f == NULL)
    {
        snprintf(err, err_len, "cannot write %s: %s", path, strerror(errno));
        return false;
    }
    fprintf(f, "to=%s\n", job->cfg.to);
    fprintf(f, "file=%s\n", job->cfg.file);
    if (job->cfg.job_ref[0] != '\0')
        fprintf(f, "ref=%s\n", job->cfg.job_ref);
    if (job->cfg.station_id[0] != '\0')
        fprintf(f, "station-id=%s\n", job->cfg.station_id);
    if (job->cfg.header[0] != '\0')
        fprintf(f, "header=%s\n", job->cfg.header);
    fprintf(f, "attempts=%d\n", job->attempts);
    if (job->not_before > 0)
        fprintf(f, "not-before=%ld\n", job->not_before);
    if (fclose(f) != 0)
    {
        snprintf(err, err_len, "cannot flush %s: %s", path, strerror(errno));
        return false;
    }
    return true;
}

static void result_write(const char *dir, const char *job_name, const fm_job_t *job,
                         const fm_call_result_t *r, int exit_code)
{
    char path[FM_STR_MAX + 320];
    FILE *f;
    time_t now = time(NULL);

    snprintf(path, sizeof(path), "%s/%s.result", dir, job_name);
    f = fopen(path, "w");
    if (f == NULL)
    {
        FM_WARN("spool", "cannot write %s: %s", path, strerror(errno));
        return;
    }
    fprintf(f, "job=%s\n", job_name);
    fprintf(f, "ref=%s\n", job->cfg.job_ref);
    fprintf(f, "to=%s\n", job->cfg.to);
    fprintf(f, "file=%s\n", job->cfg.file);
    fprintf(f, "finished=%ld\n", (long) now);
    fprintf(f, "attempts=%d\n", job->attempts);
    fprintf(f, "exit-code=%d\n", exit_code);
    fprintf(f, "status=%s\n", exit_code == FM_EXIT_OK ? "sent" : "failed");
    fprintf(f, "sip-status=%d\n", r->sip_status);
    fprintf(f, "sip-reason=%s\n", r->sip_reason);
    fprintf(f, "t30-result=%d\n", r->t30_result);
    fprintf(f, "t30-text=%s\n", r->t30_text);
    fprintf(f, "pages=%d\n", r->pages);
    fprintf(f, "bit-rate=%d\n", r->bit_rate);
    fprintf(f, "ecm=%s\n", r->ecm ? "yes" : "no");
    fprintf(f, "remote-id=%s\n", r->remote_ident);
    fprintf(f, "duration-ms=%d\n", r->duration_ms);
    fclose(f);
}

int fm_spool_enqueue(const fm_config_t *cfg)
{
    fm_spool_t s;
    fm_job_t job;
    char err[512];
    char stamp[32];
    char tmp_path[FM_STR_MAX + 320];
    char queue_path[FM_STR_MAX + 320];
    char ref[128];
    char abs_file[FM_STR_MAX];

    spool_paths(cfg->spool_dir, &s);
    if (!spool_prepare(&s, err, sizeof(err)))
    {
        FM_ERROR("spool", "%s", err);
        return FM_EXIT_CONFIG;
    }
    if (!fm_file_exists(cfg->file))
    {
        FM_ERROR("spool", "'%s' does not exist", cfg->file);
        return FM_EXIT_CONFIG;
    }

    memset(&job, 0, sizeof(job));
    job.cfg = *cfg;
    /* The daemon runs elsewhere, so store a path it can resolve. */
    if (cfg->file[0] != '/' && realpath(cfg->file, abs_file) != NULL)
        snprintf(job.cfg.file, sizeof(job.cfg.file), "%s", abs_file);

    fm_timestamp_compact(stamp, sizeof(stamp));
    snprintf(ref, sizeof(ref), "%s", cfg->job_ref[0] ? cfg->job_ref : "job");
    fm_sanitise_filename(ref);
    snprintf(job.name, sizeof(job.name), "%s-%s-%d%s", stamp, ref, (int) getpid(), JOB_SUFFIX);

    snprintf(tmp_path, sizeof(tmp_path), "%s/%s", s.tmp, job.name);
    snprintf(queue_path, sizeof(queue_path), "%s/%s", s.queue, job.name);

    if (!job_write(tmp_path, &job, err, sizeof(err)))
    {
        FM_ERROR("spool", "%s", err);
        return FM_EXIT_CONFIG;
    }
    if (rename(tmp_path, queue_path) != 0)
    {
        FM_ERROR("spool", "cannot move %s into the queue: %s", tmp_path, strerror(errno));
        unlink(tmp_path);
        return FM_EXIT_CONFIG;
    }

    fm_log_event(FM_LOG_INFO, "spool", "job queued", "job=%s to=\"%s\" file=\"%s\" ref=\"%s\"", job.name,
                 job.cfg.to, job.cfg.file, job.cfg.job_ref);
    printf("%s\n", queue_path);
    fflush(stdout);
    return FM_EXIT_OK;
}

/* Picks the lexicographically first job whose not-before has passed. Job names
 * start with a timestamp, so that is also the oldest eligible job. */
static bool next_job(const fm_spool_t *s, const fm_config_t *base, fm_job_t *out)
{
    DIR *d = opendir(s->queue);
    struct dirent *ent;
    char best_name[256] = {0};
    bool found = false;
    time_t now = time(NULL);

    if (d == NULL)
        return false;

    while ((ent = readdir(d)) != NULL)
    {
        char path[FM_STR_MAX + 320];
        fm_job_t candidate;
        char err[512];
        size_t len = strlen(ent->d_name);

        if (len < strlen(JOB_SUFFIX) || strcmp(ent->d_name + len - strlen(JOB_SUFFIX), JOB_SUFFIX) != 0)
            continue;
        if (found && strcmp(ent->d_name, best_name) >= 0)
            continue;

        snprintf(path, sizeof(path), "%s/%s", s->queue, ent->d_name);
        memset(&candidate, 0, sizeof(candidate));
        if (!job_read(path, base, &candidate, err, sizeof(err)))
        {
            FM_WARN("spool", "%s", err);
            continue;
        }
        if (candidate.not_before > (long) now)
            continue;

        snprintf(candidate.name, sizeof(candidate.name), "%s", ent->d_name);
        *out = candidate;
        snprintf(best_name, sizeof(best_name), "%s", ent->d_name);
        found = true;
    }
    closedir(d);
    return found;
}

static void run_job(const fm_spool_t *s, fm_job_t *job)
{
    char queue_path[FM_STR_MAX + 320];
    char active_path[FM_STR_MAX + 320];
    char final_path[FM_STR_MAX + 320];
    char err[512];
    fm_call_result_t result;
    int rc;

    snprintf(queue_path, sizeof(queue_path), "%s/%s", s->queue, job->name);
    snprintf(active_path, sizeof(active_path), "%s/%s", s->active, job->name);

    /* rename() is the claim: if another daemon got there first, we move on. */
    if (rename(queue_path, active_path) != 0)
    {
        FM_DEBUG("spool", "job %s was claimed by someone else", job->name);
        return;
    }

    job->attempts++;
    fm_log_event(FM_LOG_INFO, "spool", "job started", "job=%s attempt=%d/%d to=\"%s\" file=\"%s\" ref=\"%s\"",
                 job->name, job->attempts, job->cfg.max_attempts, job->cfg.to, job->cfg.file,
                 job->cfg.job_ref);

    rc = fm_sip_send_fax(&job->cfg, job->cfg.to, job->cfg.file, job->name, &result);

    if (rc == FM_EXIT_OK)
    {
        snprintf(final_path, sizeof(final_path), "%s/%s", s->done, job->name);
        if (rename(active_path, final_path) != 0)
            FM_WARN("spool", "cannot move %s to done: %s", job->name, strerror(errno));
        result_write(s->done, job->name, job, &result, rc);
        fm_log_event(FM_LOG_INFO, "spool", "job sent", "job=%s pages=%d duration_ms=%d", job->name,
                     result.pages, result.duration_ms);
        return;
    }

    if (job->attempts < job->cfg.max_attempts && rc != FM_EXIT_CONFIG)
    {
        job->not_before = (long) time(NULL) + (long) job->cfg.retry_backoff_s * job->attempts;
        if (!job_write(active_path, job, err, sizeof(err)))
        {
            FM_ERROR("spool", "%s", err);
        }
        else if (rename(active_path, queue_path) != 0)
        {
            FM_ERROR("spool", "cannot requeue %s: %s", job->name, strerror(errno));
        }
        else
        {
            fm_log_event(FM_LOG_WARN, "spool", "job requeued",
                         "job=%s attempt=%d/%d exit=%d reason=\"%s\" retry_in_s=%ld", job->name,
                         job->attempts, job->cfg.max_attempts, rc,
                         result.t30_text[0] ? result.t30_text : result.sip_reason,
                         job->not_before - (long) time(NULL));
            return;
        }
    }

    snprintf(final_path, sizeof(final_path), "%s/%s", s->failed, job->name);
    if (rename(active_path, final_path) != 0)
        FM_WARN("spool", "cannot move %s to failed: %s", job->name, strerror(errno));
    result_write(s->failed, job->name, job, &result, rc);
    fm_log_event(FM_LOG_ERROR, "spool", "job failed", "job=%s attempts=%d exit=%d reason=\"%s\"", job->name,
                 job->attempts, rc, result.t30_text[0] ? result.t30_text : result.sip_reason);
}

/* Recovers jobs left in active/ by a crash or a hard kill. */
static void requeue_orphans(const fm_spool_t *s)
{
    DIR *d = opendir(s->active);
    struct dirent *ent;

    if (d == NULL)
        return;
    while ((ent = readdir(d)) != NULL)
    {
        char from[FM_STR_MAX + 320];
        char to[FM_STR_MAX + 320];
        size_t len = strlen(ent->d_name);

        if (len < strlen(JOB_SUFFIX) || strcmp(ent->d_name + len - strlen(JOB_SUFFIX), JOB_SUFFIX) != 0)
            continue;
        snprintf(from, sizeof(from), "%s/%s", s->active, ent->d_name);
        snprintf(to, sizeof(to), "%s/%s", s->queue, ent->d_name);
        if (rename(from, to) == 0)
            FM_WARN("spool", "requeued %s, left behind by a previous run", ent->d_name);
    }
    closedir(d);
}

int fm_spool_run(const fm_config_t *cfg, volatile sig_atomic_t *stop)
{
    fm_spool_t s;
    char err[512];

    spool_paths(cfg->spool_dir, &s);
    if (!spool_prepare(&s, err, sizeof(err)))
    {
        FM_ERROR("spool", "%s", err);
        return FM_EXIT_CONFIG;
    }
    if (cfg->daemon_receive && !fm_mkdir_p(cfg->output_dir, err, sizeof(err)))
    {
        FM_ERROR("spool", "%s", err);
        return FM_EXIT_CONFIG;
    }

    requeue_orphans(&s);
    fm_sip_set_inbound(cfg->daemon_receive);

    FM_INFO("spool", "watching %s every %ds (inbound %s, ctrl-c to stop)", s.queue, cfg->poll_interval_s,
            cfg->daemon_receive ? "enabled" : "disabled");

    while (*stop == 0)
    {
        fm_job_t job;

        fm_sip_poll_inbound();

        /* The line is held before the job is claimed: an inbound call
         * arriving in between would otherwise take it, and the job would
         * spend an attempt finding the line busy. */
        if (!fm_sip_call_active() && next_job(&s, cfg, &job) && fm_sip_reserve_line())
        {
            run_job(&s, &job);
            fm_sip_release_line();
            continue; /* drain the queue before sleeping again */
        }

        for (int slept = 0; slept < cfg->poll_interval_s * 1000 && *stop == 0; slept += 200)
        {
            usleep(200 * 1000);
            fm_sip_poll_inbound();
        }
    }

    FM_INFO("spool", "shutting down");
    fm_sip_set_inbound(false);
    return FM_EXIT_OK;
}
