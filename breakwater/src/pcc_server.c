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

/* Enable PCC debug logs */
#define SPCC_CTL_DEBUG      1

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

/* A floating point credit pool value representing the accepted and currently set
 * credit pool value. We already have srpc_credit_pool which stores the credit
 * pool value using an integer. We use this additional floating point value to
 * keep track of partial credits that were added or deducted during the control.
 * Using integer credit pool results in losing those small partial credits and
 * might lead to incorrect results.
 */
static double                   srpc_pcc_curr_cp;

/* During the decision making state, this will be set to SPCC_DIR_BASE. At
 * the end of the decision making state either SPCC_DIR_INCR or SPCC_DIR_DECR
 * will be picked. And in the rate adjusting state, the same value will be
 * carried forward, until we enter decision making state again.
 */
static enum spcc_dir            srpc_pcc_dir;

/* Confidence value in the selected direction. This value is used to rapdily
 * increase/decrease the credit pool size while the controller is in the
 * rate adjusting state. Higher the value, higher the confidence the controller
 * has in the currently picked direction.
 */
static uint64_t                 srpc_pcc_n;

/* Rate change percentage, i.e., amount by which we perform credit pool
 * increase/decrease in every monitor interval. This value is used only in the
 * decision making state. This value can vary from SPCC_EPSILON_MIN to
 * SPCC_EPSILON_MAX.
 */
static double                   srpc_pcc_epsilon;

/* The utility value obtained in the previous decision making state or the
 * previous rate adjusting state. At the end of the decision making state
 * the controller sets this to the average utility across all repetitions
 * of the optimal direction (increase or decrease). At the end of rate adjusting
 * state, if we picked the new rate in the same direction, we set utility
 * of the new rate here.
 */
static double                   srpc_pcc_prev_util;

/* An array of directions used to decide whether to increase the rate or
 * decrease the rate while the controller is in the decision making state. This
 * is an array of +1(s) and -1(s), with a 0 at the end. There are SRPC_REPS
 * number of +1(s) and SRPC_REPS number of -1(s) in this array. This array
 * is randomly shuffled to randomize the order in which we perform the
 * microexperiments in the decision making state. Note, the 0 should always
 * appear at the end of the array.
 */
static enum spcc_dir            srpc_pcc_dirs[2 * SPCC_REPS + 1];
/* Index into the array of directions. Used in the decision making state to
 * keep track of which microexperiments are yet to be performed.
 */
static int                      srpc_pcc_dirs_idx;

/* Per-microexperiment performance statistics. Each microexperiment will
 * have one entry in these stats tables to populate the performance stats
 * observed during its monitor interval.
 *
 * The first element of the array (index 0) is not used by any microexperiment.
 * It is the default entry used by the program to update the stats if no
 * microexperiment is running.
 *
 * The first SPCC_REPS (index 1 to SPCC_REPS) entries are used by the rate
 * increase microexperiments. The last SPCC_REPS (index SPCC_REPS+1 to 2*SPCC_REPS)
 * entries are used by the rate decrease microexperiments.
 *
 * As of now four values are recorded for every microexperiment:
 *   1) Input requests received
 *   2) Output responses sent
 *   3) Dropped requests
 *   4) Processed packets (output + dropped)
 *   5) Start timestamp of the monitor interval
 *   6) End timestamp of the monitor interval
 *   7) Queueing delay sample during the monitor interval
 */
static atomic64_t               srpc_pcc_in_cnts[2 * SPCC_REPS + 1];
static atomic64_t               srpc_pcc_proc_cnts[2 * SPCC_REPS + 1];
static atomic64_t               srpc_pcc_out_cnts[2 * SPCC_REPS + 1];
static atomic64_t               srpc_pcc_drop_cnts[2 * SPCC_REPS + 1];
static atomic64_t               srpc_pcc_start_ts[2 * SPCC_REPS + 1];
static atomic64_t               srpc_pcc_end_ts[2 * SPCC_REPS + 1];
static atomic64_t               srpc_pcc_qdelays[2 * SPCC_REPS + 1];
/* The stat index is set by the controller to direct the worker threads
 * to update the correct stat entries while a microexperiment is running.
 */
static int                      srpc_pcc_stat_idx;

/* Variables keeping track of the how many rate increase and rate decrease
 * microexperiment repetitions have been performed while the controller is in
 * the decision making state.
 */
static int                      srpc_pcc_curr_incr_rep_cnt;
static int                      srpc_pcc_curr_decr_rep_cnt;


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


/* Function to randomly shuffle the order in which we will test the rate
 * increases and decreases in decision making state.
 */
static inline void srpc_pcc_dirs_shuffle() {
    for (int i = 2 * SPCC_REPS - 1; i > 0; --i) {
        int j = rand() % (i + 1);
        enum spcc_dir temp = srpc_pcc_dirs[i];
        srpc_pcc_dirs[i] = srpc_pcc_dirs[j];
        srpc_pcc_dirs[j] = temp;
    }
}

/**
 * PCC control implementation details:
 *
 * This code implements PCC-Allegro's control logic as described in -
 * https://www.usenix.org/system/files/conference/nsdi15/nsdi15-paper-dong.pdf
 *
 * Unlike original PCC-Allegro, this implementation is designed for RPC-layer
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
    double curr_cp;
    int new_cp;
    int credit_used;
    int credit_unused;
    enum spcc_dir dir;
    int stat_idx;
    int in_cnts[2 * SPCC_REPS + 1];
    int proc_cnts[2 * SPCC_REPS + 1];
    int out_cnts[2 * SPCC_REPS + 1];
    int drop_cnts[2 * SPCC_REPS + 1];
    int start_ts[2 * SPCC_REPS + 1];
    int end_ts[2 * SPCC_REPS + 1];
    int qdelays[2 * SPCC_REPS + 1];
    double utils[2 * SPCC_REPS + 1];
    bool wait_more;
    int incr_better_cnt;
    int decr_better_cnt;
    bool do_wakeup;

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
    case SPCC_CTL_STATE_DECISION_MAKING_SET_RATE:

        assert(srpc_pcc_dir == SPCC_DIR_BASE);

        /* Update the rate */
        curr_cp = srpc_pcc_curr_cp;
        dir = srpc_pcc_dirs[srpc_pcc_dirs_idx];
        new_cp = (1.0 + dir * srpc_pcc_epsilon) * curr_cp;
        new_cp = MAX(new_cp, runtime_max_cores());
        new_cp = MIN(new_cp, atomic_read(&srpc_num_sess) << SPCC_MAX_WINDOW_EXP);
        atomic_write(&srpc_credit_pool, new_cp);

        /* Wake up drained sessions */
        do_wakeup = true;

        if (dir == SPCC_DIR_BASE) {
            /* Stop */
            srpc_pcc_stat_idx = 0;
            srpc_pcc_state = SPCC_CTL_STATE_DECISION_MAKING_END;
            srpc_pcc_next_update = 0; /* move immediately */
            break;
        }

        /* Update the state */
        srpc_pcc_state = SPCC_CTL_STATE_DECISION_MAKING_START_MONITOR;
        srpc_pcc_next_update = microtime() + SPCC_PRE_MI_US;

#if SPCC_CTL_DEBUG == 1
        printf("[%ld] - DECISION_MAKING_SET_RATE -"
               " rep_idx=%d, dir=%d, curr_cp=%lf, new_cp=%d\n",
               now, srpc_pcc_dirs_idx, dir, curr_cp, new_cp);
#endif

        break;
    case SPCC_CTL_STATE_DECISION_MAKING_START_MONITOR:

        /* Get the stat index */
        dir = srpc_pcc_dirs[srpc_pcc_dirs_idx];
        assert(dir != SPCC_DIR_BASE);

#if SPCC_CTL_DEBUG == 1
        printf("[%ld] - DECISION_MAKING_START_MONITOR -"
               " rep_idx=%d, dir=%d\n",
               now, srpc_pcc_dirs_idx, dir);
#endif

        if (dir == SPCC_DIR_INCR) {
            srpc_pcc_curr_incr_rep_cnt++;
            stat_idx = srpc_pcc_curr_incr_rep_cnt;
        } else if (dir == SPCC_DIR_DECR) {
            srpc_pcc_curr_decr_rep_cnt++;
            stat_idx = srpc_pcc_curr_decr_rep_cnt + SPCC_REPS;
        }

#if SPCC_CTL_DEBUG == 1
        printf("[%ld] - DECISION_MAKING_START_MONITOR -"
               " stat_idx=%d\n", now, stat_idx);
#endif

        /* Clear the stats */
        atomic64_write(&srpc_pcc_in_cnts[stat_idx], 0);
        atomic64_write(&srpc_pcc_out_cnts[stat_idx], 0);
        atomic64_write(&srpc_pcc_drop_cnts[stat_idx], 0);
        atomic64_write(&srpc_pcc_proc_cnts[stat_idx], 0);

        /* Set the start time for the monitor interval */
        atomic64_write(&srpc_pcc_start_ts[stat_idx], microtime());

        /* Set the stat index for the workers to use */
        srpc_pcc_stat_idx = stat_idx;

        /* Update the state */
        srpc_pcc_state = SPCC_CTL_STATE_DECISION_MAKING_END_MONITOR;
        srpc_pcc_next_update = microtime() + SPCC_MI_US;

        break;
    case SPCC_CTL_STATE_DECISION_MAKING_END_MONITOR:

#if SPCC_CTL_DEBUG == 1
        printf("[%ld] - DECISION_MAKING_END_MONITOR -"
               " stat_idx=%d\n", now, srpc_pcc_stat_idx);
#endif

        /* Update stats, if any */
        atomic64_write(&srpc_pcc_qdelays[srpc_pcc_stat_idx], runtime_queue_us());

        srpc_pcc_stat_idx = 0;
        srpc_pcc_dirs_idx++;

        /* Update the state */
        srpc_pcc_state = SPCC_CTL_STATE_DECISION_MAKING_SET_RATE;
        srpc_pcc_next_update = 0; /* move immediately */

        break;
    case SPCC_CTL_STATE_DECISION_MAKING_END:

        /* Wait for the monitor interval results to accumulate */
        wait_more = false;
        for (int i = 1; i < 2 * SPCC_REPS + 1; ++i) {
            in_cnts[i] = atomic64_read(&srpc_pcc_in_cnts[i]);
            proc_cnts[i] = atomic64_read(&srpc_pcc_proc_cnts[i]);
            if (proc_cnts[i] < in_cnts[i]) {
                /* We need to wait more */
                wait_more = true;
                break;
            }
            out_cnts[i] = atomic64_read(&srpc_pcc_out_cnts[i]);
            drop_cnts[i] = atomic64_read(&srpc_pcc_drop_cnts[i]);
            start_ts[i] = atomic64_read(&srpc_pcc_start_ts[i]);
            end_ts[i] = atomic64_read(&srpc_pcc_end_ts[i]);
            qdelays[i] = atomic64_read(&srpc_pcc_qdelays[i]);
        }
        if (wait_more) {
#if SPCC_CTL_DEBUG == 1
            printf("[%ld] - DECISION_MAKING_END - Need to wait more\n", now);
#endif
            srpc_pcc_next_update = microtime() + SPCC_RTT_US;
            break;
        }

        /* Calculate the utilities */
        for (int i = 1; i < 2 * SPCC_REPS + 1; ++i) {
            utils[i] = spcc_util_fn(in_cnts[i], out_cnts[i], drop_cnts[i],
                                    qdelays[i], end_ts[i] - start_ts[i]);
#if SPCC_CTL_DEBUG == 1
            printf("[%ld] - DECISION_MAKING_END -"
                   " stat_idx=%d, in_cnt=%d, proc_cnt=%d,"
                   " out_cnt=%d, drop_cnt=%d, qdelay=%d, duration=%d,"
                   " utility=%lf\n",
                   now, i, in_cnts[i], proc_cnts[i], out_cnts[i],
                   drop_cnts[i], qdelays[i], end_ts[i] - start_ts[i], utils[i]);
#endif
        }

        /* Compare the utilities */
        incr_better_cnt = 0;
        decr_better_cnt = 0;
        for (int i = 1; i < SPCC_REPS + 1; ++i) {
            for (int j = SPCC_REPS + 1; j < 2 * SPCC_REPS + 1; ++j) {
                /* Check if the increase test received more requests than the
                 * decrease test. This is crucial to ensure that we are testing
                 * receiving more requests vs receiving less requests. If it
                 * is not the case, then the tests are inconclusive.
                 */
                if (in_cnts[i] <= in_cnts[j]) {
                    goto inconclusive;
                }

                double incr_util = utils[i];
                double decr_util = utils[j];
                if (incr_util > decr_util) {
                    ++incr_better_cnt;
                } else if (incr_util < decr_util) {
                    ++decr_better_cnt;
                } else {
                    goto inconclusive;
                }
            }
        }

        if (incr_better_cnt == SPCC_REPS * SPCC_REPS) {
            /* If rate increase was consistently better */

            /* Update the rate */
            curr_cp = srpc_pcc_curr_cp;
            curr_cp = (1.0 + srpc_pcc_epsilon) * curr_cp;
            curr_cp = MAX(curr_cp, (double)runtime_max_cores());
            curr_cp = MIN(curr_cp, (double)(atomic_read(&srpc_num_sess) << SPCC_MAX_WINDOW_EXP));
            srpc_pcc_curr_cp = curr_cp;
            new_cp = (int)curr_cp;
            atomic_write(&srpc_credit_pool, new_cp);

            /* Wake up drained sessions */
            do_wakeup = true;

            /* Set the direction */
            srpc_pcc_dir = SPCC_DIR_INCR;
            srpc_pcc_n = 1;
            srpc_pcc_epsilon = SPCC_EPSILON_MIN;

            /* Set the utility to average of all rate increase experiments */
            srpc_pcc_prev_util = 0.0;
            for (int i = 1; i < SPCC_REPS + 1; ++i) {
                srpc_pcc_prev_util += utils[i];
            }
            srpc_pcc_prev_util /= SPCC_REPS;

            /* Update the state */
            srpc_pcc_state = SPCC_CTL_STATE_RATE_ADJUSTING_SET_RATE;
            srpc_pcc_next_update = 0; /* move immediately */

#if SPCC_CTL_DEBUG == 1
            printf("[%ld] - DECISION_MAKING_END -"
                   " Rate Increased - new_cp=%lf\n", now, curr_cp);
#endif

            break;

        } else if (decr_better_cnt == SPCC_REPS * SPCC_REPS) {
            /* If rate decrease was consistently better */

            /* Update the rate */
            curr_cp = srpc_pcc_curr_cp;
            curr_cp = (1.0 - srpc_pcc_epsilon) * curr_cp;
            curr_cp = MAX(curr_cp, (double)runtime_max_cores());
            curr_cp = MIN(curr_cp, (double)(atomic_read(&srpc_num_sess) << SPCC_MAX_WINDOW_EXP));
            srpc_pcc_curr_cp = curr_cp;
            new_cp = (int)curr_cp;
            atomic_write(&srpc_credit_pool, new_cp);

            /* Wake up drained sessions */
            do_wakeup = true;

            /* Set the direction */
            srpc_pcc_dir = SPCC_DIR_DECR;
            srpc_pcc_n = 1;
            srpc_pcc_epsilon = SPCC_EPSILON_MIN;

            /* Set the utility to average of all rate decrease experiments */
            srpc_pcc_prev_util = 0.0;
            for (int i = SPCC_REPS + 1; i < 2 * SPCC_REPS + 1; ++i) {
                srpc_pcc_prev_util += utils[i];
            }
            srpc_pcc_prev_util /= SPCC_REPS;

            /* Update the state */
            srpc_pcc_state = SPCC_CTL_STATE_RATE_ADJUSTING_SET_RATE;
            srpc_pcc_next_update = 0; /* move immediately */

#if SPCC_CTL_DEBUG == 1
            printf("[%ld] - DECISION_MAKING_END -"
                   " Rate Decreased - new_cp=%lf\n", now, curr_cp);
#endif

            break;
        }

        /* The tests were inconclusive */
    inconclusive:

        /* Increase the rate change granularity */
        srpc_pcc_epsilon = MIN(srpc_pcc_epsilon + SPCC_EPSILON_MIN, SPCC_EPSILON_MAX);

        /* Reset the decision making state */
        srpc_pcc_dirs_shuffle();
        srpc_pcc_dirs_idx = 0;
        srpc_pcc_curr_incr_rep_cnt = 0;
        srpc_pcc_curr_decr_rep_cnt = 0;

        /* Update the state */
        srpc_pcc_state = SPCC_CTL_STATE_DECISION_MAKING_SET_RATE;
        srpc_pcc_next_update = 0; /* move immediately */

#if SPCC_CTL_DEBUG == 1
        printf("[%ld] - DECISION_MAKING_END - Inconclusive\n", now);
#endif

        break;
    case SPCC_CTL_STATE_RATE_ADJUSTING_SET_RATE:

        assert(srpc_pcc_dir != SPCC_DIR_BASE);

        /* Update the rate in the same direction */
        curr_cp = srpc_pcc_curr_cp;
        new_cp = (1.0 + (double)srpc_pcc_dir * (double)srpc_pcc_n * SPCC_EPSILON_MIN) * curr_cp;
        new_cp = MAX(new_cp, runtime_max_cores());
        new_cp = MIN(new_cp, atomic_read(&srpc_num_sess) << SPCC_MAX_WINDOW_EXP);
        atomic_write(&srpc_credit_pool, new_cp);

        /* Wake up drained sessions */
        do_wakeup = true;

        /* Update the state */
        srpc_pcc_state = SPCC_CTL_STATE_RATE_ADJUSTING_START_MONITOR;
        srpc_pcc_next_update = microtime() + SPCC_PRE_MI_US;

#if SPCC_CTL_DEBUG == 1
        printf("[%ld] - RATE_ADJUSTING_SET_RATE -"
               " dir=%d, n=%ld, curr_cp=%lf, new_cp=%d\n",
               now, srpc_pcc_dir, srpc_pcc_n, curr_cp, new_cp);
#endif

        break;
    case SPCC_CTL_STATE_RATE_ADJUSTING_START_MONITOR:

        /* Get the stat index */
        if (srpc_pcc_dir == SPCC_DIR_INCR) {
            stat_idx = 1;
        } else if (srpc_pcc_dir == SPCC_DIR_DECR) {
            stat_idx = SPCC_REPS + 1;
        }

        /* Clear the stats */
        atomic64_write(&srpc_pcc_in_cnts[stat_idx], 0);
        atomic64_write(&srpc_pcc_out_cnts[stat_idx], 0);
        atomic64_write(&srpc_pcc_drop_cnts[stat_idx], 0);
        atomic64_write(&srpc_pcc_proc_cnts[stat_idx], 0);

        /* Set the start time for the monitor interval */
        atomic64_write(&srpc_pcc_start_ts[stat_idx], microtime());

        /* Set the stat index for the workers to use */
        srpc_pcc_stat_idx = stat_idx;

        /* Update the state */
        srpc_pcc_state = SPCC_CTL_STATE_RATE_ADJUSTING_END_MONITOR;
        srpc_pcc_next_update = microtime() + SPCC_MI_US;

#if SPCC_CTL_DEBUG == 1
        printf("[%ld] - RATE_ADJUSTING_UPDATE_MONITOR -"
               " dir=%d, n=%ld\n", now, srpc_pcc_dir, srpc_pcc_n);
#endif

        break;
    case SPCC_CTL_STATE_RATE_ADJUSTING_END_MONITOR:

#if SPCC_CTL_DEBUG == 1
        printf("[%ld] - RATE_ADJUSTING_END_MONITOR -"
               " stat_idx=%d\n", now, srpc_pcc_stat_idx);
#endif

        /* Update stats, if any */
        atomic64_write(&srpc_pcc_qdelays[srpc_pcc_stat_idx], runtime_queue_us());

        srpc_pcc_stat_idx = 0;

        /* Update the state */
        srpc_pcc_state = SPCC_CTL_STATE_RATE_ADJUSTING_RESET_RATE;
        srpc_pcc_next_update = 0; /* move immediately */

        break;
    case SPCC_CTL_STATE_RATE_ADJUSTING_RESET_RATE:

        /* Update the rate to base */
        curr_cp = srpc_pcc_curr_cp;
        new_cp = (int)curr_cp;
        new_cp = MAX(new_cp, runtime_max_cores());
        new_cp = MIN(new_cp, atomic_read(&srpc_num_sess) << SPCC_MAX_WINDOW_EXP);
        atomic_write(&srpc_credit_pool, new_cp);

        /* Wake up drained sessions */
        do_wakeup = true;

        /* Update the state */
        srpc_pcc_state = SPCC_CTL_STATE_RATE_ADJUSTING_END;
        srpc_pcc_next_update = 0; /* move immediately */

#if SPCC_CTL_DEBUG == 1
        printf("[%ld] - RATE_ADJUSTING_RESET_RATE -"
               " dir=%d, n=%ld, curr_cp=%lf, new_cp=%d\n",
               now, srpc_pcc_dir, srpc_pcc_n, curr_cp, new_cp);
#endif

        break;
    case SPCC_CTL_STATE_RATE_ADJUSTING_END:

        /* Get the stat index where we have to look for the results */
        if (srpc_pcc_dir == SPCC_DIR_INCR) {
            stat_idx = 1;
        } else if (srpc_pcc_dir == SPCC_DIR_DECR) {
            stat_idx = SPCC_REPS + 1;
        }

        /* If we have not yet received the monitor interval worth of results */
        in_cnts[stat_idx] = atomic64_read(&srpc_pcc_in_cnts[stat_idx]);
        proc_cnts[stat_idx] = atomic64_read(&srpc_pcc_proc_cnts[stat_idx]);
        if (proc_cnts[stat_idx] < in_cnts[stat_idx]) {
            /* We need to wait more */
#if SPCC_CTL_DEBUG == 1
            printf("[%ld] - RATE_ADJUSTING_END - Need to wait more\n", now);
#endif
            srpc_pcc_next_update = microtime() + SPCC_RTT_US;
            break;
        }
        out_cnts[stat_idx] = atomic64_read(&srpc_pcc_out_cnts[stat_idx]);
        drop_cnts[stat_idx] = atomic64_read(&srpc_pcc_drop_cnts[stat_idx]);
        start_ts[stat_idx] = atomic64_read(&srpc_pcc_start_ts[stat_idx]);
        end_ts[stat_idx] = atomic64_read(&srpc_pcc_end_ts[stat_idx]);
        qdelays[stat_idx] = atomic64_read(&srpc_pcc_qdelays[stat_idx]);

        /* Calculate the utility for the previous experiment */
        utils[stat_idx] = spcc_util_fn(in_cnts[stat_idx], out_cnts[stat_idx],
                                       drop_cnts[stat_idx], qdelays[stat_idx],
                                       end_ts[stat_idx] - start_ts[stat_idx]);
#if SPCC_CTL_DEBUG == 1
            printf("[%ld] - RATE_ADJUSTING_END -"
                   " in_cnt=%d, proc_cnt=%d,"
                   " out_cnt=%d, drop_cnt=%d, duration=%d,"
                   " utility=%lf, prev_utility=%lf\n",
                   now, in_cnts[stat_idx], proc_cnts[stat_idx],
                   out_cnts[stat_idx], drop_cnts[stat_idx],
                   end_ts[stat_idx] - start_ts[stat_idx], utils[stat_idx],
                   srpc_pcc_prev_util);
#endif

        /* XXX: We probably need to check if we received more packets, if
         *      we tested a rate increase, or if we received less packets,
         *      if we tested a rate decrease. Leaving that for later. */

        /* Compare the utilities */
        if (utils[stat_idx] >= srpc_pcc_prev_util) {
            /* Set the rate in the same direction */
            curr_cp = srpc_pcc_curr_cp;
            curr_cp = (1.0 + (double)srpc_pcc_dir * (double)srpc_pcc_n * SPCC_EPSILON_MIN) * curr_cp;
            curr_cp = MAX(curr_cp, (double)runtime_max_cores());
            curr_cp = MIN(curr_cp, (double)(atomic_read(&srpc_num_sess) << SPCC_MAX_WINDOW_EXP));
            srpc_pcc_curr_cp = curr_cp;
            new_cp = (int)curr_cp;
            atomic_write(&srpc_credit_pool, new_cp);

            /* Wake up drained sessions */
            do_wakeup = true;

            /* Increase the confidence */
            ++srpc_pcc_n;

            /* Save the current utility */
            srpc_pcc_prev_util = utils[stat_idx];

            /* Update the state */
            srpc_pcc_state = SPCC_CTL_STATE_RATE_ADJUSTING_SET_RATE;
            srpc_pcc_next_update = 0; /* move immediately */

#if SPCC_CTL_DEBUG == 1
            printf("[%ld] - RATE_ADJUSTING_END -"
                   " Picked Same Direction - dir=%d, n=%ld, new_cp=%lf\n",
                   now, srpc_pcc_dir, srpc_pcc_n, curr_cp);
#endif
            break;
        }

        /* The new rate was not better, need to move to decision making state */
        srpc_pcc_dir = SPCC_DIR_BASE;
        srpc_pcc_n = 0;
        srpc_pcc_epsilon = SPCC_EPSILON_MIN;
        srpc_pcc_dirs_shuffle();
        srpc_pcc_dirs_idx = 0;
        srpc_pcc_curr_incr_rep_cnt = 0;
        srpc_pcc_curr_decr_rep_cnt = 0;

        /* Update the state */
        srpc_pcc_state = SPCC_CTL_STATE_DECISION_MAKING_SET_RATE;
        srpc_pcc_next_update = 0; /* move immediately */

#if SPCC_CTL_DEBUG == 1
        printf("[%ld] - RATE_ADJUSTING_END - Moving to Decision Making\n", now);
#endif

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
        atomic64_inc(&srpc_pcc_out_cnts[c->opaque]);
    } else {
        atomic64_inc(&srpc_stat_req_dropped_);
        atomic64_inc(&srpc_pcc_drop_cnts[c->opaque]);
    }

    atomic64_inc(&srpc_pcc_proc_cnts[c->opaque]);
    atomic64_write(&srpc_pcc_end_ts[c->opaque], microtime());

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
        atomic64_inc(&srpc_stat_req_rx_);
        /* reserve a slot */
        idx = srpc_get_slot(s);
        if (unlikely(idx < 0)) {
            tcp_read_full(s->cmn.c, buf_tmp, chdr.len);
            atomic64_inc(&srpc_stat_req_dropped_);
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
        c->opaque = (uint64_t)srpc_pcc_stat_idx;

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
        atomic64_inc(&srpc_pcc_in_cnts[c->opaque]);

        /* perform AQM */
        us = runtime_queue_us();
        if (us >= SPCC_DROP_THRESH) {
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
            atomic64_inc(&srpc_pcc_drop_cnts[c->opaque]);
            atomic64_inc(&srpc_pcc_proc_cnts[c->opaque]);
            atomic64_write(&srpc_pcc_end_ts[c->opaque], microtime());
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
    srpc_pcc_last_wakeup = microtime();
    srpc_pcc_next_update = microtime();

    srpc_pcc_state = SPCC_CTL_STATE_DECISION_MAKING_SET_RATE;
    srpc_pcc_curr_cp = (double)atomic_read(&srpc_credit_pool);

    srpc_pcc_dir = SPCC_DIR_BASE;
    srpc_pcc_n = 0;
    srpc_pcc_epsilon = SPCC_EPSILON_MIN;

    for (int i = 0; i < SPCC_REPS; ++i) {
        srpc_pcc_dirs[i] = SPCC_DIR_INCR;
        srpc_pcc_dirs[i + SPCC_REPS] = SPCC_DIR_DECR;
    }
    srpc_pcc_dirs[2 * SPCC_REPS]= SPCC_DIR_BASE;
    srpc_pcc_dirs_shuffle();
    srpc_pcc_dirs_idx = 0;

    srpc_pcc_stat_idx = 0;
    srpc_pcc_curr_incr_rep_cnt = 0;
    srpc_pcc_curr_decr_rep_cnt = 0;

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
