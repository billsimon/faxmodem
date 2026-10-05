#include "faxmodem/sip.h"
#include "faxmodem/log.h"
#include "faxmodem/tiff_probe.h"
#include "faxmodem/util.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <pjsua-lib/pjsua.h>

#define FM_CLOCK_RATE 8000
#define FM_PTIME_MS 20
#define FM_HANGUP_GRACE_MS 5000
/* After a received fax completes, the transmitter still has to send DCN and
 * hang up. Give it a moment rather than cutting the call from under it. */
#define FM_RX_LINGER_MS 4000
/* Call setup, CNG/CED, negotiation and training, before page one moves. */
#define FM_SETUP_BUDGET_S 60
#define FM_MAX_TIMEOUT_S 7200

/* A pjmedia port that is a fax modem: the conference bridge pulls 20 ms of
 * modem output from get_frame() and pushes 20 ms of the far end into
 * put_frame(). */
typedef struct
{
    pjmedia_port base;
    pj_pool_t *pool;            /* the port's own; the port lives in it */
    fm_fax_t *fax;
    unsigned samples_per_frame;
    pj_timestamp ts;
} fm_fax_port_t;

typedef struct
{
    pjsua_call_id call_id;
    fm_fax_t *fax;              /* owned by port; valid until call_destroy() */
    fm_fax_port_t *port;
    pjsua_conf_port_id slot;
    bool media_active;
    bool disconnected;
    bool inbound;
    int last_status;
    char last_reason[128];
    char tag[64];
    char rx_file[FM_STR_MAX + 96];
    int64_t started_ms;
    int timeout_s;
    int media_timeout_s;
    int progress_timeout_s;
    int advance_timeout_s;
    /* RTP arrival watch: the packet counter and when it last moved. */
    unsigned last_rx_pkts;
    int64_t last_rx_change_ms;
    const char *stall_reason;   /* set when a watchdog gives up on the call */
} fm_call_t;

/* Threads. pjsua calls the on_* callbacks on its own worker threads, while
 * the main thread places, polls and clears calls; everything both of them
 * touch - the registration result, whether inbound is enabled, who holds the
 * line, g.active and the fields of the call it points to that the callbacks
 * write - is under g.lock. Two rules keep that deadlock-free whatever locks
 * pjsua holds when it calls us: no pjsua function is called with g.lock held,
 * and a call context is only freed by the main thread, after g.active stops
 * pointing at it, under the lock - so a callback that finds its context still
 * active holds it alive until it lets go.
 *
 * The line. One call at a time, and the line is held from the moment a call
 * is decided on - before its context is built, which takes a while - until
 * that context is freed. Otherwise an inbound call and an outbound one could
 * both see the line free and both proceed, and one context would be orphaned:
 * never polled, never timed out, never freed. */
static struct
{
    bool started;
    pjsua_acc_id acc_id;
    fm_config_t cfg;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    bool reg_done;
    bool reg_ok;
    int reg_status;
    bool inbound_enabled;
    bool line_held;         /* a call has the line, or is being set up to */
    bool outbound_held;     /* ...and it is the main thread's, for sending */
    fm_call_t *active;      /* one call at a time; inbound is rejected while busy */
    unsigned call_counter;
} g = {.acc_id = PJSUA_INVALID_ID, .lock = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER};

/* ---------------------------------------------------------------- helpers */

static pj_str_t pjs(const char *s)
{
    pj_str_t r;
    r.ptr = (char *) s;
    r.slen = s ? (pj_ssize_t) strlen(s) : 0;
    return r;
}

/* The main thread may be waiting in wait_change_ms() for something to
 * change. Callers hold g.lock. */
static void signal_change_locked(void)
{
    pthread_cond_broadcast(&g.cond);
}

/* The context of a pjsua call, locked, if it is still the call we are
 * running - or NULL, unlocked, if it is not: not ours, or already cleared.
 * The user data is only compared, never followed, until it is known to be
 * g.active, so a stale pointer to a freed context is harmless. */
static fm_call_t *call_lock(pjsua_call_id call_id)
{
    fm_call_t *c = pjsua_call_get_user_data(call_id);

    pthread_mutex_lock(&g.lock);
    if (c == NULL || c != g.active)
    {
        pthread_mutex_unlock(&g.lock);
        return NULL;
    }
    return c;
}

static void call_unlock(void)
{
    pthread_mutex_unlock(&g.lock);
}

/* For the main thread, which owns c: the fields the callbacks write. */
static bool call_disconnected(fm_call_t *c)
{
    bool v;

    pthread_mutex_lock(&g.lock);
    v = c->disconnected;
    pthread_mutex_unlock(&g.lock);
    return v;
}

/* Takes the line for an outbound call, unless the main thread already holds
 * it for one. */
static bool line_take_outbound(void)
{
    bool ok = true;

    pthread_mutex_lock(&g.lock);
    if (!g.outbound_held)
    {
        if (g.line_held)
            ok = false;
        else
            g.line_held = g.outbound_held = true;
    }
    pthread_mutex_unlock(&g.lock);
    return ok;
}

static void line_release(void)
{
    pthread_mutex_lock(&g.lock);
    g.line_held = false;
    g.outbound_held = false;
    signal_change_locked();
    pthread_mutex_unlock(&g.lock);
}

static void wait_change_ms(int ms)
{
    struct timespec ts;

    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += (long) (ms % 1000) * 1000000L;
    ts.tv_sec += ms / 1000 + ts.tv_nsec / 1000000000L;
    ts.tv_nsec %= 1000000000L;

    pthread_mutex_lock(&g.lock);
    pthread_cond_timedwait(&g.cond, &g.lock, &ts);
    pthread_mutex_unlock(&g.lock);
}

static void log_pj_error(const char *what, pj_status_t status)
{
    char buf[PJ_ERR_MSG_SIZE];
    pj_strerror(status, buf, sizeof(buf));
    FM_ERROR("sip", "%s failed: %s (%d)", what, buf, (int) status);
}

/* ------------------------------------------------------------ media port  */

static pj_status_t fax_port_get_frame(pjmedia_port *port, pjmedia_frame *frame)
{
    fm_fax_port_t *p = (fm_fax_port_t *) port;
    int16_t *buf = (int16_t *) frame->buf;
    unsigned spf = p->samples_per_frame;
    int n;

    n = fm_fax_tx(p->fax, buf, (int) spf);
    if (n < (int) spf)
        memset(buf + n, 0, (spf - (unsigned) n) * sizeof(int16_t));

    frame->type = PJMEDIA_FRAME_TYPE_AUDIO;
    frame->size = spf * sizeof(int16_t);
    frame->timestamp.u64 = p->ts.u64;
    p->ts.u64 += spf;
    return PJ_SUCCESS;
}

static pj_status_t fax_port_put_frame(pjmedia_port *port, pjmedia_frame *frame)
{
    fm_fax_port_t *p = (fm_fax_port_t *) port;

    if (frame->type == PJMEDIA_FRAME_TYPE_AUDIO && frame->buf != NULL && frame->size > 0)
        fm_fax_rx(p->fax, (const int16_t *) frame->buf, (int) (frame->size / sizeof(int16_t)));
    else
        fm_fax_rx_missing(p->fax, (int) p->samples_per_frame); /* keep the modem's clock running */
    return PJ_SUCCESS;
}

/* The conference bridge removes a port asynchronously, on its clock thread,
 * and may still call get_frame()/put_frame() after pjsua_conf_remove_port()
 * has returned. So the port owns its pool and the fax engine, and the port's
 * group lock holds them until the bridge and call_destroy() have both let go;
 * this runs then, on whichever thread let go last. */
static pj_status_t fax_port_on_destroy(pjmedia_port *port)
{
    fm_fax_port_t *p = (fm_fax_port_t *) port;
    pj_pool_t *pool = p->pool;

    FM_DEBUG("sip", "fax media port released by the bridge");
    fm_fax_destroy(p->fax);
    p->fax = NULL;
    pj_pool_release(pool); /* p lives in it */
    return PJ_SUCCESS;
}

/* One audio stream and nothing else. pjsua defaults txt_cnt to 1, which puts a
 * T.140 "m=text" line in the SDP that no fax machine has any use for. */
static void fax_call_setting(pjsua_call_setting *cs)
{
    pjsua_call_setting_default(cs);
    cs->aud_cnt = 1;
    cs->vid_cnt = 0;
    cs->txt_cnt = 0;
}

static fm_fax_port_t *create_fax_port(pj_pool_t *pool, fm_fax_t *fax)
{
    fm_fax_port_t *p = PJ_POOL_ZALLOC_T(pool, fm_fax_port_t);
    pj_str_t name = pjs("faxmodem");
    unsigned spf = FM_CLOCK_RATE * FM_PTIME_MS / 1000;

    if (p == NULL)
        return NULL;
    pjmedia_port_info_init(&p->base.info, &name, PJMEDIA_SIG_CLASS_PORT_AUD('F', 'X'), FM_CLOCK_RATE, 1, 16,
                           spf);
    p->base.get_frame = &fax_port_get_frame;
    p->base.put_frame = &fax_port_put_frame;
    p->base.on_destroy = &fax_port_on_destroy;
    p->samples_per_frame = spf;
    p->pool = pool;
    p->fax = fax;
    return p;
}

/* --------------------------------------------------------------- call ctx */

static fm_call_t *call_create(const fm_config_t *cfg, bool inbound, const char *tag, const char *tx_file,
                              const char *rx_file)
{
    fm_call_t *c = calloc(1, sizeof(*c));
    fm_fax_params_t params;
    pj_pool_t *pool;
    pj_status_t status;

    if (c == NULL)
        return NULL;

    c->call_id = PJSUA_INVALID_ID;
    c->slot = PJSUA_INVALID_ID;
    c->inbound = inbound;
    c->last_status = 0;
    c->timeout_s = inbound ? cfg->inbound_timeout_s : cfg->timeout_s;
    c->media_timeout_s = cfg->media_timeout_s;
    c->progress_timeout_s = cfg->progress_timeout_s;
    c->advance_timeout_s = cfg->advance_timeout_s;
    c->started_ms = fm_now_ms();
    c->last_rx_change_ms = c->started_ms;
    snprintf(c->tag, sizeof(c->tag), "%s", tag);
    if (rx_file != NULL)
        snprintf(c->rx_file, sizeof(c->rx_file), "%s", rx_file);

    memset(&params, 0, sizeof(params));
    params.calling = !inbound;
    params.tx_file = tx_file;
    params.rx_file = rx_file;
    params.station_id = cfg->station_id;
    params.header = cfg->header;
    params.ecm = cfg->ecm;
    params.max_speed = cfg->max_speed;
    params.v34 = cfg->v34;
    params.v34_max_rate = cfg->v34_max_rate;
    params.fine_only = cfg->fine_resolution_only;
    params.unlimited_length = cfg->unlimited_page_length;
    params.tag = c->tag;

    c->fax = fm_fax_create(&params);
    if (c->fax == NULL)
    {
        free(c);
        return NULL;
    }

    pool = pjsua_pool_create("faxcall", 1024, 1024);
    if (pool == NULL)
    {
        fm_fax_destroy(c->fax);
        free(c);
        return NULL;
    }

    c->port = create_fax_port(pool, c->fax);
    if (c->port == NULL)
    {
        pj_pool_release(pool);
        fm_fax_destroy(c->fax);
        free(c);
        return NULL;
    }

    /* From here the port owns the pool and the fax engine; see
     * fax_port_on_destroy(). The group lock starts with our reference. */
    status = pjmedia_port_init_grp_lock(&c->port->base, pool, NULL);
    if (status != PJ_SUCCESS)
    {
        log_pj_error("pjmedia_port_init_grp_lock", status);
        fax_port_on_destroy(&c->port->base);
        free(c);
        return NULL;
    }

    status = pjsua_conf_add_port(pool, &c->port->base, &c->slot);
    if (status != PJ_SUCCESS)
    {
        log_pj_error("pjsua_conf_add_port", status);
        pjmedia_port_destroy(&c->port->base);
        free(c);
        return NULL;
    }
    FM_DEBUG("sip", "fax media port attached to the bridge at slot %d (tag=%s)", (int) c->slot, c->tag);
    return c;
}

/* The caller has already taken c out of g.active, under the lock. */
static void call_destroy(fm_call_t *c)
{
    if (c == NULL)
        return;
    /* pjsua may still hold the call - a BYE the far end never answered keeps
     * it alive for another half a minute - and would hand this pointer to the
     * next callback. call_lock() would refuse it, but there is no reason to
     * leave a dangling pointer lying around. */
    if (c->call_id != PJSUA_INVALID_ID && pjsua_call_is_active(c->call_id) &&
        pjsua_call_get_user_data(c->call_id) == c)
        pjsua_call_set_user_data(c->call_id, NULL);
    if (c->slot != PJSUA_INVALID_ID)
        pjsua_conf_remove_port(c->slot);
    /* Our reference; the pool and fax engine go when the bridge's does. */
    if (c->port != NULL)
        pjmedia_port_destroy(&c->port->base);
    free(c);
}

/* The end of a call, on the main thread: out of g.active first, so that from
 * here no callback will touch it; then freed; then the line is free. */
static void call_release(fm_call_t *c)
{
    pthread_mutex_lock(&g.lock);
    if (g.active == c)
        g.active = NULL;
    pthread_mutex_unlock(&g.lock);
    call_destroy(c);
    line_release();
}

static void call_fill_result(fm_call_t *c, fm_call_result_t *result)
{
    fm_fax_status_t st;

    memset(result, 0, sizeof(*result));
    fm_fax_status(c->fax, &st);

    pthread_mutex_lock(&g.lock);
    result->connected = c->media_active;
    result->sip_status = c->last_status;
    snprintf(result->sip_reason, sizeof(result->sip_reason), "%s", c->last_reason);
    pthread_mutex_unlock(&g.lock);
    result->t30_result = st.completed ? st.result : -1;
    snprintf(result->t30_text, sizeof(result->t30_text), "%s",
             st.result_text ? st.result_text : "not started");
    result->pages = c->inbound ? st.pages_rx : st.pages_tx;
    result->bit_rate = st.bit_rate;
    result->ecm = st.ecm;
    result->v34 = st.v34;
    result->duration_ms = (int) (fm_now_ms() - c->started_ms);
    snprintf(result->remote_ident, sizeof(result->remote_ident), "%s", st.remote_ident);
}

/* Watchdogs. A fax that is going nowhere should give the channel back quickly
 * rather than sit on it until the overall --timeout: the far end is usually
 * redialling, and every second we hold the line it gets a 486.
 *
 * Two independent signals, because they catch different faults:
 *   - no RTP arriving at all, which is a media path problem, not a fax problem;
 *   - RTP arriving but T.30 making no progress, which is a stuck session. */
static bool call_stalled(fm_call_t *c)
{
    pjsua_stream_stat stat;
    int64_t now = fm_now_ms();
    bool live;

    pthread_mutex_lock(&g.lock);
    live = c->media_active && !c->disconnected;
    pthread_mutex_unlock(&g.lock);
    if (!live)
        return false;

    if (c->media_timeout_s > 0 &&
        pjsua_call_get_stream_stat(c->call_id, 0, &stat) == PJ_SUCCESS)
    {
        int64_t since;

        pthread_mutex_lock(&g.lock);
        if (stat.rtcp.rx.pkt != c->last_rx_pkts)
        {
            c->last_rx_pkts = stat.rtcp.rx.pkt;
            c->last_rx_change_ms = now;
        }
        since = now - c->last_rx_change_ms;
        pthread_mutex_unlock(&g.lock);

        if (since > (int64_t) c->media_timeout_s * 1000)
        {
            FM_ERROR("sip", "no RTP from the far end for %ds on call %d (tag=%s, %u packets received in "
                            "total) - check the media path and that the negotiated RTP port is reachable",
                     c->media_timeout_s, (int) c->call_id, c->tag, stat.rtcp.rx.pkt);
            c->stall_reason = "no inbound RTP";
            return true;
        }
    }

    /* Nothing coming in at all: no T.30 frame AND no image data growing. Both
     * halves matter. A non-ECM page sends no frames whatsoever from the first
     * row to the last, so a frames-only test would cut off any page that takes
     * longer than the timeout - and a dense halftone page at 9600 takes many
     * minutes. Conversely a far end looping on one signal keeps sending frames
     * while delivering nothing. Only the two together mean the call is dead. */
    if (c->progress_timeout_s > 0 &&
        fm_fax_since_rx_frame_ms(c->fax) > (int64_t) c->progress_timeout_s * 1000 &&
        fm_fax_since_advance_ms(c->fax) > (int64_t) c->progress_timeout_s * 1000)
    {
        FM_ERROR("sip", "nothing received for %ds on call %d (tag=%s): no T.30 frame and no image data "
                        "- abandoning the call",
                 c->progress_timeout_s, (int) c->call_id, c->tag);
        c->stall_reason = "no T.30 frame and no image data";
        return true;
    }

    /* Busy loop: frames keep arriving, but nothing is moving. A far end that
     * re-sends the same signal every T5 forever looks alive to every other
     * check we have. */
    if (c->advance_timeout_s > 0 &&
        fm_fax_since_advance_ms(c->fax) > (int64_t) c->advance_timeout_s * 1000)
    {
        FM_ERROR("sip", "no page or image-data advance for %ds on call %d (tag=%s) - abandoning the call",
                 c->advance_timeout_s, (int) c->call_id, c->tag);
        c->stall_reason = "transfer made no progress";
        return true;
    }
    return false;
}

/* -------------------------------------------------------------- callbacks */

static void on_call_state(pjsua_call_id call_id, pjsip_event *e)
{
    fm_call_t *c;
    pjsua_call_info ci;

    (void) e;
    if (pjsua_call_get_info(call_id, &ci) != PJ_SUCCESS)
        return;

    FM_INFO("sip", "call %d state %.*s (%d %.*s)", (int) call_id, (int) ci.state_text.slen, ci.state_text.ptr,
            ci.last_status, (int) ci.last_status_text.slen, ci.last_status_text.ptr);

    c = call_lock(call_id);
    if (c == NULL)
        return;

    c->last_status = ci.last_status;
    snprintf(c->last_reason, sizeof(c->last_reason), "%.*s", (int) ci.last_status_text.slen,
             ci.last_status_text.ptr);
    if (ci.state == PJSIP_INV_STATE_DISCONNECTED)
        c->disconnected = true;
    signal_change_locked();
    call_unlock();
}

static void on_call_media_state(pjsua_call_id call_id)
{
    fm_call_t *c;
    pjsua_call_info ci;
    pjsua_conf_port_id slot;
    char tag[sizeof(c->tag)];

    if (pjsua_call_get_info(call_id, &ci) != PJ_SUCCESS)
        return;

    if (ci.media_status == PJSUA_CALL_MEDIA_ACTIVE)
    {
        pjmedia_transport_info tp_info;

        c = call_lock(call_id);
        if (c == NULL)
            return;
        slot = c->slot;
        snprintf(tag, sizeof(tag), "%s", c->tag);
        call_unlock();

        /* Not under the lock: see the comment on g. */
        pjsua_conf_connect(ci.conf_slot, slot);
        pjsua_conf_connect(slot, ci.conf_slot);

        c = call_lock(call_id);
        if (c == NULL)
            return;
        c->media_active = true;
        /* The RTP watchdog only starts counting once there is media to wait for. */
        c->last_rx_change_ms = fm_now_ms();
        signal_change_locked();
        call_unlock();

        pjmedia_transport_info_init(&tp_info);
        if (pjsua_call_get_med_transport_info(call_id, 0, &tp_info) == PJ_SUCCESS)
        {
            char addr[PJ_INET6_ADDRSTRLEN + 10];
            pj_sockaddr_print(&tp_info.sock_info.rtp_addr_name, addr, sizeof(addr), 3);
            FM_INFO("sip", "media active on call %d, modem connected (tag=%s, local RTP %s)",
                    (int) call_id, tag, addr);
        }
        else
        {
            FM_INFO("sip", "media active on call %d, modem connected (tag=%s)", (int) call_id, tag);
        }
    }
    else
    {
        FM_DEBUG("sip", "call %d media status %d", (int) call_id, (int) ci.media_status);
    }
}

static void on_incoming_call(pjsua_acc_id acc_id, pjsua_call_id call_id, pjsip_rx_data *rdata)
{
    pjsua_call_info ci;
    pjsua_call_setting answer_cfg;
    fm_call_t *c;
    char rx_file[FM_STR_MAX + 96];
    char stamp[32];
    char from[160];
    char tag[64];
    unsigned n;

    (void) acc_id;
    (void) rdata;

    if (pjsua_call_get_info(call_id, &ci) != PJ_SUCCESS)
    {
        pjsua_call_hangup(call_id, 500, NULL, NULL);
        return;
    }
    snprintf(from, sizeof(from), "%.*s", (int) ci.remote_info.slen, ci.remote_info.ptr);

    /* Take the line, under the lock, before building anything: an outbound
     * fax being set up on the main thread must not get it too. */
    pthread_mutex_lock(&g.lock);
    if (!g.inbound_enabled)
    {
        pthread_mutex_unlock(&g.lock);
        FM_INFO("sip", "rejecting inbound call from %s: not accepting calls", from);
        pjsua_call_hangup(call_id, PJSIP_SC_NOT_ACCEPTABLE_HERE, NULL, NULL);
        return;
    }
    if (g.line_held)
    {
        pthread_mutex_unlock(&g.lock);
        FM_WARN("sip", "rejecting inbound call from %s: another fax is in progress", from);
        pjsua_call_hangup(call_id, PJSIP_SC_BUSY_HERE, NULL, NULL);
        return;
    }
    g.line_held = true;
    n = ++g.call_counter;
    pthread_mutex_unlock(&g.lock);

    fm_timestamp_compact(stamp, sizeof(stamp));
    snprintf(tag, sizeof(tag), "in-%u", n);
    snprintf(rx_file, sizeof(rx_file), "%s/fax-%s-%s.tif", g.cfg.output_dir, stamp, tag);

    FM_INFO("sip", "inbound call %d from %s, answering (tag=%s -> %s)", (int) call_id, from, tag, rx_file);

    c = call_create(&g.cfg, true, tag, NULL, rx_file);
    if (c == NULL)
    {
        FM_ERROR("sip", "could not set up the receiver, rejecting call %d", (int) call_id);
        pjsua_call_hangup(call_id, PJSIP_SC_INTERNAL_SERVER_ERROR, NULL, NULL);
        line_release();
        return;
    }
    c->call_id = call_id;
    /* Before it is active, so no callback can find it half set up. From
     * here it belongs to the main thread, which reaps it. */
    pjsua_call_set_user_data(call_id, c);
    pthread_mutex_lock(&g.lock);
    g.active = c;
    signal_change_locked();
    pthread_mutex_unlock(&g.lock);

    fax_call_setting(&answer_cfg);
    pjsua_call_answer2(call_id, &answer_cfg, 180, NULL, NULL);
    pjsua_call_answer2(call_id, &answer_cfg, 200, NULL, NULL);
}

/* pjmedia adds a telephone-event (RFC 2833) payload to every audio stream at
 * compile time, so there is no setting to turn it off - the SDP has to be
 * edited on its way out. We never send or receive DTMF, and offering a payload
 * we will not use only gives a trunk something else to negotiate over. */
static void strip_telephone_event(pjmedia_sdp_media *m)
{
    unsigned i = 0;

    while (i < m->desc.fmt_count)
    {
        pjmedia_sdp_attr *rtpmap_attr = pjmedia_sdp_media_find_attr2(m, "rtpmap", &m->desc.fmt[i]);
        pjmedia_sdp_attr *fmtp_attr;
        pjmedia_sdp_rtpmap rtpmap;

        if (rtpmap_attr == NULL || pjmedia_sdp_attr_get_rtpmap(rtpmap_attr, &rtpmap) != PJ_SUCCESS ||
            pj_stricmp2(&rtpmap.enc_name, "telephone-event") != 0)
        {
            i++;
            continue;
        }

        FM_DEBUG("sip", "dropping telephone-event payload %.*s from the SDP", (int) m->desc.fmt[i].slen,
                 m->desc.fmt[i].ptr);

        fmtp_attr = pjmedia_sdp_media_find_attr2(m, "fmtp", &m->desc.fmt[i]);
        pjmedia_sdp_media_remove_attr(m, rtpmap_attr);
        if (fmtp_attr != NULL)
            pjmedia_sdp_media_remove_attr(m, fmtp_attr);

        for (unsigned k = i + 1; k < m->desc.fmt_count; k++)
            m->desc.fmt[k - 1] = m->desc.fmt[k];
        m->desc.fmt_count--;
    }
}

static void on_call_sdp_created(pjsua_call_id call_id, pjmedia_sdp_session *sdp, pj_pool_t *pool,
                                const pjmedia_sdp_session *rem_sdp)
{
    (void) call_id;
    (void) pool;
    (void) rem_sdp; /* non-NULL when this SDP is an answer; strip it either way */

    for (unsigned i = 0; i < sdp->media_count; i++)
    {
        if (pj_stricmp2(&sdp->media[i]->desc.media, "audio") == 0)
            strip_telephone_event(sdp->media[i]);
    }
}

static void on_reg_state2(pjsua_acc_id acc_id, pjsua_reg_info *info)
{
    struct pjsip_regc_cbparam *rp = info->cbparam;
    bool ok = (rp->code / 100 == 2) && rp->expiration > 0;

    (void) acc_id;
    pthread_mutex_lock(&g.lock);
    g.reg_done = true;
    g.reg_status = rp->code;
    g.reg_ok = ok;
    signal_change_locked();
    pthread_mutex_unlock(&g.lock);

    if (ok)
        FM_INFO("sip", "registered as %s (expires in %ds)", g.cfg.username, (int) rp->expiration);
    else if (rp->code / 100 == 2)
        FM_INFO("sip", "unregistered (%d)", rp->code);
    else
        FM_ERROR("sip", "registration failed: %d %.*s", rp->code, (int) rp->reason.slen, rp->reason.ptr);
}

/* ------------------------------------------------------------------- gate */

/* Rejects a new INVITE we would refuse anyway - inbound off, or the line
 * held - before pjsua sees it. pjsua gives every incoming INVITE a call slot
 * and an RTP and RTCP socket before on_incoming_call can say no, and pjlib
 * keeps a closed socket's slot for half a second: a few hundred INVITEs a
 * second - a SIP scanner finding a public address - ran out its 64 and pjlib
 * asserted, taking the process with it. Refused here, an INVITE costs one
 * transaction and nothing else. on_incoming_call still makes the decision
 * that counts, under the lock, for anything that gets past in the instant
 * the line is taken. */
static pj_bool_t gate_on_rx_request(pjsip_rx_data *rdata)
{
    const pjsip_msg *msg = rdata->msg_info.msg;
    int code = 0;
    char from[160];
    int n;

    if (msg->line.req.method.id != PJSIP_INVITE_METHOD)
        return PJ_FALSE;
    if (rdata->msg_info.to == NULL || rdata->msg_info.to->tag.slen != 0)
        return PJ_FALSE; /* within a dialog: a re-INVITE, ours to keep */

    pthread_mutex_lock(&g.lock);
    if (!g.inbound_enabled)
        code = PJSIP_SC_NOT_ACCEPTABLE_HERE;
    else if (g.line_held)
        code = PJSIP_SC_BUSY_HERE;
    pthread_mutex_unlock(&g.lock);
    if (code == 0)
        return PJ_FALSE;

    n = pjsip_uri_print(PJSIP_URI_IN_FROMTO_HDR, rdata->msg_info.from->uri, from, sizeof(from) - 1);
    from[n > 0 ? n : 0] = '\0';
    if (code == PJSIP_SC_BUSY_HERE)
        FM_WARN("sip", "rejecting inbound call from %s: another fax is in progress", from);
    else
        FM_INFO("sip", "rejecting inbound call from %s: not accepting calls", from);
    /* Statefully, so that retransmissions and the ACK are absorbed. */
    pjsip_endpt_respond(pjsua_get_pjsip_endpt(), NULL, rdata, code, NULL, NULL, NULL, NULL);
    return PJ_TRUE;
}

static pjsip_module g_gate = {
    .name = {"mod-faxmodem-gate", 17},
    .id = -1,
    /* After the transaction and dialog layers, which take retransmissions
     * and in-dialog requests; before pjsua, at PJSIP_MOD_PRIORITY_APPLICATION. */
    .priority = PJSIP_MOD_PRIORITY_APPLICATION - 1,
    .on_rx_request = &gate_on_rx_request,
};

/* ------------------------------------------------------------------ setup */

/* 0 while REGISTER is unanswered, then 1 registered or -1 refused. */
static int reg_result(void)
{
    int r;

    pthread_mutex_lock(&g.lock);
    r = !g.reg_done ? 0 : g.reg_ok ? 1 : -1;
    pthread_mutex_unlock(&g.lock);
    return r;
}

static int pjsip_level_for(const fm_config_t *cfg)
{
    if (cfg->pjsip_log_level >= 0)
        return cfg->pjsip_log_level;
    /* pjsip's level 3 includes periodic RTCP dumps for every call, which
     * drowns out our own output; keep it at warnings until --log-level debug. */
    switch (cfg->log_level)
    {
    case FM_LOG_ERROR:
        return 1;
    case FM_LOG_WARN:
    case FM_LOG_INFO:
        return 2;
    case FM_LOG_DEBUG:
        return 4;
    default:
        return 6;
    }
}

static void tune_codecs(const fm_config_t *cfg)
{
    pjsua_codec_info codecs[64];
    unsigned count = PJ_ARRAY_SIZE(codecs);
    bool want_pcma = (strcasecmp(cfg->codec, "pcma") == 0);

    if (pjsua_enum_codecs(codecs, &count) != PJ_SUCCESS)
        return;

    for (unsigned i = 0; i < count; i++)
    {
        const pj_str_t *id = &codecs[i].codec_id;
        pj_uint8_t prio = 0;
        bool is_g711 = false;

        if (pj_strnicmp2(id, "pcmu/", 5) == 0)
        {
            prio = want_pcma ? 254 : 255;
            is_g711 = true;
        }
        else if (pj_strnicmp2(id, "pcma/", 5) == 0)
        {
            prio = want_pcma ? 255 : 254;
            is_g711 = true;
        }

        pjsua_codec_set_priority(id, prio);
        FM_DEBUG("sip", "codec %.*s priority %d", (int) id->slen, id->ptr, (int) prio);

        if (is_g711)
        {
            /* Packet loss concealment invents audio, and perceptual
             * enhancement reshapes it. Both are poison for a modem. */
            pjmedia_codec_param param;
            if (pjsua_codec_get_param(id, &param) == PJ_SUCCESS)
            {
                param.setting.vad = 0;
                param.setting.plc = 0;
                param.setting.penh = 0;
                if (pjsua_codec_set_param(id, &param) == PJ_SUCCESS)
                    FM_DEBUG("sip", "codec %.*s: vad, plc and penh disabled", (int) id->slen, id->ptr);
            }
        }
    }
    FM_INFO("sip", "offering G.711 only (%s first); compressed codecs destroy fax tones",
            want_pcma ? "PCMA" : "PCMU");
}

int fm_sip_start(const fm_config_t *cfg)
{
    pjsua_config ua_cfg;
    pjsua_logging_config log_cfg;
    pjsua_media_config med_cfg;
    pjsua_transport_config tp_cfg;
    pjsua_acc_config acc_cfg;
    pjsip_transport_type_e tp_type = PJSIP_TRANSPORT_UDP;
    pjsua_transport_id tp_id;
    pj_status_t status;
    char id_uri[FM_STR_MAX * 2];
    char reg_uri[FM_STR_MAX + 8];
    char proxy_uri[FM_STR_MAX];

    g.cfg = *cfg;

    /* pjsua_create() logs before pjsua_init() installs log_cfg.cb, so claim
     * pjlib's writer first - otherwise those first lines land on stdout raw,
     * unformatted and unfiltered. */
    pj_log_set_log_func(&fm_log_pjsip_writer);
    pj_log_set_level(pjsip_level_for(cfg));

    status = pjsua_create();
    if (status != PJ_SUCCESS)
    {
        log_pj_error("pjsua_create", status);
        return FM_EXIT_INTERNAL;
    }
    g.started = true;

    pjsua_config_default(&ua_cfg);
    /* One fax at a time is enforced by the line, not here. These are pjsua's
     * slots, and a call can hold one well after we are done with it - a far
     * end that never answers our CANCEL keeps it for half a minute - so
     * leave room for the next fax to be placed meanwhile. */
    ua_cfg.max_calls = 4;
    ua_cfg.cb.on_call_state = &on_call_state;
    ua_cfg.cb.on_call_media_state = &on_call_media_state;
    ua_cfg.cb.on_incoming_call = &on_incoming_call;
    ua_cfg.cb.on_reg_state2 = &on_reg_state2;
    ua_cfg.cb.on_call_sdp_created = &on_call_sdp_created;
    ua_cfg.user_agent = pjs("faxmodem");

    if (cfg->stun_server[0] != '\0')
    {
        ua_cfg.stun_srv_cnt = 1;
        ua_cfg.stun_srv[0] = pjs(cfg->stun_server);
    }
    if (cfg->nameserver[0] != '\0')
    {
        ua_cfg.nameserver_count = 1;
        ua_cfg.nameserver[0] = pjs(cfg->nameserver);
    }

    pjsua_logging_config_default(&log_cfg);
    log_cfg.console_level = pjsip_level_for(cfg);
    log_cfg.level = pjsip_level_for(cfg);
    log_cfg.msg_logging = fm_log_enabled(FM_LOG_DEBUG) ? PJ_TRUE : PJ_FALSE;
    log_cfg.cb = &fm_log_pjsip_writer; /* everything pjsip says goes to stdout */

    pjsua_media_config_default(&med_cfg);
    med_cfg.clock_rate = FM_CLOCK_RATE;
    med_cfg.snd_clock_rate = FM_CLOCK_RATE;
    med_cfg.channel_count = 1;
    med_cfg.audio_frame_ptime = FM_PTIME_MS;
    med_cfg.no_vad = PJ_TRUE;    /* silence suppression would cut the carrier */
    med_cfg.ec_tail_len = 0;     /* echo cancellation mangles modem tones */
    med_cfg.quality = 10;        /* no resampling artefacts */
    med_cfg.jb_init = cfg->jitter_buffer_ms;
    med_cfg.jb_min_pre = cfg->jitter_buffer_ms;
    med_cfg.jb_max_pre = cfg->jitter_buffer_ms;
    med_cfg.jb_max = cfg->jitter_buffer_ms * 2;
    /* A jitter buffer that drops or stretches frames to chase latency would
     * corrupt the modem's sample stream; keep every sample. */
    med_cfg.jb_discard_algo = PJMEDIA_JB_DISCARD_NONE;
    med_cfg.snd_auto_close_time = -1;

    status = pjsua_init(&ua_cfg, &log_cfg, &med_cfg);
    if (status != PJ_SUCCESS)
    {
        log_pj_error("pjsua_init", status);
        return FM_EXIT_INTERNAL;
    }
    status = pjsip_endpt_register_module(pjsua_get_pjsip_endpt(), &g_gate);
    if (status != PJ_SUCCESS)
    {
        log_pj_error("pjsip_endpt_register_module", status);
        return FM_EXIT_INTERNAL;
    }

    if (strcasecmp(cfg->transport, "tcp") == 0)
        tp_type = PJSIP_TRANSPORT_TCP;
    else if (strcasecmp(cfg->transport, "tls") == 0)
        tp_type = PJSIP_TRANSPORT_TLS;

    pjsua_transport_config_default(&tp_cfg);
    tp_cfg.port = (unsigned) cfg->local_port;
    if (cfg->bind_addr[0] != '\0')
        tp_cfg.bound_addr = pjs(cfg->bind_addr);
    if (cfg->public_addr[0] != '\0')
        tp_cfg.public_addr = pjs(cfg->public_addr);

    status = pjsua_transport_create(tp_type, &tp_cfg, &tp_id);
    if (status != PJ_SUCCESS)
    {
        log_pj_error("pjsua_transport_create", status);
        return FM_EXIT_SIP;
    }
    {
        /* Log the port actually bound, not the one asked for: with --local-port 0
         * they differ, and this is the number that has to be reachable. */
        pjsua_transport_info tp_info;
        if (pjsua_transport_get_info(tp_id, &tp_info) == PJ_SUCCESS)
            FM_INFO("sip", "listening on %s %.*s:%d", cfg->transport, (int) tp_info.local_name.host.slen,
                    tp_info.local_name.host.ptr, tp_info.local_name.port);
        else
            FM_INFO("sip", "listening on %s port %d", cfg->transport, cfg->local_port);
    }

    status = pjsua_start();
    if (status != PJ_SUCCESS)
    {
        log_pj_error("pjsua_start", status);
        return FM_EXIT_INTERNAL;
    }

    /* There is no sound card in this design: the fax engine is the endpoint. */
    pjsua_set_null_snd_dev();
    tune_codecs(cfg);

    pjsua_acc_config_default(&acc_cfg);
    if (cfg->from_uri[0] != '\0')
        snprintf(id_uri, sizeof(id_uri), "%s", cfg->from_uri);
    else if (cfg->caller_id[0] != '\0' && strpbrk(cfg->caller_id, "<>\"@:;") == NULL)
        /* A bare number or name doubles as the From display name; a URI or a
         * full name-addr belongs only in P-Asserted-Identity. */
        snprintf(id_uri, sizeof(id_uri), "\"%s\" <sip:%s@%s>", cfg->caller_id, cfg->username, cfg->server);
    else
        snprintf(id_uri, sizeof(id_uri), "sip:%s@%s", cfg->username, cfg->server);
    acc_cfg.id = pjs(id_uri);

    if (cfg->do_register)
    {
        snprintf(reg_uri, sizeof(reg_uri), "sip:%s", cfg->server);
        acc_cfg.reg_uri = pjs(reg_uri);
        acc_cfg.reg_timeout = (unsigned) cfg->reg_expires_s;
    }
    if (cfg->proxy[0] != '\0')
    {
        if (strncasecmp(cfg->proxy, "sip:", 4) == 0 || strncasecmp(cfg->proxy, "sips:", 5) == 0)
            snprintf(proxy_uri, sizeof(proxy_uri), "%s", cfg->proxy);
        else
            snprintf(proxy_uri, sizeof(proxy_uri), "sip:%s;lr", cfg->proxy);
        acc_cfg.proxy_cnt = 1;
        acc_cfg.proxy[0] = pjs(proxy_uri);
    }
    if (cfg->password[0] != '\0')
    {
        acc_cfg.cred_count = 1;
        acc_cfg.cred_info[0].realm = pjs(cfg->realm[0] ? cfg->realm : "*");
        acc_cfg.cred_info[0].scheme = pjs("digest");
        acc_cfg.cred_info[0].username = pjs(cfg->auth_user[0] ? cfg->auth_user : cfg->username);
        acc_cfg.cred_info[0].data_type = PJSIP_CRED_DATA_PLAIN_PASSWD;
        acc_cfg.cred_info[0].data = pjs(cfg->password);
    }
    if (cfg->rtp_port > 0)
    {
        /* Without a range pjsip walks the port number upward forever as calls
         * come and go, and eventually every call negotiates a port outside
         * whatever the firewall or SBC allows - which looks exactly like a
         * carrier that has gone silent. Keep media inside a known window. */
        acc_cfg.rtp_cfg.port = (unsigned) cfg->rtp_port;
        acc_cfg.rtp_cfg.port_range = (unsigned) cfg->rtp_port_range;
        FM_INFO("sip", "media ports %d-%d must be reachable from the far end", cfg->rtp_port,
                cfg->rtp_port + cfg->rtp_port_range);
    }
    acc_cfg.vid_in_auto_show = PJ_FALSE;
    acc_cfg.vid_out_auto_transmit = PJ_FALSE;

    status = pjsua_acc_add(&acc_cfg, PJ_TRUE, &g.acc_id);
    if (status != PJ_SUCCESS)
    {
        log_pj_error("pjsua_acc_add", status);
        return FM_EXIT_SIP;
    }

    if (!cfg->do_register)
    {
        if (acc_cfg.cred_count > 0)
            FM_INFO("sip", "account %s ready, not registering; credentials for %s will answer the "
                           "401/407 challenge on each call",
                    id_uri, cfg->auth_user[0] ? cfg->auth_user : cfg->username);
        else
            FM_INFO("sip", "account %s ready, not registering and no credentials configured; "
                           "the far end must authenticate by source IP",
                    id_uri);
        return FM_EXIT_OK;
    }

    FM_INFO("sip", "registering %s at %s", id_uri, cfg->server);
    {
        int64_t deadline = fm_now_ms() + (int64_t) cfg->reg_timeout_s * 1000;
        while (reg_result() == 0 && fm_now_ms() < deadline)
            wait_change_ms(200);
    }
    switch (reg_result())
    {
    case 0:
        FM_ERROR("sip", "no answer to REGISTER within %ds", cfg->reg_timeout_s);
        return FM_EXIT_SIP;
    case 1:
        return FM_EXIT_OK;
    default:
        return FM_EXIT_SIP;
    }
}

void fm_sip_stop(void)
{
    fm_call_t *c;
    bool hang_up;

    if (!g.started)
        return;
    pthread_mutex_lock(&g.lock);
    c = g.active;
    g.active = NULL;
    g.inbound_enabled = false;
    hang_up = (c != NULL && c->call_id != PJSUA_INVALID_ID && !c->disconnected);
    pthread_mutex_unlock(&g.lock);
    if (c != NULL)
    {
        if (hang_up)
            pjsua_call_hangup(c->call_id, 0, NULL, NULL);
        call_destroy(c);
        line_release();
    }
    if (g.acc_id != PJSUA_INVALID_ID && g.cfg.do_register)
        pjsua_acc_set_registration(g.acc_id, PJ_FALSE);
    pjsua_destroy();
    g.started = false;
    FM_INFO("sip", "user agent stopped");
}

/* -------------------------------------------------------------- outbound  */

static void build_request_uri(const fm_config_t *cfg, const char *to, char *out, size_t out_len)
{
    bool is_uri = (strncasecmp(to, "sip:", 4) == 0 || strncasecmp(to, "sips:", 5) == 0);
    const char *transport_param = "";

    if (strcasecmp(cfg->transport, "tcp") == 0)
        transport_param = ";transport=tcp";
    else if (strcasecmp(cfg->transport, "tls") == 0)
        transport_param = ";transport=tls";

    if (is_uri)
        snprintf(out, out_len, "%s", to);
    else
        snprintf(out, out_len, "sip:%s@%s%s%s", to, cfg->server, cfg->user_phone ? ";user=phone" : "",
                 transport_param);
}

/* A fixed deadline is wrong at both ends of the range: long enough for a
 * twenty page document is absurd for a one page one, and a number that suits a
 * single page cuts off a long fax that is still making progress. Since the
 * sender knows the page count before it dials, scale the deadline to the work.
 * A stalled call no longer needs the deadline to catch it - the media and
 * progress watchdogs do that in seconds - so this can afford to be generous.
 * An explicit --timeout is always honoured as given. */
static int scaled_timeout(const fm_config_t *cfg, int pages, const char *tag)
{
    long scaled;

    if (cfg->timeout_explicit || cfg->seconds_per_page <= 0 || pages <= 0)
        return cfg->timeout_s;

    scaled = (long) FM_SETUP_BUDGET_S + (long) pages * cfg->seconds_per_page;
    if (scaled > FM_MAX_TIMEOUT_S)
        scaled = FM_MAX_TIMEOUT_S;
    if (scaled <= cfg->timeout_s)
        return cfg->timeout_s;

    FM_INFO("send", "deadline scaled to %lds for %d page%s (tag=%s, %ds setup + %ds per page)", scaled,
            pages, pages == 1 ? "" : "s", tag, FM_SETUP_BUDGET_S, cfg->seconds_per_page);
    return (int) scaled;
}

/* Builds the P-Asserted-Identity value (RFC 3325) from --caller-id. Anything
 * that already looks like a URI or a name-addr is passed through untouched;
 * a bare number becomes <sip:number@server>, which is what trunks that derive
 * the outbound CLI from this header expect. */
static void build_asserted_identity(const fm_config_t *cfg, char *out, size_t out_len)
{
    const char *cid = cfg->caller_id;

    if (cid[0] == '\0')
    {
        out[0] = '\0';
        return;
    }
    if (cid[0] == '<' || cid[0] == '"')
        snprintf(out, out_len, "%s", cid); /* caller supplied a full header value */
    else if (strncasecmp(cid, "sip:", 4) == 0 || strncasecmp(cid, "sips:", 5) == 0 ||
             strncasecmp(cid, "tel:", 4) == 0)
        snprintf(out, out_len, "<%s>", cid);
    else
        snprintf(out, out_len, "<sip:%s@%s>", cid, cfg->server);
}

int fm_sip_send_fax(const fm_config_t *cfg, const char *to, const char *file, const char *tag,
                    fm_call_result_t *result)
{
    pjsua_msg_data msg_data;
    pjsip_generic_string_hdr pai_hdr;
    pj_str_t pai_name = pjs("P-Asserted-Identity");
    pj_str_t pai_value;
    char pai[FM_STR_MAX + 64];
    fm_call_t *c;
    fm_tiff_info_t info;
    char uri[FM_STR_MAX * 2];
    char err[512];
    pj_str_t dst;
    pjsua_call_setting call_cfg;
    pjsua_call_id call_id = PJSUA_INVALID_ID;
    pj_status_t status;
    int64_t deadline;
    int rc = FM_EXIT_OK;
    bool timed_out = false;

    memset(result, 0, sizeof(*result));
    result->t30_result = -1;

    if (!fm_tiff_probe(file, &info, err, sizeof(err)))
    {
        FM_ERROR("send", "%s", err);
        snprintf(result->t30_text, sizeof(result->t30_text), "bad input document");
        return FM_EXIT_CONFIG;
    }
    fm_tiff_log(&info, file);

    build_request_uri(cfg, to, uri, sizeof(uri));
    if (pjsua_verify_sip_url(uri) != PJ_SUCCESS)
    {
        FM_ERROR("send", "'%s' is not a valid SIP URI", uri);
        return FM_EXIT_CONFIG;
    }

    if (!line_take_outbound())
    {
        FM_ERROR("send", "another call is already in progress");
        return FM_EXIT_INTERNAL;
    }

    c = call_create(cfg, false, tag, file, NULL);
    if (c == NULL)
    {
        line_release();
        return FM_EXIT_INTERNAL;
    }
    c->timeout_s = scaled_timeout(cfg, info.pages, tag);
    pthread_mutex_lock(&g.lock);
    g.active = c;
    pthread_mutex_unlock(&g.lock);

    fax_call_setting(&call_cfg);

    /* pjsua clones the header into the INVITE while sending, so these locals
     * only have to outlive pjsua_call_make_call(). */
    pjsua_msg_data_init(&msg_data);
    build_asserted_identity(cfg, pai, sizeof(pai));
    if (pai[0] != '\0')
    {
        pai_value = pjs(pai);
        pjsip_generic_string_hdr_init2(&pai_hdr, &pai_name, &pai_value);
        pj_list_push_back(&msg_data.hdr_list, &pai_hdr);
        FM_DEBUG("send", "P-Asserted-Identity: %s", pai);
    }

    dst = pjs(uri);
    FM_INFO("send", "calling %s (tag=%s, %d page%s)", uri, tag, info.pages, info.pages == 1 ? "" : "s");

    /* pjsua takes c as the call's user data here, and its callbacks can run
     * before this returns; they find c through it, and it is already
     * active. */
    status = pjsua_call_make_call(g.acc_id, &dst, &call_cfg, c, &msg_data, &call_id);
    if (status != PJ_SUCCESS)
    {
        log_pj_error("pjsua_call_make_call", status);
        call_fill_result(c, result);
        call_release(c);
        return FM_EXIT_CALL;
    }
    c->call_id = call_id;

    deadline = c->started_ms + (int64_t) c->timeout_s * 1000;
    while (!call_disconnected(c) && !fm_fax_completed(c->fax) && fm_now_ms() < deadline)
    {
        wait_change_ms(200);
        if (call_stalled(c))
            break;
    }

    if (c->stall_reason != NULL && !call_disconnected(c))
    {
        pjsua_call_hangup(call_id, PJSIP_SC_REQUEST_TIMEOUT, NULL, NULL);
        int64_t grace = fm_now_ms() + FM_HANGUP_GRACE_MS;
        while (!call_disconnected(c) && fm_now_ms() < grace)
            wait_change_ms(100);
        timed_out = true;
    }
    else if (fm_fax_completed(c->fax) && !call_disconnected(c))
    {
        FM_INFO("send", "fax finished, hanging up call %d", (int) call_id);
        pjsua_call_hangup(call_id, PJSIP_SC_OK, NULL, NULL);
        int64_t grace = fm_now_ms() + FM_HANGUP_GRACE_MS;
        while (!call_disconnected(c) && fm_now_ms() < grace)
            wait_change_ms(100);
    }
    else if (!call_disconnected(c))
    {
        FM_ERROR("send", "timed out after %ds, tearing the call down", c->timeout_s);
        pjsua_call_hangup(call_id, PJSIP_SC_REQUEST_TIMEOUT, NULL, NULL);
        int64_t grace = fm_now_ms() + FM_HANGUP_GRACE_MS;
        while (!call_disconnected(c) && fm_now_ms() < grace)
            wait_change_ms(100);
        timed_out = true;
    }

    call_fill_result(c, result);

    if (result->t30_result == FM_T30_OK)
        rc = FM_EXIT_OK;
    else if (result->connected)
        rc = timed_out ? FM_EXIT_TIMEOUT : FM_EXIT_FAX;
    else if (result->sip_status >= 400)
        rc = FM_EXIT_CALL; /* rejected, busy, unreachable - the call never ran */
    else
        rc = timed_out ? FM_EXIT_TIMEOUT : FM_EXIT_CALL;

    fm_log_event(rc == FM_EXIT_OK ? FM_LOG_INFO : FM_LOG_ERROR, "send", "call finished",
                 "tag=%s to=\"%s\" sip_status=%d sip_reason=\"%s\" connected=%s result=\"%s\" pages=%d "
                 "bit_rate=%d v34=%s ecm=%s remote_id=\"%s\" duration_ms=%d exit=%d",
                 tag, to, result->sip_status, result->sip_reason, result->connected ? "yes" : "no",
                 result->t30_text, result->pages, result->bit_rate, result->v34 ? "yes" : "no",
                 result->ecm ? "yes" : "no",
                 result->remote_ident, result->duration_ms, rc);

    call_release(c);
    return rc;
}

/* --------------------------------------------------------------- inbound  */

void fm_sip_set_inbound(bool enabled)
{
    pthread_mutex_lock(&g.lock);
    g.inbound_enabled = enabled;
    pthread_mutex_unlock(&g.lock);
    FM_INFO("sip", "inbound calls %s", enabled ? "will be answered" : "will be rejected");
}

bool fm_sip_call_active(void)
{
    bool held;

    pthread_mutex_lock(&g.lock);
    held = g.line_held;
    pthread_mutex_unlock(&g.lock);
    return held;
}

bool fm_sip_reserve_line(void)
{
    return line_take_outbound();
}

void fm_sip_release_line(void)
{
    bool ours;

    pthread_mutex_lock(&g.lock);
    ours = g.outbound_held && g.active == NULL;
    pthread_mutex_unlock(&g.lock);
    if (ours)
        line_release();
}

void fm_sip_poll_inbound(void)
{
    fm_call_t *c;
    fm_call_result_t result;

    /* An inbound context, once in g.active, is the main thread's to free, so
     * it stays valid after the lock is dropped. */
    pthread_mutex_lock(&g.lock);
    c = g.active;
    pthread_mutex_unlock(&g.lock);
    if (c == NULL || !c->inbound)
        return;

    if (!call_disconnected(c))
    {
        bool done = fm_fax_completed(c->fax);
        bool stalled = call_stalled(c);
        bool expired = fm_now_ms() > c->started_ms + (int64_t) c->timeout_s * 1000;

        if (!done && !expired && !stalled)
            return;
        if (!done)
        {
            FM_ERROR("receive", "abandoning inbound fax %s: %s", c->tag,
                     stalled ? c->stall_reason : "overall timeout reached");
        }
        else
        {
            /* The far end sends DCN and clears the call itself; only take the
             * call down ourselves if it overstays. */
            int64_t linger = fm_now_ms() + FM_RX_LINGER_MS;
            FM_INFO("receive", "inbound fax %s finished, waiting for the caller to clear", c->tag);
            while (!call_disconnected(c) && fm_now_ms() < linger)
                wait_change_ms(100);
            if (call_disconnected(c))
                goto reap;
            FM_DEBUG("receive", "caller %s did not hang up, clearing the call", c->tag);
        }

        pjsua_call_hangup(c->call_id, done ? PJSIP_SC_OK : PJSIP_SC_REQUEST_TIMEOUT, NULL, NULL);

        int64_t grace = fm_now_ms() + FM_HANGUP_GRACE_MS;
        while (!call_disconnected(c) && fm_now_ms() < grace)
            wait_change_ms(100);
    }

reap:
    call_fill_result(c, &result);
    fm_log_event(result.t30_result == 0 ? FM_LOG_INFO : FM_LOG_ERROR, "receive", "fax received",
                 "tag=%s file=\"%s\" result=\"%s\" pages=%d bit_rate=%d v34=%s ecm=%s remote_id=\"%s\" "
                 "sip_status=%d duration_ms=%d",
                 c->tag, c->rx_file, result.t30_text, result.pages, result.bit_rate,
                 result.v34 ? "yes" : "no", result.ecm ? "yes" : "no", result.remote_ident, result.sip_status, result.duration_ms);

    if (result.t30_result != 0 && fm_file_exists(c->rx_file))
        FM_WARN("receive", "%s may be incomplete", c->rx_file);

    call_release(c);
}

int fm_sip_run_inbound(const fm_config_t *cfg, volatile sig_atomic_t *stop)
{
    char err[512];

    if (!fm_mkdir_p(cfg->output_dir, err, sizeof(err)))
    {
        FM_ERROR("receive", "%s", err);
        return FM_EXIT_CONFIG;
    }

    fm_sip_set_inbound(true);
    FM_INFO("receive", "waiting for inbound faxes, writing to %s (ctrl-c to stop)", cfg->output_dir);

    while (*stop == 0)
    {
        wait_change_ms(200);
        fm_sip_poll_inbound();
    }

    FM_INFO("receive", "shutting down");
    fm_sip_set_inbound(false);
    return FM_EXIT_OK;
}
