/*
 * RPC server-side support
 */

#include <stdio.h>

#include <base/atomic.h>
#include <base/stddef.h>
#include <base/time.h>
#include <base/list.h>
#include <base/log.h>
#include <runtime/tcp.h>
#include <runtime/sync.h>
#include <runtime/smalloc.h>
#include <runtime/thread.h>
#include <runtime/timer.h>
#include <runtime/runtime.h>

#include <breakwater/pcc.h>

#include "util.h"
#include "pcc_proto.h"
#include "pcc_config.h"

/* PCC controller-specific definitions */
#define SPCC_DEBUG              0

/* Helper to print debug logs */
#if SPCC_DEBUG == 1
#define SPCC_DEBUG_LOG(...) printf(__VA_ARGS__)
#else
#define SPCC_DEBUG_LOG(...)
#endif

/* time-series output */
#define SPCC_TS_OUT         false
#define TS_BUF_SIZE_EXP     10
#define TS_BUF_SIZE         (1 << TS_BUF_SIZE_EXP)
#define TS_BUF_MASK         (TS_BUF_SIZE - 1)

#define SPCC_TRACK_FLOW     false
#define SPCC_TRACK_FLOW_ID  1

#define EWMA_WEIGHT     0.1f

BUILD_ASSERT((1 << SPCC_MAX_WINDOW_EXP) == SPCC_MAX_WINDOW);

#if SPCC_TS_OUT
int nextIndex = 0;
FILE *ts_out = NULL;

struct Event {
    uint64_t timestamp;
    int      credit_pool;
    int      credit_used;
    int      num_pending;
    int      num_drained;
    int      num_active;
    int      num_sess;
    uint64_t delay;
    int      num_cores;
    uint64_t avg_st;
};

static struct Event events[TS_BUF_SIZE];
#endif

/* the handler function for each RPC */
static srpc_fn_t srpc_handler;

/* total number of session */
static atomic_t srpc_num_sess;

/* the number of drained session */
static atomic_t srpc_num_drained;

/* the number of active sessions */
static atomic_t srpc_num_active;

/* global credit pool */
static atomic_t srpc_credit_pool;

/* global credit used */
static atomic_t srpc_credit_used;

/* downstream credit for multi-hierarchy */
static atomic_t srpc_credit_ds;

/* the number of pending requests */
static atomic_t srpc_num_pending;

/* EWMA execution time */
static atomic_t srpc_avg_st;

/* drained session list */
struct srpc_drained_ {
    spinlock_t lock;
    // high priority sessions (demand > 0)
    // LIFO queue
    struct list_head list_h;
    // low priority sessions (demand == 0)
    // FIFO queue
    struct list_head list_l;
    void             *pad[3];
};

BUILD_ASSERT(sizeof(struct srpc_drained_) == CACHE_LINE_SIZE);

static struct srpc_drained_ srpc_drained[NCPU]
__attribute__((aligned(CACHE_LINE_SIZE)));

struct spcc_session {
    struct srpc_session cmn;
    int                 id;
    struct list_node    drained_link;
    /* drained_list's core number. -1 if not in the drained list */
    int                 drained_core;
    bool                is_linked;
    /* when this session has been drained (used for priority change) */
    uint64_t            drained_ts;
    bool                wake_up;
    waitgroup_t         send_waiter;
    int                 credit;
    /* the number of recently advertised credit */
    int                 advertised;
    int                 num_pending;
    /* Whether this session requires explicit credit */
    bool                need_ecredit;
    uint64_t            demand;
    /* timestamp for the last explicit credit issue */
    uint64_t            last_ecredit_timestamp;

    /* shared state between receiver and sender */
    DEFINE_BITMAP(avail_slots, SPCC_MAX_WINDOW);

    /* shared state between workers and sender */
    spinlock_t          lock;
    int                 closed;
    thread_t            *sender_th;
    DEFINE_BITMAP(completed_slots, SPCC_MAX_WINDOW);

    /* worker slots (one for each credit issued) */
    struct spcc_ctx     *slots[SPCC_MAX_WINDOW];
};

/* credit-related stats */
static atomic64_t srpc_stat_cupdate_rx_;
static atomic64_t srpc_stat_ecredit_tx_;
static atomic64_t srpc_stat_credit_tx_;
static atomic64_t srpc_stat_req_rx_;
static atomic64_t srpc_stat_req_dropped_;
static atomic64_t srpc_stat_resp_tx_;


/* (P)erformance-oriented (C)ongestion (C)ontrol state */

/* A lock to ensure only one thread performs the control logic. */
static spinlock_t               srpc_pcc_lock;

/* Timestamps indicating when the controller last woke up drained sessions.
 */
static uint64_t                 srpc_pcc_last_wakeup;

/* Timestamps indicating when the controller state machine should be updated
 * next.
 */
static uint64_t                 srpc_pcc_next_update;

/* State of the PCC controller. */
static enum spcc_ctl_state      srpc_pcc_state;

/* Per microexperiment performance statistics.
 *
 * The first element of the array (index 0) is not used by any microexperiment.
 * It is the default entry used by the program to update the stats if no
 * microexperiment is running. The next two elements in the array (index 1
 * and 2) are reserved for the two microexperiment.
 *
 * As of the following metrics are recorded for every microexperiment:
 *   1) Input requests received
 *   2) Output responses sent
 *   3) Dropped requests
 *   4) Start timestamp of the monitor interval
 *   5) End timestamp of the monitor interval
 *   6) Max queueing delay observed during the monitor interval
 *   7) Memory accesses
 *   8) Energy consumed
 */
static atomic64_t               srpc_pcc_in_reqs[SPCC_MAX_NUM_MICRO_EXPS+1];
static atomic64_t               srpc_pcc_out_resps[SPCC_MAX_NUM_MICRO_EXPS+1];
static atomic64_t               srpc_pcc_good_out_resps[SPCC_MAX_NUM_MICRO_EXPS+1];
static atomic64_t               srpc_pcc_drop_reqs[SPCC_MAX_NUM_MICRO_EXPS+1];
static atomic64_t               srpc_pcc_start_ts[SPCC_MAX_NUM_MICRO_EXPS+1];
static atomic64_t               srpc_pcc_end_ts[SPCC_MAX_NUM_MICRO_EXPS+1];
static atomic64_t               srpc_pcc_qdelay[SPCC_MAX_NUM_MICRO_EXPS+1];
static uint64_t                 srpc_pcc_mem_accesses[SPCC_MAX_NUM_MICRO_EXPS+1];
static double                   srpc_pcc_energy_consumed[SPCC_MAX_NUM_MICRO_EXPS+1];

/* Currently running microexperiment ID. Used to index in the stats arrays
 * described above. */
static int                      srpc_pcc_micro_exp_id;

/* The number of microexperiments performed till now. Only two microexperiments
 * are performed by the PCC controller. */
static int                      srpc_pcc_num_micro_exps;

/* The direction of change for each microexperiment. We want to perform the two
 * microexperiments in random order. Before starting the first microexperiment,
 * this array is filled with directions of credit pool change in a random order.
 * This array is indexed using the microexperiment ID. Value at index 0 is ignored.
 */
static int                      srpc_pcc_micro_exp_dirs[SPCC_MAX_NUM_MICRO_EXPS+1];

/* The original credit pool value, before we start performing the
 * microexperiments. */
static int                      srpc_pcc_orig_cp;


/* Helper to randomly initialize the microexperiment directions */
static inline void srpc_pcc_gen_micro_exp_dirs() {

    int do_incr = rand() % 2;
    srpc_pcc_micro_exp_dirs[1] = (do_incr) ? 1 : -1;
    srpc_pcc_micro_exp_dirs[2] = -srpc_pcc_micro_exp_dirs[1];
}


#if SPCC_TS_OUT
static void printRecord()
{
    int i;

    if (!ts_out)
        ts_out = fopen("timeseries.csv", "w");

    for (i = 0; i < TS_BUF_SIZE; ++i) {
        struct Event *event = &events[i];
        fprintf(ts_out, "%lu,%d,%d,%d,%d,%d,%d,%lu,%d,%lu\n",
                event->timestamp, event->credit_pool,
                event->credit_used, event->num_pending,
                event->num_drained, event->num_active,
                event->num_sess, event->delay,
                event->num_cores, event->avg_st);
    }
    fflush(ts_out);
}

static void record(int credit_pool, uint64_t delay)
{
    struct Event *event = &events[nextIndex];
    nextIndex = (nextIndex + 1) & TS_BUF_MASK;

    event->timestamp = microtime();
    event->credit_pool = credit_pool;
    event->credit_used = atomic_read(&srpc_credit_used[0]);
    event->num_pending = atomic_read(&srpc_num_pending[0]);
    event->num_drained = atomic_read(&srpc_num_drained[0]);
    event->num_active = atomic_read(&srpc_num_active[0]);
    event->num_sess = atomic_read(&srpc_num_sess[0]);
    event->delay = delay;
    event->num_cores = runtime_active_cores();
    event->avg_st = atomic_read(&srpc_avg_st[0]);

    if (nextIndex == 0)
        printRecord();
}
#endif

static int srpc_get_slot(struct spcc_session *s)
{
    int base;
    int slot = -1;
    for (base = 0; base < BITMAP_LONG_SIZE(SPCC_MAX_WINDOW); ++base) {
        slot = __builtin_ffsl(s->avail_slots[base]) - 1;
        if (slot >= 0)
            break;
    }

    if (slot >= 0) {
        slot += BITS_PER_LONG * base;
        bitmap_atomic_clear(s->avail_slots, slot);
        s->slots[slot] = smalloc(sizeof(struct spcc_ctx));
        s->slots[slot]->cmn.s = (struct srpc_session *)s;
        s->slots[slot]->cmn.idx = slot;
        s->slots[slot]->cmn.ds_credit = 0;
        s->slots[slot]->cmn.drop = false;
    }

    return slot;
}

static void srpc_put_slot(struct spcc_session *s, int slot)
{
    sfree(s->slots[slot]);
    s->slots[slot] = NULL;
    bitmap_atomic_set(s->avail_slots, slot);
}

static int srpc_send_ecredit(struct spcc_session *s)
{
    struct spcc_hdr shdr;
    int ret;

    /* craft the response header */
    shdr.magic = PCC_RESP_MAGIC;
    shdr.op = PCC_OP_CREDIT;
    shdr.len = 0;
    shdr.credit = (uint64_t)s->credit;

    /* send the packet */
    ret = tcp_write_full(s->cmn.c, &shdr, sizeof(shdr));
    if (unlikely(ret < 0))
        return ret;

    atomic64_inc(&srpc_stat_ecredit_tx_);

#if SPCC_TRACK_FLOW
    if (s->id == SPCC_TRACK_FLOW_ID) {
        printf("[%lu] <== ECredit: credit = %lu\n",
               microtime(), shdr.credit);
    }
#endif

    return 0;
}

static int srpc_send_completion_vector(struct spcc_session *s,
                                       unsigned long *slots)
{
    struct spcc_hdr shdr[SPCC_MAX_WINDOW];
    struct iovec v[SPCC_MAX_WINDOW * 2];
    int nriov = 0;
    int nrhdr = 0;
    int i;
    ssize_t ret = 0;

    bitmap_for_each_set(slots, SPCC_MAX_WINDOW, i) {
        struct spcc_ctx *c = s->slots[i];
        size_t len;
        char *buf;
        uint8_t flags = 0;

        if (!c->cmn.drop) {
            len = c->cmn.resp_len;
            buf = c->cmn.resp_buf;
        } else {
            len = c->cmn.req_len;
            buf = c->cmn.req_buf;
            flags |= PCC_SFLAG_DROP;
        }

        shdr[nrhdr].magic = PCC_RESP_MAGIC;
        shdr[nrhdr].op = PCC_OP_CALL;
        shdr[nrhdr].len = len;
        shdr[nrhdr].id = c->cmn.id;
        shdr[nrhdr].credit = (uint64_t)s->credit;
        shdr[nrhdr].ts_sent = c->ts_sent;
        shdr[nrhdr].flags = flags;

        v[nriov].iov_base = &shdr[nrhdr];
        v[nriov].iov_len = sizeof(struct spcc_hdr);
        nrhdr++;
        nriov++;

        if (len > 0) {
            v[nriov].iov_base = buf;
            v[nriov++].iov_len = len;
        }
    }

    /* send the completion(s) */
    if (nriov == 0)
        return 0;
    ret = tcp_writev_full(s->cmn.c, v, nriov);
    bitmap_for_each_set(slots, SPCC_MAX_WINDOW, i)
        srpc_put_slot(s, i);

#if SPCC_TRACK_FLOW
    if (s->id == SPCC_TRACK_FLOW_ID) {
        printf("[%lu] <=== Response (%d): credit=%d\n",
               microtime(), nrhdr, s->credit);
    }
#endif
    atomic_sub_and_fetch(&srpc_num_pending, nrhdr);
    atomic64_fetch_and_add(&srpc_stat_resp_tx_, nrhdr);

    if (unlikely(ret < 0))
        return ret;
    return 0;
}


static void srpc_update_credit(struct spcc_session *s, bool req_dropped)
{
    int credit_pool = atomic_read(&srpc_credit_pool);
    int credit_ds = atomic_read(&srpc_credit_ds);
    int credit_used = atomic_read(&srpc_credit_used);
    int num_sess = atomic_read(&srpc_num_sess);
    int old_credit = s->credit;
    int credit_diff;
    int credit_unused;
    int max_overprovision;

    if (credit_ds > 0)
        credit_pool = MIN(credit_pool, credit_ds);

    assert_spin_lock_held(&s->lock);

    if (s->drained_core != -1)
        return;

    credit_unused = credit_pool - credit_used;
    max_overprovision = MAX((int)(credit_unused / num_sess), 1);
    if (credit_used < credit_pool) {
        s->credit = MIN(s->num_pending + s->demand + max_overprovision,
                        s->credit + credit_unused);
    } else if (credit_used > credit_pool) {
        s->credit--;
    }

	if (s->wake_up || num_sess <= runtime_max_cores())
		s->credit = MAX(s->credit, max_overprovision);

	// prioritize the session
	if (old_credit > 0 && s->credit == 0 && !req_dropped)
		s->credit = max_overprovision;

    /* clamp to supported values */
    /* now we allow zero credit */
    s->credit = MAX(s->credit, s->num_pending);
    s->credit = MIN(s->credit, SPCC_MAX_WINDOW - 1);
    s->credit = MIN(s->credit, s->num_pending + s->demand + max_overprovision);

    credit_diff = s->credit - old_credit;
    atomic_fetch_and_add(&srpc_credit_used, credit_diff);
#if SPCC_TRACK_FLOW
    if (s->id == SPCC_TRACK_FLOW_ID) {
        printf("[%lu] credit update: credit_pool = %d, credit_used = %d, req_dropped = %d, num_pending = %d, demand = %d, num_sess = %d, old_credit = %d, new_credit = %d\n",
               microtime(), credit_pool, credit_used, req_dropped, s->num_pending, s->demand, num_sess, old_credit, s->credit);
    }
#endif
}

/* srpc_choose_drained_h: choose a drained session with high priority */
static struct spcc_session *srpc_choose_drained_h(int core_id)
{
    struct spcc_session *s;
    uint64_t now = microtime();
    int demand_timeout = MAX(CPCC_MAX_CLIENT_DELAY_US - SPCC_RTT_US, 0);

    assert(core_id >= 0);
    assert(core_id < runtime_max_cores());

    if (list_empty(&srpc_drained[core_id].list_h))
        return NULL;

    spin_lock_np(&srpc_drained[core_id].lock);

    // First check for the sessions with outdated demand
    while (true) {
        s = list_tail(&srpc_drained[core_id].list_h,
                      struct spcc_session,
                      drained_link);
        if (!s) break;

        spin_lock_np(&s->lock);
        if (now > (s->drained_ts + demand_timeout)) {
            // enough time has passed
            list_del(&s->drained_link);
            // move to low priority queue
            list_add_tail(&srpc_drained[core_id].list_l,
                          &s->drained_link);
        } else {
            spin_unlock_np(&s->lock);
            break;
        }
        spin_unlock_np(&s->lock);
    }

    if (list_empty(&srpc_drained[core_id].list_h)) {
        spin_unlock_np(&srpc_drained[core_id].lock);
        return NULL;
    }

    s = list_pop(&srpc_drained[core_id].list_h,
                 struct spcc_session,
                 drained_link);

    BUG_ON(!s->is_linked);
    s->is_linked = false;
    spin_unlock_np(&srpc_drained[core_id].lock);
    spin_lock_np(&s->lock);
    s->drained_core = -1;
    spin_unlock_np(&s->lock);
    atomic_dec(&srpc_num_drained);

    return s;
}

/* srpc_choose_drained_l: choose a drained session with low priority */
static struct spcc_session *srpc_choose_drained_l(int core_id)
{
    struct spcc_session *s;

    assert(core_id >= 0);
    assert(core_id < runtime_max_cores());

    if (list_empty(&srpc_drained[core_id].list_l))
        return NULL;

    spin_lock_np(&srpc_drained[core_id].lock);
    if (list_empty(&srpc_drained[core_id].list_l)) {
        spin_unlock_np(&srpc_drained[core_id].lock);
        return NULL;
    }

    s = list_pop(&srpc_drained[core_id].list_l,
                 struct spcc_session,
                 drained_link);

    assert(s->is_linked);
    s->is_linked = false;
    spin_unlock_np(&srpc_drained[core_id].lock);
    spin_lock_np(&s->lock);
    s->drained_core = -1;
    spin_unlock_np(&s->lock);
    atomic_dec(&srpc_num_drained);
#if SPCC_TRACK_FLOW
    if (s->id == SPCC_TRACK_FLOW_ID) {
        printf("[%lu] Session waken up\n", microtime());
    }
#endif

    return s;
}

static void srpc_remove_from_drained_list(struct spcc_session *s)
{
    assert_spin_lock_held(&s->lock);

    if (s->drained_core == -1)
        return;

    spin_lock_np(&srpc_drained[s->drained_core].lock);
    if (s->is_linked) {
        list_del(&s->drained_link);
        s->is_linked = false;
        atomic_dec(&srpc_num_drained);
#if SPCC_TRACK_FLOW
        if (s->id == SPCC_TRACK_FLOW_ID) {
            printf("[%lu] Seesion is removed from drained list\n",
                   microtime());
        }
#endif
    }
    spin_unlock_np(&srpc_drained[s->drained_core].lock);
    s->drained_core = -1;
}

/* wakeup_drained_session: wakes up drained session which will send explicit
 * credit if there is available credit in credit pool
 *
 * @num_session: the number of sessions to wake up
 * */
static void wakeup_drained_session(int num_session)
{
    unsigned int i;
    unsigned int core_id = get_current_affinity();
    unsigned int max_cores = runtime_max_cores();
    struct spcc_session *s;
    thread_t *th;

    while (num_session > 0) {
        s = srpc_choose_drained_h(core_id);

        i = (core_id + 1) % max_cores;
        while (!s && i != core_id) {
            s = srpc_choose_drained_h(i);
            i = (i + 1) % max_cores;
        }

        if (!s) {
            s = srpc_choose_drained_l(core_id);

            i = (core_id + 1) % max_cores;
            while (!s && i != core_id) {
                s = srpc_choose_drained_l(i);
                i = (i + 1) % max_cores;
            }
        }

        if (!s)
            break;

        spin_lock_np(&s->lock);
        BUG_ON(s->credit > 0);
        th = s->sender_th;
        s->sender_th = NULL;
        s->wake_up = true;
        s->credit = 1;
        spin_unlock_np(&s->lock);

        atomic_inc(&srpc_credit_used);

        if (th)
            thread_ready(th);
        num_session--;
    }
}

/*
 * Decrement credit pool by epsilon. Returns the new value
 * of the credit pool after decrementing. Does not update
 * the actual credit pool.
 */
static int decr_credit_pool()
{
    int num_sess = atomic_read(&srpc_num_sess);
    int curr_cp = atomic_read(&srpc_credit_pool);
    int new_cp;

    new_cp = curr_cp - SPCC_EPSILON;
	new_cp = MAX(new_cp, runtime_max_cores());
	new_cp = MIN(new_cp, num_sess << SPCC_MAX_WINDOW_EXP);

    SPCC_DEBUG_LOG("[%ld] Decreased credit pool: %d -> %d\n",
                   microtime(), curr_cp, new_cp);

	return new_cp;
}

/*
 * Increment credit pool by epsilon. Returns the new value
 * of the credit pool after incrementing. Does not update
 * the actual credit pool.
 */
static int incr_credit_pool()
{
    int num_sess = atomic_read(&srpc_num_sess);
    int curr_cp = atomic_read(&srpc_credit_pool);
    int new_cp;

    new_cp = curr_cp + SPCC_EPSILON;
	new_cp = MAX(new_cp, runtime_max_cores());
	new_cp = MIN(new_cp, num_sess << SPCC_MAX_WINDOW_EXP);

    SPCC_DEBUG_LOG("[%ld] Increased credit pool: %d -> %d\n",
                   microtime(), curr_cp, new_cp);

	return new_cp;
}

/*
 * Either increment or decrement the credit pool based on the
 * provided direction. @dir should either be 1 for incrementing
 * and -1 for decrementing the credit pool.
 */
static int update_credit_pool(int dir)
{
    assert(dir == 1 || dir == -1);

    int num_sess = atomic_read(&srpc_num_sess);
    int curr_cp = atomic_read(&srpc_credit_pool);
    int new_cp;

    new_cp = curr_cp + dir * SPCC_EPSILON;
	new_cp = MAX(new_cp, runtime_max_cores());
	new_cp = MIN(new_cp, num_sess << SPCC_MAX_WINDOW_EXP);

	return new_cp;
}


/**
 * PCC control implementation details:
 *
 * Unlike original PCC, this implementation is designed for RPC-layer
 * communication, instead of TCP-layer communication. And this implementation
 * implements a receiver-driven control, instead of the traditional sender-based
 * control. The controller uses PCC-like logic to modulate the RPC credit pool
 * in a way that optimizes some predefined objective.
 *
 * The PCC controller in our case runs in the data-path, i.e., whenever a request
 * has finished executing or is dropped on reception. This prevents the need
 * to have a separate control thread. Even though, having a control thread for
 * PCC makes it very simple to understand and implement the control logic.
 *
 * We implement the controller using a state machine approach.
 */
static void srpc_update_credit_pool()
{
    uint64_t now;
    int new_cp;
    int credit_used;
    int credit_unused;
    int micro_exp_id;
    struct spcc_micro_exp_stats stats[SPCC_MAX_NUM_MICRO_EXPS+1];
    bool do_wakeup;
    enum spcc_dir update_dir;
#if SPCC_MICRO_EXP_STRICT_LABELLING == 1
    int plus_micro_exp_id;
    int minus_micro_exp_id;
#endif

    if (!spin_try_lock_np(&srpc_pcc_lock)) {
        return;
    }

    /* Set a few default values */
    do_wakeup = false;

    /* Check if we need to perform any control */
    now = microtime();
    if (now < srpc_pcc_next_update) {
        /* No need to drive the state machine, simply perform the epilogue */
        goto exit;
    }

    /* Drive the state machine */
    switch (srpc_pcc_state) {
    case SPCC_CTL_STATE_PREPARE_MICRO_EXP:

        /* Get the microexperiment index */
        assert(srpc_pcc_num_micro_exps < SPCC_MAX_NUM_MICRO_EXPS);
        micro_exp_id = srpc_pcc_num_micro_exps + 1;

#if SPCC_MICRO_EXP_PERTURB_CB == 1
        /* Update the credit pool for the microexperiment */
        new_cp = update_credit_pool(srpc_pcc_micro_exp_dirs[micro_exp_id]);
        atomic_write(&srpc_credit_pool, new_cp);
        do_wakeup = true;
#endif

        /* Update the state */
        srpc_pcc_state = SPCC_CTL_STATE_START_MICRO_EXP;
        srpc_pcc_next_update = microtime() + SPCC_PRE_MI_US;

        SPCC_DEBUG_LOG("[%ld] Prepared microexperiment %d (dir=%d)\n",
                       now, micro_exp_id, srpc_pcc_micro_exp_dirs[micro_exp_id]);

        break;
    case SPCC_CTL_STATE_START_MICRO_EXP:

        /* Get the microexperiment index */
        assert(srpc_pcc_num_micro_exps < SPCC_MAX_NUM_MICRO_EXPS);
        micro_exp_id = srpc_pcc_num_micro_exps + 1;

        /* Clear the stats */
        atomic64_write(&srpc_pcc_in_reqs[micro_exp_id], 0);
        atomic64_write(&srpc_pcc_out_resps[micro_exp_id], 0);
        atomic64_write(&srpc_pcc_good_out_resps[micro_exp_id], 0);
        atomic64_write(&srpc_pcc_drop_reqs[micro_exp_id], 0);
        atomic64_write(&srpc_pcc_qdelay[micro_exp_id], runtime_queue_us());
        srpc_pcc_mem_accesses[micro_exp_id] = runtime_glob_mem_accesses();
        srpc_pcc_energy_consumed[micro_exp_id] = runtime_glob_energy_consumed();

        /* Set the start time for the monitor interval */
        atomic64_write(&srpc_pcc_start_ts[micro_exp_id], microtime());
        atomic64_write(&srpc_pcc_end_ts[micro_exp_id], microtime());

        /* Set the stat index for the workers to use */
        srpc_pcc_micro_exp_id = micro_exp_id;

        /* Update the state */
        srpc_pcc_state = SPCC_CTL_STATE_END_MICRO_EXP;
        srpc_pcc_next_update = microtime() + SPCC_MI_US;

        SPCC_DEBUG_LOG("[%ld] Started microexperiment %d (dir=%d)\n",
                       now, micro_exp_id, srpc_pcc_micro_exp_dirs[micro_exp_id]);

        break;
    case SPCC_CTL_STATE_END_MICRO_EXP:

        SPCC_DEBUG_LOG("[%ld] Finished microexperiment %d (dir=%d)\n",
                       now, srpc_pcc_micro_exp_id,
                       srpc_pcc_micro_exp_dirs[srpc_pcc_micro_exp_id]);

        micro_exp_id = srpc_pcc_micro_exp_id;

        /* Update any remaining stats */
        srpc_pcc_mem_accesses[micro_exp_id] = runtime_glob_mem_accesses() - \
            srpc_pcc_mem_accesses[micro_exp_id];
        srpc_pcc_energy_consumed[micro_exp_id] = runtime_glob_energy_consumed() - \
            srpc_pcc_energy_consumed[micro_exp_id];

        // Stop the microexperiment
        srpc_pcc_micro_exp_id = 0;
        srpc_pcc_num_micro_exps++;

        /* Perform the remaining microexperiments, if any */
        if (srpc_pcc_num_micro_exps < SPCC_MAX_NUM_MICRO_EXPS) {
            srpc_pcc_state = SPCC_CTL_STATE_PREPARE_MICRO_EXP;
            srpc_pcc_next_update = microtime(); /* move immediately */
            break;
        }

        /* We have performed all the microexperiments */
        SPCC_DEBUG_LOG("[%ld] Performed all microexperiments\n", now);

#if SPCC_MICRO_EXP_PERTURB_CB == 1
        /* Reset the rate */
        atomic_write(&srpc_credit_pool, srpc_pcc_orig_cp);
        do_wakeup = true;
#endif

        /* Update state */
        srpc_pcc_state = SPCC_CTL_STATE_MAKE_DECISION;
        srpc_pcc_next_update = microtime(); /* move immediately */

        break;
    case SPCC_CTL_STATE_MAKE_DECISION:

        /* Calculate the utilities for each microexperiment */
        for (int i = 1; i <= SPCC_MAX_NUM_MICRO_EXPS; ++i) {
            /* Read the performance stat */
            stats[i].duration = atomic64_read(&srpc_pcc_end_ts[i]) -    \
                atomic64_read(&srpc_pcc_start_ts[i]);
            stats[i].in_reqs = atomic64_read(&srpc_pcc_in_reqs[i]);
            stats[i].out_resps = atomic64_read(&srpc_pcc_out_resps[i]);
            stats[i].good_out_resps = atomic64_read(&srpc_pcc_good_out_resps[i]);
            stats[i].drop_reqs = atomic64_read(&srpc_pcc_drop_reqs[i]);
            stats[i].qdelay = atomic64_read(&srpc_pcc_qdelay[i]);
            stats[i].mem_accesses = srpc_pcc_mem_accesses[i];
            stats[i].energy_consumed = srpc_pcc_energy_consumed[i];

            /* Calculate the operator-defined utility */
            stats[i].utility = spcc_calc_util_fn(&stats[i]);

            SPCC_DEBUG_LOG("[%ld] Microexperiment=%d -> in_reqs=%ld, out_resps=%ld, good_out_resps=%ld,"
                           " drop_reqs=%ld, qdelay=%ld, duration=%ld, mem_accesses=%ld,"
                           " energy_consumed=%lf -> utility=%lf\n",
                           now, i, stats[i].in_reqs, stats[i].out_resps, stats[i].good_out_resps,
                           stats[i].drop_reqs, stats[i].qdelay, stats[i].duration,
                           stats[i].mem_accesses, stats[i].energy_consumed, stats[i].utility);
        }

        /* Get the current credit pool size */
        new_cp = atomic_read(&srpc_credit_pool);

#if SPCC_MICRO_EXP_STRICT_LABELLING == 1
        /* Under strict labelling, (+) microexperiment should receive more
         * load (i.e., input packets) than (-) microexperiment)
         */
        plus_micro_exp_id = (srpc_pcc_micro_exp_dirs[1] == 1) ? 1 : 2;
        minus_micro_exp_id = (srpc_pcc_micro_exp_dirs[1] == -1) ? 1 : 2;
        if (stats[plus_micro_exp_id].in_reqs <= stats[minus_micro_exp_id].in_reqs) {
            SPCC_DEBUG_LOG("[%ld] Inconclusive experiments. Microexperiment %d "
                           "(dir=1) received less load than microexperiment %d "
                           "(dir=-1)\n", now, plus_micro_exp_id, minus_micro_exp_id);
            goto skip_make_decision;
        }
#endif

        /* Compare the utilities */
        if (!stats[1].in_reqs || !stats[2].in_reqs) {
            update_dir = SPCC_DIR_PLUS;
        } else if (stats[1].in_reqs > stats[2].in_reqs) {
            /* If the received input requests (proxy for load) is more in the first
              microexperiment than the second microexperiment.*/
            SPCC_DEBUG_LOG("[%ld] Microexperiment 1 (dir=%d) received more"
                           " load than 2 (dir=%d)\n", now,
                           srpc_pcc_micro_exp_dirs[1], srpc_pcc_micro_exp_dirs[2]);

            update_dir = spcc_comp_util_fn(&stats[2], &stats[1]);
        } else if (stats[2].in_reqs > stats[1].in_reqs) {
            /* If the received input requests (proxy for load) is more in the second
              microexperiment than the first microexperiment.*/
            SPCC_DEBUG_LOG("[%ld] Microexperiment 2 (dir=%d) received more"
                           " load than 1 (dir=%d)\n", now,
                           srpc_pcc_micro_exp_dirs[2], srpc_pcc_micro_exp_dirs[1]);

            update_dir = spcc_comp_util_fn(&stats[1], &stats[2]);
        } else {
            /* Received the same number of input requests in both the microexperiments */
            SPCC_DEBUG_LOG("[%ld] Both microexperiments experienced same load\n", now);
        }

        /* Update the rate */
        switch (update_dir) {
        case SPCC_DIR_MINUS:
            new_cp = decr_credit_pool();
            break;
        case SPCC_DIR_STAY:
            break;
        case SPCC_DIR_PLUS:
            new_cp = incr_credit_pool();
            break;
        default:
            SPCC_DEBUG_LOG("[%ld] Invalid direction of change - %d\n", now, update_dir);
            break;
        }

        /* Update the rate */
        atomic_write(&srpc_credit_pool, new_cp);
        do_wakeup = true;

    skip_make_decision:
        /* Move back to the start state */
        srpc_pcc_num_micro_exps = 0;
        srpc_pcc_micro_exp_id = 0;
        srpc_pcc_gen_micro_exp_dirs();
        srpc_pcc_orig_cp = new_cp;
        srpc_pcc_state = SPCC_CTL_STATE_PREPARE_MICRO_EXP;
        srpc_pcc_next_update = microtime(); /* move immediately */

        break;
    default:
        break;
    }

exit:

    /* Check if we need to at least wake up any sessions */
    now = microtime();
    if (do_wakeup || (now - srpc_pcc_last_wakeup > SPCC_RTT_US)) {
        new_cp = atomic_read(&srpc_credit_pool);
        credit_used = atomic_read(&srpc_credit_used);
        credit_unused = new_cp - credit_used;
        wakeup_drained_session(credit_unused);
        srpc_pcc_last_wakeup = now;
    }

    spin_unlock_np(&srpc_pcc_lock);

#if SPCC_TS_OUT
    record(new_cp, qus);
#endif
}

/* srpc_handle_req_drop: a routine called when a request is dropped while
 * enqueueing
 *
 * @ qus: ingress queueing delay
 * */

static void srpc_handle_req_drop(uint64_t qus)
{
    srpc_update_credit_pool();
    return;
}

static void srpc_worker(void *arg)
{
    struct spcc_ctx *c = (struct spcc_ctx *)arg;
    struct spcc_session *s = (struct spcc_session *)c->cmn.s;
    uint64_t service_time;
    uint64_t avg_st;
    thread_t *th;
    uint64_t micro_exp_id = c->opaque;

    set_rpc_ctx((void *)&c->cmn);
    set_acc_qdel(runtime_queue_us() * cycles_per_us);
    c->cmn.drop = false;

    if (!c->cmn.drop) {
        service_time = microtime();
        srpc_handler((struct srpc_ctx *)c);
    }

    if (!c->cmn.drop) {
        service_time = microtime() - service_time;
        avg_st = atomic_read(&srpc_avg_st);
        avg_st = (uint64_t)(avg_st - (avg_st >> 3) + (service_time >> 3));

        atomic_write(&srpc_avg_st, avg_st);
        atomic_write(&srpc_credit_ds, c->cmn.ds_credit);
    } else {
        atomic64_inc(&srpc_stat_req_dropped_);
    }

    /* If the microexperiment associated with this request is still running,
     * then update the performance stats for the microexperiment.
     */
    if (srpc_pcc_micro_exp_id == micro_exp_id) {
        if (!c->cmn.drop) {
            atomic64_inc(&srpc_pcc_out_resps[micro_exp_id]);
            if (get_acc_qdel_us() < SPCC_QDELAY_BUDGET) {
                atomic64_inc(&srpc_pcc_good_out_resps[micro_exp_id]);
            }
        } else {
            atomic64_inc(&srpc_pcc_drop_reqs[micro_exp_id]);
        }
        atomic64_write(&srpc_pcc_end_ts[micro_exp_id], microtime());
        uint64_t qdelay = runtime_queue_us();
        if (qdelay > atomic64_read(&srpc_pcc_qdelay[micro_exp_id])) {
            atomic64_write(&srpc_pcc_qdelay[micro_exp_id], qdelay);
        }
    }

    spin_lock_np(&s->lock);
    bitmap_set(s->completed_slots, c->cmn.idx);
    th = s->sender_th;
    s->sender_th = NULL;
    spin_unlock_np(&s->lock);

    // update credit pool
    srpc_update_credit_pool();

    if (th)
        thread_ready(th);
}

static int srpc_recv_one(struct spcc_session *s)
{
    struct cpcc_hdr chdr;
    int idx, ret;
    thread_t *th;
    uint64_t old_demand;
    int credit_diff;
    char buf_tmp[SRPC_BUF_SIZE];
    struct spcc_ctx *c;
    uint64_t us;
    uint64_t micro_exp_id;

again:
    th = NULL;
    /* read the client header */
    ret = tcp_read_full(s->cmn.c, &chdr, sizeof(chdr));
    if (unlikely(ret <= 0)) {
        if (ret == 0)
            return -EIO;
        return ret;
    }

    /* parse the client header */
    if (unlikely(chdr.magic != PCC_REQ_MAGIC)) {
        log_warn("srpc: got invalid magic %x", chdr.magic);
        return -EINVAL;
    }
    if (unlikely(chdr.len > SRPC_BUF_SIZE)) {
        log_warn("srpc: request len %ld too large (limit %d)",
                 chdr.len, SRPC_BUF_SIZE);
        return -EINVAL;
    }

    switch (chdr.op) {
    case PCC_OP_CALL:
        micro_exp_id = (uint64_t)srpc_pcc_micro_exp_id;

        atomic64_inc(&srpc_stat_req_rx_);
        if (srpc_pcc_micro_exp_id == micro_exp_id) {
            atomic64_inc(&srpc_pcc_in_reqs[micro_exp_id]);
        }

        /* reserve a slot */
        idx = srpc_get_slot(s);
        if (unlikely(idx < 0)) {
            tcp_read_full(s->cmn.c, buf_tmp, chdr.len);
            atomic64_inc(&srpc_stat_req_dropped_);
            if (srpc_pcc_micro_exp_id == micro_exp_id) {
                atomic64_inc(&srpc_pcc_drop_reqs[micro_exp_id]);
                atomic64_write(&srpc_pcc_end_ts[micro_exp_id], microtime());
                us = runtime_queue_us();
                if (us > atomic64_read(&srpc_pcc_qdelay[micro_exp_id])) {
                    atomic64_write(&srpc_pcc_qdelay[micro_exp_id], us);
                }
            }
            return 0;
        }
        c = s->slots[idx];

        /* retrieve the payload */
        ret = tcp_read_full(s->cmn.c, c->cmn.req_buf, chdr.len);
        if (unlikely(ret <= 0)) {
            srpc_put_slot(s, idx);
            if (ret == 0)
                return -EIO;
            return ret;
        }

        c->cmn.req_len = chdr.len;
        c->cmn.resp_len = 0;
        c->cmn.id = chdr.id;
        c->ts_sent = chdr.ts_sent;
        c->opaque = micro_exp_id;

        spin_lock_np(&s->lock);
        old_demand = s->demand;
        s->demand = chdr.demand;
        srpc_remove_from_drained_list(s);
        s->num_pending++;
        /* adjust credit if demand changed */
        if (s->credit > s->num_pending + s->demand) {
            credit_diff = s->credit - (s->num_pending + s->demand);
            s->credit = s->num_pending + s->demand;
            atomic_sub_and_fetch(&srpc_credit_used, credit_diff);
        }

        atomic_inc(&srpc_num_pending);

        /* perform AQM */
        us = runtime_queue_us();
        if (us >= SPCC_QDELAY_BUDGET) {
            thread_t *th;

            // precedure called when the incoming request is dropped
            srpc_handle_req_drop(us);
            c->cmn.drop = true;
            bitmap_set(s->completed_slots, idx);
            th = s->sender_th;
            s->sender_th = NULL;
            spin_unlock_np(&s->lock);
            if (th)
                thread_ready(th);
            atomic64_inc(&srpc_stat_req_dropped_);
            if (srpc_pcc_micro_exp_id == micro_exp_id) {
                atomic64_inc(&srpc_pcc_drop_reqs[micro_exp_id]);
                atomic64_write(&srpc_pcc_end_ts[micro_exp_id], microtime());
                if (us > atomic64_read(&srpc_pcc_qdelay[micro_exp_id])) {
                    atomic64_write(&srpc_pcc_qdelay[micro_exp_id], us);
                }
            }
            goto again;
        }

        spin_unlock_np(&s->lock);

        ret = thread_spawn(srpc_worker, c);
        BUG_ON(ret);

#if SPCC_TRACK_FLOW
        uint64_t now = microtime();
        if (s->id == SPCC_TRACK_FLOW_ID) {
            printf("[%lu] ===> Request: id=%lu, demand=%lu, delay=%lu\n",
                   now, chdr.id, chdr.demand, now - s->last_ecredit_timestamp);
        }
#endif
        break;
    case PCC_OP_CREDIT:
        if (unlikely(chdr.len != 0)) {
            log_warn("srpc: cupdate has nonzero len");
            return -EINVAL;
        }
        assert(chdr.len == 0);

        spin_lock_np(&s->lock);
        old_demand = s->demand;
        s->demand = chdr.demand;

        BUG_ON(old_demand > 0);
        BUG_ON(s->drained_core > -1);
        // if s->num_pending > 0 do nothing.
        // sender thread will handle this session.
        if (s->num_pending == 0 && s->demand > 0) {
            // With positive demand
            // sender will handle this session
            if (s->num_pending == 0) {
                th = s->sender_th;
                s->sender_th = NULL;
                s->need_ecredit = true;
            }
        } else if (s->num_pending == 0) {
            // s->demand == 0
            // push the session to the low priority drained queue
            unsigned int core_id = get_current_affinity();

            spin_lock_np(&srpc_drained[core_id].lock);
            BUG_ON(s->is_linked);
            BUG_ON(s->credit > 0);
            // FIFO queue
            list_add_tail(&srpc_drained[core_id].list_l,
                          &s->drained_link);
            s->is_linked = true;
            spin_unlock_np(&srpc_drained[core_id].lock);
            s->drained_core = core_id;
            atomic_inc(&srpc_num_drained);
            s->advertised = 0;
        }

        /* adjust credit if demand changed */
        if (s->credit > s->num_pending + s->demand) {
            credit_diff = s->credit - (s->num_pending + s->demand);
            s->credit = s->num_pending + s->demand;
            atomic_sub_and_fetch(&srpc_credit_used, credit_diff);
        }
        spin_unlock_np(&s->lock);

        if (th)
            thread_ready(th);

        atomic64_inc(&srpc_stat_cupdate_rx_);
#if SPCC_TRACK_FLOW
        if (s->id == SPCC_TRACK_FLOW_ID) {
            printf("[%lu] ===> Winupdate: demand=%lu, \n",
                   microtime(), chdr.demand);
        }
#endif
        goto again;
    default:
        log_warn("srpc: got invalid op %d", chdr.op);
        return -EINVAL;
    }

    return ret;
}

static void srpc_sender(void *arg)
{
    DEFINE_BITMAP(tmp, SPCC_MAX_WINDOW);
    struct spcc_session *s = (struct spcc_session *)arg;
    int ret, i;
    bool sleep;
    int num_resp;
    unsigned int core_id;
    bool send_explicit_credit;
    int drained_core;
    int old_credit;
    int credit;
    int credit_issued;
    bool req_dropped;

    while (true) {
        /* find slots that have completed */
        spin_lock_np(&s->lock);
        while (true) {
            sleep = !s->closed && !s->need_ecredit && !s->wake_up &&
                bitmap_popcount(s->completed_slots,
                                SPCC_MAX_WINDOW) == 0;
            if (!sleep) {
                s->sender_th = NULL;
                break;
            }
            s->sender_th = thread_self();
            thread_park_and_unlock_np(&s->lock);
            spin_lock_np(&s->lock);
        }
        if (unlikely(s->closed)) {
            spin_unlock_np(&s->lock);
            break;
        }
        req_dropped = false;
        memcpy(tmp, s->completed_slots, sizeof(tmp));
        bitmap_init(s->completed_slots, SPCC_MAX_WINDOW, false);

        bitmap_for_each_set(tmp, SPCC_MAX_WINDOW, i) {
            struct spcc_ctx *c = s->slots[i];
            if (c->cmn.drop) {
                req_dropped = true;
                break;
            }
        }

        if (s->wake_up)
            srpc_remove_from_drained_list(s);

        drained_core = s->drained_core;
        num_resp = bitmap_popcount(tmp, SPCC_MAX_WINDOW);
        s->num_pending -= num_resp;
        old_credit = s->credit;
        srpc_update_credit(s, req_dropped);
        credit = s->credit;

        credit_issued = MAX(0, credit - old_credit + num_resp);
        atomic64_fetch_and_add(&srpc_stat_credit_tx_, credit_issued);

        send_explicit_credit = (s->need_ecredit || s->wake_up) &&
            num_resp == 0 && s->advertised < s->credit;

        if (num_resp > 0 || send_explicit_credit)
            s->advertised = s->credit;

        s->need_ecredit = false;
        s->wake_up = false;

        if (send_explicit_credit)
            s->last_ecredit_timestamp = microtime();
        spin_unlock_np(&s->lock);

        /* Send WINUPDATE message */
        if (send_explicit_credit) {
            ret = srpc_send_ecredit(s);
            if (unlikely(ret))
                goto close;
            continue;
        }

        /* send a response for each completed slot */
        ret = srpc_send_completion_vector(s, tmp);

        /* add to the drained list if (1) credit becomes zero,
         * (2) s is not in the list already,
         * (3) it has no outstanding requests */
        if (credit == 0 && drained_core == -1 &&
            bitmap_popcount(s->avail_slots, SPCC_MAX_WINDOW) ==
            SPCC_MAX_WINDOW) {
            core_id = get_current_affinity();
            spin_lock_np(&s->lock);
            spin_lock_np(&srpc_drained[core_id].lock);
            BUG_ON(s->is_linked);
            BUG_ON(s->credit > 0);
            if (s->demand > 0) {
                // positive demand: drained with high priority
                // LIFO queue
                list_add(&srpc_drained[core_id].list_h,
                         &s->drained_link);
                s->drained_ts = microtime();
            } else {
                // zero demand: drained with low priority
                // FIFO queue
                list_add_tail(&srpc_drained[core_id].list_l,
                              &s->drained_link);
            }
            s->is_linked = true;
            spin_unlock_np(&srpc_drained[core_id].lock);
            s->drained_core = core_id;
            atomic_inc(&srpc_num_drained);
            spin_unlock_np(&s->lock);
#if SPCC_TRACK_FLOW
            if (s->id == SPCC_TRACK_FLOW_ID) {
                printf("[%lu] Session is drained: credit=%d, drained_core = %d\n",
                       microtime(), credit, s->drained_core);
            }
#endif
        }
    }

close:
    /* wait for in-flight completions to finish */
    spin_lock_np(&s->lock);
    while (!s->closed ||
           bitmap_popcount(s->avail_slots, SPCC_MAX_WINDOW) +
           bitmap_popcount(s->completed_slots, SPCC_MAX_WINDOW) <
           SPCC_MAX_WINDOW) {
        s->sender_th = thread_self();
        thread_park_and_unlock_np(&s->lock);
        spin_lock_np(&s->lock);
        s->sender_th = NULL;
    }

    /* remove from the drained list */
    srpc_remove_from_drained_list(s);
    spin_unlock_np(&s->lock);

    /* free any left over slots */
    for (i = 0; i < SPCC_MAX_WINDOW; i++) {
        if (s->slots[i])
            srpc_put_slot(s, i);
    }

    /* notify server thread that the sender is done */
    waitgroup_done(&s->send_waiter);
}

static void srpc_server(void *arg)
{
    tcpconn_t *c = (tcpconn_t *)arg;
    struct spcc_session *s;
    struct rpc_session_info info;
    thread_t *th;
    int ret;

    s = smalloc(sizeof(*s));
    BUG_ON(!s);
    memset(s, 0, sizeof(*s));

    /* receive session info */
    ret = tcp_read_full(c, &info, sizeof(info));
    BUG_ON(ret <= 0);

    s->cmn.c = c;
    s->cmn.session_type = 0;
    s->drained_core = -1;
    s->id = atomic_fetch_and_add(&srpc_num_sess, 1) + 1;
    bitmap_init(s->avail_slots, SPCC_MAX_WINDOW, true);

    waitgroup_init(&s->send_waiter);
    waitgroup_add(&s->send_waiter, 1);

#if SPCC_TRACK_FLOW
    if (s->id == SPCC_TRACK_FLOW_ID) {
        printf("[%lu] connection established.\n",
               microtime());
    }
#endif

    ret = thread_spawn(srpc_sender, s);
    BUG_ON(ret);

    while (true) {
        ret = srpc_recv_one(s);
        if (ret)
            break;
    }

    spin_lock_np(&s->lock);
    th = s->sender_th;
    s->sender_th = NULL;
    s->closed = true;
    if (s->is_linked)
        srpc_remove_from_drained_list(s);
    atomic_sub_and_fetch(&srpc_credit_used, s->credit);
    atomic_sub_and_fetch(&srpc_num_pending, s->num_pending);
    s->num_pending = 0;
    s->demand = 0;
    spin_unlock_np(&s->lock);

    if (th)
        thread_ready(th);

    atomic_dec(&srpc_num_sess);
    waitgroup_wait(&s->send_waiter);
    tcp_close(c);
    sfree(s);

    /* initialize credits */
    if (atomic_read(&srpc_num_sess) == 0) {
        assert(atomic_read(&srpc_credit_used) == 0);
        assert(atomic_read(&srpc_num_drained) == 0);
        atomic_write(&srpc_credit_used, 0);
        atomic_write(&srpc_credit_pool, runtime_max_cores());
        atomic_write(&srpc_credit_ds, 0);
        fflush(stdout);
    }
}

static void srpc_listener(void *arg)
{
    waitgroup_t *wg_listener = (waitgroup_t *)arg;
    struct netaddr laddr;
    tcpconn_t *c;
    tcpqueue_t *q;
    int ret;
    int i;

    for (i = 0; i < NCPU; ++i) {
        spin_lock_init(&srpc_drained[i].lock);
        list_head_init(&srpc_drained[i].list_h);
        list_head_init(&srpc_drained[i].list_l);
    }

    atomic_write(&srpc_num_sess, 0);
    atomic_write(&srpc_num_drained, 0);
    atomic_write(&srpc_credit_pool, runtime_max_cores());
    atomic_write(&srpc_credit_used, 0);
    atomic_write(&srpc_num_pending, 0);
    atomic_write(&srpc_credit_ds, 0);
    atomic_write(&srpc_avg_st, 0);

    /* Init the PCC state. */
    spin_lock_init(&srpc_pcc_lock);
    srpc_pcc_state = SPCC_CTL_STATE_PREPARE_MICRO_EXP;
    srpc_pcc_next_update = microtime();
    srpc_pcc_last_wakeup = microtime();
    srpc_pcc_micro_exp_id = 0;
    srpc_pcc_num_micro_exps = 0;
    srpc_pcc_gen_micro_exp_dirs();
    srpc_pcc_orig_cp = atomic_read(&srpc_credit_pool);

    /* init stats */
    atomic64_write(&srpc_stat_cupdate_rx_, 0);
    atomic64_write(&srpc_stat_ecredit_tx_, 0);
    atomic64_write(&srpc_stat_credit_tx_, 0);
    atomic64_write(&srpc_stat_req_rx_, 0);
    atomic64_write(&srpc_stat_req_dropped_, 0);
    atomic64_write(&srpc_stat_resp_tx_, 0);

    laddr.ip = 0;
    laddr.port = SRPC_PORT;

    ret = tcp_listen(laddr, 4096, &q);
    BUG_ON(ret);

    waitgroup_done(wg_listener);

    while (true) {
        ret = tcp_accept(q, &c);
        if (WARN_ON(ret))
            continue;
        ret = thread_spawn(srpc_server, c);
        WARN_ON(ret);
    }
}

int spcc_enable(srpc_fn_t handler)
{
    static DEFINE_SPINLOCK(l);
    int ret;
    waitgroup_t wg_listener;

    spin_lock_np(&l);
    if (srpc_handler) {
        spin_unlock_np(&l);
        return -EBUSY;
    }
    srpc_handler = handler;
    spin_unlock_np(&l);

    waitgroup_init(&wg_listener);
    waitgroup_add(&wg_listener, 1);
    ret = thread_spawn(srpc_listener, &wg_listener);
    BUG_ON(ret);

    waitgroup_wait(&wg_listener);

    return 0;
}

void spcc_drop() {
    struct srpc_ctx *ctx = (struct srpc_ctx *)get_rpc_ctx();
    ctx->drop = true;
}

uint64_t spcc_stat_cupdate_rx()
{
    return atomic64_read(&srpc_stat_cupdate_rx_);
}

uint64_t spcc_stat_ecredit_tx()
{
    return atomic64_read(&srpc_stat_ecredit_tx_);
}

uint64_t spcc_stat_credit_tx()
{
    return atomic64_read(&srpc_stat_credit_tx_);
}

uint64_t spcc_stat_req_rx()
{
    return atomic64_read(&srpc_stat_req_rx_);
}

uint64_t spcc_stat_req_dropped()
{
    return atomic64_read(&srpc_stat_req_dropped_);
}

uint64_t spcc_stat_resp_tx()
{
    return atomic64_read(&srpc_stat_resp_tx_);
}

struct srpc_ops spcc_ops = {
    .srpc_enable            = spcc_enable,
    .srpc_drop              = spcc_drop,
    .srpc_stat_cupdate_rx   = spcc_stat_cupdate_rx,
    .srpc_stat_ecredit_tx   = spcc_stat_ecredit_tx,
    .srpc_stat_credit_tx    = spcc_stat_credit_tx,
    .srpc_stat_req_rx       = spcc_stat_req_rx,
    .srpc_stat_req_dropped  = spcc_stat_req_dropped,
    .srpc_stat_resp_tx      = spcc_stat_resp_tx,
};
