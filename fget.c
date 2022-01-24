#include <err.h>
#include <libgen.h> /* basename(3) */
#include <limits.h> /* INT_MAX */
#include <inttypes.h>   /* PRIu32 */
#include <stdarg.h>
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

typedef struct {
    struct fid_ep *aep;
    struct fid_eq *listen_eq;
    struct fid_eq *active_eq;
    struct fid_pep *pep;
    struct fid_cq *cq;
} get_state_t;

typedef struct {
    struct fid_ep *ep;
    struct fid_eq *connect_eq;
    struct fid_cq *cq;
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

static const uint64_t desired_rx_flags = FI_RECV | FI_MSG;
static const uint64_t desired_tx_flags = FI_SEND | FI_MSG;

static char txbuf[] =
    "If this message was received in error then please "
    "print it out and shred it.";

#define bailout_for_ofi_ret(ret, ...)                          \
        bailout_for_ofi_ret_impl(ret, __func__, __LINE__, __VA_ARGS__)

static void
bailout_for_ofi_ret_impl(int ret, const char *fn, int lineno,
    const char *fmt, ...)
{
    va_list ap;

    fprintf(stderr, "%s.%d: ", fn, lineno);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, ": %s\n", fi_strerror(-ret));
    exit(EXIT_FAILURE);
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

static int
get(state_t *st)
{
    /* completion fields:
     *
     * void     *op_context;
     * uint64_t flags;
     * size_t   len;
     */
    struct fi_cq_msg_entry completion;
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
    struct fi_msg msg;
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
    get_state_t *gst = &st->u.get;
    uint64_t next_key = 0;
    ssize_t i, ncompleted;
    uint32_t event;
    int rc;

    initial.niovs = fibonacci_iov_setup(&initial.msg, sizeof(initial.msg),
        initial.iov, st->rx_maxsegs);

    if (initial.niovs < 1) {
        errx(EXIT_FAILURE, "%s: unexpected I/O vector length %zd",
            __func__, initial.niovs);
    }

    progress.niovs = fibonacci_iov_setup(&progress.msg, sizeof(progress.msg),
        progress.iov, st->rx_maxsegs);

    if (progress.niovs < 1) {
        errx(EXIT_FAILURE, "%s: unexpected I/O vector length %zd",
            __func__, progress.niovs);
    }

    payload.niovs = fibonacci_iov_setup(payload.rxbuf, sizeof(payload.rxbuf),
        payload.iov, st->rx_maxsegs);

    if (payload.niovs < 1) {
        errx(EXIT_FAILURE, "%s: unexpected I/O vector length %zd",
            __func__, payload.niovs);
    }

    rc = mr_regv_all(st->domain, initial.iov, initial.niovs,
        minsize(2, st->mr_maxsegs), FI_RECV, 0, &next_key, 0,
        initial.mr, initial.desc, initial.raddr, NULL);

    if (rc != 0)
        bailout_for_ofi_ret(rc, "mr_regv_all");

    rc = mr_regv_all(st->domain, progress.iov, progress.niovs,
        minsize(2, st->mr_maxsegs), FI_RECV, 0, &next_key, 0,
        progress.mr, progress.desc, progress.raddr, NULL);

    if (rc != 0)
        bailout_for_ofi_ret(rc, "mr_regv_all");

    rc = mr_regv_all(st->domain, payload.iov, payload.niovs,
        minsize(2, st->mr_maxsegs), FI_REMOTE_WRITE, 0, &next_key, 0,
        payload.mr, payload.desc, payload.raddr, NULL);

    if (rc != 0)
        bailout_for_ofi_ret(rc, "mr_regv_all");

    vector.msg.niovs = payload.niovs;
    for (i = 0; i < payload.niovs; i++) {
        printf("payload.iov[%zd].iov_len = %zu\n", i, payload.iov[i].iov_len);
        vector.msg.iov[i].addr = payload.raddr[i];
        vector.msg.iov[i].len = payload.iov[i].iov_len;
        vector.msg.iov[i].key = fi_mr_key(payload.mr[i]);
    }

    vector.niovs = fibonacci_iov_setup(&vector.msg,
        (char *)&vector.msg.iov[vector.msg.niovs] - (char *)&vector.msg,
        vector.iov, st->rx_maxsegs);

    if (vector.niovs < 1) {
        errx(EXIT_FAILURE, "%s: unexpected I/O vector length %zd",
            __func__, vector.niovs);
    }

    rc = mr_regv_all(st->domain, vector.iov, vector.niovs,
        minsize(2, st->mr_maxsegs), FI_SEND, 0, &next_key, 0,
        vector.mr, vector.desc, vector.raddr, NULL);

    if (rc != 0)
        bailout_for_ofi_ret(rc, "mr_regv_all");

    rc = fi_passive_ep(st->fabric, st->info, &gst->pep, NULL);

    if (rc != 0)
        bailout_for_ofi_ret(rc, "fi_passive_ep");

    rc = fi_eq_open(st->fabric, &eq_attr, &gst->listen_eq, NULL);

    if (rc != 0)
        bailout_for_ofi_ret(rc, "fi_eq_open (listen)");

    rc = fi_eq_open(st->fabric, &eq_attr, &gst->active_eq, NULL);

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

    rc = fi_endpoint(st->domain, cm_entry.info, &gst->aep, NULL);

    if (rc < 0)
        bailout_for_ofi_ret(rc, "fi_endpoint");

    rc = fi_ep_bind(gst->aep, &gst->active_eq->fid, 0);

    if (rc < 0)
        bailout_for_ofi_ret(rc, "fi_ep_bind");

    rc = fi_cq_open(st->domain, &cq_attr, &gst->cq, NULL);

    if (rc != 0)
        bailout_for_ofi_ret(rc, "fi_cq_open");

    rc = fi_ep_bind(gst->aep, &gst->cq->fid,
        FI_SELECTIVE_COMPLETION | FI_RECV | FI_TRANSMIT);

    if (rc != 0)
        bailout_for_ofi_ret(rc, "fi_ep_bind");

    rc = fi_enable(gst->aep);

    if (rc != 0)
        bailout_for_ofi_ret(rc, "fi_enable");

    msg = (struct fi_msg){
      .msg_iov = initial.iov
    , .desc = initial.desc
    , .iov_count = initial.niovs
    , .addr = 0
    , .context = NULL
    , .data = 0
    };

    rc = fi_recvmsg(gst->aep, &msg, FI_COMPLETION);

    if (rc < 0)
        bailout_for_ofi_ret(rc, "fi_recvmsg");

    msg = (struct fi_msg){
      .msg_iov = progress.iov
    , .desc = progress.desc
    , .iov_count = progress.niovs
    , .addr = 0
    , .context = NULL
    , .data = 0
    };

    rc = fi_recvmsg(gst->aep, &msg, FI_COMPLETION);

    if (rc < 0)
        bailout_for_ofi_ret(rc, "fi_recvmsg");

    rc = fi_accept(gst->aep, NULL, 0);

    if (rc < 0)
        bailout_for_ofi_ret(rc, "fi_accept");

    fi_freeinfo(cm_entry.info);

    do {
        rc = fi_eq_sread(gst->active_eq, &event, &cm_entry, sizeof(cm_entry),
            -1 /* wait forever */, 0 /* flags */ );
    } while (rc == -FI_EAGAIN);

    if (rc < 0)
        bailout_for_ofi_ret(rc, "fi_eq_sread");

    if (event != FI_CONNECTED) {
        errx(EXIT_FAILURE,
            "%s: expected connected event (%" PRIu32 "), received %" PRIu32,
            __func__, FI_CONNECTED, event);
    }

    /* Await initial message. */
    do {
        ncompleted = fi_cq_sread(gst->cq, &completion, 1, NULL, -1);
    } while (rc == -FI_EAGAIN);

    if (ncompleted < 0)
        bailout_for_ofi_ret(rc, "fi_cq_sread");

    if (ncompleted != 1) {
        errx(EXIT_FAILURE,
            "%s: expected 1 completion, read %zd", __func__, ncompleted);
    }

    if ((completion.flags & desired_rx_flags) != desired_rx_flags) {
        errx(EXIT_FAILURE,
            "%s: expected flags %" PRIu64 ", received flags %" PRIu64,
            __func__, desired_rx_flags, completion.flags & desired_rx_flags);
    }

    if (completion.len != sizeof(initial.msg)) {
        errx(EXIT_FAILURE,
            "initially received %zu bytes, expected %zu\n", completion.len,
            sizeof(initial.msg));
    }

    if (initial.msg.nsources != 1 || initial.msg.id != 0) {
        errx(EXIT_FAILURE,
            "received nsources %" PRIu32 ", id %" PRIu32 ", expected 1, 0\n",
            initial.msg.nsources, 
            initial.msg.id);
    }

    /* Transmit vector. */

    msg = (struct fi_msg){
      .msg_iov = vector.iov
    , .desc = vector.desc
    , .iov_count = vector.niovs
    , .addr = 0
    , .context = NULL
    , .data = 0
    };

    rc = fi_sendmsg(gst->aep, &msg, 0);

    if (rc < 0)
        bailout_for_ofi_ret(rc, "fi_sendmsg");

    /* Await progress message. */
    do {
        printf("%s: awaiting progress message\n", __func__);
        ncompleted = fi_cq_sread(gst->cq, &completion, 1, NULL, -1);

        if (ncompleted == -FI_EAVAIL) {
            struct fi_cq_err_entry e;
            ssize_t nfailed = fi_cq_readerr(gst->cq, &e, 0);
#if 0
                  void     *op_context; /* operation context */
                  uint64_t flags;       /* completion flags */
                  size_t   len;         /* size of received data */
                  void     *buf;        /* receive data buffer */
                  uint64_t data;        /* completion data */
                  uint64_t tag;         /* message tag */
                  size_t   olen;        /* overflow length */
                  int      err;         /* positive error code */
                  int      prov_errno;  /* provider error code */
                  void    *err_data;    /*  error data */
                  size_t   err_data_size; /* size of err_data */
$1 = {op_context = 0x0, flags = 16778242, len = 296, buf = 0x0, data = 0, 
  tag = 0, olen = 280, err = 265, prov_errno = -265, err_data = 0x0, 
  err_data_size = 140215255555464}
#endif

            warnx("%s: read %zd errors, %s", __func__, nfailed,
                fi_strerror(e.err));
            warnx("%s: completion flags %" PRIx64 " expected %" PRIx64,
                __func__, e.flags, desired_rx_flags);
            abort();
        }
    } while (rc == -FI_EAGAIN);

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

    if (completion.len != sizeof(progress.msg)) {
        errx(EXIT_FAILURE,
            "received %zu bytes, expected %zu-byte progress\n", completion.len,
            sizeof(progress.msg));
    }

    if (progress.msg.nfilled != strlen(txbuf)) {
        errx(EXIT_FAILURE,
            "progress: %" PRIu64 " bytes filled, expected %" PRIu64 "\n",
            progress.msg.nfilled, 
            strlen(txbuf));
    }

    if (progress.msg.nleftover != 0) {
        errx(EXIT_FAILURE,
            "progress: %" PRIu64 " bytes leftover, expected 0\n",
            progress.msg.nleftover);
    }

    /* Verify received payload. */
    printf("%zu bytes filled\n", progress.msg.nfilled);

    if (strlen(txbuf) != progress.msg.nfilled)
        errx(EXIT_FAILURE, "unexpected received message length");

    if (strncmp(txbuf, payload.rxbuf, progress.msg.nfilled) != 0)
        errx(EXIT_FAILURE, "unexpected received message content");

    return EXIT_SUCCESS;
}

static int
put(state_t *st)
{
    /* completion fields:
     *
     * void     *op_context;
     * uint64_t flags;
     * size_t   len;
     */
    struct fi_cq_msg_entry completion;
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
    struct fi_msg msg;
    struct {
        struct iovec iov[12];
        void *desc[12];
        struct fid_mr *mr[12];
        ssize_t niovs;
        initial_msg_t msg;
    } initial;
    struct {
        struct iovec iov[12];
        void *desc[12];
        struct fid_mr *mr[12];
        ssize_t niovs;
        vector_msg_t msg;
    } vector;
    struct {
        struct iovec iov[12];
        void *desc[12];
        struct fid_mr *mr[12];
        ssize_t niovs;
        progress_msg_t msg;
    } progress;
    put_state_t *pst = &st->u.put;
    struct {
        struct iovec iov[12];
        void *desc[12];
        struct fid_mr *mr[12];
        ssize_t niovs;
    } payload;
    struct fi_rma_iov riov[12];
    uint64_t next_key = 0;
    ssize_t ncompleted;
    uint32_t event;
    size_t i;
    int rc;

    rc = fi_mr_reg(st->domain, &initial.msg, sizeof(initial.msg), FI_SEND,
        0, next_key++, 0, initial.mr, NULL);

    if (rc != 0)
        bailout_for_ofi_ret(rc, "fi_mr_reg");

    rc = fi_mr_reg(st->domain, &vector.msg, sizeof(vector.msg), FI_RECV,
        0, next_key++, 0, vector.mr, NULL);

    if (rc != 0)
        bailout_for_ofi_ret(rc, "fi_mr_reg");

    rc = fi_mr_reg(st->domain, &progress.msg, sizeof(progress.msg), FI_SEND,
        0, next_key++, 0, progress.mr, NULL);

    if (rc != 0)
        bailout_for_ofi_ret(rc, "fi_mr_reg");

    rc = fi_mr_reg(st->domain, txbuf, strlen(txbuf), FI_WRITE,
        0, next_key++, 0, payload.mr, NULL);

    if (rc != 0)
        bailout_for_ofi_ret(rc, "fi_mr_reg");

    rc = fi_endpoint(st->domain, st->info, &pst->ep, NULL);

    if (rc != 0)
        bailout_for_ofi_ret(rc, "fi_endpoint");

    rc = fi_cq_open(st->domain, &cq_attr, &pst->cq, NULL);

    if (rc != 0)
        bailout_for_ofi_ret(rc, "fi_cq_open");

    rc = fi_eq_open(st->fabric, &eq_attr, &pst->connect_eq, NULL);

    if (rc != 0)
        bailout_for_ofi_ret(rc, "fi_eq_open");

    rc = fi_ep_bind(pst->ep, &pst->connect_eq->fid, 0);

    if (rc != 0)
        bailout_for_ofi_ret(rc, "fi_ep_bind");

    rc = fi_ep_bind(pst->ep, &pst->cq->fid,
        FI_SELECTIVE_COMPLETION | FI_RECV | FI_TRANSMIT);

    if (rc != 0)
        bailout_for_ofi_ret(rc, "fi_ep_bind");

    rc = fi_enable(pst->ep);

    if (rc != 0)
        bailout_for_ofi_ret(rc, "fi_enable");

    rc = fi_connect(pst->ep, st->info->dest_addr, NULL, 0);

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

    /* Post receive for first vector message. */
    vector.iov[0] = (struct iovec){.iov_base = &vector.msg,
                                   .iov_len = sizeof(vector.msg)};
    vector.desc[0] = fi_mr_desc(vector.mr[0]);

    msg = (struct fi_msg){
      .msg_iov = vector.iov
    , .desc = vector.desc
    , .iov_count = 1
    , .addr = 0
    , .context = NULL
    , .data = 0
    };

    rc = fi_recvmsg(pst->ep, &msg, FI_COMPLETION);

    if (rc < 0)
        bailout_for_ofi_ret(rc, "fi_recvmsg");

    /* Setup & transmit initial message. */
    memset(&initial.msg, 0, sizeof(initial.msg));
    initial.msg.nsources = 1;
    initial.msg.id = 0;

    initial.iov[0] = (struct iovec){.iov_base = &initial.msg,
                                    .iov_len = sizeof(initial.msg)};
    initial.desc[0] = fi_mr_desc(initial.mr[0]);

    msg = (struct fi_msg){
      .msg_iov = initial.iov
    , .desc = initial.desc
    , .iov_count = 1
    , .addr = 0
    , .context = NULL
    , .data = 0
    };

    rc = fi_sendmsg(pst->ep, &msg, 0);

    if (rc < 0)
        bailout_for_ofi_ret(rc, "fi_sendmsg");

    /* Await reply to initial message: first vector message. */
    do {
        ncompleted = fi_cq_sread(pst->cq, &completion, 1, NULL, -1);
    } while (rc == -FI_EAGAIN);

    if (ncompleted < 0)
        bailout_for_ofi_ret(rc, "fi_cq_sread");

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
        (char *)&vector.msg.iov[0] - (char *)&vector.msg;

    if (completion.len < least_vector_msglen) {
        errx(EXIT_FAILURE, "%s: expected >= %zu bytes, received %zu",
            __func__, least_vector_msglen, completion.len);
    }

    if (completion.len == least_vector_msglen) {
        errx(EXIT_SUCCESS, "%s: peer sent 0 vectors, disconnecting...",
            __func__);
    }

    if ((completion.len - least_vector_msglen) %
        sizeof(vector.msg.iov[0]) != 0) {
        errx(EXIT_SUCCESS,
            "%s: %zu-byte vector message did not end on vector boundary, "
            "disconnecting...", __func__, completion.len);
    }

    const size_t niovs_space = (completion.len - least_vector_msglen) /
        sizeof(vector.msg.iov[0]);

    if (niovs_space < vector.msg.niovs) {
        errx(EXIT_SUCCESS, "%s: peer sent truncated vectors, disconnecting...",
            __func__);
    }

    if (vector.msg.niovs > arraycount(riov)) {
        errx(EXIT_SUCCESS, "%s: peer sent too many vectors, disconnecting...",
            __func__);
    }

    payload.iov[0] = (struct iovec){.iov_base = txbuf,
                                    .iov_len = strlen(txbuf)};
    payload.desc[0] = fi_mr_desc(payload.mr[0]);

    for (i = 0; i < vector.msg.niovs; i++) {
        printf("%s: received vector %zd "
            "addr %" PRIu64 " len %" PRIu64 " key %" PRIx64 "\n",
            __func__, i, vector.msg.iov[i].addr, vector.msg.iov[i].len,
            vector.msg.iov[i].key);
        riov[i].len = vector.msg.iov[i].len;
        riov[i].addr = vector.msg.iov[i].addr;
        riov[i].key = vector.msg.iov[i].key;
    }

#if 0
    struct fi_msg_rma mrma;
    mrma.msg_iov = payload.iov;
    mrma.desc = payload.desc;
    mrma.iov_count = 1;
    mrma.addr = 0;
    mrma.rma_iov = riov;
    mrma.rma_iov_count = vector.msg.niovs;
    mrma.context = NULL;
    mrma.data = 0;

    rc = fi_writemsg(pst->ep, &mrma, FI_COMPLETION | FI_DELIVERY_COMPLETE);

    if (rc != 0)
        bailout_for_ofi_ret(rc, "fi_writemsg");

    /* Await RDMA completion. */
    do {
        printf("%s: awaiting RMA completion.\n", __func__);
        ncompleted = fi_cq_sread(pst->cq, &completion, 1, NULL, -1);
    } while (rc == -FI_EAGAIN);

    if (ncompleted < 0)
        bailout_for_ofi_ret(rc, "fi_cq_sread");

    if (ncompleted != 1) {
        errx(EXIT_FAILURE,
            "%s: expected 1 completion, read %zd", __func__, ncompleted);
    }
#else

    const size_t txbuflen = strlen(txbuf);
    size_t nwritten = 0;
    for (i = 0; i < vector.msg.niovs && nwritten < txbuflen; i++) {
        rc = fi_write(pst->ep, txbuf + nwritten,
            minsize(riov[i].len, txbuflen - nwritten),
            fi_mr_desc(payload.mr[0]), 0, riov[i].addr, riov[i].key, NULL);
        if (rc != 0)
            bailout_for_ofi_ret(rc, "fi_write");
        nwritten += minsize(riov[i].len, txbuflen - nwritten);
    }

#endif

    progress.msg.nfilled = strlen(txbuf);
    progress.msg.nleftover = 0;

    progress.iov[0] = (struct iovec){.iov_base = &progress.msg,
                                    .iov_len = sizeof(progress.msg)};
    progress.desc[0] = fi_mr_desc(progress.mr[0]);

    msg = (struct fi_msg){
      .msg_iov = progress.iov
    , .desc = progress.desc
    , .iov_count = 1
    , .addr = 0
    , .context = NULL
    , .data = 0
    };

    rc = fi_sendmsg(pst->ep, &msg, FI_FENCE | FI_COMPLETION);

    /* Await transmission of progress message. */
    do {
        printf("%s: awaiting progress message transmission.\n", __func__);
        ncompleted = fi_cq_sread(pst->cq, &completion, 1, NULL, -1);
    } while (rc == -FI_EAGAIN);

    if (ncompleted < 0)
        bailout_for_ofi_ret(rc, "fi_cq_sread");

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
        sizeof(progress.msg));

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
