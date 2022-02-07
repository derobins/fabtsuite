#include <assert.h>
#include <err.h>
#include <libgen.h> /* basename(3) */
#include <limits.h> /* INT_MAX */
#include <inttypes.h>   /* PRIu32 */
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h> /* strcmp(3), strdup(3) */
#include <unistd.h> /* sysconf(3) */

#include <rdma/fabric.h>
#include <rdma/fi_cm.h>     /* fi_listen, fi_getname */
#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_rma.h>    /* struct fi_msg_rma */

#define arraycount(a)   (sizeof(a) / sizeof(a[0]))

/*
 * Message definitions
 */
typedef struct {
    uint64_t bits[2];
} nonce_t;

typedef struct initial_msg {
    nonce_t nonce;
    uint32_t nsources;
    uint32_t id;
} initial_msg_t;

typedef struct vector_msg {
    uint32_t niovs;
    struct {
        uint64_t addr, len, key;
    } iov[12];
} vector_msg_t;

typedef struct progress_msg {
    uint64_t nfilled;
    uint64_t nleftover;
} progress_msg_t;

/* Communication buffers */

struct buf;
typedef struct buf buf_t;

struct buf {
    size_t nfull;
    size_t nallocated;
    struct fid_mr mr;
    void *desc;
    char content[];
};

typedef struct fifo {
    uint64_t insertions;
    uint64_t removals;
    size_t index_mask;  // for some integer n > 0, 2^n - 1 == index_mask
    buf_t *buf[];
} fifo_t;

typedef struct buflist {
    size_t nfull;
    size_t nallocated;
    buf_t *buf[];
} buflist_t;

/* Communication terminals: sources and sinks */

typedef struct {
    int (*trade)(buflist_t *ready, buflist_t *completed);
} terminal_t;

typedef struct {
    terminal_t term;
} sink_t;

typedef struct {
    terminal_t term;
} source_t;

/*
 * Communications state definitions
 */

struct cxn;
typedef struct cxn cxn_t;

struct session;
typedef struct session session_t;

struct worker;
typedef struct worker worker_t;

struct cxn {
    session_t *(*loop)(worker_t *, session_t *);
    struct fid_cq *cq;
};

typedef struct {
    cxn_t cxn;
    bool started;
    struct fid_ep *aep;
    struct fid_eq *active_eq;
    struct {
        struct iovec iov[12];
        void *desc[12];
        struct fid_mr *mr[12];
        uint64_t raddr[12];
        ssize_t niovs;
        initial_msg_t msg;
    } initial;
    struct {
        struct iovec iov[12];
        void *desc[12];
        struct fid_mr *mr[12];
        uint64_t raddr[12];
        ssize_t niovs;
        vector_msg_t msg;
    } vector;
    struct {
        struct iovec iov[12];
        void *desc[12];
        struct fid_mr *mr[12];
        uint64_t raddr[12];
        ssize_t niovs;
        progress_msg_t msg;
    } progress;
    struct {
        struct iovec iov[12];
        void *desc[12];
        struct fid_mr *mr[12];
        uint64_t raddr[12];
        ssize_t niovs;
        char rxbuf[128];
    } payload;
} rcvr_t;

typedef struct {
    cxn_t cxn;
    bool started;
    struct fid_ep *ep;
    struct {
        void *desc;
        struct fid_mr *mr;
        initial_msg_t msg;
    } initial;
    struct {
        void *desc;
        struct fid_mr *mr;
        vector_msg_t msg;
    } vector;
    struct {
        void *desc;
        struct fid_mr *mr;
        progress_msg_t msg;
    } progress;
    struct {
        struct iovec iov[12];
        void *desc[12];
        struct fid_mr *mr[12];
        uint64_t raddr[12];
        ssize_t niovs;
    } payload;
} xmtr_t;

/* On each loop, a worker checks its poll set for any completions.
 * If `loops_since_mark < UINT16_MAX`, a worker increases it by
 * one, and increases `ctxs_serviced_since_mark` by the number
 * of queues that actually held one or more completions.  If
 * `loops_since_mark == UINT16_MAX`, then a worker updates
 * `average`, `average = (average + 256 * ctxs_serviced_since_mark /
 * (UINT16_MAX + 1)) / 2`, and resets `loops_since_mark` and
 * `ctxs_serviced_since_mark` to 0.
 */
typedef struct {
    /* A fixed-point number with 8 bits right of the decimal point: */
    volatile atomic_uint_fast16_t average;
    uint_fast16_t loops_since_mark;
    uint32_t ctxs_serviced_since_mark;
} loadavg_t;

#define WORKER_SESSIONS_MAX 64
#define WORKERS_MAX 128
#define SESSIONS_MAX (WORKER_SESSIONS_MAX * WORKERS_MAX)

struct session {
    terminal_t *terminal;
    cxn_t *cxn;
    fifo_t *ready_for_cxn;
    fifo_t *ready_for_terminal;
};

struct worker {
    pthread_t thd;
    loadavg_t avg;
    terminal_t *term[WORKER_SESSIONS_MAX];
    session_t session[WORKER_SESSIONS_MAX];
    volatile _Atomic size_t nsessions[2];   // number of sessions in each half
                                            // of session[]
    struct fid_poll *pollset[2];
    pthread_mutex_t mtx[2]; /* mtx[0] protects pollset[0] and the first half
                             * of session[]; mtx[1], pollset[1] and the second
                             * half
                             */
    pthread_cond_t sleep;   /* Used in conjunction with workers_mtx. */
    volatile atomic_bool cancelled;
    buflist_t *freebufs;    /* Free buffer reservoir. */
};

typedef struct {
    struct fid_eq *listen_eq;
    struct fid_pep *pep;
    rcvr_t rcvr;
} get_state_t;

typedef struct {
    struct fid_eq *connect_eq;
    xmtr_t xmtr;
} put_state_t;

typedef struct {
    struct fid_domain *domain;
    struct fid_fabric *fabric;
    struct fi_info *info;
    union {
        get_state_t get;
        put_state_t put;
    } u;
    size_t mr_maxsegs;
    size_t rx_maxsegs;
    size_t tx_maxsegs;
    size_t rma_maxsegs;
} state_t;

typedef int (*personality_t)(state_t *);

static const char fget_fput_service_name[] = "4242";

static pthread_mutex_t workers_mtx = PTHREAD_MUTEX_INITIALIZER;
static worker_t workers[WORKERS_MAX];
static size_t nworkers_running;
static size_t nworkers_allocated;
static pthread_cond_t nworkers_cond = PTHREAD_COND_INITIALIZER;

static bool workers_assignment_suspended = false;

static const uint64_t desired_rx_flags = FI_RECV | FI_MSG;
static const uint64_t desired_tx_flags = FI_SEND | FI_MSG;

static char txbuf[] =
    "If this message was received in error then please "
    "print it out and shred it.";

#define bailout_for_ofi_ret(ret, ...)                          \
        bailout_for_ofi_ret_impl(ret, __func__, __LINE__, __VA_ARGS__)

#define warn_about_ofi_ret(ret, ...)                          \
        warn_about_ofi_ret_impl(ret, __func__, __LINE__, __VA_ARGS__)

static void
warnv_about_ofi_ret_impl(int ret, const char *fn, int lineno,
    const char *fmt, va_list ap)
{
    fprintf(stderr, "%s.%d: ", fn, lineno);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, ": %s\n", fi_strerror(-ret));
}

static void
warn_about_ofi_ret_impl(int ret, const char *fn, int lineno,
    const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    warnv_about_ofi_ret_impl(ret, fn, lineno, fmt, ap);
    va_end(ap);
}

static void
bailout_for_ofi_ret_impl(int ret, const char *fn, int lineno,
    const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    warnv_about_ofi_ret_impl(ret, fn, lineno, fmt, ap);
    va_end(ap);

    exit(EXIT_FAILURE);
}

#if 0
static int
maxsize(size_t l, size_t r)
{
    return (l > r) ? l : r;
}
#endif

static int
minsize(size_t l, size_t r)
{
    return (l < r) ? l : r;
}

#if 0
static inline bool
size_is_power_of_2(size_t size)
{
    return ((size - 1) & size) == 0;
}

static fifo_t *
fifo_create(size_t size)
{
    if (!size_is_power_of_2(size))
        return NULL;

    fifo_t *f = malloc(offsetof(fifo_t, buf[0]) + sizeof(f->buf[0]) * size);

    if (f == NULL)
        return NULL;

    f->insertions = f->removals = 0;
    f->index_mask = size - 1;

    return f;
}

static inline buf_t *
fifo_get(fifo_t *f)
{
    assert(f->insertions >= f->removals);

    if (f->insertions == f->removals)
        return NULL;

    buf_t *b = f->buf[f->removals & (uint64_t)f->index_mask];
    f->removals++;

    return b;
}

static inline bool
fifo_empty(fifo_t *f)
{
    return f->insertions == f->removals;
}

static inline bool
fifo_full(fifo_t *f)
{
    return f->insertions - f->removals == f->index_mask + 1;
}

static inline bool
fifo_put(fifo_t *f, buf_t *b)
{
    assert(f->insertions - f->removals <= f->index_mask + 1);

    if (f->insertions - f->removals > f->index_mask)
        return false;

    f->buf[f->insertions & (uint64_t)f->index_mask] = b;
    f->insertions++;

    return true;
}
#endif

static session_t *
rcvr_start(worker_t *w, session_t *s)
{
    rcvr_t *r = (rcvr_t *)s->cxn;
    struct fi_cq_msg_entry completion;
    ssize_t ncompleted;

    r->started = true;

    /* Await initial message. */
    do {
        ncompleted = fi_cq_sread(r->cxn.cq, &completion, 1, NULL, -1);
    } while (ncompleted == -FI_EAGAIN);

    if (ncompleted < 0)
        bailout_for_ofi_ret(ncompleted, "fi_cq_sread");

    if (ncompleted != 1) {
        errx(EXIT_FAILURE,
            "%s: expected 1 completion, read %zd", __func__, ncompleted);
    }

    if ((completion.flags & desired_rx_flags) != desired_rx_flags) {
        errx(EXIT_FAILURE,
            "%s: expected flags %" PRIu64 ", received flags %" PRIu64,
            __func__, desired_rx_flags, completion.flags & desired_rx_flags);
    }

    if (completion.len != sizeof(r->initial.msg)) {
        errx(EXIT_FAILURE,
            "initially received %zu bytes, expected %zu\n", completion.len,
            sizeof(r->initial.msg));
    }

    if (r->initial.msg.nsources != 1 || r->initial.msg.id != 0) {
        errx(EXIT_FAILURE,
            "received nsources %" PRIu32 ", id %" PRIu32 ", expected 1, 0\n",
            r->initial.msg.nsources, r->initial.msg.id);
    }

    return s;
}

static session_t *
rcvr_loop(worker_t *w, session_t *s)
{
    rcvr_t *r = (rcvr_t *)s->cxn;
    struct fi_cq_msg_entry completion;
    ssize_t ncompleted;
    int rc;

    if (!r->started)
        return rcvr_start(w, s);

    /* Transmit vector. */

    rc = fi_sendmsg(r->aep, &(struct fi_msg){
          .msg_iov = r->vector.iov
        , .desc = r->vector.desc
        , .iov_count = r->vector.niovs
        , .addr = 0
        , .context = NULL
        , .data = 0
        }, 0);

    if (rc < 0)
        bailout_for_ofi_ret(rc, "fi_sendmsg");

    /* Await progress message. */
    do {
        printf("%s: awaiting progress message\n", __func__);
        ncompleted = fi_cq_sread(r->cxn.cq, &completion, 1, NULL, -1);

        if (ncompleted == -FI_EAVAIL) {
            struct fi_cq_err_entry e;
            ssize_t nfailed = fi_cq_readerr(r->cxn.cq, &e, 0);

            warnx("%s: read %zd errors, %s", __func__, nfailed,
                fi_strerror(e.err));
            warnx("%s: completion flags %" PRIx64 " expected %" PRIx64,
                __func__, e.flags, desired_rx_flags);
            abort();
        }
    } while (ncompleted == -FI_EAGAIN);

    if (ncompleted < 0)
        bailout_for_ofi_ret(ncompleted, "fi_cq_sread");

    if (ncompleted != 1) {
        errx(EXIT_FAILURE,
            "%s: expected 1 completion, read %zd", __func__, ncompleted);
    }

    if ((completion.flags & desired_rx_flags) != desired_rx_flags) {
        errx(EXIT_FAILURE,
            "%s: expected flags %" PRIu64 ", received flags %" PRIu64,
            __func__, desired_rx_flags, completion.flags & desired_rx_flags);
    }

    if (completion.len != sizeof(r->progress.msg)) {
        errx(EXIT_FAILURE,
            "received %zu bytes, expected %zu-byte progress\n", completion.len,
            sizeof(r->progress.msg));
    }

    if (r->progress.msg.nfilled != strlen(txbuf)) {
        errx(EXIT_FAILURE,
            "progress: %" PRIu64 " bytes filled, expected %" PRIu64 "\n",
            r->progress.msg.nfilled,
            strlen(txbuf));
    }

    if (r->progress.msg.nleftover != 0) {
        errx(EXIT_FAILURE,
            "progress: %" PRIu64 " bytes leftover, expected 0\n",
            r->progress.msg.nleftover);
    }

    /* Verify received payload. */
    printf("%zu bytes filled\n", r->progress.msg.nfilled);

    if (strlen(txbuf) != r->progress.msg.nfilled)
        errx(EXIT_FAILURE, "unexpected received message length");

    if (strncmp(txbuf, r->payload.rxbuf, r->progress.msg.nfilled) != 0)
        errx(EXIT_FAILURE, "unexpected received message content");

    return NULL;
}

static session_t *
xmtr_start(session_t *s)
{
    xmtr_t *x = (xmtr_t *)s->cxn;
    int rc;

    x->started = true;

    /* Post receive for first vector message. */
    x->vector.desc = fi_mr_desc(x->vector.mr);

    rc = fi_recvmsg(x->ep, &(struct fi_msg){
          .msg_iov = &(struct iovec){.iov_base = &x->vector.msg,
                                     .iov_len = sizeof(x->vector.msg)}
        , .desc = &x->vector.desc
        , .iov_count = 1
        , .addr = 0
        , .context = NULL
        , .data = 0
        }, FI_COMPLETION);

    if (rc < 0)
        bailout_for_ofi_ret(rc, "fi_recvmsg");

    /* Setup & transmit initial message. */
    memset(&x->initial.msg, 0, sizeof(x->initial.msg));
    x->initial.msg.nsources = 1;
    x->initial.msg.id = 0;

    x->initial.desc = fi_mr_desc(x->initial.mr);

    rc = fi_sendmsg(x->ep, &(struct fi_msg){
          .msg_iov = &(struct iovec){.iov_base = &x->initial.msg,
                                     .iov_len = sizeof(x->initial.msg)}
        , .desc = &x->initial.desc
        , .iov_count = 1
        , .addr = 0
        , .context = NULL
        , .data = 0
        }, 0);

    if (rc < 0)
        bailout_for_ofi_ret(rc, "fi_sendmsg");

    return s;
}

static session_t *
xmtr_loop(worker_t *w, session_t *s)
{
    xmtr_t *x = (xmtr_t *)s->cxn;
    struct fi_rma_iov riov[12];
    struct fi_cq_msg_entry completion;
    const size_t txbuflen = strlen(txbuf);
    size_t i, nriovs;
    ssize_t ncompleted;
    int rc;

    if (!x->started)
        return xmtr_start(s);

    /* Await reply to initial message: first vector message. */
    do {
        ncompleted = fi_cq_sread(x->cxn.cq, &completion, 1, NULL, -1);
    } while (ncompleted == -FI_EAGAIN);

    if (ncompleted < 0)
        bailout_for_ofi_ret(ncompleted, "fi_cq_sread");

    if (ncompleted != 1) {
        errx(EXIT_FAILURE,
            "%s: expected 1 completion, read %zd", __func__, ncompleted);
    }

    if ((completion.flags & desired_rx_flags) != desired_rx_flags) {
        errx(EXIT_FAILURE,
            "%s: expected flags %" PRIu64 ", received flags %" PRIu64,
            __func__, desired_rx_flags, completion.flags & desired_rx_flags);
    }

    const ptrdiff_t least_vector_msglen =
        (char *)&x->vector.msg.iov[0] - (char *)&x->vector.msg;

    if (completion.len < least_vector_msglen) {
        errx(EXIT_FAILURE, "%s: expected >= %zu bytes, received %zu",
            __func__, least_vector_msglen, completion.len);
    }

    if (completion.len == least_vector_msglen) {
        errx(EXIT_SUCCESS, "%s: peer sent 0 vectors, disconnecting...",
            __func__);
    }

    if ((completion.len - least_vector_msglen) %
        sizeof(x->vector.msg.iov[0]) != 0) {
        errx(EXIT_SUCCESS,
            "%s: %zu-byte vector message did not end on vector boundary, "
            "disconnecting...", __func__, completion.len);
    }

    const size_t niovs_space = (completion.len - least_vector_msglen) /
        sizeof(x->vector.msg.iov[0]);

    if (niovs_space < x->vector.msg.niovs) {
        errx(EXIT_SUCCESS, "%s: peer sent truncated vectors, disconnecting...",
            __func__);
    }

    if (x->vector.msg.niovs > arraycount(riov)) {
        errx(EXIT_SUCCESS, "%s: peer sent too many vectors, disconnecting...",
            __func__);
    }

    x->payload.iov[0] = (struct iovec){.iov_base = txbuf, .iov_len = txbuflen};
    x->payload.desc[0] = fi_mr_desc(x->payload.mr[0]);

    size_t nremaining = txbuflen;

    /* TBD only fill riov[] to the number of vectors we need.  Track the
     * need in a new variable, `nriovs`.  Set the length of the last
     * vector to the bytes remaining in `txbuf` after assigning bytes to
     * the preceding vectors.
     */
    for (i = 0; 0 < nremaining && i < x->vector.msg.niovs; i++) {
        printf("%s: received vector %zd "
            "addr %" PRIu64 " len %" PRIu64 " key %" PRIx64 "\n",
            __func__, i, x->vector.msg.iov[i].addr, x->vector.msg.iov[i].len,
            x->vector.msg.iov[i].key);
        riov[i].len = minsize(nremaining, x->vector.msg.iov[i].len);
        nremaining -= riov[i].len;
        riov[i].addr = x->vector.msg.iov[i].addr;
        riov[i].key = x->vector.msg.iov[i].key;
    }

    nriovs = i;

    if (nremaining > 0) {
        errx(EXIT_FAILURE, "%s: the receiver's buffer cannot fit the payload",
            __func__);
    }

#if 1
    struct fi_msg_rma mrma;
    mrma.msg_iov = x->payload.iov;
    mrma.desc = x->payload.desc;
    mrma.iov_count = 1;
    mrma.addr = 0;
    mrma.rma_iov = riov;
    mrma.rma_iov_count = nriovs;
    mrma.context = NULL;
    mrma.data = 0;

    rc = fi_writemsg(x->ep, &mrma, 0);

    if (rc != 0)
        bailout_for_ofi_ret(rc, "fi_writemsg");

#   if 0
    /* Await RDMA completion. Pass flags FI_COMPLETION |
     * FI_DELIVERY_COMPLETE to fi_writemsg, above.
     */
    do {
        printf("%s: awaiting RMA completion.\n", __func__);
        ncompleted = fi_cq_sread(x->cxn.cq, &completion, 1, NULL, -1);
    } while (ncompleted == -FI_EAGAIN);

    if (ncompleted < 0)
        bailout_for_ofi_ret(ncompleted, "fi_cq_sread");

    if (ncompleted != 1) {
        errx(EXIT_FAILURE,
            "%s: expected 1 completion, read %zd", __func__, ncompleted);
    }
#   endif
#else
    size_t nwritten = 0;
    for (i = 0; i < x->vector.msg.niovs && nwritten < txbuflen; i++) {
        const size_t split = 0;
        if (split > 0 && minsize(riov[i].len, txbuflen - nwritten) > split) {
            rc = fi_write(x->ep, txbuf + nwritten, split,
                fi_mr_desc(x->payload.mr[0]), 0, riov[i].addr, riov[i].key, NULL);
            if (rc != 0)
                bailout_for_ofi_ret(rc, "fi_write");
            rc = fi_write(x->ep, txbuf + nwritten + split,
                minsize(riov[i].len, txbuflen - nwritten) - split,
                fi_mr_desc(x->payload.mr[0]), 0, riov[i].addr + split,
                riov[i].key, NULL);
        } else {
            rc = fi_write(x->ep, txbuf + nwritten,
                minsize(riov[i].len, txbuflen - nwritten),
                fi_mr_desc(x->payload.mr[0]), 0, riov[i].addr, riov[i].key, NULL);
        }
        if (rc != 0)
            bailout_for_ofi_ret(rc, "fi_write");
        nwritten += minsize(riov[i].len, txbuflen - nwritten);
    }

#endif

    x->progress.msg.nfilled = txbuflen;
    x->progress.msg.nleftover = 0;

    x->progress.desc = fi_mr_desc(x->progress.mr);

    rc = fi_sendmsg(x->ep, &(struct fi_msg){
          .msg_iov = &(struct iovec){.iov_base = &x->progress.msg,
                                     .iov_len = sizeof(x->progress.msg)}
        , .desc = &x->progress.desc
        , .iov_count = 1
        , .addr = 0
        , .context = NULL
        , .data = 0
        }, FI_FENCE | FI_COMPLETION);

    /* Await transmission of progress message. */
    do {
        printf("%s: awaiting progress message transmission.\n", __func__);
        ncompleted = fi_cq_sread(x->cxn.cq, &completion, 1, NULL, -1);
    } while (ncompleted == -FI_EAGAIN);

    if (ncompleted < 0)
        bailout_for_ofi_ret(ncompleted, "fi_cq_sread");

    if (ncompleted != 1) {
        errx(EXIT_FAILURE,
            "%s: expected 1 completion, read %zd", __func__, ncompleted);
    }

    if ((completion.flags & desired_tx_flags) != desired_tx_flags) {
        errx(EXIT_FAILURE,
            "%s: expected flags %" PRIu64 ", received flags %" PRIu64,
            __func__, desired_tx_flags, completion.flags & desired_tx_flags);
    }

    printf("sent %zu of %zu bytes progress message\n", completion.len,
        sizeof(x->progress.msg));

    return NULL;
}

static session_t *
cxn_loop(worker_t *w, session_t *s)
{
    return s->cxn->loop(w, s);
}

static void
worker_run_loop(worker_t *self)
{
    size_t half, i;

    for (half = 0; half < 2; half++) {
        pthread_mutex_t *mtx = &self->mtx[half];

        if (pthread_mutex_trylock(mtx) == EBUSY)
            continue;

        // find a non-empty receiver slot
        for (i = 0; i < arraycount(self->session) / 2; i++) {
            int rc;
            session_t *s;
            cxn_t *c, **cp;

            s = &self->session[half * arraycount(self->session) / 2 + i];
            cp = &s->cxn;

            // skip empty slots
            if ((c = *cp) == NULL)
                continue;

            // continue at next cxn_t if `c` did not exit
            if (cxn_loop(self, s) != NULL)
                continue;

            *cp = NULL;

            if ((rc = fi_poll_del(self->pollset[half], &c->cq->fid, 0)) != 0) {
                warn_about_ofi_ret(rc, "fi_poll_del");
                continue;
            }
            atomic_fetch_add_explicit(&self->nsessions[half], -1,
                memory_order_relaxed);
        }

        (void)pthread_mutex_unlock(mtx);
    }
}

static bool
worker_is_idle(worker_t *self)
{
    const ptrdiff_t self_idx = self - &workers[0];
    size_t half, nlocked;

    if (self->nsessions[0] != 0 || self->nsessions[1] != 0)
        return false;

    if (self_idx + 1 !=
            atomic_load_explicit(&nworkers_running, memory_order_relaxed))
        return false;

    if (pthread_mutex_trylock(&workers_mtx) == EBUSY)
        return false;

    for (nlocked = 0; nlocked < 2; nlocked++) {
        if (pthread_mutex_trylock(&self->mtx[nlocked]) == EBUSY)
            break;
    }

    bool idle = (nlocked == 2 &&
                 self->nsessions[0] == 0 && self->nsessions[1] == 0 &&
                 self_idx + 1 == nworkers_running);

    if (idle) {
        nworkers_running--;
        pthread_cond_signal(&nworkers_cond);
    }

    for (half = 0; half < nlocked; half++)
        (void)pthread_mutex_unlock(&self->mtx[half]);

    (void)pthread_mutex_unlock(&workers_mtx);

    return idle;
}

static void
worker_idle_loop(worker_t *self)
{
    const ptrdiff_t self_idx = self - &workers[0];

    (void)pthread_mutex_lock(&workers_mtx);
    while (nworkers_running <= self_idx && !self->cancelled)
        pthread_cond_wait(&self->sleep, &workers_mtx);
    (void)pthread_mutex_unlock(&workers_mtx);
}

static void *
worker_outer_loop(void *arg)
{
    worker_t *self = arg;

    while (!self->cancelled) {
        worker_idle_loop(self);
        do {
            worker_run_loop(self);
        } while (!worker_is_idle(self) && !self->cancelled);
    }
    return NULL;
}

static void
worker_init(struct fid_domain *dom, worker_t *w)
{
    struct fi_poll_attr attr = {.flags = 0};
    int rc;
    size_t i, buflen;

    w->cancelled = false;

    if ((rc = pthread_cond_init(&w->sleep, NULL)) != 0) {
        errx(EXIT_FAILURE, "%s.%d: pthread_cond_init: %s", __func__, __LINE__,
            strerror(rc));
    }

    for (i = 0; i < arraycount(w->mtx); i++) {
        if ((rc = pthread_mutex_init(&w->mtx[i], NULL)) != 0) {
            errx(EXIT_FAILURE, "%s.%d: pthread_mutex_init: %s",
                __func__, __LINE__, strerror(rc));
        }
        if ((rc = fi_poll_open(dom, &attr, &w->pollset[i])) != 0)
            bailout_for_ofi_ret(rc, "fi_poll_open");
    }
    for (i = 0; i < arraycount(w->session); i++)
        w->session[i] = (session_t){.cxn = NULL, .terminal = NULL};

    buflist_t *bl = malloc(offsetof(buflist_t, buf[256]));
    if (bl == NULL)
        err(EXIT_FAILURE, "%s.%d: malloc", __func__, __LINE__);

    bl->nallocated = 256;
    bl->nfull = bl->nallocated / 2;
    for (buflen = 0, i = 0; i < bl->nfull; i++) {
        buf_t *buf;

        // buflen cycle: 7 -> 256 -> 4096 -> 7 -> ....
        switch (buflen) {
        case 0:
        default:
            buflen = 7;
            break;
        case 7:
            buflen = 256;
            break;
        case 256:
            buflen = 4096;
            break;
        case 4096:
            buflen = 7;
            break;
        }
        buf = malloc(offsetof(buf_t, content[0]) + buflen);
        if (buf == NULL)
            err(EXIT_FAILURE, "%s.%d: malloc", __func__, __LINE__);
        buf->nallocated = buflen;
        buf->nfull = 0;
        bl->buf[i] = buf;
    }
    w->freebufs = bl;
}

static void
worker_launch(worker_t *w)
{
    int rc;

    if ((rc = pthread_create(&w->thd, NULL, worker_outer_loop, w)) != 0) {
            errx(EXIT_FAILURE, "%s.%d: pthread_create: %s",
                __func__, __LINE__, strerror(rc));
    }
}

#if 0
static void
worker_teardown(worker_t *w)
{
    int rc;
    size_t i;

    for (i = 0; i < arraycount(w->mtx); i++) {
        if ((rc = pthread_mutex_destroy(&w->mtx[i])) != 0) {
            errx(EXIT_FAILURE, "%s.%d: pthread_mutex_destroy: %s",
                __func__, __LINE__, strerror(rc));
        }
        if ((rc = fi_close(&w->pollset[i]->fid)) != 0)
            bailout_for_ofi_ret(rc, "fi_close");
    }
    for (i = 0; i < arraycount(w->session); i++)
        assert(w->session[i].cxn == NULL && w->session[i].terminal == NULL);

    if ((rc = pthread_cond_destroy(&w->sleep)) != 0) {
        errx(EXIT_FAILURE, "%s.%d: pthread_cond_destroy: %s",
            __func__, __LINE__, strerror(rc));
    }
    // TBD release buffers and free-buffer reservior.
}
#endif

static worker_t *
worker_create(struct fid_domain *dom)
{
    worker_t *w;

    (void)pthread_mutex_lock(&workers_mtx);
    w = (nworkers_allocated < arraycount(workers))
        ? &workers[nworkers_allocated++]
        : NULL;
    if (w != NULL)
        worker_init(dom, w);
    (void)pthread_mutex_unlock(&workers_mtx);

    if (w != NULL)
        worker_launch(w);

    return w;
}

static void
workers_initialize(void)
{
}

static bool
worker_assign_cxn(worker_t *w, cxn_t *c, struct fid_domain *dom)
{
    cxn_t **cp;
    size_t half, i;
    int rc;

    for (half = 0; half < 2; half++) {
        pthread_mutex_t *mtx = &w->mtx[half];

        if (pthread_mutex_trylock(mtx) == EBUSY)
            continue;

        // find an empty receiver slot
        for (i = 0; i < arraycount(w->session) / 2; i++) {
            cp = &w->session[half * arraycount(w->session) / 2 + i].cxn;
            if (*cp != NULL)
                continue;

            rc = fi_poll_add(w->pollset[half], &c->cq->fid, 0);
            if (rc != 0) {
                warn_about_ofi_ret(rc, "fi_poll_add");
                continue;
            }
            atomic_fetch_add_explicit(&w->nsessions[half], 1,
                memory_order_relaxed);
            *cp = c;
            (void)pthread_mutex_unlock(mtx);
            return true;
        }
        (void)pthread_mutex_unlock(mtx);
    }
    return false;
}

/* Try to allocate `c` to an active worker, least active, first.
 * Caller must hold `workers_mtx`.
 */
static worker_t *
workers_assign_cxn_to_running(cxn_t *c, struct fid_domain *dom)
{
    size_t iplus1;

    for (iplus1 = nworkers_running; 0 < iplus1; iplus1--) {
        size_t i = iplus1 - 1;
        worker_t *w = &workers[i];
        if (worker_assign_cxn(w, c, dom))
            return w;
    }
    return NULL;
}

/* Try to assign `c` to the next idle worker servicing `dom`.
 * Caller must hold `workers_mtx`.
 */
static worker_t *
workers_assign_cxn_to_idle(cxn_t *c, struct fid_domain *dom)
{
    size_t i;

    if ((i = nworkers_running) < nworkers_allocated) {
        worker_t *w = &workers[i];
        if (worker_assign_cxn(w, c, dom))
            return w;
    }
    return NULL;
}

/* Try to wake the first idle worker.
 *
 * Caller must hold `workers_mtx`.
 */
static void
workers_wake(worker_t *w)
{
    assert(&workers[nworkers_running] == w);
    nworkers_running++;
    pthread_cond_signal(&w->sleep);
}

static worker_t *
workers_assign_cxn(cxn_t *c, struct fid_domain *dom)
{
    worker_t *w;

    do {
        (void)pthread_mutex_lock(&workers_mtx);

        if (workers_assignment_suspended) {
            (void)pthread_mutex_unlock(&workers_mtx);
            return NULL;
        }

        if ((w = workers_assign_cxn_to_running(c, dom)) != NULL)
            ;
        else if ((w = workers_assign_cxn_to_idle(c, dom)) != NULL)
            workers_wake(w);
        (void)pthread_mutex_unlock(&workers_mtx);
    } while (w == NULL && (w = worker_create(dom)) != NULL);

    return w;
}

static void
workers_join_all(void)
{
    size_t i;

    (void)pthread_mutex_lock(&workers_mtx);

    workers_assignment_suspended = true;

    while (nworkers_running > 0) {
        pthread_cond_wait(&nworkers_cond, &workers_mtx);
    }

    for (i = 0; i < nworkers_allocated; i++) {
        worker_t *w = &workers[i];
        w->cancelled = true;
        pthread_cond_signal(&w->sleep);
    }

    (void)pthread_mutex_unlock(&workers_mtx);

    for (i = 0; i < nworkers_allocated; i++) {
        worker_t *w = &workers[i];
        int rc;

        if ((rc = pthread_join(w->thd, NULL)) != 0) {
                errx(EXIT_FAILURE, "%s.%d: pthread_join: %s",
                    __func__, __LINE__, strerror(rc));
        }
    }
}

static size_t
fibonacci_iov_setup(void *_buf, size_t len, struct iovec *iov, size_t niovs)
{
    char *buf = _buf;
    ssize_t i;
    struct fibo {
        size_t prev, curr;
    } state = {.prev = 0, .curr = 1}; // Fibonacci sequence state

    if (niovs < 1 && len > 0)
        return -1;

    if (niovs > SSIZE_MAX)
        niovs = SSIZE_MAX;

    for (i = 0; len > 0 && i < niovs - 1; i++) {
        iov[i].iov_len = (state.curr < len) ? state.curr : len;
        iov[i].iov_base = buf;
        len -= iov[i].iov_len;
        buf += iov[i].iov_len;
        state = (struct fibo){.prev = state.curr,
                              .curr = state.prev + state.curr};
    }
    if (len > 0) {
        iov[i].iov_len = len;
        iov[i].iov_base = buf;
        i++;
    }
    return i;
}

/* Register the `niovs`-segment I/O vector `iov` using up to `niovs`
 * of the registrations, descriptors, and remote addresses in the
 * vectors `mrp`, `descp`, and `raddrp`, respectively.  Register no
 * more than `maxsegs` segments in a single `fi_mr_regv` call.
 */
static int
mr_regv_all(struct fid_domain *domain, const struct iovec *iov,
    size_t niovs, size_t maxsegs, uint64_t access, uint64_t offset,
    uint64_t *next_keyp, uint64_t flags, struct fid_mr **mrp,
    void **descp, uint64_t *raddrp, void *context)
{
    int rc;
    size_t i, j, nregs = (niovs + maxsegs - 1) / maxsegs;
    size_t nleftover;
    uint64_t next_key = *next_keyp;

    for (nleftover = niovs, i = 0;
         i < nregs;
         iov += maxsegs, nleftover -= maxsegs, i++) {
        struct fid_mr *mr;
        uint64_t raddr = 0;

        size_t nsegs = minsize(nleftover, maxsegs);

        printf("%zu remaining I/O vectors\n", nleftover);

        rc = fi_mr_regv(domain, iov, nsegs,
            access, offset, next_key++, flags, &mr, context);

        if (rc != 0)
            goto err;

        for (j = 0; j < nsegs; j++) {
            printf("filling descriptor %zu\n", i * maxsegs + j);
            mrp[i * maxsegs + j] = mr;
            descp[i * maxsegs + j] = fi_mr_desc(mr);
            raddrp[i * maxsegs + j] = raddr;
            raddr += iov[j].iov_len;
        }
    }

    *next_keyp = next_key;

    return 0;

err:
    for (j = 0; j < i; j++)
        (void)fi_close(&mrp[j]->fid);

    return rc;
}

static void
cxn_init(cxn_t *c, session_t *(*loop)(worker_t *, session_t *))
{
    memset(c, 0, sizeof(*c));
    c->loop = loop;
}

static void
xmtr_init(xmtr_t *x)
{
    memset(x, 0, sizeof(*x));

    cxn_init(&x->cxn, xmtr_loop);
    x->started = false;
}

static void
rcvr_init(rcvr_t *r)
{
    memset(r, 0, sizeof(*r));

    cxn_init(&r->cxn, rcvr_loop);
    r->started = false;
}

static int
get(state_t *st)
{
    struct fi_cq_attr cq_attr = {
      .size = 128
    , .flags = 0
    , .format = FI_CQ_FORMAT_MSG
    , .wait_obj = FI_WAIT_UNSPEC
    , .signaling_vector = 0
    , .wait_cond = FI_CQ_COND_NONE
    , .wait_set = NULL
    };
    struct fi_eq_attr eq_attr = {
      .size = 128
    , .flags = 0
    , .wait_obj = FI_WAIT_UNSPEC
    , .signaling_vector = 0     /* don't care */
    , .wait_set = NULL          /* don't care */
    };
    struct fi_eq_cm_entry cm_entry;
    get_state_t *gst = &st->u.get;
    rcvr_t *r = &gst->rcvr;
    worker_t *w;
    uint64_t next_key = 0;
    ssize_t i;
    uint32_t event;
    int rc;

    rcvr_init(r);

    r->initial.niovs = fibonacci_iov_setup(&r->initial.msg,
        sizeof(r->initial.msg), r->initial.iov, st->rx_maxsegs);

    if (r->initial.niovs < 1) {
        errx(EXIT_FAILURE, "%s: unexpected I/O vector length %zd",
            __func__, r->initial.niovs);
    }

    r->progress.niovs = fibonacci_iov_setup(&r->progress.msg,
        sizeof(r->progress.msg), r->progress.iov, st->rx_maxsegs);

    if (r->progress.niovs < 1) {
        errx(EXIT_FAILURE, "%s: unexpected I/O vector length %zd",
            __func__, r->progress.niovs);
    }

    r->payload.niovs = fibonacci_iov_setup(r->payload.rxbuf,
        sizeof(r->payload.rxbuf), r->payload.iov, st->rx_maxsegs);

    if (r->payload.niovs < 1) {
        errx(EXIT_FAILURE, "%s: unexpected I/O vector length %zd",
            __func__, r->payload.niovs);
    }

    rc = mr_regv_all(st->domain, r->initial.iov, r->initial.niovs,
        minsize(2, st->mr_maxsegs), FI_RECV, 0, &next_key, 0,
        r->initial.mr, r->initial.desc, r->initial.raddr, NULL);

    if (rc != 0)
        bailout_for_ofi_ret(rc, "mr_regv_all");

    rc = mr_regv_all(st->domain, r->progress.iov, r->progress.niovs,
        minsize(2, st->mr_maxsegs), FI_RECV, 0, &next_key, 0,
        r->progress.mr, r->progress.desc, r->progress.raddr, NULL);

    if (rc != 0)
        bailout_for_ofi_ret(rc, "mr_regv_all");

    rc = mr_regv_all(st->domain, r->payload.iov, r->payload.niovs,
        minsize(2, st->mr_maxsegs), FI_REMOTE_WRITE, 0, &next_key, 0,
        r->payload.mr, r->payload.desc, r->payload.raddr, NULL);

    if (rc != 0)
        bailout_for_ofi_ret(rc, "mr_regv_all");

    r->vector.msg.niovs = r->payload.niovs;
    for (i = 0; i < r->payload.niovs; i++) {
        printf("payload.iov[%zd].iov_len = %zu\n", i,
            r->payload.iov[i].iov_len);
        r->vector.msg.iov[i].addr = r->payload.raddr[i];
        r->vector.msg.iov[i].len = r->payload.iov[i].iov_len;
        r->vector.msg.iov[i].key = fi_mr_key(r->payload.mr[i]);
    }

    r->vector.niovs = fibonacci_iov_setup(&r->vector.msg,
        (char *)&r->vector.msg.iov[r->vector.msg.niovs] -
        (char *)&r->vector.msg,
        r->vector.iov, st->rx_maxsegs);

    if (r->vector.niovs < 1) {
        errx(EXIT_FAILURE, "%s: unexpected I/O vector length %zd",
            __func__, r->vector.niovs);
    }

    rc = mr_regv_all(st->domain, r->vector.iov, r->vector.niovs,
        minsize(2, st->mr_maxsegs), FI_SEND, 0, &next_key, 0,
        r->vector.mr, r->vector.desc, r->vector.raddr, NULL);

    if (rc != 0)
        bailout_for_ofi_ret(rc, "mr_regv_all");

    rc = fi_passive_ep(st->fabric, st->info, &gst->pep, NULL);

    if (rc != 0)
        bailout_for_ofi_ret(rc, "fi_passive_ep");

    rc = fi_eq_open(st->fabric, &eq_attr, &gst->listen_eq, NULL);

    if (rc != 0)
        bailout_for_ofi_ret(rc, "fi_eq_open (listen)");

    rc = fi_eq_open(st->fabric, &eq_attr, &r->active_eq, NULL);

    if (rc != 0)
        bailout_for_ofi_ret(rc, "fi_eq_open (active)");

    rc = fi_pep_bind(gst->pep, &gst->listen_eq->fid, 0);

    if (rc != 0)
        bailout_for_ofi_ret(rc, "fi_pep_bind");

    rc = fi_listen(gst->pep);

    if (rc != 0)
        bailout_for_ofi_ret(rc, "fi_listen");

    do {
        rc = fi_eq_sread(gst->listen_eq, &event, &cm_entry, sizeof(cm_entry),
            -1 /* wait forever */, 0 /* flags */ );
    } while (rc == -FI_EAGAIN);

#if 0
    if (rc == -FI_EINTR)
        errx(EXIT_FAILURE, "%s: fi_eq_sread: interrupted", __func__);
#endif

    if (rc < 0)
        bailout_for_ofi_ret(rc, "fi_eq_sread");

    if (event != FI_CONNREQ) {
        errx(EXIT_FAILURE,
            "%s: expected connreq event (%" PRIu32 "), received %" PRIu32,
            __func__, FI_CONNREQ, event);
    }

    rc = fi_endpoint(st->domain, cm_entry.info, &r->aep, NULL);

    if (rc < 0)
        bailout_for_ofi_ret(rc, "fi_endpoint");

    rc = fi_ep_bind(r->aep, &r->active_eq->fid, 0);

    if (rc < 0)
        bailout_for_ofi_ret(rc, "fi_ep_bind");

    rc = fi_cq_open(st->domain, &cq_attr, &r->cxn.cq, NULL);

    if (rc != 0)
        bailout_for_ofi_ret(rc, "fi_cq_open");

    rc = fi_ep_bind(r->aep, &r->cxn.cq->fid,
        FI_SELECTIVE_COMPLETION | FI_RECV | FI_TRANSMIT);

    if (rc != 0)
        bailout_for_ofi_ret(rc, "fi_ep_bind");

    rc = fi_enable(r->aep);

    if (rc != 0)
        bailout_for_ofi_ret(rc, "fi_enable");

    rc = fi_recvmsg(r->aep, &(struct fi_msg){
          .msg_iov = r->initial.iov
        , .desc = r->initial.desc
        , .iov_count = r->initial.niovs
        , .addr = 0
        , .context = NULL
        , .data = 0
        }, FI_COMPLETION);

    if (rc < 0)
        bailout_for_ofi_ret(rc, "fi_recvmsg");

    rc = fi_recvmsg(r->aep, &(struct fi_msg){
          .msg_iov = r->progress.iov
        , .desc = r->progress.desc
        , .iov_count = r->progress.niovs
        , .addr = 0
        , .context = NULL
        , .data = 0
        }, FI_COMPLETION);

    if (rc < 0)
        bailout_for_ofi_ret(rc, "fi_recvmsg");

    rc = fi_accept(r->aep, NULL, 0);

    if (rc < 0)
        bailout_for_ofi_ret(rc, "fi_accept");

    fi_freeinfo(cm_entry.info);

    do {
        rc = fi_eq_sread(r->active_eq, &event, &cm_entry, sizeof(cm_entry),
            -1 /* wait forever */, 0 /* flags */ );
    } while (rc == -FI_EAGAIN);

    if (rc < 0)
        bailout_for_ofi_ret(rc, "fi_eq_sread");

    if (event != FI_CONNECTED) {
        errx(EXIT_FAILURE,
            "%s: expected connected event (%" PRIu32 "), received %" PRIu32,
            __func__, FI_CONNECTED, event);
    }

    if ((w = workers_assign_cxn(&r->cxn, st->domain)) == NULL) {
        errx(EXIT_FAILURE, "%s: could not assign a new receiver to a worker",
            __func__);
    }

    workers_join_all();

    return EXIT_SUCCESS;
}

static int
put(state_t *st)
{
    struct fi_cq_attr cq_attr = {
      .size = 128
    , .flags = 0
    , .format = FI_CQ_FORMAT_MSG
    , .wait_obj = FI_WAIT_UNSPEC
    , .signaling_vector = 0
    , .wait_cond = FI_CQ_COND_NONE
    , .wait_set = NULL
    };
    struct fi_eq_attr eq_attr = {
      .size = 128
    , .flags = 0
    , .wait_obj = FI_WAIT_UNSPEC
    , .signaling_vector = 0     /* don't care */
    , .wait_set = NULL          /* don't care */
    };
    struct fi_eq_cm_entry cm_entry;
    put_state_t *pst = &st->u.put;
    xmtr_t *x = &pst->xmtr;
    worker_t *w;
    uint64_t next_key = 0;
    uint32_t event;
    int rc;
    const size_t txbuflen = strlen(txbuf);

    xmtr_init(x);

    rc = fi_mr_reg(st->domain, &x->initial.msg, sizeof(x->initial.msg),
        FI_SEND, 0, next_key++, 0, &x->initial.mr, NULL);

    if (rc != 0)
        bailout_for_ofi_ret(rc, "fi_mr_reg");

    rc = fi_mr_reg(st->domain, &x->vector.msg, sizeof(x->vector.msg),
        FI_RECV, 0, next_key++, 0, &x->vector.mr, NULL);

    if (rc != 0)
        bailout_for_ofi_ret(rc, "fi_mr_reg");

    rc = fi_mr_reg(st->domain, &x->progress.msg, sizeof(x->progress.msg),
        FI_SEND, 0, next_key++, 0, &x->progress.mr, NULL);

    if (rc != 0)
        bailout_for_ofi_ret(rc, "fi_mr_reg");

    rc = fi_mr_reg(st->domain, txbuf, txbuflen,
        FI_WRITE, 0, next_key++, 0, x->payload.mr, NULL);

    if (rc != 0)
        bailout_for_ofi_ret(rc, "fi_mr_reg");

    rc = fi_endpoint(st->domain, st->info, &x->ep, NULL);

    if (rc != 0)
        bailout_for_ofi_ret(rc, "fi_endpoint");

    rc = fi_cq_open(st->domain, &cq_attr, &x->cxn.cq, NULL);

    if (rc != 0)
        bailout_for_ofi_ret(rc, "fi_cq_open");

    rc = fi_eq_open(st->fabric, &eq_attr, &pst->connect_eq, NULL);

    if (rc != 0)
        bailout_for_ofi_ret(rc, "fi_eq_open");

    rc = fi_ep_bind(x->ep, &pst->connect_eq->fid, 0);

    if (rc != 0)
        bailout_for_ofi_ret(rc, "fi_ep_bind");

    rc = fi_ep_bind(x->ep, &x->cxn.cq->fid,
        FI_SELECTIVE_COMPLETION | FI_RECV | FI_TRANSMIT);

    if (rc != 0)
        bailout_for_ofi_ret(rc, "fi_ep_bind");

    rc = fi_enable(x->ep);

    if (rc != 0)
        bailout_for_ofi_ret(rc, "fi_enable");

    rc = fi_connect(x->ep, st->info->dest_addr, NULL, 0);

    if (rc != 0)
        bailout_for_ofi_ret(rc, "fi_connect dest_addr %p", st->info->dest_addr);

    do {
        rc = fi_eq_sread(pst->connect_eq, &event, &cm_entry, sizeof(cm_entry),
            -1 /* wait forever */, 0 /* flags */ );
    } while (rc == -FI_EAGAIN);

    if (rc < 0)
        bailout_for_ofi_ret(rc, "fi_eq_sread");

    if (event != FI_CONNECTED) {
        errx(EXIT_FAILURE,
            "%s: expected connected event (%" PRIu32 "), received %" PRIu32,
            __func__, FI_CONNECTED, event);
    }

    if ((w = workers_assign_cxn(&x->cxn, st->domain)) == NULL) {
        errx(EXIT_FAILURE, "%s: could not assign a new transmitter to a worker",
            __func__);
    }

    workers_join_all();

    return EXIT_SUCCESS;
}

static int
count_info(const struct fi_info *first)
{
    int count;
    const struct fi_info *info;

    for (info = first, count = 1; (info = info->next) != NULL; count++)
        ;

    return count;
}

static const char *
personality_to_name(personality_t p)
{
    if (p == get)
        return "fget";
    else if (p == put)
        return "fput";
    else
        return "unknown";
}

int
main(int argc, char **argv)
{
    struct fi_info *hints;
    personality_t personality;
    char *prog, *tmp;
    state_t st;
    int rc;

    if ((tmp = strdup(argv[0])) == NULL)
        err(EXIT_FAILURE, "%s: strdup", __func__);

    prog = basename(tmp);

    if (strcmp(prog, "fget") == 0)
        personality = get;
    else if (strcmp(prog, "fput") == 0)
        personality = put;
    else
        errx(EXIT_FAILURE, "program personality '%s' is not implemented", prog);

    workers_initialize();

    printf("%ld POSIX I/O vector items maximum\n", sysconf(_SC_IOV_MAX));

    if ((hints = fi_allocinfo()) == NULL)
        errx(EXIT_FAILURE, "%s: fi_allocinfo", __func__);

    hints->ep_attr->type = FI_EP_MSG;
    hints->caps = FI_FENCE | FI_MSG | FI_RMA | FI_REMOTE_WRITE | FI_WRITE;
    hints->mode = FI_CONTEXT;
    hints->domain_attr->mr_mode = FI_MR_PROV_KEY;

    rc = fi_getinfo(FI_VERSION(1, 13), "10.10.10.120" /* -b */,
        fget_fput_service_name, (personality == get) ? FI_SOURCE : 0, hints,
        &st.info);

    fi_freeinfo(hints);

    switch (-rc) {
    case FI_ENODATA:
        warnx("capabilities not available?");
        break;
    case FI_ENOSYS:
        warnx("available libfabric version < 1.13?");
        break;
    default:
        break;
    }

    if (rc != 0)
        bailout_for_ofi_ret(rc, "fi_getinfo");

    printf("%d infos found\n", count_info(st.info));

    rc = fi_fabric(st.info->fabric_attr, &st.fabric, NULL /* app context */);

    if (rc != 0)
        bailout_for_ofi_ret(rc, "fi_fabric");

    rc = fi_domain(st.fabric, st.info, &st.domain, NULL);

    printf("provider %s, memory-registration I/O vector limit %zu\n",
        st.info->fabric_attr->prov_name,
        st.info->domain_attr->mr_iov_limit);

    printf("provider %s %s application-requested memory-registration keys\n",
        st.info->fabric_attr->prov_name,
        ((st.info->domain_attr->mr_mode & FI_MR_PROV_KEY) != 0)
            ? "does not support"
            : "supports");

    if ((st.info->domain_attr->mr_mode & FI_MR_VIRT_ADDR) != 0) {
        printf("provider %s RDMA uses virtual addresses instead of offsets, "
            "quitting.\n",
            st.info->fabric_attr->prov_name);
        exit(EXIT_FAILURE);
    }

    printf("Rx/Tx I/O vector limits %zu/%zu\n",
        st.info->rx_attr->iov_limit, st.info->tx_attr->iov_limit);

    printf("RMA I/O vector limit %zu\n", st.info->tx_attr->rma_iov_limit);

    st.mr_maxsegs = 1; // st.info->domain_attr->mr_iov_limit;
    st.rx_maxsegs = st.info->rx_attr->iov_limit;
    st.tx_maxsegs = st.info->tx_attr->iov_limit;
    st.rma_maxsegs = st.info->tx_attr->rma_iov_limit;

#if 0
    printf("maximum endpoint message size (RMA limit) %zu\n",
        st.info->ep_attr->max_msg_size);
#endif

    if (rc != 0)
        bailout_for_ofi_ret(rc, "fi_domain");

    printf("starting personality '%s'\n", personality_to_name(personality));

    return (*personality)(&st);
}
